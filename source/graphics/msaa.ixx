module;

#include <common.hxx>

export module msaa;

import common;
import settings;
import logging;
import dx9;

// The renderer never asks for a multisampled back buffer, so the request is written into the
// present parameters on their way into CreateDevice and Reset.
//
// XIII reads the back buffer back every frame in UD3DRenderDevice::ReadPixels, and D3D8 refuses
// LockRect on a multisampled surface, so that readback is redirected to the front buffer. MSAA
// stays off unless ReadPixels hooks.

// IDirect3D8
static constexpr auto nCheckMultiSampleSlot = 11;
static constexpr auto nCreateDeviceSlot = 15;

// IDirect3DDevice8
static constexpr auto nGetDisplayModeSlot = 8;
static constexpr auto nResetSlot = 14;
static constexpr auto nCreateImageSurfaceSlot = 27;
static constexpr auto nGetFrontBufferSlot = 30;

// IUnknown, IDirect3DSurface8
static constexpr auto nReleaseSlot = 2;
static constexpr auto nLockRectSlot = 9;
static constexpr auto nUnlockRectSlot = 10;

static constexpr auto D3DMULTISAMPLE_NONE = 0;
static constexpr auto D3DSWAPEFFECT_DISCARD = 1;
static constexpr auto D3DFMT_A8R8G8B8 = 21;
static constexpr auto D3DLOCK_READONLY = 0x10;

// A lockable back buffer rules multisampling out, on Direct3D 8 and on the Direct3D 9 wrapper
// alike. The game asks for one so stock ReadPixels can lock it, so the flag comes off with MSAA.
static constexpr uint32_t D3DPRESENTFLAG_LOCKABLE_BACKBUFFER = 0x1;

// UD3DRenderDevice, D3DDrv.dll. Fields documented in display.ixx.
static constexpr auto nOffsetSizeX = 0x658;
static constexpr auto nOffsetSizeY = 0x65C;
static constexpr auto nOffsetDevice = 0x66C;

// UViewport SizeX/SizeY, the extent stock ReadPixels copies.
static constexpr auto nOffsetViewportSizeX = 0x88;
static constexpr auto nOffsetViewportSizeY = 0x8C;

// D3D8 numbers its sample counts, so type is sample count.
static constexpr int nSampleLevels[] = { 8, 4, 2 };

struct D3DPRESENT_PARAMETERS8
{
    uint32_t nBackBufferWidth;
    uint32_t nBackBufferHeight;
    uint32_t nBackBufferFormat;
    uint32_t nBackBufferCount;
    uint32_t nMultiSampleType;
    uint32_t nSwapEffect;
    HWND hDeviceWindow;
    int32_t bWindowed;
    int32_t bEnableAutoDepthStencil;
    uint32_t nAutoDepthStencilFormat;
    uint32_t nFlags;
    uint32_t nFullScreenRefreshRateInHz;
    uint32_t nFullScreenPresentationInterval;
};

struct D3DDISPLAYMODE8
{
    uint32_t nWidth;
    uint32_t nHeight;
    uint32_t nRefreshRate;
    uint32_t nFormat;
};

struct D3DLOCKED_RECT8
{
    int32_t nPitch;
    void* pBits;
};

static std::atomic<int> nMSAASamples = 0;
static std::atomic<bool> bFailed = false;

static void* pD3D = nullptr;
static uint32_t nAdapter = 0;
static uint32_t nDeviceType = 1;
static int nLastApplied = -1;
static bool bReadPixelsHooked = false;

static HWND hPresentWindow = nullptr;
static bool bWindowedMode = false;

// GetFrontBuffer is slow, so the destination surface lives across frames.
static void* pFrontSurface = nullptr;
static void* pSurfaceDevice = nullptr;
static uint32_t nSurfaceWidth = 0;
static uint32_t nSurfaceHeight = 0;

static SafetyHookInline shDirect3DCreate8{};
static SafetyHookInline shReadPixels{};
static SafetyHookVmt vmtD3D{};
static SafetyHookVm vmCreateDevice{};
static SafetyHookVmt vmtDevice{};
static SafetyHookVm vmReset{};

