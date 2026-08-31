module;

#include <common.hxx>
#include <Xinput.h>
#include <format>

export module controller;

import common;
import settings;
import logging;
import mongoosemenu;
import rawmouse;

// Xbox pad support half survives in the PC binaries, dead as shipped:
//   Rumble: UForceFeedbackManager::Instance is exported but never written. Points at ours now.
//   Input: UseJoystick=False, and DirectInput misses triggers and d-pad. XInput instead, fed through
//          UInput::DirectAxis and CauseInputEvent.
//   Bindings: XIIIMenuControlsWindow's generic block ships commented out, leaving the pad unbound.

// UForceFeedbackManager vtable slots the script bindings call.
static constexpr auto nSlotStartEffect = 0x74 / 4;
static constexpr auto nSlotEnableForceFeedback = 0x78 / 4;
static constexpr auto nSlotIsForceFeedbackEnable = 0x7C / 4;
static constexpr auto nSlotCount = 32;

// Effect table lives in compiled XIIIPlayerController. Every observed effect sends duration 0, so
// only the two motor strengths matter.
//
// ponytail: no envelope, one fixed window. Revisit if an effect needs to sustain.
static constexpr auto nRumbleMilliseconds = 250;

using fnXInputSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using fnXInputGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

static fnXInputSetState pXInputSetState = nullptr;
static fnXInputGetState pXInputGetState = nullptr;

static std::atomic<bool> bRumbleEnabled = true;
static std::atomic<bool> bVibration = true;
static std::atomic<uint64_t> nRumbleUntil = 0;

// Rumble, the menu cursor and stick navigation all follow whichever device produced input last.
static std::atomic<bool> bPadInput = false;

// Pad is not always on user 0. First slot that answers wins.
static DWORD XInputUser()
{
    static DWORD nUser = 0;

    XINPUT_STATE state{};
    if (!pXInputGetState || pXInputGetState(nUser, &state) == ERROR_SUCCESS)
        return nUser;

    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++)
    {
        if (pXInputGetState(i, &state) == ERROR_SUCCESS)
        {
            nUser = i;
            break;
        }
    }

    return nUser;
}

static void SetVibration(float fLeft, float fRight)
{
    if (!pXInputSetState)
        return;

    const auto Motor = [](float f) { return static_cast<WORD>(std::clamp(f, 0.0f, 1.0f) * 65535.0f); };

    XINPUT_VIBRATION vibration{ Motor(fLeft), Motor(fRight) };
    pXInputSetState(XInputUser(), &vibration);
}

static void StopRumble()
{
    nRumbleUntil = 0;
    SetVibration(0.0f, 0.0f);
}

struct PadState
{
    uint16_t nButtons;
    float fLeftX, fLeftY, fRightX, fRightY;
    bool bConnected;
};

static std::mutex mutexPad;
static PadState padState{};

static PadState ReadPad();
static void PadThread();

