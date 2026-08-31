module;

#include <common.hxx>
#include <d3d8to9/d3dx9.hpp>

export module crtgamma;

import common;
import settings;
import logging;

// Vibrance leans on undersaturated pixels harder than vivid ones, then a smootherstep on luma adds
// contrast without clipping either end. Together they put back the bite a CRT gave a frame authored
// for one.
//
// The vibrance and curve maths is CeeJayDK's, from the SweetFX shaders, reimplemented here as
// ps_2_0. No SweetFX source is compiled into this project. Its licence:
//
//   MIT License
//
//   Copyright (c) 2014 CeeJay.dk
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//   and associated documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy, modify, merge, publish,
//   distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all copies or
//   substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
//   BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Direct3D 8 has no shader model the curve fits in, so the pass needs DirectXVersion 1. d3d8to9
// hands back its real device through QueryInterface and stock d3d8.dll refuses, which is the test.

// UD3DRenderDevice, D3DDrv.dll: +0x66C IDirect3DDevice8*.
static constexpr auto nOffsetDevice = 0x66C;

// ps_2_0 reads one constant register per instruction, so everything an instruction needs shares a
// register: c0 luma weights, c1 the smootherstep constants, c2 contrast, vibrance and 1.0.
static const char* szShader = R"(
    ps_2_0
    dcl t0.xy
    dcl_2d s0
    def c0, 0.212656, 0.715158, 0.072186, 0.0
    def c1, 0.0, 6.0, -15.0, 10.0
    def c2, 0.225, 0.15, 1.0, 0.0

    texld r0, t0, s0

    max r1.x, r0.x, r0.y
    max r1.x, r1.x, r0.z
    min r1.y, r0.x, r0.y
    min r1.y, r1.y, r0.z
    sub r1.z, r1.x, r1.y
    sub r1.z, c2.z, r1.z
    mad r1.z, r1.z, c2.y, c2.z

    dp3 r2.x, r0, c0
    sub r3, r0, r2.x
    mad r0, r3, r1.z, r2.x

    dp3 r2.x, r0, c0
    mad r2.y, r2.x, c1.y, c1.z
    mad r2.y, r2.y, r2.x, c1.w
    mul r2.z, r2.x, r2.x
    mul r2.z, r2.z, r2.x
    mul r2.y, r2.y, r2.z
    sub r2.y, r2.y, r2.x
    mul r2.y, r2.y, c2.x
    add r0.xyz, r0, r2.y

    mov r0.w, c2.z
    mov oC0, r0
)";

struct ScreenVertex
{
    float x, y, z, rhw, u, v;
};

static std::atomic<bool> bEnabled = false;
static bool bFailed = false;

// Borrowed: the wrapper owns the device, this only remembers which one it built against.
static IDirect3DDevice9* pDevice = nullptr;
static IDirect3DPixelShader9* pShader = nullptr;
static IDirect3DTexture9* pCopy = nullptr;
static IDirect3DSurface9* pCopySurface = nullptr;
static IDirect3DStateBlock9* pStateBlock = nullptr;
static UINT nWidth = 0;
static UINT nHeight = 0;

template<class T>
static void ReleaseCom(T*& pObject)
{
    if (pObject)
        pObject->Release();

    pObject = nullptr;
}

// The copy target is D3DPOOL_DEFAULT, so none of it may be alive at the next Reset.
export void ReleaseCRTGamma()
{
    ReleaseCom(pStateBlock);
    ReleaseCom(pCopySurface);
    ReleaseCom(pCopy);
    ReleaseCom(pShader);

    pDevice = nullptr;
    nWidth = 0;
    nHeight = 0;
}

static bool Fail(const char* szReason)
{
    ReleaseCRTGamma();
    bFailed = true;
    LogWarn("CRT Gamma: {}, the pass is off", szReason);
    return false;
}

static bool Build(IDirect3DDevice9* pDev, const D3DSURFACE_DESC& back)
{
    ID3DXBuffer* pCode = nullptr;
    ID3DXBuffer* pError = nullptr;

    if (!D3DXAssembleShader || FAILED(D3DXAssembleShader(szShader, static_cast<UINT>(std::strlen(szShader)),
            nullptr, nullptr, D3DXASM_FLAGS, &pCode, &pError)) || !pCode)
    {
        if (pError)
            LogWarn("CRT Gamma: {}", static_cast<const char*>(pError->GetBufferPointer()));

        ReleaseCom(pError);
        ReleaseCom(pCode);
        return Fail("the shader would not assemble");
    }

    ReleaseCom(pError);

    const auto hr = pDev->CreatePixelShader(static_cast<const DWORD*>(pCode->GetBufferPointer()), &pShader);
    ReleaseCom(pCode);

    if (FAILED(hr) || !pShader)
        return Fail("the device refused a pixel shader, ps_2_0 is the minimum");

    if (FAILED(pDev->CreateTexture(back.Width, back.Height, 1, D3DUSAGE_RENDERTARGET, back.Format,
            D3DPOOL_DEFAULT, &pCopy, nullptr)) || !pCopy
        || FAILED(pCopy->GetSurfaceLevel(0, &pCopySurface)) || !pCopySurface)
    {
        return Fail("the driver refused a back buffer copy");
    }

    // The renderer caches its own state, so the pass hands back everything it touches.
    if (FAILED(pDev->CreateStateBlock(D3DSBT_ALL, &pStateBlock)) || !pStateBlock)
        return Fail("the device would not give up a state block");

    pDevice = pDev;
    nWidth = back.Width;
    nHeight = back.Height;

    LogInfo("CRT Gamma: on at {}x{}", nWidth, nHeight);
    return true;
}

