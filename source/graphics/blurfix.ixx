module;

#include <common.hxx>

export module blurfix;

import common;
import settings;
import logging;

// Blur and sharpen render the frame into an offscreen target, blur it, stretch it back. D3DDrv
// creates that target flat 512x512, one buffer pixel per five screen pixels at 2560x1440, hence
// the block edges in flashbacks. Three immediates decide it:
//
//   CreateTexture(512, 512, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &colour)
//   CreateDepthStencilSurface(512, 512, D3DFMT_D24S8, 0, &depth)
//   SizeX = SizeY = 512, written into the render interface as the bound target size.
//
// Flashback glow is a fourth immediate: frame downsampled into a flat 128x128 texture, stretched
// back over itself. Taps normalised, so spread is the same at any size and only detail is lost -
// follows the back buffer too.
//
//   FD3DRenderInterface  +0x004 UD3DRenderDevice*  +0x028 SizeX of bound target  +0x02C SizeY
//   UD3DRenderDevice     +0x658 back buffer SizeX  +0x65C SizeY  +0x1494 effect colour texture
static constexpr auto nAuthoredBufferSize = 512;
static constexpr auto nOffsetRenderDevice = 0x4;
static constexpr auto nOffsetTargetSizeX = 0x28;
static constexpr auto nOffsetTargetSizeY = 0x2C;
static constexpr auto nOffsetBackBufferSizeX = 0x658;
static constexpr auto nOffsetBackBufferSizeY = 0x65C;
static constexpr auto nOffsetEffectTexture = 0x1494;
static constexpr auto nOffsetGlowTexture = 0x1498;

static SafetyHookInline shCreateEffectBuffers{};
static SafetyHookInline shBindEffectTarget{};

// Width/height operands of the colour, depth and glow create calls.
static std::array<std::pair<int32_t*, int32_t*>, 3> apBufferSize{};

static std::atomic<int> nEffectWidth = nAuthoredBufferSize;
static std::atomic<int> nEffectHeight = nAuthoredBufferSize;

static void SetBufferSize(int nWidth, int nHeight)
{
    for (auto& [pWidth, pHeight] : apBufferSize)
    {
        injector::WriteMemory(pWidth, nWidth, true);
        injector::WriteMemory(pHeight, nHeight, true);
    }

    nEffectWidth = nWidth;
    nEffectHeight = nHeight;
}

// From SetRes, once the device exists and the back buffer size is settled.
static void __fastcall CreateEffectBuffers(uint8_t* pThis, void*)
{
    auto pRenderDevice = *reinterpret_cast<uint8_t**>(pThis + nOffsetRenderDevice);

    auto nWidth = *reinterpret_cast<int32_t*>(pRenderDevice + nOffsetBackBufferSizeX);
    auto nHeight = *reinterpret_cast<int32_t*>(pRenderDevice + nOffsetBackBufferSizeY);

    // Runs inside SetRes, before the internal size is stamped over SizeX/SizeY, so the setting is
    // the only source. The effect pass follows the frame, not the window.
    const auto nInternalX = MongooseFixSettings.GetInt(PREF_INTERNALRESX);
    const auto nInternalY = MongooseFixSettings.GetInt(PREF_INTERNALRESY);
    if (nInternalX > 0 && nInternalY > 0)
    {
        nWidth = nInternalX;
        nHeight = nInternalY;
    }

    SetBufferSize(nWidth, nHeight);
    shCreateEffectBuffers.thiscall<void>(pThis);

    // Non-power-of-two render targets are optional under D3D8 and the game never checks what
    // CreateTexture returned, and a refusal would crash on the first flashback instead of here.
    if (*reinterpret_cast<void**>(pRenderDevice + nOffsetEffectTexture) == nullptr ||
        *reinterpret_cast<void**>(pRenderDevice + nOffsetGlowTexture) == nullptr)
    {
        LogWarn("BlurScale: the driver refused a {}x{} effect buffer, back to {}x{}",
            nWidth, nHeight, nAuthoredBufferSize, nAuthoredBufferSize);

        SetBufferSize(nAuthoredBufferSize, nAuthoredBufferSize);
        shCreateEffectBuffers.thiscall<void>(pThis);
        return;
    }

    LogInfo("BlurScale: effect buffer {}x{}", nWidth, nHeight);
}

// The two stores are one instruction apart and share a register: no room to patch in place.
static void __fastcall BindEffectTarget(uint8_t* pThis, void*)
{
    shBindEffectTarget.thiscall<void>(pThis);

    *reinterpret_cast<int32_t*>(pThis + nOffsetTargetSizeX) = nEffectWidth.load();
    *reinterpret_cast<int32_t*>(pThis + nOffsetTargetSizeY) = nEffectHeight.load();
}

static void InitD3DDrv()
{
    // PUSH 0x200 / PUSH 0x200 / PUSH ESI / CALL [ECX+0x50]: height, width, device, then
    // IDirect3DDevice8::CreateTexture. Depth surface is the same shape one slot along at
    // CALL [ECX+0x68], IDirect3DDevice8::CreateDepthStencilSurface.
    auto patternColour = module_pattern(L"D3DDrv.dll", "68 00 02 00 00 68 00 02 00 00 56 FF 51 50");
    auto patternDepth = module_pattern(L"D3DDrv.dll", "68 00 02 00 00 68 00 02 00 00 56 FF 51 68");

    // Same shape, one CreateTexture along: the 128x128 glow buffer.
    auto patternGlow = module_pattern(L"D3DDrv.dll", "68 80 00 00 00 68 80 00 00 00 56 FF 52 50");

    // The functions the operands live in, matched at their prologues rather than walked back to.
    auto patternCreate = module_pattern(L"D3DDrv.dll", "56 57 8B F9 8B 47 04 8B B0 6C 06 00 00 8B 0E 05 94 14 00 00");
    auto patternBind = module_pattern(L"D3DDrv.dll", "56 8B F1 8B 46 04 8B 88 6C 06 00 00 8B 11 57 8B B8 AC 14 00 00");

    if (patternColour.empty() || patternDepth.empty() || patternGlow.empty()
        || patternCreate.empty() || patternBind.empty())
    {
        LogWarn("BlurScale: effect buffer patterns not found, the blur stays at 512x512");
        return;
    }

    // One byte of PUSH, then the operand.
    apBufferSize[0] = { patternColour.get_first<int32_t>(6), patternColour.get_first<int32_t>(1) };
    apBufferSize[1] = { patternDepth.get_first<int32_t>(6), patternDepth.get_first<int32_t>(1) };
    apBufferSize[2] = { patternGlow.get_first<int32_t>(6), patternGlow.get_first<int32_t>(1) };

    shCreateEffectBuffers = safetyhook::create_inline(patternCreate.get_first(), CreateEffectBuffers);
    shBindEffectTarget = safetyhook::create_inline(patternBind.get_first(), BindEffectTarget);
}

class BlurFix
{
public:
    BlurFix()
    {
        MongooseFix::onD3DDrvInitEvent() += []() { InitD3DDrv(); };
    }
} BlurFix;