static void ReleaseSurface()
{
    if (pFrontSurface)
        reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(pFrontSurface))[nReleaseSlot])(pFrontSurface);

    pFrontSurface = nullptr;
    pSurfaceDevice = nullptr;
    nSurfaceWidth = 0;
    nSurfaceHeight = 0;
}

static bool IsMultiSampleSupported(uint32_t nFormat, int32_t bWindowed, uint32_t nType)
{
    using fnCheck = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    auto pCheck = reinterpret_cast<fnCheck>((*reinterpret_cast<void***>(pD3D))[nCheckMultiSampleSlot]);
    return SUCCEEDED(pCheck(pD3D, nAdapter, nDeviceType, nFormat, bWindowed, nType));
}

// D3D8 cannot texture from or resolve a multisampled surface, so MSAA and an internal resolution
// cannot combine. Direct3D 9 can resolve, so there internalres multisamples its own target and the
// back buffer stays single sampled.
static int WantedSamples()
{
    if (!bReadPixelsHooked || bFailed.load())
        return 0;

    if (MongooseFixSettings.GetInt(PREF_INTERNALRESX) != 0 || MongooseFixSettings.GetInt(PREF_INTERNALRESY) != 0)
    {
        if (IsDX9Active())
            return 0;

        static std::once_flag flag;
        std::call_once(flag, []() { LogWarn("MSAA: supersampling and MSAA cannot be combined on Direct3D 8, MSAA is off"); });
        return 0;
    }

    return nMSAASamples.load();
}

static void ApplyMSAA(D3DPRESENT_PARAMETERS8* pParams)
{
    if (!pParams || !pD3D)
        return;

    pParams->nMultiSampleType = D3DMULTISAMPLE_NONE;

    bWindowedMode = pParams->bWindowed != 0;
    if (pParams->hDeviceWindow)
        hPresentWindow = pParams->hDeviceWindow;

    const auto nWanted = WantedSamples();
    if (nWanted < 2)
    {
        if (std::exchange(nLastApplied, 0) > 0)
            LogInfo("MSAA: off");
        return;
    }

    for (auto nType : nSampleLevels)
    {
        if (nType > nWanted)
            continue;

        // Auto depth stencil is made at the back buffer sample count, so it needs it too.
        if (!IsMultiSampleSupported(pParams->nBackBufferFormat, pParams->bWindowed, nType))
            continue;
        if (pParams->bEnableAutoDepthStencil && !IsMultiSampleSupported(pParams->nAutoDepthStencilFormat, pParams->bWindowed, nType))
            continue;

        pParams->nMultiSampleType = static_cast<uint32_t>(nType);
        break;
    }

    const auto nGot = static_cast<int>(pParams->nMultiSampleType);

    // D3D8 allows multisampling on DISCARD only.
    if (nGot > 0)
    {
        pParams->nSwapEffect = D3DSWAPEFFECT_DISCARD;
        pParams->nFlags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    }

    if (std::exchange(nLastApplied, nGot) == nGot)
        return;

    if (nGot == 0)
        LogWarn("MSAA: {}x asked for, the device supports none of it", nWanted);
    else if (nGot < nWanted)
        LogWarn("MSAA: {}x asked for, {}x is the highest the device supports", nWanted, nGot);
    else
        LogInfo("MSAA: {}x", nGot);
}

