module;

#include <common.hxx>

export module internalres;

import common;
import settings;
import logging;

// Exclusive fullscreen only takes a back buffer that is a real display mode, so the internal size
// cannot live in the swap chain. SetRes runs untouched at the output size and caches the device
// render target and depth in two of its own fields; the renderer binds from those two every frame,
// so swapping them for a target of our own moves the whole frame with no hook at all. A fullscreen
// textured quad scales it back into the real back buffer just before the game presents.
//
// UD3DRenderDevice, D3DDrv.dll:
//   +0x658 SizeX  +0x65C SizeY   present parameters, and what the effect buffers size themselves to
//   +0x66C IDirect3DDevice8*
//   +0x148C bound render target  +0x1490 bound depth stencil, filled by GetRenderTarget /
//           GetDepthStencilSurface at the end of SetRes, Released at the top of the next one
static constexpr auto nOffsetSizeX = 0x658;
static constexpr auto nOffsetSizeY = 0x65C;

// UViewport SizeX/SizeY: the render interface takes the bound target extent from these.
static constexpr auto nOffsetViewportSizeX = 0x88;
static constexpr auto nOffsetViewportSizeY = 0x8C;
static constexpr auto nOffsetDevice = 0x66C;
static constexpr auto nOffsetBoundTarget = 0x148C;
static constexpr auto nOffsetBoundDepth = 0x1490;

// IDirect3DDevice8 vtable slots.
static constexpr auto nReleaseSlot = 2;
static constexpr auto nAddRefSlot = 1;
static constexpr auto nCreateTextureSlot = 20;
static constexpr auto nCreateDepthStencilSurfaceSlot = 26;
static constexpr auto nSetRenderTargetSlot = 31;
static constexpr auto nBeginSceneSlot = 34;
static constexpr auto nEndSceneSlot = 35;
static constexpr auto nClearSlot = 36;
static constexpr auto nSetRenderStateSlot = 50;
static constexpr auto nApplyStateBlockSlot = 54;
static constexpr auto nCaptureStateBlockSlot = 55;
static constexpr auto nDeleteStateBlockSlot = 56;
static constexpr auto nCreateStateBlockSlot = 57;
static constexpr auto nSetTextureSlot = 61;
static constexpr auto nSetTextureStageStateSlot = 63;
static constexpr auto nDrawPrimitiveUPSlot = 72;
static constexpr auto nSetVertexShaderSlot = 76;
static constexpr auto nSetIndicesSlot = 85;
static constexpr auto nSetPixelShaderSlot = 88;

// IDirect3DSurface8::GetDesc, IDirect3DTexture8::GetSurfaceLevel.
static constexpr auto nGetDescSlot = 8;
static constexpr auto nGetSurfaceLevelSlot = 15;

static constexpr uint32_t D3DMULTISAMPLE_NONE = 0;
static constexpr uint32_t D3DPOOL_DEFAULT = 0;
static constexpr uint32_t D3DUSAGE_RENDERTARGET = 1;
static constexpr uint32_t D3DFMT_D24S8 = 75;
static constexpr uint32_t D3DSBT_ALL = 1;
static constexpr uint32_t D3DPT_TRIANGLESTRIP = 5;
static constexpr uint32_t D3DFVF_XYZRHW_TEX1 = 0x104;

static constexpr uint32_t D3DRS_ZENABLE = 7;
static constexpr uint32_t D3DRS_ZWRITEENABLE = 14;
static constexpr uint32_t D3DRS_ALPHATESTENABLE = 15;
static constexpr uint32_t D3DRS_CULLMODE = 22;
static constexpr uint32_t D3DRS_DITHERENABLE = 26;
static constexpr uint32_t D3DRS_ALPHABLENDENABLE = 27;
static constexpr uint32_t D3DRS_FOGENABLE = 28;
static constexpr uint32_t D3DRS_SPECULARENABLE = 29;
static constexpr uint32_t D3DRS_STENCILENABLE = 52;
static constexpr uint32_t D3DRS_WRAP0 = 128;
static constexpr uint32_t D3DRS_CLIPPING = 136;
static constexpr uint32_t D3DRS_LIGHTING = 137;
static constexpr uint32_t D3DRS_COLORWRITEENABLE = 168;
static constexpr uint32_t D3DCLEAR_TARGET = 1;
static constexpr uint32_t D3DCULL_NONE = 1;

