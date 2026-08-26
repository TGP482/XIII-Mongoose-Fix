module;

#include <common.hxx>

export module rawmouse;

import common;
import settings;
import logging;
import display;

// DirectInput feeds UPlayerInput::SmoothMouse, which averages frames, keeps a 0.07s tail and clamps
// to +/-1, so flicks vanish and the feel follows the polling rate. Raw input instead: true counts,
// filter jumped.
//
// One raw registration per device per process, last caller wins. Ours replaces DirectInput's and
// its stream stops dead, no axis, no buttons, frozen menu, so the deltas go back in through
// UWindowsViewport::CauseInputEvent, the door DirectInput used. Player, console and menu unchanged.
//
// Menu cursor per packet:  MouseX = (int)((float)(int)(Delta - 0.5) * MenuMouseSens + MouseX)
// The 0.5 is a deadzone: one count lands on 0.5, rounds to even, moves nothing. Subtraction NOPed.
static constexpr auto nKeyMouseX = 0xE4;
static constexpr auto nKeyMouseY = 0xE5;

// Key codes as the game numbers them.
static constexpr auto nKeyLeftMouse = 1;
static constexpr auto nKeyRightMouse = 2;
static constexpr auto nKeyMiddleMouse = 4;
static constexpr auto nKeyWheelUp = 0xEC;
static constexpr auto nKeyWheelDown = 0xED;

static constexpr auto nInputActionPress = 1;
static constexpr auto nInputActionRelease = 3;

// The menu moves per event, not per count: a frame summed into one event leaves the cursor
// crawling. One event per packet, as DirectInput did. The player's counts still sum the same.
struct InputEvent { int nKey; int nAction; float fDelta; };

static std::mutex mutexEvents;
static std::vector<InputEvent> aEvents;

// Only so a stall cannot grow the queue; a frame never holds this many.
static constexpr size_t nMaxQueued = 512;

static void Queue(int nKey, int nAction, float fDelta)
{
    std::scoped_lock lock(mutexEvents);

    if (aEvents.size() < nMaxQueued)
        aEvents.push_back({ nKey, nAction, fDelta });
}

// The FOV factor the player's axes are multiplied by, ours to write, the patch below points the
// engine's FLD here. Gameplay only; the menu never runs that code.
static float fPlayerMouseScale = 1.0f;

// One count, both ends.
//   Menu:   cursor += Delta * MenuMouseSens, in pixels.
//   Player: counts, * 20/DeltaTime in UInput::ReadInput, * the factor above and the game's own
//           MouseSensitivity, * 0.24 into aTurn, then Yaw += 32 * DeltaTime * aTurn. DeltaTime
//           cancels: 32 * 20 * 0.24 = 153.6 yaw units per unit of factor, 65536 units = 360.
static constexpr auto fYawPerCount = 153.6f * 360.0f / 65536.0f;

// GUIController's default; XIII's ini never touches it.
static constexpr auto fMenuMouseSens = 1.0f;

// What XIDInterf lays the menus out in, the pair menuscale scales from.
static constexpr auto fMenuAuthoredWidth = 640.0f;
static constexpr auto fMenuAuthoredHeight = 480.0f;

static SafetyHookInline shUpdateInput{};

// UWindowsViewport::CauseInputEvent(iKey, EInputAction, Delta) in WinDrv.dll. IST_Axis is 4.
using fnCauseInputEvent = int(__thiscall*)(void*, int, int, float);
static fnCauseInputEvent pCauseInputEvent = nullptr;

static constexpr auto nInputActionAxis = 4;

// Written while the setting is off, restored while it is on.
static std::unique_ptr<raw_mem> patchSkipSmoothing;
static std::unique_ptr<raw_mem> patchNoFovScaling;
static std::vector<std::unique_ptr<raw_mem>> patchNoMenuDeadzone;

// Not the game's window: it owns that window procedure and recreates the window on a mode change,
// either of which drops the deltas silently. A message only window of our own, own pump.
static HWND hRawInputWindow = nullptr;
static std::atomic<bool> bRawInputReady = false;

// RIDEV_INPUTSINK delivers with or without focus, so the focus test moves here, alt-tab still
// stops movement.
static bool ForegroundIsGame()
{
    DWORD nProcessId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &nProcessId);
    return nProcessId == GetCurrentProcessId();
}

