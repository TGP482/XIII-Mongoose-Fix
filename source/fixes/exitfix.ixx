module;

#include <common.hxx>

export module exitfix;

import common;

// Closing the game can wedge the machine hard enough to need the power button: the exclusive
// fullscreen D3D8 teardown releases the device, restores the display mode and destroys the window
// from inside the message handler while the loading animation thread is still alive. Windows does
// all of that on process exit anyway, so the process is killed at the first point the game commits
// to quitting: WM_CLOSE, which the viewport turns into an EXIT command, and appPreExit for the
// menu's Quit. Nothing is lost: XIII writes options and saves as they change, not on exit.
static SafetyHookInline shViewportWndProc{};
static SafetyHookInline shAppPreExit{};
static SafetyHookInline shVideoTick{};

static void ExitNow()
{
    // Both die with the process; giving them back first covers a slow kill.
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0);

    TerminateProcess(GetCurrentProcess(), 0);
}

static LRESULT __fastcall ViewportWndProc(void* pThis, void*, uint32_t nMessage, uint32_t wParam, LPARAM lParam)
{
    // The viewport eats system keys as input events on some states and never lets DefWindowProc
    // turn Alt+F4 into SC_CLOSE, so the key itself is taken as well. The low four bits of wParam
    // are reserved on a system command.
    if (nMessage == WM_CLOSE || (nMessage == WM_SYSKEYDOWN && wParam == VK_F4) ||
        (nMessage == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE))
        ExitNow();

    return shViewportWndProc.fastcall<LRESULT>(pThis, nullptr, nMessage, wParam, lParam);
}

// The exe plays each movie in a while loop that only ticks the player, so nothing drains the
// message queue and Alt+F4 sits there until the movie ends. Take the system keys and SC_CLOSE per
// tick. Normal keys are left in the queue for whatever skips the movie.
static void __fastcall VideoTick(void* pThis, void*, float fDelta)
{
    MSG msg{};

    while (PeekMessageW(&msg, nullptr, WM_SYSKEYDOWN, WM_SYSCOMMAND, PM_REMOVE))
        DispatchMessageW(&msg);

    shVideoTick.fastcall<void>(pThis, nullptr, fDelta);
}

static void __cdecl AppPreExit()
{
    ExitNow();
}

static void InitWinDrv()
{
    auto hWinDrv = GetModuleHandleW(L"WinDrv.dll");
    if (!hWinDrv)
        return;

    if (auto p = GetProcAddress(hWinDrv, "?ViewportWndProc@UWindowsViewport@@QAEJIIJ@Z"))
        shViewportWndProc = safetyhook::create_inline(p, ViewportWndProc);

    if (auto p = GetProcAddress(hWinDrv, "?Tick@UPCVideoPlayerDevice@@UAEXM@Z"))
        shVideoTick = safetyhook::create_inline(p, VideoTick);
}

static void InitCore()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hCore)
        return;

    if (auto p = GetProcAddress(hCore, "?appPreExit@@YAXXZ"))
        shAppPreExit = safetyhook::create_inline(p, AppPreExit);
}

class ExitFix
{
public:
    ExitFix()
    {
        MongooseFix::onWinDrvInitEvent() += []() { InitWinDrv(); };
        MongooseFix::onCoreInitEvent() += []() { InitCore(); };
    }
} ExitFix;
