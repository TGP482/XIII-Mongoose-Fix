module;

#include <common.hxx>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

export module display;

import common;
import settings;
import logging;
import internalres;
import crtgamma;

// UD3DRenderDevice field offsets, all from D3DDrv.dll:
//   +0x0D8  UseVSync            config bool the game only honours fullscreen
//   +0x178  MaxAnisotropy       fetched from D3DCAPS8 at Init, never used by the game
//   +0x660  RefreshRate         picked by SetRes, written into present parameters
//   +0x658  SizeX
//   +0x65C  SizeY
//   +0x66C  IDirect3DDevice8*
//   +0x648  RebuildDevice       set by Lock, cleared and acted on by SetRes
static constexpr auto nOffsetUseVSync = 0xD8;
static constexpr auto nOffsetRebuildDevice = 0x648;
static constexpr auto nOffsetMaxAnisotropy = 0x178;
static constexpr auto nOffsetRefreshRate = 0x660;
static constexpr auto nOffsetSizeX = 0x658;
static constexpr auto nOffsetSizeY = 0x65C;
static constexpr auto nOffsetDevice = 0x66C;

// UD3DRenderDevice::Lock builds its D3DVIEWPORT8 straight off the UViewport it is handed
// (X +0x90, Y +0x94, Width +0x88, Height +0x8C) and asserts when SetViewport refuses the rect.
static constexpr auto nOffsetViewportSizeX = 0x88;
static constexpr auto nOffsetViewportSizeY = 0x8C;
static constexpr auto nOffsetViewportOrgX = 0x90;
static constexpr auto nOffsetViewportOrgY = 0x94;


// The game knows fullscreen or a plain window only: borderless is a windowed device with the
// frame off and the window snapped to the monitor.
static constexpr auto nModeWindowed = 0;
static constexpr auto nModeBorderless = 1;
static constexpr auto nModeFullscreen = 2;

static LONG nSavedStyle = 0;
static LONG nSavedExStyle = 0;
static HWND hGameWindow = nullptr;
static int nOutputWidth = 0;
static int nOutputHeight = 0;

static uint8_t* pRenderDevice = nullptr;
static void* pLastViewport = nullptr;

// Read back by comicpanels.ixx, which has no other way to learn the real height.
export std::atomic<int> nBackBufferWidth = 0;
export std::atomic<int> nBackBufferHeight = 0;

export void* GetD3DDevice()
{
    return pRenderDevice ? *reinterpret_cast<void**>(pRenderDevice + nOffsetDevice) : nullptr;
}

// Output size, read before internalres stamps the render size over SizeX/SizeY. What a render
// resolution is a percentage of.
export int GetDisplayOutputWidth() { return nOutputWidth; }
export int GetDisplayOutputHeight() { return nOutputHeight; }

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

static bool bForceRebuild = false;
// Whether the windowed client has been asked for since the last SetRes.
static bool bSizeApplied = false;

// What the device is built from; anything else in the ini can change without a reset.
// Multisampling goes into the present parameters and the internal target is built in SetRes, so
// both only move on a reset.
using DeviceKey_t = std::array<int32_t, 7>;

static DeviceKey_t DeviceKey()
{
    return { MongooseFixSettings.GetInt(PREF_DISPLAYMODE), MongooseFixSettings.GetInt(PREF_RESOLUTIONX),
        MongooseFixSettings.GetInt(PREF_RESOLUTIONY), MongooseFixSettings.GetInt(PREF_VSYNC),
        MongooseFixSettings.GetInt(PREF_MSAA), MongooseFixSettings.GetInt(PREF_INTERNALRESX),
        MongooseFixSettings.GetInt(PREF_INTERNALRESY) };
}

static DeviceKey_t aDeviceKey{};

// SetRes builds the whole D3DPRESENT_PARAMETERS block on its stack and uses it for CreateDevice,
// Reset and device-lost recovery, so everything display related is decided there.
static SafetyHookInline shSetRes{};
static SafetyHookInline shPresent{};
static SafetyHookInline shLock{};