// GetFrontBuffer hands back the whole screen in X8R8G8B8, the dword layout of UE2 FColor.
static bool CaptureFrontBuffer(uint8_t* pRenderDev, uint8_t* pViewport, uint32_t* pDest)
{
    if (!pRenderDev || !pViewport || !pDest)
        return false;

    auto pDevice = *reinterpret_cast<void**>(pRenderDev + nOffsetDevice);
    const auto nDestX = *reinterpret_cast<int32_t*>(pViewport + nOffsetViewportSizeX);
    const auto nDestY = *reinterpret_cast<int32_t*>(pViewport + nOffsetViewportSizeY);

    if (!pDevice || nDestX < 1 || nDestY < 1)
        return false;

    auto pVtbl = *reinterpret_cast<void***>(pDevice);

    D3DDISPLAYMODE8 mode{};
    auto pGetMode = reinterpret_cast<HRESULT(__stdcall*)(void*, D3DDISPLAYMODE8*)>(pVtbl[nGetDisplayModeSlot]);
    if (FAILED(pGetMode(pDevice, &mode)) || mode.nWidth < 1 || mode.nHeight < 1)
        return false;

    if (pFrontSurface && (pSurfaceDevice != pDevice || nSurfaceWidth != mode.nWidth || nSurfaceHeight != mode.nHeight))
        ReleaseSurface();

    if (!pFrontSurface)
    {
        auto pCreate = reinterpret_cast<HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, void**)>(pVtbl[nCreateImageSurfaceSlot]);
        if (FAILED(pCreate(pDevice, mode.nWidth, mode.nHeight, D3DFMT_A8R8G8B8, &pFrontSurface)) || !pFrontSurface)
        {
            pFrontSurface = nullptr;
            return false;
        }

        pSurfaceDevice = pDevice;
        nSurfaceWidth = mode.nWidth;
        nSurfaceHeight = mode.nHeight;
    }

    auto pGetFront = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(pVtbl[nGetFrontBufferSlot]);
    if (FAILED(pGetFront(pDevice, pFrontSurface)))
        return false;

    // Windowed: the client area in screen coordinates.
    RECT src = { 0, 0, static_cast<LONG>(mode.nWidth), static_cast<LONG>(mode.nHeight) };
    if (bWindowedMode)
    {
        POINT origin{};
        if (!hPresentWindow || !GetClientRect(hPresentWindow, &src) || !ClientToScreen(hPresentWindow, &origin))
            return false;

        OffsetRect(&src, origin.x, origin.y);
        src.left = std::clamp<LONG>(src.left, 0, static_cast<LONG>(mode.nWidth));
        src.right = std::clamp<LONG>(src.right, src.left, static_cast<LONG>(mode.nWidth));
        src.top = std::clamp<LONG>(src.top, 0, static_cast<LONG>(mode.nHeight));
        src.bottom = std::clamp<LONG>(src.bottom, src.top, static_cast<LONG>(mode.nHeight));
    }

    const auto nCopyX = std::min<int32_t>(nDestX, src.right - src.left);
    const auto nCopyY = std::min<int32_t>(nDestY, src.bottom - src.top);
    if (nCopyX < 1 || nCopyY < 1)
        return false;

    D3DLOCKED_RECT8 locked{};
    auto pSurfVtbl = *reinterpret_cast<void***>(pFrontSurface);
    auto pLock = reinterpret_cast<HRESULT(__stdcall*)(void*, D3DLOCKED_RECT8*, const RECT*, uint32_t)>(pSurfVtbl[nLockRectSlot]);
    if (FAILED(pLock(pFrontSurface, &locked, &src, D3DLOCK_READONLY)) || !locked.pBits)
        return false;

    // Destination pitch is the viewport SizeX.
    for (int32_t nRow = 0; nRow < nCopyY; ++nRow)
        std::memcpy(pDest + nRow * nDestX, static_cast<uint8_t*>(locked.pBits) + nRow * locked.nPitch, static_cast<size_t>(nCopyX) * 4);

    reinterpret_cast<HRESULT(__stdcall*)(void*)>(pSurfVtbl[nUnlockRectSlot])(pFrontSurface);
    return true;
}

// virtual void UD3DRenderDevice::ReadPixels(UViewport*, FColor*), __thiscall.
static void __fastcall ReadPixels(uint8_t* pThis, void*, uint8_t* pViewport, uint32_t* pPixels)
{
    if (nLastApplied <= 0)
    {
        shReadPixels.fastcall<void>(pThis, nullptr, pViewport, pPixels);
        return;
    }

    // Present calls this with both arguments null every frame. Stock locks the back buffer and
    // copies nothing, so skipping it only skips a lock that would fail.
    if (!pViewport || !pPixels)
        return;

    // Never fall through to the original while MSAA is on, that lock is the crash.
    if (!CaptureFrontBuffer(pThis, pViewport, pPixels) && !bFailed.exchange(true))
        LogWarn("MSAA: the front buffer readback failed, MSAA goes off at the next device reset");
}