static void QueueButtons(uint16_t nFlags, uint16_t nData)
{
    static constexpr std::pair<uint16_t, int> aDown[] =
    {
        { RI_MOUSE_LEFT_BUTTON_DOWN, nKeyLeftMouse },
        { RI_MOUSE_RIGHT_BUTTON_DOWN, nKeyRightMouse },
        { RI_MOUSE_MIDDLE_BUTTON_DOWN, nKeyMiddleMouse },
    };

    static constexpr std::pair<uint16_t, int> aUp[] =
    {
        { RI_MOUSE_LEFT_BUTTON_UP, nKeyLeftMouse },
        { RI_MOUSE_RIGHT_BUTTON_UP, nKeyRightMouse },
        { RI_MOUSE_MIDDLE_BUTTON_UP, nKeyMiddleMouse },
    };

    for (const auto& [nFlag, nKey] : aDown)
        if (nFlags & nFlag)
            Queue(nKey, nInputActionPress, 0.0f);

    for (const auto& [nFlag, nKey] : aUp)
        if (nFlags & nFlag)
            Queue(nKey, nInputActionRelease, 0.0f);

    // Wheel has no up and down of its own: press and release per notch.
    if (nFlags & RI_MOUSE_WHEEL)
    {
        const auto nKey = static_cast<int16_t>(nData) > 0 ? nKeyWheelUp : nKeyWheelDown;
        Queue(nKey, nInputActionPress, 0.0f);
        Queue(nKey, nInputActionRelease, 0.0f);
    }
}

static LRESULT CALLBACK WndProc(HWND hWindow, UINT nMessage, WPARAM wParam, LPARAM lParam)
{
    if (nMessage == WM_INPUT && ForegroundIsGame())
    {
        RAWINPUT input{};
        UINT nSize = sizeof(input);

        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &nSize, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)
            && input.header.dwType == RIM_TYPEMOUSE
            && (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
        {
            // Raw Y counts down as positive, the engine's axis counts up.
            if (input.data.mouse.lLastX)
                Queue(nKeyMouseX, nInputActionAxis, static_cast<float>(input.data.mouse.lLastX));
            if (input.data.mouse.lLastY)
                Queue(nKeyMouseY, nInputActionAxis, static_cast<float>(-input.data.mouse.lLastY));

            QueueButtons(input.data.mouse.usButtonFlags, input.data.mouse.usButtonData);
        }
    }

    return DefWindowProcW(hWindow, nMessage, wParam, lParam);
}

// DirectInput takes the registration back whenever the game acquires the mouse, hence not once.
static bool RegisterRawMouse()
{
    RAWINPUTDEVICE device{ 0x01, 0x02, RIDEV_INPUTSINK, hRawInputWindow };
    return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
}

static void InputThread()
{
    WNDCLASSEXW windowClass{ sizeof(windowClass) };
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"XIIIMongooseFixRawInput";
    RegisterClassExW(&windowClass);

    hRawInputWindow = CreateWindowExW(0, windowClass.lpszClassName, nullptr, 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);

    if (!hRawInputWindow)
        return;

    if (!RegisterRawMouse())
    {
        DestroyWindow(hRawInputWindow);
        hRawInputWindow = nullptr;
        return;
    }

    bRawInputReady = true;

    uint32_t nTicks = 0;

    for (MSG message{}; ; )
    {
        // Timeout keeps the re-registration below ticking on a still mouse.
        MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
                return;

            DispatchMessageW(&message);
        }

        // Half a second is the longest the game can hold the mouse off us.
        if (++nTicks % 5 == 0)
            RegisterRawMouse();
    }
}

// First input event, not load: DllMain holds the loader lock, no window can be created under it.
static bool EnsureRawInput()
{
    static std::once_flag flag;
    std::call_once(flag, []() { std::thread(InputThread).detach(); });

    return bRawInputReady.load();
}

// UWindowsViewport::UpdateInput is the per frame DirectInput poll: game thread, viewport to hand.
// A count moves the cursor MenuMouseSens units, and the menus are 640x480 blown up by
// min(width/640, height/480), so that many screen pixels. The view should turn by what those
// pixels are worth: the field of view over the width.
static void UpdatePlayerScale()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();
    if (nWidth < 1 || nHeight < 1)
        return;

    const auto fMenuScale = (std::min)(nWidth / fMenuAuthoredWidth, nHeight / fMenuAuthoredHeight);
    const auto fDegreesPerPixel = MongooseFixSettings.GetFloat(PREF_FIELDOFVIEW) / static_cast<float>(nWidth);
    const auto fMatched = fMenuMouseSens * fMenuScale * fDegreesPerPixel / fYawPerCount;

    fPlayerMouseScale = fMatched * MongooseFixSettings.GetFloat(PREF_MOUSESENSITIVITY);
}

