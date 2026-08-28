module;

#include <common.hxx>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

export module display;

import common;
import settings;
import logging;
import internalres;

// UD3DRenderDevice holds the window size, the vsync flag and the D3D8 device, so one module owns
// the render device pointer and hands it out.
//
// Field offsets, all from D3DDrv.dll:
//   +0x0D8  UseVSync            config bool the game has but only honours fullscreen
//   +0x178  MaxAnisotropy       fetched from D3DCAPS8 at Init, never used by the game
//   +0x660  RefreshRate         picked by SetRes, written into present parameters
//   +0x658  SizeX
//   +0x65C  SizeY
//   +0x66C  IDirect3DDevice8*
static constexpr auto nOffsetUseVSync = 0xD8;
static constexpr auto nOffsetMaxAnisotropy = 0x178;
static constexpr auto nOffsetRefreshRate = 0x660;
static constexpr auto nOffsetSizeX = 0x658;
static constexpr auto nOffsetSizeY = 0x65C;
static constexpr auto nOffsetDevice = 0x66C;

// The game knows fullscreen or a plain window only, so borderless is a windowed device with the
// frame off and the window snapped to the monitor.
static constexpr auto nModeWindowed = 0;
static constexpr auto nModeBorderless = 1;
static constexpr auto nModeFullscreen = 2;

static LONG nSavedStyle = 0;
static LONG nSavedExStyle = 0;
static bool bModeApplied = false;
static HWND hGameWindow = nullptr;
static int nOutputWidth = 0;
static int nOutputHeight = 0;

static uint8_t* pRenderDevice = nullptr;
static void* pLastViewport = nullptr;
static int nLastFullscreen = 0;

// Read back by comicpanels.ixx, which has no other way to learn the real height when it needs it.
export std::atomic<int> nBackBufferWidth = 0;
export std::atomic<int> nBackBufferHeight = 0;

export void* GetD3DDevice()
{
    return pRenderDevice ? *reinterpret_cast<void**>(pRenderDevice + nOffsetDevice) : nullptr;
}

export int GetDeviceMaxAnisotropy()
{
    return pRenderDevice ? *reinterpret_cast<int*>(pRenderDevice + nOffsetMaxAnisotropy) : 1;
}

// Raised after a device is created or reset, so modules that hook the device can re-hook.
export MongooseFix::Event<>& onDeviceResetEvent()
{
    static MongooseFix::Event<> e;
    return e;
}

static std::atomic<bool> bDisplayChangePending = false;

// SetRes builds the whole D3DPRESENT_PARAMETERS block on its stack and uses it for CreateDevice,
// Reset and device-lost recovery, so everything display related is decided here and nowhere else.
static SafetyHookInline shSetRes{};
static SafetyHookInline shPresent{};

// D3DSWAPEFFECT_COPY_VSYNC is the only windowed vsync Direct3D 8 has, and it rules out the
// multisampling MSAA needs. Windowed presents go through the compositor anyway, so the frame is
// paced against it in Present instead and the swap effect is left alone.
static bool bWindowedVSync = false;

// Fullscreen mode picking scores every display mode by |refresh - 75| and takes the lowest, so a
// 75 Hz mode wins on any monitor that has one and the frame rate is pinned there whatever the cap
// says. Scoring by -refresh instead takes the highest the monitor offers.
static std::unique_ptr<raw_mem> patchRefreshPreference;

// The viewport window can be a child, and a child ignores WS_POPUP: styles go on the top level.
static HWND GameWindow()
{
    if (!hGameWindow || !IsWindow(hGameWindow))
    {
        const auto hFound = FindGameWindow();
        hGameWindow = hFound ? GetAncestor(hFound, GA_ROOT) : nullptr;
    }

    return hGameWindow;
}

static bool MonitorRect(HWND hWindow, RECT& rect)
{
    MONITORINFO monitor{ sizeof(monitor) };
    if (!GetMonitorInfoW(MonitorFromWindow(hWindow, MONITOR_DEFAULTTOPRIMARY), &monitor))
        return false;

    rect = monitor.rcMonitor;
    return true;
}

// The monitor the game is on; the primary one until the window exists.
static void MonitorSize(int& nX, int& nY)
{
    MONITORINFO monitor{ sizeof(monitor) };
    if (GetMonitorInfoW(MonitorFromWindow(GameWindow(), MONITOR_DEFAULTTOPRIMARY), &monitor))
    {
        nX = monitor.rcMonitor.right - monitor.rcMonitor.left;
        nY = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    }
}

