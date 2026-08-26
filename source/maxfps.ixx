module;

#include <common.hxx>
#include <timeapi.h>

#pragma comment(lib, "winmm.lib")

export module maxfps;

import common;
import settings;
import logging;
import display;

// The main loop ends every frame with appSleep(1/GetMaxTickRate - elapsed), and appSleep is a
// plain Sleep(ms): whole milliseconds, stretched to the next scheduler tick, which is why a 60 cap
// lands near 33. There is no "> 0" test around it either, so returning 0 asks it to sleep 1/0.
//
// So the wait is taken here instead, on the performance counter, and the loop is handed a rate it
// can never be behind on. GetMaxTickRate is called once per frame right before that sleep, so this
// is the same point in the frame.
//
// Multiplayer answers are left alone: they are the net tick rate, not a frame rate.
static constexpr auto fStockMaxTickRate = 120.0f;
static constexpr auto fNoEngineWait = 10000.0f;

static std::atomic<int> nMaxFrameRate = 0;
static std::atomic<int> nRefreshRate = 60;

// A cap of 1 means the monitor. Read per device reset too - fullscreen picks its own mode.
static void ReadRefreshRate()
{
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    const auto bNamed = GetMonitorInfoW(MonitorFromWindow(FindGameWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor) != 0;

    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);

    if (EnumDisplaySettingsW(bNamed ? monitor.szDevice : nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        nRefreshRate = static_cast<int>(mode.dmDisplayFrequency);
}

static SafetyHookInline shGetMaxTickRate{};

static int64_t QpcFreq()
{
    static LARGE_INTEGER liFreq{};
    if (!liFreq.QuadPart)
        QueryPerformanceFrequency(&liFreq);
    return liFreq.QuadPart;
}

static void PaceFrame(int nFps)
{
    if (nFps <= 0)
        return;

    const auto nPeriod = QpcFreq() / nFps;

    LARGE_INTEGER liNow{};
    QueryPerformanceCounter(&liNow);

    // First frame, or a stall long enough that catching up would run free: start over.
    static int64_t nNextFrame = 0;
    if (nNextFrame == 0 || liNow.QuadPart > nNextFrame + nPeriod)
    {
        nNextFrame = liNow.QuadPart + nPeriod;
        return;
    }

    // Sleep the bulk, spin the last millisecond. Sleep alone cannot land on a frame boundary.
    for (;;)
    {
        QueryPerformanceCounter(&liNow);
        const auto nLeft = nNextFrame - liNow.QuadPart;
        if (nLeft <= 0)
            break;

        const auto nMs = (nLeft * 1000) / QpcFreq();
        if (nMs > 1)
            Sleep(static_cast<DWORD>(nMs - 1));
        else
            YieldProcessor();
    }

    // Exactly one period, so rounding never accumulates into drift.
    nNextFrame += nPeriod;
}

static float __fastcall GetMaxTickRate(void* pThis, void*)
{
    const auto fOriginal = shGetMaxTickRate.fastcall<float>(pThis, nullptr);

    if (fOriginal != fStockMaxTickRate)
        return fOriginal;

    const auto nCap = nMaxFrameRate.load();
    PaceFrame(nCap == 1 ? nRefreshRate.load() : nCap);
    return fNoEngineWait;
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto pGetMaxTickRate = GetProcAddress(hEngine, "?GetMaxTickRate@UGameEngine@@UAEMXZ");
    if (!pGetMaxTickRate)
    {
        LogWarn("MaxFrameRate: Engine.dll did not export GetMaxTickRate, the frame rate cap is untouched");
        return;
    }

    // Whole milliseconds are all the fallback Sleep can do; without this it is ~15.6.
    timeBeginPeriod(1);
    MongooseFix::onShutdownEvent() += []() { timeEndPeriod(1); };

    shGetMaxTickRate = safetyhook::create_inline(pGetMaxTickRate, GetMaxTickRate);
    BindInt(nMaxFrameRate, PREF_MAXFRAMERATE);

    ReadRefreshRate();
    onDeviceResetEvent() += []() { ReadRefreshRate(); };
}

class MaxFrameRate
{
public:
    MaxFrameRate()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} MaxFrameRate;