// All XInput on this thread: empty slots cost milliseconds and the first call initialises XInput.
// Deferred to first use so the load and the thread start miss the loader lock.
static void StartPadThread()
{
    static std::once_flag flag;
    std::call_once(flag, []()
    {
        for (auto szModule : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
        {
            if (auto hXInput = LoadLibraryA(szModule))
            {
                pXInputSetState = reinterpret_cast<fnXInputSetState>(GetProcAddress(hXInput, "XInputSetState"));
                pXInputGetState = reinterpret_cast<fnXInputGetState>(GetProcAddress(hXInput, "XInputGetState"));

                if (pXInputSetState && pXInputGetState)
                    break;
            }
        }

        if (!pXInputSetState || !pXInputGetState)
        {
            LogWarn("Controller: XInput unavailable, no pad and no rumble");
            return;
        }

        std::thread(PadThread).detach();
    });
}

// Effects expire here too: menus fire test rumble while nothing else ticks.
static void PadThread()
{
    for (;;)
    {
        Sleep(8);

        const auto state = ReadPad();
        {
            std::scoped_lock lock(mutexPad);
            padState = state;
        }

        const auto nUntil = nRumbleUntil.load();
        if (nUntil && GetTickCount64() >= nUntil)
            StopRumble();
    }
}

static void __fastcall StartEffect(void*, void*, int nPad, int, float fRumbleLeft, float fRumbleRight,
    float, float, float, float, float)
{
    if (nPad != 0 || !bRumbleEnabled || !bVibration || !bPadInput)
        return;

    StartPadThread();

    nRumbleUntil = GetTickCount64() + nRumbleMilliseconds;
    SetVibration(fRumbleLeft, fRumbleRight);
}

// Advanced controls menu drives this via SetRumbleFX.
static void __fastcall EnableForceFeedback(void*, void*, int, int bEnable)
{
    bRumbleEnabled = bEnable != 0;

    if (!bEnable)
        StopRumble();
}

static int __fastcall IsForceFeedbackEnable(void*, void*, int)
{
    return bRumbleEnabled ? 1 : 0;
}

// Unreached slots stay null: a crash beats a stub with the wrong argument count wrecking the stack.
static void* apVtable[nSlotCount]{};
static void* pVtable = apVtable;

static constexpr auto nKeyJoyX = 0xE0;
static constexpr auto nKeyJoyY = 0xE1;
static constexpr auto nKeyJoyU = 0xE8;
static constexpr auto nKeyJoyV = 0xE9;
static constexpr auto nKeyJoy1 = 200;

static constexpr auto nInputActionPress = 1;
static constexpr auto nInputActionRelease = 3;
static constexpr auto nInputActionAxis = 4;

// Xbox button order from the game's script: XIIIMPBotInteraction PF_XBOX puts the d-pad on Joy9 to
// Joy12, XIIIMenuPressStart takes Joy13 as start. Shoulders sit in Black and White's slots.
static constexpr WORD aButtons[] =
{
    XINPUT_GAMEPAD_A,                // Joy1
    XINPUT_GAMEPAD_B,                // Joy2
    XINPUT_GAMEPAD_X,                // Joy3
    XINPUT_GAMEPAD_Y,                // Joy4
    XINPUT_GAMEPAD_RIGHT_SHOULDER,   // Joy5, black
    XINPUT_GAMEPAD_LEFT_SHOULDER,    // Joy6, white
    0,                               // Joy7, left trigger
    0,                               // Joy8, right trigger
    XINPUT_GAMEPAD_DPAD_UP,          // Joy9
    XINPUT_GAMEPAD_DPAD_DOWN,        // Joy10
    XINPUT_GAMEPAD_DPAD_LEFT,        // Joy11
    XINPUT_GAMEPAD_DPAD_RIGHT,       // Joy12
    XINPUT_GAMEPAD_START,            // Joy13
    XINPUT_GAMEPAD_BACK,             // Joy14
    XINPUT_GAMEPAD_LEFT_THUMB,       // Joy15
    XINPUT_GAMEPAD_RIGHT_THUMB,      // Joy16
};

static constexpr size_t nButtonCount = std::size(aButtons);
static constexpr auto nLeftTriggerBit = 6;
static constexpr auto nRightTriggerBit = 7;
static constexpr auto nTriggerThreshold = 30;

// Radial, so diagonals are not clipped square, rescaled so the live range still reaches full.
static constexpr auto fPadDeadzone = 0.5f;

// Measured: an axis binding multiplies the fed value by 20; SPEED=, DEADZONE= and XIII's
// SpeedBase=1.0 are tokens the parser never reads. A full stick reached 1 against the keyboard's
// fInputRange of 1150, so stock PC pad axes did nothing. Scaled on the way in instead.
//
// ponytail: 20 confirmed on all four axes. Recheck if a build reads differently.
static constexpr auto fEngineAxisScale = 20.0f;

// DealWithPlayerInputEvent scales pad axes by DeltaTime * 60, tuning the pad for 60 fps: walk/run
// compares scaled aBaseY against 0.875 * fInputRange, missed by 4x at 240 fps. Divided back out.
//
// ponytail: movement only. Look rides the same factor and stays frame rate dependent; rescaling
// changes the tuning.
static float fFrameFactor = 1.0f;
static float fFrameDelta = 0.0f;
static float fInputRange = 1150.0f;

using fnDirectAxis = void(__thiscall*)(void*, int, float, float);
using fnCauseInputEvent = int(__thiscall*)(void*, int, int, float);
using fnOptimizeInputBindings = void(__thiscall*)(void*);
using fnStaticExec = int(__cdecl*)(const char*, void*);

static fnDirectAxis pDirectAxis = nullptr;
static fnCauseInputEvent pCauseInputEvent = nullptr;
static fnOptimizeInputBindings pOptimizeInputBindings = nullptr;
static fnStaticExec pStaticExec = nullptr;
static void** ppGLog = nullptr;

// UWindowsViewport::Input.
static constexpr auto nViewportInputOffset = 0x6C;

static uint16_t nHeldButtons = 0;

// No setting: the pad turns itself on the first time it reports anything and the byte patch goes in
// then, so a keyboard-only machine runs an untouched game.
static bool bPadActive = false;

// Every keyboard, mouse and pad event reaches UWindowsViewport::CauseInputEvent, so one hook tells
// the sources apart by key code. Releases ignored: the key sweep issues them for unpressed keys.
static SafetyHookInline shCauseInputEvent{};

// The cursor branch in UGUIController::NativePostRender, jumped over. Not a NOP over the draw call:
// the epilogue pops EBX, ESI and EDI before restoring ESP, so abandoned arguments come back as
// registers and the interaction loop walks off a garbage array.
static std::unique_ptr<raw_mem> patchHideCursor;

static void SetPadInput(bool bPad)
{
    if (bPadInput.exchange(bPad) != bPad && patchHideCursor)
        patchHideCursor->Set(bPad);

    // The settings page draws its own cursor, so it needs the same answer this patch acts on.
    MongooseMenuNotePadInput(bPad);
}

// Only buttons: pad axes go in through DirectAxis, never here.
static int __fastcall CauseInputEvent(void* pThis, void*, int nKey, int nAction, float fDelta)
{
    if (nAction == nInputActionPress || (nAction == nInputActionAxis && fDelta != 0.0f))
        SetPadInput(nKey >= nKeyJoy1 && nKey < nKeyJoy1 + static_cast<int>(nButtonCount));

    return shCauseInputEvent.fastcall<int>(pThis, nullptr, nKey, nAction, fDelta);
}

// UpdateInput ends by sweeping keys 0 to 255 through GetKeyState. 200 and up are Joy and axis codes,
// not virtual keys, so the sweep answers "up" and releases our buttons a frame after each press.
// Stock marked them handled; the sweep stops at 200 instead.
static std::unique_ptr<raw_mem> patchKeySweepRange;

// Bindings wait for a player: the XIII package is unloaded at startup so ConfigType misses, and
// touching the input system under the splash hangs the boot.
static void* pLastViewport = nullptr;

static constexpr const char* aButtonBindings[] =
{
    "SET Input Joy2 PrevWeapon",
    "SET Input Joy5 NextWeapon",
    "SET Input Joy6 QuickHeal",
    "SET Input Joy8 Fire | onrelease UnFire",
    "SET Input Joy9 CenterView",
    "SET Input Joy10 CenterView",
    "SET Input Joy11 InventoryNext",
    "SET Input Joy12 InventoryPrevious",
    "SET Input Joy13 ShowMenu",
    "SET Input Joy14 ShowScores | onrelease HideScores",
    "SET Input Joy15 Duck",
    "SET Input Joy16 Reload",
};

// Classic Halo, Goofy Halo, Classic XIII, Goofy XIII, the menu's order; Goofy swaps turn and
// strafe. Byte for byte what the in-game menu rewrites, so the menu keeps working.
struct Layout
{
    const char* szConfigType;
    const char* szJoy1;
    const char* szJoy3;
    const char* szJoy4;
    const char* szJoy7;
    const char* szJoyX;
    const char* szJoyU;
};

static constexpr Layout aLayouts[] =
{
    { "CT_StrafeLookNotSameAxis", "Jump", "Grab", "PrevWeapon", "AltFire | onrelease UnFire", "aStrafe", "aTurn" },
    { "CT_StrafeLookSameAxis",    "Jump", "Grab", "PrevWeapon", "AltFire | onrelease UnFire", "aTurn", "aStrafe" },
    { "CT_StrafeLookNotSameAxis", "Grab", "PrevWeapon", "AltFire | onrelease UnFire", "Jump", "aStrafe", "aTurn" },
    { "CT_StrafeLookSameAxis",    "Grab", "PrevWeapon", "AltFire | onrelease UnFire", "Jump", "aTurn", "aStrafe" },
};

static void ApplyBindings(void* pViewport)
{
    if (!pStaticExec || !ppGLog)
        return;

    const auto Exec = [](const std::string& command) { pStaticExec(command.c_str(), *ppGLog); };

    for (auto szCommand : aButtonBindings)
        Exec(szCommand);

    const auto& layout = aLayouts[std::clamp(MongooseFixSettings.GetInt(PREF_PADLAYOUT), 0, 3)];

    Exec(std::format("SET XIIIPlayerController ConfigType {}", layout.szConfigType));
    Exec(std::format("SET Input Joy1 {}", layout.szJoy1));
    Exec(std::format("SET Input Joy3 {}", layout.szJoy3));
    Exec(std::format("SET Input Joy4 {}", layout.szJoy4));
    Exec(std::format("SET Input Joy7 {}", layout.szJoy7));
    Exec(std::format("SET Input JoyX Axis {}", layout.szJoyX));
    Exec(std::format("SET Input JoyU Axis {}", layout.szJoyU));
    Exec("SET Input JoyY Axis aBaseY");
    Exec("SET Input JoyV Axis aLookup");

    if (pOptimizeInputBindings)
        pOptimizeInputBindings(pViewport);
}

static PadState ReadPad()
{
    PadState state{};

    XINPUT_STATE input{};
    if (!pXInputGetState || pXInputGetState(XInputUser(), &input) != ERROR_SUCCESS)
        return state;

    const auto& pad = input.Gamepad;

    const auto Stick = [](SHORT nX, SHORT nY, float& fOutX, float& fOutY)
    {
        const auto fX = std::clamp(nX / 32767.0f, -1.0f, 1.0f);
        const auto fY = std::clamp(nY / 32767.0f, -1.0f, 1.0f);
        const auto fMagnitude = std::sqrt(fX * fX + fY * fY);

        if (fMagnitude <= fPadDeadzone)
            return;

        const auto fScaled = (std::min)((fMagnitude - fPadDeadzone) / (1.0f - fPadDeadzone), 1.0f) / fMagnitude;
        fOutX = fX * fScaled;
        fOutY = fY * fScaled;
    };

    state.bConnected = true;
    Stick(pad.sThumbLX, pad.sThumbLY, state.fLeftX, state.fLeftY);
    Stick(pad.sThumbRX, pad.sThumbRY, state.fRightX, state.fRightY);

    for (size_t i = 0; i < nButtonCount; i++)
        if (aButtons[i] && (pad.wButtons & aButtons[i]))
            state.nButtons |= 1 << i;

    if (pad.bLeftTrigger > nTriggerThreshold)
        state.nButtons |= 1 << nLeftTriggerBit;
    if (pad.bRightTrigger > nTriggerThreshold)
        state.nButtons |= 1 << nRightTriggerBit;

    return state;
}

static void FeedPad(void* pViewport)
{
    if (!pDirectAxis || !pCauseInputEvent)
        return;

    StartPadThread();
    pLastViewport = pViewport;

    PadState state{};
    {
        std::scoped_lock lock(mutexPad);
        state = padState;
    }

    if (!state.bConnected)
        return;

    if (!bPadActive)
    {
        if (!state.nButtons && !state.fLeftX && !state.fLeftY && !state.fRightX && !state.fRightY)
            return;

        bPadActive = true;

        // Not at WinDrv load: that runs under the loader lock.
        auto patternSweep = module_pattern(L"WinDrv.dll", "33 FF EB 03 8D 49 00 81 FF 00 01 00 00");
        if (patternSweep.empty())
        {
            LogWarn("Controller: key sweep pattern not found, pad buttons release themselves");
            return;
        }

        patchKeySweepRange = std::make_unique<raw_mem>(patternSweep.get_first(9),
            std::initializer_list<uint8_t>{ 0xC8, 0x00, 0x00, 0x00 });
        patchKeySweepRange->Write();
    }

    auto pInput = *reinterpret_cast<void**>(static_cast<uint8_t*>(pViewport) + nViewportInputOffset);
    if (!pInput)
        return;

    // Sticks, since only buttons reach the CauseInputEvent hook.
    if (state.fLeftX || state.fLeftY || state.fRightX || state.fRightY)
        SetPadInput(true);

    const auto fLookScale = fInputRange / fEngineAxisScale;
    const auto fMoveScale = fLookScale / fFrameFactor;

    // DirectInput reported the look axis upside down; the engine kept that convention.
    pDirectAxis(pInput, nKeyJoyX, state.fLeftX * fMoveScale, fFrameDelta);
    pDirectAxis(pInput, nKeyJoyY, state.fLeftY * fMoveScale, fFrameDelta);
    pDirectAxis(pInput, nKeyJoyU, state.fRightX * fLookScale, fFrameDelta);
    pDirectAxis(pInput, nKeyJoyV, -state.fRightY * fLookScale, fFrameDelta);

    for (size_t i = 0; i < nButtonCount; i++)
    {
        const auto bDown = (state.nButtons >> i) & 1;

        if (bDown != ((nHeldButtons >> i) & 1))
            pCauseInputEvent(pViewport, nKeyJoy1 + static_cast<int>(i),
                bDown ? nInputActionPress : nInputActionRelease, 0.0f);
    }

    nHeldButtons = state.nButtons;
}

// Menus test raw keyboard codes (Enter, Backspace, Escape, arrows), never pad keys. All keys enter
// through UGUIController::NativeKeyEvent, whose return doubles as "is a menu open": offer the
// keyboard equivalent first, pass the real pad key through if nothing consumes it.
static SafetyHookInline shNativeKeyEvent{};

static uint8_t MenuKey(uint8_t nKey)
{
    switch (nKey)
    {
    case nKeyJoy1 + 0:  return 0x0D;   // A, enter
    case nKeyJoy1 + 1:  return 0x08;   // B, backspace
    case nKeyJoy1 + 8:  return 0x26;   // d-pad up
    case nKeyJoy1 + 9:  return 0x28;   // d-pad down
    case nKeyJoy1 + 10: return 0x25;   // d-pad left
    case nKeyJoy1 + 11: return 0x27;   // d-pad right
    case nKeyJoy1 + 12: return 0x1B;   // start, escape
    default:            return 0;
    }
}

static int __fastcall NativeKeyEvent(void* pThis, void*, uint8_t* pKey, uint8_t* pState, float fDelta)
{
    if (auto nKey = MenuKey(*pKey))
    {
        if (MongooseMenuPadKey(nKey, *pState))
            return 1;

        if (shNativeKeyEvent.fastcall<int>(pThis, nullptr, &nKey, pState, fDelta))
            return 1;
    }

    return shNativeKeyEvent.fastcall<int>(pThis, nullptr, pKey, pState, fDelta);
}

// Left stick nudges menus with arrow presses through the original NativeKeyEvent, which consumes
// nothing while no menu is up, so gameplay never sees them.
static SafetyHookInline shNativeTick{};

// UGUIController::MenuStack.Num, and MouseX, followed by MouseY, LastMouseX, LastMouseY.
static constexpr auto nMenuStackNumOffset = 0x40;
static constexpr auto nMouseXOffset = 0x90;

static constexpr auto fNavThreshold = 0.5f;
static constexpr auto fNavDelayFirst = 0.4f;
static constexpr auto fNavDelayRepeat = 0.12f;

static int nNavKey = 0;
static float fNavTimer = 0.0f;
static bool bNavRepeating = false;

static void __fastcall NativeTick(void* pThis, void*, float fDeltaTime)
{
    shNativeTick.fastcall<void>(pThis, nullptr, fDeltaTime);

    if (!shNativeKeyEvent || !bPadInput
        || !*reinterpret_cast<int*>(static_cast<uint8_t*>(pThis) + nMenuStackNumOffset))
        return;

    // Skipping the branch skips the LastMouse write inside it, so UGUIVertList::Draw would see
    // HasMouseMoved every frame and drag the selection back under the cursor.
    auto pMouse = reinterpret_cast<int*>(static_cast<uint8_t*>(pThis) + nMouseXOffset);
    pMouse[2] = pMouse[0];
    pMouse[3] = pMouse[1];

    PadState state{};
    {
        std::scoped_lock lock(mutexPad);
        state = padState;
    }

    // Vertical wins a diagonal: menus are lists.
    int nKey = 0;
    if (state.fLeftY > fNavThreshold)
        nKey = 0x26;
    else if (state.fLeftY < -fNavThreshold)
        nKey = 0x28;
    else if (state.fLeftX > fNavThreshold)
        nKey = 0x27;
    else if (state.fLeftX < -fNavThreshold)
        nKey = 0x25;

    // A held d-pad repeats the same way, but its first step already went out as a button press
    // through NativeKeyEvent, so it starts on the long delay.
    auto bHeldButton = false;

    if (!nKey)
    {
        // PadState::nButtons is a bit per aButtons index, not the XInput mask, so the d-pad is
        // bits 8 to 11.
        static constexpr auto nDPadUpBit = 8;

        if (state.nButtons & (1 << (nDPadUpBit + 0)))
            nKey = 0x26;
        else if (state.nButtons & (1 << (nDPadUpBit + 1)))
            nKey = 0x28;
        else if (state.nButtons & (1 << (nDPadUpBit + 2)))
            nKey = 0x25;
        else if (state.nButtons & (1 << (nDPadUpBit + 3)))
            nKey = 0x27;

        bHeldButton = nKey != 0;
    }

    if (nKey != nNavKey)
    {
        nNavKey = nKey;
        fNavTimer = bHeldButton ? fNavDelayFirst : 0.0f;
        bNavRepeating = bHeldButton;
    }

    if (!nKey)
        return;

    fNavTimer -= fDeltaTime;
    if (fNavTimer > 0.0f)
        return;

    auto nMenuKey = static_cast<uint8_t>(nKey);
    uint8_t nPress = nInputActionPress;
    uint8_t nRelease = nInputActionRelease;

    if (!MongooseMenuPadKey(nMenuKey, nPress))
    {
        shNativeKeyEvent.fastcall<int>(pThis, nullptr, &nMenuKey, &nPress, fDeltaTime);
        shNativeKeyEvent.fastcall<int>(pThis, nullptr, &nMenuKey, &nRelease, fDeltaTime);
    }

    fNavTimer = bNavRepeating ? fNavDelayRepeat : fNavDelayFirst;
    bNavRepeating = true;
}

static SafetyHookInline shDealWithPlayerInputEvent{};

using fnGetOuter = void*(__thiscall*)(void*);
static fnGetOuter pGetOuter = nullptr;

static float fPadSensitivity = 1.0f;

// APlayerController::fLookSpeed, the pad look slider. Mouse never passes through it.
static constexpr auto nLookSpeedOffset = 0x380;

// UPlayerInput::fInputRange.
static constexpr auto nInputRangeOffset = 0x5C;

// Aim acceleration: aTurn takes ViewTurnAcc + ViewTurnBoost, aLookUp ViewUpAcc, accumulators that
// ramp while a direction is held. Pinned to unity, no boost, so the stick maps straight to turn
// rate. Every frame: the engine rewrites them during the pass.
static constexpr auto nViewTurnAccOffset = 0x64;
static constexpr auto nViewTurnBoostOffset = 0x68;
static constexpr auto nViewUpAccOffset = 0x6C;

// The engine's FOV factor, DesiredFOV * 0.01111, multiplies mouse and pad alike, and the mouse
// module's replacement load drags pad look onto MouseSensitivity and resolution. Divided back out,
// stock factor substituted, so pad look answers to Sensitivity alone.
static constexpr auto fFovFactor = 0.01111f;

static float PadLookFactor()
{
    const auto fEngineScale = PlayerAxisScale();
    if (fEngineScale == 0.0f)
        return fPadSensitivity;

    return fPadSensitivity * fStockFieldOfView * fFovFactor / fEngineScale;
}

// Sensitivity applied around the original, not after: aTurn and aLookUp hold pure pad values only
// until this call folds the mouse in.
static void __fastcall DealWithPlayerInputEvent(void* pThis, void*, float fDeltaTime)
{
    auto pController = pGetOuter ? pGetOuter(pThis) : nullptr;

    if (!pController)
    {
        shDealWithPlayerInputEvent.fastcall<void>(pThis, nullptr, fDeltaTime);
        return;
    }

    const auto Input = [pThis](int nOffset) -> float& { return *reinterpret_cast<float*>(static_cast<uint8_t*>(pThis) + nOffset); };

    if (Input(nInputRangeOffset) > 0.0f)
        fInputRange = Input(nInputRangeOffset);

    fFrameDelta = fDeltaTime;
    fFrameFactor = std::clamp(fDeltaTime * 60.0f, 0.05f, 4.0f);

    if (bPadActive)
    {
        // Rebound when the layout moves: the binds are console commands, and the ini watcher's
        // thread is no place to run them.
        static auto nAppliedLayout = -1;

        if (pLastViewport && nAppliedLayout != MongooseFixSettings.GetInt(PREF_PADLAYOUT))
        {
            nAppliedLayout = MongooseFixSettings.GetInt(PREF_PADLAYOUT);
            ApplyBindings(pLastViewport);
        }

        Input(nViewTurnAccOffset) = 1.0f;
        Input(nViewTurnBoostOffset) = 0.0f;
        Input(nViewUpAccOffset) = 1.0f;
    }

    auto& fLookSpeed = *reinterpret_cast<float*>(static_cast<uint8_t*>(pController) + nLookSpeedOffset);
    const auto fStock = fLookSpeed;

    fLookSpeed = fStock * PadLookFactor();
    shDealWithPlayerInputEvent.fastcall<void>(pThis, nullptr, fDeltaTime);
    fLookSpeed = fStock;
}

static void InitCore()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hCore)
        return;

    pGetOuter = reinterpret_cast<fnGetOuter>(GetProcAddress(hCore, "?GetOuter@UObject@@QBEPAV1@XZ"));
    pStaticExec = reinterpret_cast<fnStaticExec>(GetProcAddress(hCore, "?StaticExec@UObject@@SAHPBDAAVFOutputDevice@@@Z"));
    ppGLog = reinterpret_cast<void**>(GetProcAddress(hCore, "?GLog@@3PAVFOutputDevice@@A"));
}

