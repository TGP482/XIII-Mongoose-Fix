module;

#include <common.hxx>

export module display;

import common;
import settings;
import logging;

// UD3DRenderDevice is where the window size, the vsync flag and the D3D8 device all live, so one
// module owns the render device pointer and hands it to whoever else needs it.
//
// Field offsets, all from D3DDrv.dll:
//   +0x0D8  UseVSync            config bool the game already has but only honours fullscreen
//   +0x178  MaxAnisotropy       fetched from D3DCAPS8 at Init and then never used by the game
//   +0x64C  bFullscreen
//   +0x658  SizeX
//   +0x65C  SizeY
//   +0x66C  IDirect3DDevice8*
static constexpr auto nOffsetUseVSync = 0xD8;
static constexpr auto nOffsetMaxAnisotropy = 0x178;
static constexpr auto nOffsetFullscreen = 0x64C;
static constexpr auto nOffsetSizeX = 0x658;
static constexpr auto nOffsetSizeY = 0x65C;
static constexpr auto nOffsetDevice = 0x66C;

static uint8_t* pRenderDevice = nullptr;
static void* pLastViewport = nullptr;
static int nLastFullscreen = 0;

// Read back by comicpanels.ixx, which has no other way to learn the real height at the point it
// needs it.
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

// Raised whenever a device has just been created or reset, so modules that hook the device
// itself can put their hooks back on.
export MongooseFix::Event<>& onDeviceResetEvent()
{
    static MongooseFix::Event<> e;
    return e;
}

static std::atomic<bool> bDisplayChangePending = false;

// SetRes builds the whole D3DPRESENT_PARAMETERS block on its stack and uses it for CreateDevice,
// for Reset and for the device-lost recovery path, so everything display related is decided here
// and nowhere else.
static SafetyHookInline shSetRes{};
static SafetyHookInline shPresent{};

// The swap effect is computed as (Windowed ? 3 : 1) by one LEA. Bumping the +1 to +2 turns the
// windowed case into D3DSWAPEFFECT_COPY_VSYNC, which is how vsync is asked for windowed under
// Direct3D 8 - the presentation interval is only allowed to be DEFAULT there. Fullscreen becomes
// FLIP instead of DISCARD, which is why the patch only goes on for windowed vsync.
static std::unique_ptr<raw_mem> patchWindowedSwapEffect;

static void ResolveDesiredResolution(int& nX, int& nY)
{
    auto nSettingX = MongooseFixSettings.GetInt(PREF_RESOLUTIONX);
    auto nSettingY = MongooseFixSettings.GetInt(PREF_RESOLUTIONY);

    // 0 on an axis means the desktop, which is the only sensible default and the one thing the
    // game's own resolution list cannot offer.
    if (nSettingX == 0)
        nSettingX = GetSystemMetrics(SM_CXSCREEN);
    if (nSettingY == 0)
        nSettingY = GetSystemMetrics(SM_CYSCREEN);

    if (nSettingX > 0 && nSettingY > 0)
    {
        nX = nSettingX;
        nY = nSettingY;
    }
}

static int __fastcall SetRes(uint8_t* pThis, void*, void* pViewport, int nX, int nY, int nFullscreen)
{
    pRenderDevice = pThis;
    pLastViewport = pViewport;
    nLastFullscreen = nFullscreen;

    ResolveDesiredResolution(nX, nY);

    const auto bVSync = MongooseFixSettings.GetInt(PREF_VSYNC) != 0;

    // Fullscreen honours this field on its own; windowed needs the swap effect instead.
    *reinterpret_cast<int*>(pThis + nOffsetUseVSync) = bVSync ? 1 : 0;

    if (patchWindowedSwapEffect)
        patchWindowedSwapEffect->Set(bVSync && nFullscreen == 0);

    const auto nResult = shSetRes.fastcall<int>(pThis, nullptr, pViewport, nX, nY, nFullscreen);

    if (patchWindowedSwapEffect)
        patchWindowedSwapEffect->Restore();

    nBackBufferWidth = *reinterpret_cast<int*>(pThis + nOffsetSizeX);
    nBackBufferHeight = *reinterpret_cast<int*>(pThis + nOffsetSizeY);

    LogInfo("Display: {}x{} {}, vsync {}", nBackBufferWidth.load(), nBackBufferHeight.load(),
        *reinterpret_cast<int*>(pThis + nOffsetFullscreen) ? "fullscreen" : "windowed", bVSync ? "on" : "off");

    onDeviceResetEvent().executeAll();
    return nResult;
}

// The ini watcher runs on its own thread and must not touch the device from there. Present is the
// one place in the frame where the device is known to be idle, so a pending change is taken here.
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

    shPresent.fastcall<void>(pThis, nullptr, pViewport);
}

static void InitD3DDrv()
{
    auto hD3DDrv = GetModuleHandleW(L"D3DDrv.dll");
    if (!hD3DDrv)
        return;

    // The render device keeps its decorated C++ names in the export table, so the two functions
    // that matter can be asked for by name instead of scanned for.
    auto pSetRes = GetProcAddress(hD3DDrv, "?SetRes@UD3DRenderDevice@@UAEHPAVUViewport@@HHH@Z");
    auto pPresent = GetProcAddress(hD3DDrv, "?Present@UD3DRenderDevice@@UAEXPAVUViewport@@@Z");

    if (!pSetRes || !pPresent)
    {
        LogWarn("Display: D3DDrv.dll did not export SetRes or Present, display options are off");
        return;
    }

    // LEA ECX,[ECX+ECX*1+1] / MOV [EBP-0x88],ECX - the swap effect being written into the
    // present parameters.
    auto patternSwapEffect = module_pattern(L"D3DDrv.dll", "8D 4C 09 01 89 8D 78 FF FF FF");
    if (!patternSwapEffect.empty())
        patchWindowedSwapEffect = std::make_unique<raw_mem>(patternSwapEffect.get_first(3), std::initializer_list<uint8_t>{ 0x02 });
    else
        LogWarn("Display: swap effect pattern not found, windowed vsync is off");

    shSetRes = safetyhook::create_inline(pSetRes, SetRes);
    shPresent = safetyhook::create_inline(pPresent, Present);

    // Resolution and vsync both need a device reset to take, so a change asks for one rather
    // than applying anything itself.
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
