module;

#include <common.hxx>

export module exitfix;

import common;
import logging;

// Closing the game can wedge the machine hard enough to need the power button: the exclusive
// fullscreen D3D8 teardown releases the device, restores the display mode and destroys the window
// from inside the message handler while the loading animation thread is still alive. Windows does
// all of that on process exit anyway, so the process is killed at the first point the game commits
// to quitting - WM_CLOSE, which the viewport turns into an EXIT command, and appPreExit for the
// menu's Quit. Nothing is lost: XIII writes options and saves as they change, not on exit.
static SafetyHookInline shViewportWndProc{};
static SafetyHookInline shAppPreExit{};

static void ExitNow(const char* szPath)
{
    LogInfo("ExitFix: {}, ending the process before the engine tears the device down", szPath);

    // Both die with the process; giving them back first covers a slow kill.
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0);

    TerminateProcess(GetCurrentProcess(), 0);
}

static LRESULT __fastcall ViewportWndProc(void* pThis, void*, uint32_t nMessage, uint32_t wParam, LPARAM lParam)
{
    // Alt+F4 is SC_CLOSE until DefWindowProc has seen it, so both are taken. The low four bits of
    // wParam are reserved on a system command.
    if (nMessage == WM_CLOSE || (nMessage == WM_SYSCOMMAND && (wParam & 0xFFF0) == SC_CLOSE))
        ExitNow("close requested");

    return shViewportWndProc.fastcall<LRESULT>(pThis, nullptr, nMessage, wParam, lParam);
}

static void __cdecl AppPreExit()
{
    ExitNow("the game asked to quit");
}

static void InitWinDrv()
{
    auto hWinDrv = GetModuleHandleW(L"WinDrv.dll");
    if (!hWinDrv)
        return;

    if (auto p = GetProcAddress(hWinDrv, "?ViewportWndProc@UWindowsViewport@@QAEJIIJ@Z"))
        shViewportWndProc = safetyhook::create_inline(p, ViewportWndProc);
    else
        LogWarn("ExitFix: WinDrv.dll did not export ViewportWndProc, Alt+F4 still goes the long way");
}

static void InitCore()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hCore)
        return;

    if (auto p = GetProcAddress(hCore, "?appPreExit@@YAXXZ"))
        shAppPreExit = safetyhook::create_inline(p, AppPreExit);
    else
        LogWarn("ExitFix: Core.dll did not export appPreExit, quitting from the menu still goes the long way");
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