static void DrawQuad(IDirect3DDevice9* pDev)
{
    pDev->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    pDev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    pDev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    pDev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    pDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    pDev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    pDev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    pDev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

    // One texel per pixel, so nothing filters.
    pDev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    pDev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    pDev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    pDev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    pDev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    pDev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    D3DVIEWPORT9 viewport{ 0, 0, nWidth, nHeight, 0.0f, 1.0f };
    pDev->SetViewport(&viewport);

    pDev->SetTexture(0, pCopy);
    pDev->SetPixelShader(pShader);
    pDev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

    // The half texel the rasteriser puts between pixel centre and texel centre.
    const auto fRight = static_cast<float>(nWidth) - 0.5f;
    const auto fBottom = static_cast<float>(nHeight) - 0.5f;

    const ScreenVertex aVertices[4]
    {
        { -0.5f,  -0.5f,   0.0f, 1.0f, 0.0f, 0.0f },
        { fRight, -0.5f,   0.0f, 1.0f, 1.0f, 0.0f },
        { -0.5f,  fBottom, 0.0f, 1.0f, 0.0f, 1.0f },
        { fRight, fBottom, 0.0f, 1.0f, 1.0f, 1.0f },
    };

    pDev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, aVertices, sizeof(ScreenVertex));

    pDev->SetTexture(0, nullptr);
    pDev->SetPixelShader(nullptr);
}

// Called by display inside its UD3DRenderDevice::Present hook, after the scaling pass, so the curve
// lands on the frame that is about to be shown rather than the internal target.
export void PresentCRTGamma(uint8_t* pRenderDevice)
{
    if (!bEnabled.load() || bFailed || !pRenderDevice)
        return;

    auto pWrapper = *reinterpret_cast<IUnknown**>(pRenderDevice + nOffsetDevice);
    if (!pWrapper)
        return;

    IDirect3DDevice9* pDev = nullptr;
    if (FAILED(pWrapper->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&pDev))) || !pDev)
    {
        Fail("the Direct3D 8 renderer has no shader model this fits in, set DirectXVersion to 1");
        return;
    }

    // The wrapper holds the reference the device lives on, this one only proved the path.
    pDev->Release();

    IDirect3DSurface9* pBack = nullptr;
    D3DSURFACE_DESC back{};

    if (FAILED(pDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBack)) || !pBack)
        return;

    if (FAILED(pBack->GetDesc(&back))
        || ((pDev != pDevice || back.Width != nWidth || back.Height != nHeight)
            && (ReleaseCRTGamma(), !Build(pDev, back))))
    {
        pBack->Release();
        return;
    }

    pStateBlock->Capture();

    IDirect3DSurface9* pOldTarget = nullptr;
    IDirect3DSurface9* pOldDepth = nullptr;
    pDev->GetRenderTarget(0, &pOldTarget);
    pDev->GetDepthStencilSurface(&pOldDepth);

    // StretchRect resolves a multisampled back buffer on the way into the copy, so the pass reads a
    // plain texture whatever MSAA is set to.
    if (SUCCEEDED(pDev->StretchRect(pBack, nullptr, pCopySurface, nullptr, D3DTEXF_NONE))
        && SUCCEEDED(pDev->SetRenderTarget(0, pBack)))
    {
        pDev->SetDepthStencilSurface(nullptr);

        if (SUCCEEDED(pDev->BeginScene()))
        {
            DrawQuad(pDev);
            pDev->EndScene();
        }
    }

    if (pOldTarget)
        pDev->SetRenderTarget(0, pOldTarget);

    pDev->SetDepthStencilSurface(pOldDepth);

    ReleaseCom(pOldDepth);
    ReleaseCom(pOldTarget);

    pStateBlock->Apply();
    pBack->Release();
}

class CRTGamma
{
public:
    CRTGamma()
    {
        MongooseFix::onInitEvent() += []() { BindBool(bEnabled, PREF_CRTGAMMA); };
        MongooseFix::onShutdownEvent() += []() { ReleaseCRTGamma(); };
    }
} CRTGamma;