static constexpr uint32_t D3DTSS_COLOROP = 1;
static constexpr uint32_t D3DTSS_COLORARG1 = 2;
static constexpr uint32_t D3DTSS_ALPHAOP = 4;
static constexpr uint32_t D3DTSS_ALPHAARG1 = 5;
static constexpr uint32_t D3DTSS_TEXCOORDINDEX = 11;
static constexpr uint32_t D3DTSS_ADDRESSU = 13;
static constexpr uint32_t D3DTSS_ADDRESSV = 14;
static constexpr uint32_t D3DTSS_MAGFILTER = 16;
static constexpr uint32_t D3DTSS_MINFILTER = 17;
static constexpr uint32_t D3DTSS_MIPFILTER = 18;
static constexpr uint32_t D3DTSS_TEXTURETRANSFORMFLAGS = 24;
static constexpr uint32_t D3DTOP_DISABLE = 1;
static constexpr uint32_t D3DTOP_SELECTARG1 = 2;
static constexpr uint32_t D3DTA_TEXTURE = 2;
static constexpr uint32_t D3DTEXF_NONE = 0;
static constexpr uint32_t D3DTEXF_POINT = 1;
static constexpr uint32_t D3DTEXF_LINEAR = 2;
static constexpr uint32_t D3DTADDRESS_CLAMP = 3;

struct D3DSURFACE_DESC8
{
    uint32_t nFormat, nType, nUsage, nPool, nSize, nMultiSampleType, nWidth, nHeight;
};

struct ScreenVertex
{
    float x, y, z, rhw, u, v;
};

// FAILED and SUCCEEDED are macros, and the comma in Call<slot, HRESULT> splits their argument.
static bool Failed(HRESULT hr) { return hr < 0; }
static bool Ok(HRESULT hr) { return hr >= 0; }

template<size_t nSlot, typename R, typename... Args>
static R Call(void* pThis, Args... args)
{
    return reinterpret_cast<R(__stdcall*)(void*, Args...)>((*reinterpret_cast<void***>(pThis))[nSlot])(pThis, args...);
}

static void ReleaseCom(void*& pObject)
{
    if (pObject)
        Call<nReleaseSlot, uint32_t>(pObject);

    pObject = nullptr;
}

static std::atomic<int> nInternalResX = 0;
static std::atomic<int> nInternalResY = 0;
static std::atomic<int> nScalingFilter = 1;

static void* pDevice = nullptr;
static void* pTexture = nullptr;
static void* pTargetSurface = nullptr;
static void* pTargetDepth = nullptr;

// Borrowed: the game holds the reference and Releases it through the field.
static void* pOriginalTarget = nullptr;
static void* pOriginalDepth = nullptr;

static uint32_t nStateBlock = 0;
static uint8_t* pStampedRenderDevice = nullptr;
static uint8_t* pStampedViewport = nullptr;
static int nOutputX = 0;
static int nOutputY = 0;
static int nRenderX = 0;
static int nRenderY = 0;
static bool bInstalled = false;
static bool bActive = false;
static bool bWarned = false;

// One line per reason, however many resets it survives.
static void WarnOnce(std::string_view sReason)
{
    if (!std::exchange(bWarned, true))
        LogWarn("InternalRes: {}, scaling is off", sReason);
}

// Puts the render device back exactly as SetRes left it and drops everything we made. Target and
// depth are D3DPOOL_DEFAULT, so none of it may still be alive at the next Reset.
static void Teardown()
{
    if (pStampedRenderDevice)
    {
        if (bInstalled)
        {
            *reinterpret_cast<void**>(pStampedRenderDevice + nOffsetBoundTarget) = pOriginalTarget;
            *reinterpret_cast<void**>(pStampedRenderDevice + nOffsetBoundDepth) = pOriginalDepth;
        }

        *reinterpret_cast<int*>(pStampedRenderDevice + nOffsetSizeX) = nOutputX;
        *reinterpret_cast<int*>(pStampedRenderDevice + nOffsetSizeY) = nOutputY;

        if (pStampedViewport)
        {
            *reinterpret_cast<int*>(pStampedViewport + nOffsetViewportSizeX) = nOutputX;
            *reinterpret_cast<int*>(pStampedViewport + nOffsetViewportSizeY) = nOutputY;
        }
    }

    // Drop the reference the game would have Released through the field, now it holds neither.
    if (std::exchange(bInstalled, false))
    {
        if (pTargetSurface)
            Call<nReleaseSlot, uint32_t>(pTargetSurface);
        if (pTargetDepth)
            Call<nReleaseSlot, uint32_t>(pTargetDepth);
    }

    if (pDevice && nStateBlock)
        Call<nDeleteStateBlockSlot, HRESULT>(pDevice, nStateBlock);

    nStateBlock = 0;
    ReleaseCom(pTargetDepth);
    ReleaseCom(pTargetSurface);
    ReleaseCom(pTexture);

    pOriginalTarget = nullptr;
    pOriginalDepth = nullptr;
    pStampedRenderDevice = nullptr;
    pStampedViewport = nullptr;
    bActive = false;
}