// D3DSWAPEFFECT_COPY_VSYNC is Direct3D 8's only windowed vsync and it rules out multisampling.
// Windowed presents go through the compositor anyway, so the frame is paced against it in Present
// and the swap effect is left alone.
static bool bWindowedVSync = false;

// Fullscreen mode picking scores every display mode by |refresh - 75| and takes the lowest, so
// 75 Hz wins on any monitor that has it. Scoring by -refresh takes the highest offered instead.
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

// Fullscreen leaves the window to the game; the other two put the frame on or off.
static void ApplyDisplayMode(int nMode, int nWidth, int nHeight)
{
    if (nMode == nModeFullscreen)
        return;

    // First SetRes runs before the window exists; Present retries until it does.
    const auto hWindow = GameWindow();
    if (!hWindow)
        return;

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
        // mouse, trapping the cursor off the client area until then.
        ClipCursor(nullptr);

        return;
    }

    // ResizeViewport puts the game's own style back at the end of every SetRes, so the frame goes
    // on whenever it is missing rather than once. The size is asked for once per SetRes: a client
    // the window manager will not hand back at exactly this size never compares equal, and resizing
    // it every present stalls the loading screen.
    const auto bStyleWrong = GetWindowLongW(hWindow, GWL_STYLE) != nSavedStyle;

    if (bStyleWrong)
    {
        SetWindowLongW(hWindow, GWL_STYLE, nSavedStyle);
        SetWindowLongW(hWindow, GWL_EXSTYLE, nSavedExStyle);
    }
    else if (bSizeApplied)
    {
        return;
    }

    bSizeApplied = true;

    // Back buffer size is the client area, not the window.
    RECT rect{ 0, 0, nWidth, nHeight };
    AdjustWindowRect(&rect, nSavedStyle, FALSE);

    SetWindowPos(hWindow, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ClipCursor(nullptr);
}

// Leaves what it was handed, the size the caller wanted, when there is no monitor to ask.
static void ResolveDesiredResolution(int& nX, int& nY)
{
    RECT monitor{};
    if (!MonitorRect(GameWindow(), monitor))
        return;

    const auto nMonitorX = monitor.right - monitor.left;
    const auto nMonitorY = monitor.bottom - monitor.top;

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

    ResolveDesiredResolution(nX, nY);

    const auto bVSync = MongooseFixSettings.GetInt(PREF_VSYNC) != 0;

    // Fullscreen honours this field itself; windowed is paced in Present.
    *reinterpret_cast<int*>(pThis + nOffsetUseVSync) = bVSync ? 1 : 0;

    bWindowedVSync = bVSync && nFullscreen == 0;

    // A windowed SetRes with a live device that was not fullscreen skips the whole rebuild block:
    // no present parameters, no Reset, render target fields untouched. That block is where
    // multisampling, the internal target and vsync land, so the flag Lock uses to demand a rebuild
    // forces it. Only for the reset this module asks for: forcing it on the game's own calls
    // rebuilds the device behind every viewport resize, which the window cannot survive.
    if (std::exchange(bForceRebuild, false) && nFullscreen == 0)
        *reinterpret_cast<int*>(pThis + nOffsetRebuildDevice) = 1;

    // Our render target is D3DPOOL_DEFAULT and SetRes resets the device, so it goes first.
    ReleaseInternalRes();
    ReleaseCRTGamma();

    const auto nResult = shSetRes.fastcall<int>(pThis, nullptr, pViewport, nX, nY, nFullscreen);

    nOutputWidth = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    nOutputHeight = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    // Ahead of ApplyInternalRes, while the sizes are still the output size.
    bSizeApplied = false;
    ApplyDisplayMode(nMode, nOutputWidth, nOutputHeight);

    // Stamps the internal render size over SizeX/SizeY, so the read below reports it.
    if (shLock)
        ApplyInternalRes(pThis, reinterpret_cast<uint8_t*>(pViewport));

    nBackBufferWidth = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    nBackBufferHeight = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    static constexpr const char* aszMode[] = { "windowed", "borderless", "fullscreen" };

    LogInfo("Display: {}x{} {} {} Hz, vsync {}", nBackBufferWidth.load(), nBackBufferHeight.load(),
        aszMode[nMode], *reinterpret_cast<int*>(pThis + nOffsetRefreshRate), bVSync ? "on" : "off");

    onDeviceResetEvent().executeAll();
    return nResult;
}

