module;

#include <common.hxx>

export module rawmouse;

import common;
import settings;
import logging;

// What is actually wrong with the mouse:
//
// The engine reads it through DirectInput one buffered packet at a time and fires a separate
// input event for each, so how the frame's movement is put together depends on how many packets
// the mouse sent. Everything then goes through UPlayerInput::SmoothMouse, a half-and-half filter
// with a borrow term, a decay tail that keeps moving the view for up to 0.07s after the mouse
// has stopped, and - the part that hurts - a clamp of the smoothed value to +/-1. Move the mouse
// quickly and the clamp eats the difference, which is why fast flicks go nowhere and why the
// feel changes with the polling rate.
//
// So: take the deltas from raw input instead, where one frame is one summed delta whatever the
// polling rate, write that in place of what DirectInput accumulated, and jump the smoothing
// filter over its own body so the clamp and the tail never run.
//
// UPlayerInput +0x30 is the APlayerController it lives within. On the controller,
// aMouseX is +0x34C and aMouseY is +0x350.
static constexpr auto nOffsetPlayerController = 0x30;
static constexpr auto nOffsetMouseX = 0x34C;
static constexpr auto nOffsetMouseY = 0x350;

// UInput::ReadInput multiplies every axis by 20/DeltaTime before the input event runs, and the
// script that consumes aTurn multiplies by DeltaTime again. The two cancel, which is what makes
// the stock look speed frame rate independent. Raw deltas go in with the same factor applied, so
// a sensitivity of 1.0 feels exactly like the game does now, only without the clamp.
static constexpr auto fEngineAxisScale = 20.0f;

// Not a setting. The only thing that clears it is raw input failing to come up, which falls the
// game back on its own DirectInput path rather than leaving the mouse dead.
static std::atomic<bool> bRawMouseInput = true;
static std::atomic<float> fMouseSensitivity = 1.0f;

static std::atomic<int32_t> nAccumulatedX = 0;
static std::atomic<int32_t> nAccumulatedY = 0;

static HWND hGameWindow = nullptr;
static WNDPROC pOriginalWndProc = nullptr;

static SafetyHookInline shDealWithPlayerInputEvent{};

// Written while the setting is off, restored while it is on, so the filter can be switched back
// and forth mid game.
static std::unique_ptr<raw_mem> patchSkipSmoothing;
static std::unique_ptr<raw_mem> patchNoFovScaling;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT nMessage, WPARAM wParam, LPARAM lParam)
{
    if (nMessage == WM_INPUT && bRawMouseInput)
    {
        RAWINPUT input{};
        UINT nSize = sizeof(input);

        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &nSize, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
            && input.header.dwType == RIM_TYPEMOUSE
            && (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
        {
            nAccumulatedX += input.data.mouse.lLastX;
            nAccumulatedY += input.data.mouse.lLastY;
        }
    }

    return CallWindowProcW(pOriginalWndProc, hWnd, nMessage, wParam, lParam);
}

// FindWindowEx walks every top-level window on the desktop, not this process's, and the viewport
// class name belongs to every Unreal Engine 2 game. Without the process check a second copy of the
// game, or any other UE2 title, is a window we would try and fail to subclass.
static HWND FindViewportWindow()
{
    for (HWND hWindow = nullptr; (hWindow = FindWindowExA(nullptr, hWindow, "WWindowsViewportWindow", nullptr)) != nullptr; )
    {
        DWORD nProcessId = 0;
        GetWindowThreadProcessId(hWindow, &nProcessId);

        if (nProcessId == GetCurrentProcessId())
            return hWindow;
    }

    return nullptr;
}

// Deferred until the first frame of input: the window does not exist while the asi is loading.
static bool EnsureRawInput()
{
    if (hGameWindow)
        return true;

    auto hWindow = FindViewportWindow();
    if (!hWindow)
        return false;

    pOriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    if (!pOriginalWndProc)
    {
        LogWarn("RawMouseInput: could not subclass the game window ({}), falling back to the game's own input", GetLastError());
        bRawMouseInput = false;
        return false;
    }

    hGameWindow = hWindow;

    // No RIDEV_INPUTSINK on purpose: without it the game stops receiving movement the moment it
    // loses focus, which is the behaviour anyone alt-tabbing expects.
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = 0;
    device.hwndTarget = hGameWindow;

    if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
    {
        LogWarn("RawMouseInput: RegisterRawInputDevices failed ({}), falling back to the game's own input", GetLastError());
        SetWindowLongPtrW(hGameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pOriginalWndProc));
        pOriginalWndProc = nullptr;
        hGameWindow = nullptr;
        bRawMouseInput = false;
        return false;
    }

    LogInfo("RawMouseInput: attached to window 0x{:08X}", reinterpret_cast<uintptr_t>(hGameWindow));
    return true;
}