static void DrawQuad(void* pDev)
{
    static constexpr uint32_t anRenderState[][2] =
    {
        { D3DRS_ZENABLE, 0 }, { D3DRS_ZWRITEENABLE, 0 }, { D3DRS_STENCILENABLE, 0 },
        { D3DRS_ALPHABLENDENABLE, 0 }, { D3DRS_ALPHATESTENABLE, 0 }, { D3DRS_FOGENABLE, 0 },
        { D3DRS_SPECULARENABLE, 0 }, { D3DRS_LIGHTING, 0 }, { D3DRS_DITHERENABLE, 0 },
        { D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_CLIPPING, 1 }, { D3DRS_WRAP0, 0 },
        { D3DRS_COLORWRITEENABLE, 0xF },
    };

    for (const auto& state : anRenderState)
        Call<nSetRenderStateSlot, HRESULT>(pDev, state[0], state[1]);

    // D3D8 keeps the sampler states in the texture stage.
    const auto nFilter = nScalingFilter.load() == 0 ? D3DTEXF_POINT : D3DTEXF_LINEAR;

    const uint32_t anStageState[][3] =
    {
        { 0, D3DTSS_MINFILTER, nFilter }, { 0, D3DTSS_MAGFILTER, nFilter },
        { 0, D3DTSS_MIPFILTER, D3DTEXF_NONE },
        { 0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP }, { 0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP },
        { 0, D3DTSS_TEXCOORDINDEX, 0 }, { 0, D3DTSS_TEXTURETRANSFORMFLAGS, 0 },
        { 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_COLORARG1, D3DTA_TEXTURE },
        { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE },
        { 1, D3DTSS_COLOROP, D3DTOP_DISABLE }, { 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE },
    };

    for (const auto& state : anStageState)
        Call<nSetTextureStageStateSlot, HRESULT>(pDev, state[0], state[1], state[2]);

    Call<nSetTextureSlot, HRESULT>(pDev, 0u, pTexture);
    Call<nSetVertexShaderSlot, HRESULT>(pDev, D3DFVF_XYZRHW_TEX1);
    Call<nSetPixelShaderSlot, HRESULT>(pDev, 0u);
    Call<nSetIndicesSlot, HRESULT>(pDev, static_cast<void*>(nullptr), 0u);

    // The internal frame keeps its own shape inside the output; the rest is bars.
    auto nFitX = 0, nFitY = 0, nFitW = nOutputX, nFitH = nOutputY;
    if (static_cast<int64_t>(nRenderX) * nOutputY != static_cast<int64_t>(nOutputX) * nRenderY)
    {
        if (static_cast<int64_t>(nRenderX) * nOutputY > static_cast<int64_t>(nOutputX) * nRenderY)
            nFitH = (std::max)(1, static_cast<int>((static_cast<int64_t>(nOutputX) * nRenderY + nRenderX / 2) / nRenderX));
        else
            nFitW = (std::max)(1, static_cast<int>((static_cast<int64_t>(nOutputY) * nRenderX + nRenderY / 2) / nRenderY));

        nFitX = (nOutputX - nFitW) / 2;
        nFitY = (nOutputY - nFitH) / 2;

        // DISCARD leaves whatever the driver last had in the bars.
        Call<nClearSlot, HRESULT>(pDev, 0u, static_cast<const void*>(nullptr), D3DCLEAR_TARGET, 0xFF000000u, 1.0f, 0u);
    }

    // The half texel the rasteriser puts between a pixel centre and a texel centre.
    const auto fLeft = static_cast<float>(nFitX) - 0.5f;
    const auto fTop = static_cast<float>(nFitY) - 0.5f;
    const auto fRight = static_cast<float>(nFitX + nFitW) - 0.5f;
    const auto fBottom = static_cast<float>(nFitY + nFitH) - 0.5f;

    const ScreenVertex aVertices[4] =
    {
        { fLeft,  fTop,    0.0f, 1.0f, 0.0f, 0.0f },
        { fRight, fTop,    0.0f, 1.0f, 1.0f, 0.0f },
        { fLeft,  fBottom, 0.0f, 1.0f, 0.0f, 1.0f },
        { fRight, fBottom, 0.0f, 1.0f, 1.0f, 1.0f },
    };

    Call<nDrawPrimitiveUPSlot, HRESULT>(pDev, D3DPT_TRIANGLESTRIP, 2u,
        static_cast<const void*>(aVertices), static_cast<uint32_t>(sizeof(ScreenVertex)));

    // The next frame renders into what is left bound here.
    Call<nSetTextureSlot, HRESULT>(pDev, 0u, static_cast<void*>(nullptr));
}

