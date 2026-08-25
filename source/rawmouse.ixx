module;

#include <common.hxx>

export module rawmouse;

import common;
import settings;
import logging;

// The engine reads the mouse through DirectInput one buffered packet at a time, one input event
// each, then runs it through UPlayerInput::SmoothMouse - a half-and-half filter with a borrow
// term, a 0.07s decay tail, and a clamp of the smoothed value to +/-1 that eats fast movement, so
// flicks go nowhere and the feel follows the polling rate. So the deltas come from raw input
// instead, one sum per frame, written over what DirectInput accumulated, and the filter is jumped
// over its own body.
//
// UPlayerInput +0x30 is its APlayerController; aMouseX +0x34C, aMouseY +0x350.
static constexpr auto nOffsetPlayerController = 0x30;
static constexpr auto nOffsetMouseX = 0x34C;
static constexpr auto nOffsetMouseY = 0x350;

// UInput::ReadInput multiplies every axis by 20/DeltaTime and the script consuming aTurn
// multiplies by DeltaTime again, which is what makes look speed frame rate independent. Raw deltas
// go in with the same factor, so 1.0 matches the stock feel.
static constexpr auto fEngineAxisScale = 20.0f;

// UGUIController::NativeKeyEvent in GUI.dll moves the menu cursor itself, one packet at a time:
//
//   MouseX = (int)((float)(int)(Delta - 0.5) * MenuMouseSens + MouseX)
//
// The 0.5 is a deadzone: a single-count packet lands on 0.5, rounds to nearest even and moves the
// cursor not at all, and packet size follows the polling rate. Dropping the subtraction leaves the
// count alone; the deltas come from raw input here too.
static constexpr auto nKeyMouseX = 0xE4;
static constexpr auto nKeyMouseY = 0xE5;

// Not a setting - only cleared by raw input failing to come up, which falls back on DirectInput
// rather than leaving the mouse dead.
static std::atomic<bool> bRawMouseInput = true;
static std::atomic<float> fMouseSensitivity = 1.0f;

static std::atomic<int32_t> nAccumulatedX = 0;
static std::atomic<int32_t> nAccumulatedY = 0;

// The menu drains its own pair: same messages, different moments, so sharing would lose movement.
static std::atomic<int32_t> nMenuAccumulatedX = 0;
static std::atomic<int32_t> nMenuAccumulatedY = 0;

static HWND hGameWindow = nullptr;
static WNDPROC pOriginalWndProc = nullptr;

static SafetyHookInline shDealWithPlayerInputEvent{};
static SafetyHookInline shNativeKeyEvent{};

// Written while the setting is off, restored while it is on.
static std::unique_ptr<raw_mem> patchSkipSmoothing;
static std::unique_ptr<raw_mem> patchNoFovScaling;
static std::vector<std::unique_ptr<raw_mem>> patchNoMenuDeadzone;

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
            nMenuAccumulatedX += input.data.mouse.lLastX;
            nMenuAccumulatedY += input.data.mouse.lLastY;
        }
    }

    return CallWindowProcW(pOriginalWndProc, hWnd, nMessage, wParam, lParam);
}

// FindWindowEx walks the whole desktop and the class name belongs to every UE2 game, hence the
// process check.
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

// Deferred to the first frame of input: the window does not exist while the asi is loading.
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

    // No RIDEV_INPUTSINK on purpose: without it movement stops the moment the game loses focus,
    // which is what anyone alt-tabbing expects.
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

            // The engine's own path negates Y: raw input counts down as positive.
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
    // the pass through tail to jump to instead.
    auto patternSmoothingTest = module_pattern(L"Engine.dll", "8A 41 28 A8 01 74 41 D9 44 24 08 D8 1D");
    auto patternPassThrough = module_pattern(L"Engine.dll", "8B 4C 24 08 8B 44 24 14 89 0A C7 00 00 00 00 00");

    if (!patternSmoothingTest.empty() && !patternPassThrough.empty())
    {
        const auto nFrom = reinterpret_cast<uintptr_t>(patternSmoothingTest.get_first(0));
        const auto nTo = reinterpret_cast<uintptr_t>(patternPassThrough.get_first(0));
        const auto nRelative = static_cast<int32_t>(nTo - (nFrom + 5));
        const auto pRelative = reinterpret_cast<const uint8_t*>(&nRelative);

        // Test and near jump are both five bytes, so nothing moves.
        patchSkipSmoothing = std::make_unique<raw_mem>(reinterpret_cast<void*>(nFrom),
            std::initializer_list<uint8_t>{ 0xE9, pRelative[0], pRelative[1], pRelative[2], pRelative[3] });
    }
    else
    {
        LogWarn("RawMouseInput: smoothing pattern not found, MouseSmoothing does nothing");
    }

    // FLD [ESI+0x3AC] (DesiredFOV) / FMUL qword [0.01111] - the mouse scaled by field of view, so
    // widening the FOV quietly raises sensitivity. FLD1 in its place leaves the rest of the
    // function reading a factor of one. Not a setting.
    auto patternFovScale = module_pattern(L"Engine.dll", "D9 86 AC 03 00 00 DC 0D ? ? ? ? 8B 46 7C");
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

// Each axis arrives as its own event and gets what has been summed since the last one of that
// axis. Y is negated as in game; the handler subtracts what it is given from the cursor's Y.
static int __fastcall NativeKeyEvent(uint8_t* pThis, void*, uint8_t* pKey, uint8_t* pState, float fDelta)
{
    if (bRawMouseInput && pKey && EnsureRawInput())
    {
        if (*pKey == nKeyMouseX)
            fDelta = static_cast<float>(nMenuAccumulatedX.exchange(0));
        else if (*pKey == nKeyMouseY)
            fDelta = static_cast<float>(-nMenuAccumulatedY.exchange(0));
    }

    return shNativeKeyEvent.fastcall<int>(pThis, nullptr, pKey, pState, fDelta);
}

static void InitGUI()
{
    auto hGUI = GetModuleHandleW(L"GUI.dll");
    if (!hGUI)
        return;

    if (auto p = GetProcAddress(hGUI, "?NativeKeyEvent@UGUIController@@UAEHAAE0M@Z"))
        shNativeKeyEvent = safetyhook::create_inline(p, NativeKeyEvent);
    else
        LogWarn("RawMouseInput: GUI.dll did not export NativeKeyEvent, the menu cursor is untouched");

    // FLD [ESP+0x28] (the axis delta) / FSUB [0.5] - once per axis. Not a setting: the deadzone
    // eats the smallest movement the mouse can report.
    auto patternDeadzone = module_pattern(L"GUI.dll", "D9 44 24 28 D8 25 ? ? ? ? DB 5C 24");
    if (patternDeadzone.empty())
    {
        LogWarn("RawMouseInput: menu cursor deadzone pattern not found, the deadzone is still there");
        return;
    }

    for (size_t i = 0; i < patternDeadzone.size(); i++)
    {
        patchNoMenuDeadzone.emplace_back(std::make_unique<raw_mem>(patternDeadzone.get(i).get<void>(4),
            std::initializer_list<uint8_t>{ 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }));
        patchNoMenuDeadzone.back()->Write();
    }
}

// From DLL_PROCESS_DETACH, under the loader lock, on a window that may already be gone. The window
// procedure only goes back if it is still ours - overwriting a later subclass is worse.
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
        MongooseFix::onGUIInitEvent() += []() { InitGUI(); };
        MongooseFix::onShutdownEvent() += []() { Shutdown(); };
    }
} RawMouse;