static void InitWinDrv()
{
    auto hWinDrv = GetModuleHandleW(L"WinDrv.dll");
    if (!hWinDrv)
        return;

    pCauseInputEvent = reinterpret_cast<fnCauseInputEvent>(GetProcAddress(hWinDrv, "?CauseInputEvent@UWindowsViewport@@QAEHHW4EInputAction@@M@Z"));

    if (!pCauseInputEvent)
    {
        LogWarn("Controller: CauseInputEvent not found, no pad buttons");
        return;
    }

    shCauseInputEvent = safetyhook::create_inline(pCauseInputEvent, CauseInputEvent);

    fnViewportInput = FeedPad;
}

static void InitGUI()
{
    auto hGUI = GetModuleHandleW(L"GUI.dll");
    if (!hGUI)
        return;

    if (auto p = GetProcAddress(hGUI, "?NativeKeyEvent@UGUIController@@UAEHAAE0M@Z"))
        shNativeKeyEvent = safetyhook::create_inline(p, NativeKeyEvent);
    else
        LogWarn("Controller: GUI key entry not found, pad cannot drive menus");

    if (auto p = GetProcAddress(hGUI, "?NativeTick@UGUIController@@UAEXM@Z"))
        shNativeTick = safetyhook::create_inline(p, NativeTick);
    else
        LogWarn("Controller: GUI tick not found, no stick menu navigation");

    // MOV EAX,[ESI+0x2c] / TEST byte [EAX+0x38],1 / JNZ, the bShowWindowsMouse test. The jump becomes
    // unconditional to the epilogue, the stack state the untaken branch already reaches.
    auto patternCursor = module_pattern(L"GUI.dll", "8B 46 2C F6 40 38 01 0F 85 DE 01 00 00");
    if (patternCursor.empty())
    {
        LogWarn("Controller: menu cursor branch not found, the cursor stays on with a pad");
        return;
    }

    patchHideCursor = std::make_unique<raw_mem>(patternCursor.get_first(7),
        std::initializer_list<uint8_t>{ 0xE9, 0xCC, 0x01, 0x00, 0x00, 0x90 });
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    if (auto p = GetProcAddress(hEngine, "?DealWithPlayerInputEvent@UPlayerInput@@QAEXM@Z"))
        shDealWithPlayerInputEvent = safetyhook::create_inline(p, DealWithPlayerInputEvent);
    else
        LogWarn("Controller: DealWithPlayerInputEvent not found, no pad look or aim acceleration fix");

    pDirectAxis = reinterpret_cast<fnDirectAxis>(GetProcAddress(hEngine, "?DirectAxis@UInput@@UAEXW4EInputKey@@MM@Z"));
    pOptimizeInputBindings = reinterpret_cast<fnOptimizeInputBindings>(GetProcAddress(hEngine, "?OptimizeInputBindings@UViewport@@UAEXXZ"));

    if (!pDirectAxis)
        LogWarn("Controller: UInput::DirectAxis not found, no pad input");

    BindFloat(fPadSensitivity, PREF_PADSENSITIVITY);

    ApplyAndWatch([]()
    {
        bVibration = MongooseFixSettings.GetInt(PREF_VIBRATION) != 0;

        if (!bVibration)
            StopRumble();
    });

    auto ppInstance = reinterpret_cast<void**>(GetProcAddress(hEngine, "?Instance@UForceFeedbackManager@@1PAV1@A"));
    if (!ppInstance)
    {
        LogWarn("Controller: force feedback manager not found, no rumble");
        return;
    }

    apVtable[nSlotStartEffect] = StartEffect;
    apVtable[nSlotEnableForceFeedback] = EnableForceFeedback;
    apVtable[nSlotIsForceFeedbackEnable] = IsForceFeedbackEnable;

    *ppInstance = &pVtable;
}

class Controller
{
public:
    Controller()
    {
        MongooseFix::onCoreInitEvent() += []() { InitCore(); };
        MongooseFix::onWinDrvInitEvent() += []() { InitWinDrv(); };
        MongooseFix::onGUIInitEvent() += []() { InitGUI(); };
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
        MongooseFix::onShutdownEvent() += []() { StopRumble(); };
    }
} Controller;