// Called by display inside its UD3DRenderDevice::Present hook, before the original runs.
export void PresentInternalRes(uint8_t* pRenderDevice)
{
    if (!bActive || !pDevice || pRenderDevice != pStampedRenderDevice)
        return;

    Call<nCaptureStateBlockSlot, HRESULT>(pDevice, nStateBlock);

    if (Ok(Call<nSetRenderTargetSlot, HRESULT>(pDevice, pOriginalTarget, pOriginalDepth))
        && Ok(Call<nBeginSceneSlot, HRESULT>(pDevice)))
    {
        DrawQuad(pDevice);
        Call<nEndSceneSlot, HRESULT>(pDevice);
    }

    Call<nApplyStateBlockSlot, HRESULT>(pDevice, nStateBlock);

    // Lock sets the viewport to the internal size before the render interface binds anything, so
    // the bigger target must stay current between frames or SetViewport fails.
    Call<nSetRenderTargetSlot, HRESULT>(pDevice, pTargetSurface, pTargetDepth);
}

// Called by display just before the original SetRes: everything held here is D3DPOOL_DEFAULT and
// SetRes resets the device.
export void ReleaseInternalRes()
{
    Teardown();
}

// Called by display once the original SetRes returned, before it reads SizeX/SizeY back.
export void ApplyInternalRes(uint8_t* pRenderDevice, uint8_t* pViewport)
{
    auto pNewDevice = pRenderDevice ? *reinterpret_cast<void**>(pRenderDevice + nOffsetDevice) : nullptr;

    // A recreate took the old surfaces with it, nothing left to release.
    if (pNewDevice != pDevice)
    {
        pTexture = pTargetSurface = pTargetDepth = nullptr;
        pOriginalTarget = pOriginalDepth = nullptr;
        pStampedRenderDevice = nullptr;
        pStampedViewport = nullptr;
        nStateBlock = 0;
        bInstalled = false;
        bActive = false;
    }

    Teardown();
    pDevice = pNewDevice;

    if (!pRenderDevice || !pDevice)
        return;

    const auto nWantX = nInternalResX.load();
    const auto nWantY = nInternalResY.load();

    nOutputX = *reinterpret_cast<int*>(pRenderDevice + nOffsetSizeX);
    nOutputY = *reinterpret_cast<int*>(pRenderDevice + nOffsetSizeY);

    // 0 on either axis is off; matching the output is off by another name.
    if (nWantX < 1 || nWantY < 1 || nOutputX < 1 || nOutputY < 1)
        return;
    if (nWantX == nOutputX && nWantY == nOutputY)
        return;

    auto pBackBuffer = *reinterpret_cast<void**>(pRenderDevice + nOffsetBoundTarget);
    if (!pBackBuffer)
    {
        WarnOnce("SetRes left no render target to scale into");
        return;
    }

    D3DSURFACE_DESC8 back{};
    if (Failed(Call<nGetDescSlot, HRESULT>(pBackBuffer, &back)))
    {
        WarnOnce("the back buffer would not describe itself");
        return;
    }

    // A multisampled surface cannot be sampled as a texture and D3D8 has nothing to resolve it
    // with, so the two features cannot both be on.
    if (back.nMultiSampleType != D3DMULTISAMPLE_NONE)
    {
        WarnOnce("the back buffer is multisampled, which rules out a scaling pass");
        return;
    }

    // The game depth format, so the substitute matches what it renders against.
    auto nDepthFormat = D3DFMT_D24S8;
    auto pGameDepth = *reinterpret_cast<void**>(pRenderDevice + nOffsetBoundDepth);
    if (pGameDepth)
    {
        D3DSURFACE_DESC8 depth{};
        if (Ok(Call<nGetDescSlot, HRESULT>(pGameDepth, &depth)))
            nDepthFormat = depth.nFormat;
    }

    if (Failed(Call<nCreateTextureSlot, HRESULT>(pDevice, static_cast<uint32_t>(nWantX), static_cast<uint32_t>(nWantY),
            1u, D3DUSAGE_RENDERTARGET, back.nFormat, D3DPOOL_DEFAULT, &pTexture)) || !pTexture)
    {
        LogWarn("InternalRes: the driver refused a {}x{} render target, scaling is off", nWantX, nWantY);
        Teardown();
        return;
    }

    if (Failed(Call<nGetSurfaceLevelSlot, HRESULT>(pTexture, 0u, &pTargetSurface)) || !pTargetSurface
        || Failed(Call<nCreateDepthStencilSurfaceSlot, HRESULT>(pDevice, static_cast<uint32_t>(nWantX), static_cast<uint32_t>(nWantY),
            nDepthFormat, D3DMULTISAMPLE_NONE, &pTargetDepth)) || !pTargetDepth)
    {
        LogWarn("InternalRes: the driver refused a {}x{} depth stencil, scaling is off", nWantX, nWantY);
        Teardown();
        return;
    }

    // The renderer caches its own state, so the scaling pass hands back everything it touches.
    if (Failed(Call<nCreateStateBlockSlot, HRESULT>(pDevice, D3DSBT_ALL, &nStateBlock)) || !nStateBlock)
    {
        nStateBlock = 0;
        WarnOnce("the device would not give up a state block");
        Teardown();
        return;
    }

    pStampedRenderDevice = pRenderDevice;
    pStampedViewport = pViewport;
    nRenderX = nWantX;
    nRenderY = nWantY;
    pOriginalTarget = pBackBuffer;
    pOriginalDepth = pGameDepth;

    // The game Releases whatever these fields hold at the top of the next SetRes.
    Call<nAddRefSlot, uint32_t>(pTargetSurface);
    Call<nAddRefSlot, uint32_t>(pTargetDepth);

    *reinterpret_cast<void**>(pRenderDevice + nOffsetBoundTarget) = pTargetSurface;
    *reinterpret_cast<void**>(pRenderDevice + nOffsetBoundDepth) = pTargetDepth;
    bInstalled = true;

    *reinterpret_cast<int*>(pRenderDevice + nOffsetSizeX) = nWantX;
    *reinterpret_cast<int*>(pRenderDevice + nOffsetSizeY) = nWantY;

    // The render interface sizes the bound target from the viewport, not the device.
    if (pViewport)
    {
        *reinterpret_cast<int*>(pViewport + nOffsetViewportSizeX) = nWantX;
        *reinterpret_cast<int*>(pViewport + nOffsetViewportSizeY) = nWantY;
    }
    bActive = true;

    Call<nSetRenderTargetSlot, HRESULT>(pDevice, pTargetSurface, pTargetDepth);

    LogInfo("InternalRes: {}x{} scaled to {}x{}, {} filter", nWantX, nWantY, nOutputX, nOutputY,
        nScalingFilter.load() == 0 ? "point" : "bilinear");
}

class InternalRes
{
public:
    InternalRes()
    {
        // A new size lands on the next device reset, which the ini watcher already asks for.
        MongooseFix::onInitEvent() += []()
        {
            BindInt(nInternalResX, PREF_INTERNALRESX);
            BindInt(nInternalResY, PREF_INTERNALRESY);
            BindInt(nScalingFilter, PREF_SCALINGFILTER);
        };

        MongooseFix::onShutdownEvent() += []() { Teardown(); };
    }
} InternalRes;