static void __fastcall DealWithPlayerInputEvent(uint8_t* pThis, void*, float fDeltaTime)
{
    if (bRawMouseInput && fDeltaTime > 0.0f && EnsureRawInput())
    {
        auto pController = *reinterpret_cast<uint8_t**>(pThis + nOffsetPlayerController);
        if (pController)
        {
            const auto nX = nAccumulatedX.exchange(0);
            const auto nY = nAccumulatedY.exchange(0);
            const auto fScale = fMouseSensitivity.load() * (fEngineAxisScale / fDeltaTime);

            // Y is negated because the engine's own mouse path negates it: raw input counts down
            // as positive, the view axis counts up as positive.
            *reinterpret_cast<float*>(pController + nOffsetMouseX) = static_cast<float>(nX) * fScale;
            *reinterpret_cast<float*>(pController + nOffsetMouseY) = static_cast<float>(-nY) * fScale;
        }
    }

    shDealWithPlayerInputEvent.fastcall<void>(pThis, nullptr, fDeltaTime);
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto pDealWith = GetProcAddress(hEngine, "?DealWithPlayerInputEvent@UPlayerInput@@QAEXM@Z");
    if (!pDealWith)
    {
        LogWarn("RawMouseInput: Engine.dll did not export DealWithPlayerInputEvent, mouse options are off");
        return;
    }

    shDealWithPlayerInputEvent = safetyhook::create_inline(pDealWith, DealWithPlayerInputEvent);

    // MOV AL,[ECX+0x28] / TEST AL,1 - the bMaxMouseSmoothing test at the top of SmoothMouse, and
    // the pass through tail it should be going to instead.
    auto patternSmoothingTest = module_pattern(L"Engine.dll", "8A 41 28 A8 01 74 41 D9 44 24 08 D8 1D");
    auto patternPassThrough = module_pattern(L"Engine.dll", "8B 4C 24 08 8B 44 24 14 89 0A C7 00 00 00 00 00");

    if (!patternSmoothingTest.empty() && !patternPassThrough.empty())
    {
        const auto nFrom = reinterpret_cast<uintptr_t>(patternSmoothingTest.get_first(0));
        const auto nTo = reinterpret_cast<uintptr_t>(patternPassThrough.get_first(0));
        const auto nRelative = static_cast<int32_t>(nTo - (nFrom + 5));
        const auto pRelative = reinterpret_cast<const uint8_t*>(&nRelative);

        // The test is five bytes, a near jump is five bytes. Nothing has to move.
        patchSkipSmoothing = std::make_unique<raw_mem>(reinterpret_cast<void*>(nFrom),
            std::initializer_list<uint8_t>{ 0xE9, pRelative[0], pRelative[1], pRelative[2], pRelative[3] });
    }
    else
    {
        LogWarn("RawMouseInput: smoothing pattern not found, MouseSmoothing does nothing");
    }

    // FLD [ESI+0x3AC] (DesiredFOV) / FMUL qword [0.01111] - the mouse being scaled by the field of
    // view, so widening the FOV quietly raises sensitivity. FLD1 in its place leaves the rest of
    // the function reading a factor of one. Not a setting: nobody wants their sensitivity tied to
    // their FOV.
    auto patternFovScale = module_pattern(L"Engine.dll", "D9 86 AC 03 00 00 DC 0D ?? ?? ?? ?? 8B 46 7C");
    if (!patternFovScale.empty())
    {
        patchNoFovScaling = std::make_unique<raw_mem>(patternFovScale.get_first(0),
            std::initializer_list<uint8_t>{ 0xD9, 0xE8, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
        patchNoFovScaling->Write();
    }
    else
    {
        LogWarn("RawMouseInput: field of view scaling pattern not found, sensitivity still follows the FOV");
    }

    BindFloat(fMouseSensitivity, PREF_MOUSESENSITIVITY);

    ApplyAndWatch([]()
    {
        if (patchSkipSmoothing)
            patchSkipSmoothing->Set(MongooseFixSettings.GetInt(PREF_MOUSESMOOTHING) == 0);
    });
}

// Runs from DLL_PROCESS_DETACH, under the loader lock, on a window that may already be gone. The
// raw input registration is worth giving back either way; the window procedure is only put back
// if it is still ours, because writing over whatever subclassed after us is worse than leaving it.
static void Shutdown()
{
    RAWINPUTDEVICE device{};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));

    if (!hGameWindow || !pOriginalWndProc || !IsWindow(hGameWindow))
        return;

    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hGameWindow, GWLP_WNDPROC)) == WndProc)
        SetWindowLongPtrW(hGameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(pOriginalWndProc));
}

class RawMouse
{
public:
    RawMouse()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
        MongooseFix::onShutdownEvent() += []() { Shutdown(); };
    }
} RawMouse;