static void __fastcall UpdateInput(void* pThis, void*, int bIsMainViewport)
{
    shUpdateInput.fastcall<void>(pThis, nullptr, bIsMainViewport);

    if (!pCauseInputEvent || !EnsureRawInput())
        return;

    std::vector<InputEvent> aPending;
    {
        std::scoped_lock lock(mutexEvents);
        aPending.swap(aEvents);
    }

    // Raw counts only. The menu scales by MenuMouseSens, the player by the factor above, the two
    // stay independent.
    for (const auto& event : aPending)
    {
        pCauseInputEvent(pThis, event.nKey, event.nAction, event.fDelta);
    }
}

static void InitWinDrv()
{
    auto hWinDrv = GetModuleHandleW(L"WinDrv.dll");
    if (!hWinDrv)
        return;

    pCauseInputEvent = reinterpret_cast<fnCauseInputEvent>(GetProcAddress(hWinDrv, "?CauseInputEvent@UWindowsViewport@@QAEHHW4EInputAction@@M@Z"));

    if (auto p = GetProcAddress(hWinDrv, "?UpdateInput@UWindowsViewport@@UAEXH@Z"))
        shUpdateInput = safetyhook::create_inline(p, UpdateInput);

}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    // MOV AL,[ECX+0x28] / TEST AL,1: the bMaxMouseSmoothing test at the top of SmoothMouse, and
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

    // FLD [ESI+0x3AC] (DesiredFOV) / FMUL qword [0.01111]: the mouse scaled by field of view, so
    // widening the FOV quietly raises sensitivity. Both are replaced by a FLD of our own float, so
    // the FOV drops out and the factor is ours to set. Twelve bytes for twelve. Not a setting.
    auto patternFovScale = module_pattern(L"Engine.dll", "D9 86 AC 03 00 00 DC 0D ? ? ? ? 8B 46 7C");
    if (!patternFovScale.empty())
    {
        const auto pScale = reinterpret_cast<uintptr_t>(&fPlayerMouseScale);
        const auto pByte = reinterpret_cast<const uint8_t*>(&pScale);

        patchNoFovScaling = std::make_unique<raw_mem>(patternFovScale.get_first(0),
            std::initializer_list<uint8_t>{ 0xD9, 0x05, pByte[0], pByte[1], pByte[2], pByte[3],
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
        patchNoFovScaling->Write();
    }
    else
    {
        LogWarn("RawMouseInput: field of view scaling pattern not found, sensitivity still follows the FOV");
    }

    ApplyAndWatch(UpdatePlayerScale);

    // The screen width is half the sum, so a resolution change has to redo it.
    onDeviceResetEvent() += []() { UpdatePlayerScale(); };

    ApplyAndWatch([]()
    {
        if (patchSkipSmoothing)
            patchSkipSmoothing->Set(MongooseFixSettings.GetInt(PREF_MOUSESMOOTHING) == 0);
    });
}

static void InitGUI()
{
    auto hGUI = GetModuleHandleW(L"GUI.dll");
    if (!hGUI)
        return;

    // FLD [ESP+0x28] (the axis delta) / FSUB [0.5]: once per axis. Not a setting: the deadzone
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

// From DLL_PROCESS_DETACH, under the loader lock: unregister and leave the thread to die with the
// process. Nothing of the game's was touched, so there is nothing to put back.
static void Shutdown()
{
    bRawInputReady = false;

    RAWINPUTDEVICE device{ 0x01, 0x02, RIDEV_REMOVE, nullptr };
    RegisterRawInputDevices(&device, 1, sizeof(device));
}

class RawMouse
{
public:
    RawMouse()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
        MongooseFix::onWinDrvInitEvent() += []() { InitWinDrv(); };
        MongooseFix::onGUIInitEvent() += []() { InitGUI(); };
        MongooseFix::onShutdownEvent() += []() { Shutdown(); };
    }
} RawMouse;