// Fullscreen leaves the window to the game; the other two put the frame on or off.
static void ApplyDisplayMode(int nMode, int nWidth, int nHeight)
{
    if (nMode == nModeFullscreen)
    {
        bModeApplied = true;
        return;
    }

    // First SetRes runs before the window exists; Present retries until it does.
    const auto hWindow = GameWindow();
    if (!hWindow)
        return;

    bModeApplied = true;

    if (!nSavedStyle)
    {
        nSavedStyle = GetWindowLongW(hWindow, GWL_STYLE);
        nSavedExStyle = GetWindowLongW(hWindow, GWL_EXSTYLE);
    }

    if (nMode == nModeBorderless)
    {
        RECT rect{};
        if (!MonitorRect(hWindow, rect))
            return;

        const auto nStyle = (nSavedStyle & ~(WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;

        // The game drops its mouse capture whenever the window moves, so touch nothing if correct.
        RECT window{};
        if (GetWindowLongW(hWindow, GWL_STYLE) == nStyle && GetWindowRect(hWindow, &window) && EqualRect(&window, &rect))
            return;

        SetWindowLongW(hWindow, GWL_STYLE, nStyle);
        SetWindowLongW(hWindow, GWL_EXSTYLE, nSavedExStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));

        SetWindowPos(hWindow, HWND_TOP, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
            SWP_FRAMECHANGED | SWP_NOOWNERZORDER);

        // The clip still points at the old rect and is only redone when the game next takes the
        // mouse; until then the cursor is trapped off the client area.
        ClipCursor(nullptr);

        return;
    }

    RECT client{};
    if (GetWindowLongW(hWindow, GWL_STYLE) == nSavedStyle && GetClientRect(hWindow, &client)
        && client.right == nWidth && client.bottom == nHeight)
        return;

    SetWindowLongW(hWindow, GWL_STYLE, nSavedStyle);
    SetWindowLongW(hWindow, GWL_EXSTYLE, nSavedExStyle);

    // Back buffer size is the client area, not the window.
    RECT rect{ 0, 0, nWidth, nHeight };
    AdjustWindowRect(&rect, nSavedStyle, FALSE);

    SetWindowPos(hWindow, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ClipCursor(nullptr);
}

static void ResolveDesiredResolution(int& nX, int& nY)
{
    auto nMonitorX = GetSystemMetrics(SM_CXSCREEN);
    auto nMonitorY = GetSystemMetrics(SM_CYSCREEN);
    MonitorSize(nMonitorX, nMonitorY);

    // Borderless covers the monitor and a windowed present stretches a mismatched back buffer.
    // Render lower through InternalResolution instead.
    if (MongooseFixSettings.GetInt(PREF_DISPLAYMODE) == nModeBorderless)
    {
        nX = nMonitorX;
        nY = nMonitorY;
        return;
    }

    auto nSettingX = MongooseFixSettings.GetInt(PREF_RESOLUTIONX);
    auto nSettingY = MongooseFixSettings.GetInt(PREF_RESOLUTIONY);

    // 0 on an axis means the desktop, the one thing the game's own list cannot offer.
    if (nSettingX == 0)
        nSettingX = nMonitorX;
    if (nSettingY == 0)
        nSettingY = nMonitorY;

    if (nSettingX > 0 && nSettingY > 0)
    {
        nX = nSettingX;
        nY = nSettingY;
    }
}

static int __fastcall SetRes(uint8_t* pThis, void*, void* pViewport, int nX, int nY, int nFullscreen)
{
    // The setting decides the mode, whatever the caller asked.
    const auto nMode = MongooseFixSettings.GetInt(PREF_DISPLAYMODE);
    nFullscreen = nMode == nModeFullscreen ? 1 : 0;

    pRenderDevice = pThis;
    pLastViewport = pViewport;
    nLastFullscreen = nFullscreen;

    ResolveDesiredResolution(nX, nY);

    const auto bVSync = MongooseFixSettings.GetInt(PREF_VSYNC) != 0;

    // Fullscreen honours this field itself; windowed is paced in Present.
    *reinterpret_cast<int*>(pThis + nOffsetUseVSync) = bVSync ? 1 : 0;

    bWindowedVSync = bVSync && nFullscreen == 0;

    // Our render target is D3DPOOL_DEFAULT and SetRes resets the device, so it goes first.
    ReleaseInternalRes();

    const auto nResult = shSetRes.fastcall<int>(pThis, nullptr, pViewport, nX, nY, nFullscreen);

    nOutputWidth = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    nOutputHeight = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    // Ahead of ApplyInternalRes, while the sizes are still the output size.
    bModeApplied = false;
    ApplyDisplayMode(nMode, nOutputWidth, nOutputHeight);

    // Stamps the internal render size over SizeX/SizeY, so the read below reports it.
    ApplyInternalRes(pThis, reinterpret_cast<uint8_t*>(pViewport));

    nBackBufferWidth = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    nBackBufferHeight = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    static constexpr const char* aszMode[] = { "windowed", "borderless", "fullscreen" };

    LogInfo("Display: {}x{} {} {} Hz, vsync {}", nBackBufferWidth.load(), nBackBufferHeight.load(),
        aszMode[nMode], *reinterpret_cast<int*>(pThis + nOffsetRefreshRate), bVSync ? "on" : "off");

    onDeviceResetEvent().executeAll();
    return nResult;
}

// The ini watcher runs on its own thread and must not touch the device there. Present is the one
// place in the frame where the device is known idle, so a pending change is taken here.
static void __fastcall Present(uint8_t* pThis, void*, void* pViewport)
{
    if (bDisplayChangePending.exchange(false) && pRenderDevice)
    {
        int nX = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
        int nY = *reinterpret_cast<int*>(pThis + nOffsetSizeY);
        ResolveDesiredResolution(nX, nY);

        // Straight back through our own hook, so the vsync and resolution work happens once.
        SetRes(pThis, nullptr, pLastViewport ? pLastViewport : pViewport, nX, nY, nLastFullscreen);
    }

    if (!bModeApplied)
        ApplyDisplayMode(MongooseFixSettings.GetInt(PREF_DISPLAYMODE), nOutputWidth, nOutputHeight);

    PresentInternalRes(pThis);

    // Blocks until the next composition frame, so the window gets one present per refresh.
    if (bWindowedVSync)
        DwmFlush();

    shPresent.fastcall<void>(pThis, nullptr, pViewport);
}

static void InitD3DDrv()
{
    auto hD3DDrv = GetModuleHandleW(L"D3DDrv.dll");
    if (!hD3DDrv)
        return;

    // The render device keeps its decorated C++ names in the export table, so the two functions
    // that matter are asked for by name instead of scanned for.
    auto pSetRes = GetProcAddress(hD3DDrv, "?SetRes@UD3DRenderDevice@@UAEHPAVUViewport@@HHH@Z");
    auto pPresent = GetProcAddress(hD3DDrv, "?Present@UD3DRenderDevice@@UAEXPAVUViewport@@@Z");

    if (!pSetRes || !pPresent)
    {
        LogWarn("Display: D3DDrv.dll did not export SetRes or Present, display options are off");
        return;
    }

    // SUB EAX,0x4B / JNS / NEG EAX: the abs distance to 75. NEG EAX alone leaves the score as
    // -refresh, so the highest mode wins the tie break.
    auto patternRefresh = module_pattern(L"D3DDrv.dll", "8B 44 13 08 03 DA 83 E8 4B 79 02 F7 D8");
    if (!patternRefresh.empty())
    {
        patchRefreshPreference = std::make_unique<raw_mem>(patternRefresh.get_first(6),
            std::initializer_list<uint8_t>{ 0xF7, 0xD8, 0x90, 0x90, 0x90, 0x90, 0x90 });
        patchRefreshPreference->Write();
    }
    else
    {
        LogWarn("Display: refresh rate pattern not found, fullscreen stays on 75 Hz");
    }

    shSetRes = safetyhook::create_inline(pSetRes, SetRes);
    shPresent = safetyhook::create_inline(pPresent, Present);

    // Resolution and vsync both need a device reset, so a change asks for one rather than applying
    // anything itself.
    MongooseFix::onIniFileChange() += []()
    {
        bDisplayChangePending = true;
    };
}

class Display
{
public:
    Display()
    {
        MongooseFix::onD3DDrvInitEvent() += []() { InitD3DDrv(); };
    }
} Display;