// The rect has to be the extent of what is bound, and the device's SizeX/SizeY are what that
// target was built from, internal resolution included. The viewport's own size gives a frame in
// the corner when it is smaller and a refused rect when it is larger.
static void StampViewportRect(uint8_t* pThis, uint8_t* pViewport)
{
    if (!pThis || !pViewport)
        return;

    const auto nSizeX = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    const auto nSizeY = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    if (nSizeX < 1 || nSizeY < 1)
        return;

    // The origin is the viewport's own; moving it moves everything laid out against it.
    const auto nOrgX = *reinterpret_cast<int*>(pViewport + nOffsetViewportOrgX);
    const auto nOrgY = *reinterpret_cast<int*>(pViewport + nOffsetViewportOrgY);

    *reinterpret_cast<int*>(pViewport + nOffsetViewportSizeX) = (std::max)(nSizeX - nOrgX, 1);
    *reinterpret_cast<int*>(pViewport + nOffsetViewportSizeY) = (std::max)(nSizeY - nOrgY, 1);
}

static void* __fastcall Lock(uint8_t* pThis, void*, void* pViewport, uint8_t* pHitData, int* pHitSize)
{
    StampViewportRect(pThis, reinterpret_cast<uint8_t*>(pViewport));
    StampInternalResViewport(reinterpret_cast<uint8_t*>(pViewport));
    return shLock.fastcall<void*>(pThis, nullptr, pViewport, pHitData, pHitSize);
}

// The ini watcher runs on its own thread and must not touch the device there. Present is the one
// place in the frame where the device is known idle, so pending changes are applied here.
static void __fastcall Present(uint8_t* pThis, void*, void* pViewport)
{
    if (bDisplayChangePending.exchange(false) && pRenderDevice)
    {
        // Back through our own hook, which resolves the size and mode itself. Output size, not
        // SizeX/SizeY: those hold the internal size while scaling is live.
        bForceRebuild = true;
        SetRes(pThis, nullptr, pLastViewport, nOutputWidth, nOutputHeight, 0);
    }

    // Both windowed modes are re-asserted every frame, returning on the first compare once the
    // window matches.
    ApplyDisplayMode(MongooseFixSettings.GetInt(PREF_DISPLAYMODE), nOutputWidth, nOutputHeight);

    PresentInternalRes(pThis);
    PresentCRTGamma(pThis);

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

    // The render device keeps its decorated C++ names in the export table, so these are asked for
    // by name instead of scanned for.
    auto pSetRes = GetProcAddress(hD3DDrv, "?SetRes@UD3DRenderDevice@@UAEHPAVUViewport@@HHH@Z");
    auto pPresent = GetProcAddress(hD3DDrv, "?Present@UD3DRenderDevice@@UAEXPAVUViewport@@@Z");
    auto pLock = GetProcAddress(hD3DDrv, "?Lock@UD3DRenderDevice@@UAEPAVFRenderInterface@@PAVUViewport@@PAEPAH@Z");

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

    if (pLock)
        shLock = safetyhook::create_inline(pLock, Lock);

    if (!shLock)
        LogWarn("Display: UD3DRenderDevice::Lock could not be hooked, an internal resolution is unsafe and stays off");

    // Resolution and vsync need a device reset, and only when one of them actually moved: a reset
    // drops the gamma ramp the game set through its Brightness, Gamma and Contrast console commands
    // and never puts it back, so resetting over an unrelated setting darkens the game until the
    // player reopens the video page.
    aDeviceKey = DeviceKey();

    MongooseFix::onIniFileChange() += []()
    {
        const auto key = DeviceKey();

        if (key == aDeviceKey)
            return;

        aDeviceKey = key;
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