static HRESULT __stdcall Reset(void* pDevice, D3DPRESENT_PARAMETERS8* pParams)
{
    ReleaseSurface();
    ApplyMSAA(pParams);
    return vmReset.stdcall<HRESULT>(pDevice, pParams);
}

static HRESULT __stdcall CreateDevice(void* pThis, uint32_t nAdapterIn, uint32_t nDeviceTypeIn, HWND hFocusWindow,
    uint32_t nBehaviorFlags, D3DPRESENT_PARAMETERS8* pParams, void** ppReturnedDeviceInterface)
{
    ReleaseSurface();

    nAdapter = nAdapterIn;
    nDeviceType = nDeviceTypeIn;
    if (hFocusWindow)
        hPresentWindow = hFocusWindow;

    ApplyMSAA(pParams);

    const auto hr = vmCreateDevice.stdcall<HRESULT>(pThis, nAdapterIn, nDeviceTypeIn, hFocusWindow, nBehaviorFlags, pParams, ppReturnedDeviceInterface);

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface)
    {
        vmReset = {};
        vmtDevice = {};
        vmtDevice = safetyhook::create_vmt(*ppReturnedDeviceInterface);
        vmReset = safetyhook::create_vm(vmtDevice, nResetSlot, Reset);
    }

    return hr;
}

static void* __stdcall Direct3DCreate8(uint32_t nSDKVersion)
{
    auto pResult = shDirect3DCreate8.stdcall<void*>(nSDKVersion);

    if (pResult && pResult != pD3D)
    {
        pD3D = pResult;
        vmCreateDevice = {};
        vmtD3D = {};
        vmtD3D = safetyhook::create_vmt(pD3D);
        vmCreateDevice = safetyhook::create_vm(vmtD3D, nCreateDeviceSlot, CreateDevice);
    }

    return pResult;
}

static void InitD3D8()
{
    // Exact decorated name, never a fuzzy match. The mangling proves the signature and that a
    // thiscall hook is right.
    auto hD3DDrv = GetModuleHandleW(L"D3DDrv.dll");
    auto pReadPixels = hD3DDrv ? GetProcAddress(hD3DDrv, "?ReadPixels@UD3DRenderDevice@@UAEXPAVUViewport@@PAVFColor@@@Z") : nullptr;

    if (!pReadPixels)
    {
        LogWarn("MSAA: D3DDrv.dll did not export ReadPixels, MSAA is off");
        return;
    }

    shReadPixels = safetyhook::create_inline(pReadPixels, ReadPixels);
    if (!shReadPixels)
    {
        LogWarn("MSAA: ReadPixels could not be hooked, MSAA is off");
        return;
    }

    auto hD3D8 = LoadLibraryW(L"d3d8.dll");
    auto pCreate = hD3D8 ? GetProcAddress(hD3D8, "Direct3DCreate8") : nullptr;

    if (!pCreate)
    {
        LogWarn("MSAA: d3d8.dll did not export Direct3DCreate8, MSAA is off");
        return;
    }

    shDirect3DCreate8 = safetyhook::create_inline(pCreate, Direct3DCreate8);
    if (!shDirect3DCreate8)
    {
        LogWarn("MSAA: Direct3DCreate8 could not be hooked, MSAA is off");
        return;
    }

    bReadPixelsHooked = true;

    // A change is picked up at the next device reset, nothing here forces one.
    BindInt(nMSAASamples, PREF_MSAA);
}

class MSAA
{
public:
    MSAA()
    {
        MongooseFix::onD3DDrvInitEvent() += []() { InitD3D8(); };
    }
} MSAA;
