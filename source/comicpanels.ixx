module;

#include <common.hxx>

export module comicpanels;

import common;
import settings;
import display;
import logging;

// The comic art is authored against a 640x480 screen and never told about anything larger. Two
// pieces of that are native and provably wrong whatever the resolution is:
//
//   UCanvas::GetScreenHeight returns the literal 480 and never looks at the viewport, so every
//   piece of script that positions itself by asking how tall the screen is gets 480.
//
//   The HUD's icon scale is derived from the height of the string "999 " in the current font
//   rather than from the screen, so icons track the font and not the resolution.
//
// The onomatopoeia sprites themselves are drawn by script in Xiii.u, through UCanvas. Level 2
// scales the two canvas entry points script uses for sprites that carry their own scale factor,
// which is where those draws land. It is off by default because it also catches anything else
// drawn the same way.
static constexpr auto fAuthoredScreenHeight = 480.0f;
static constexpr auto nOffsetHudIconScale = 0x2BC;
static constexpr auto nOffsetCanvasClipX = 0x3C;
static constexpr auto nOffsetCanvasClipY = 0x40;

static std::atomic<int> nComicPanelScaling = 1;
static std::atomic<float> fComicPanelScaleOverride = 0.0f;

static SafetyHookMid mhScreenHeight{};
static SafetyHookMid mhHudIconScale{};
static SafetyHookInline shDrawTileScaled{};
static SafetyHookInline shDrawTileJustified{};

static float GetScaleFactor()
{
    const auto fOverride = fComicPanelScaleOverride.load();
    if (fOverride > 0.0f)
        return fOverride;

    const auto nHeight = nBackBufferHeight.load();
    return nHeight > 0 ? static_cast<float>(nHeight) / fAuthoredScreenHeight : 1.0f;
}

// EAX still holds the script return slot the hardcoded 480 was just written into.
static void ScreenHeight(SafetyHookContext& ctx)
{
    if (nComicPanelScaling.load() == 0)
        return;

    const auto nHeight = nBackBufferHeight.load();
    if (nHeight > 0)
        *reinterpret_cast<int32_t*>(ctx.eax) = nHeight;
}

// ESI is the HUD, and its icon scale has just been written from the font metrics.
static void HudIconScale(SafetyHookContext& ctx)
{
    if (nComicPanelScaling.load() == 0)
        return;

    *reinterpret_cast<float*>(ctx.esi + nOffsetHudIconScale) *= GetScaleFactor();
}

// Grown about the middle of the screen rather than the top left corner, so a panel that sat in
// the centre stays in the centre instead of walking off the edge as it gets bigger.
static void ScaleAboutCentre(uint8_t* pCanvas, float& fX, float& fY, float& fXScale, float& fYScale)
{
    const auto fFactor = GetScaleFactor();
    const auto fCentreX = *reinterpret_cast<float*>(pCanvas + nOffsetCanvasClipX) * 0.5f;
    const auto fCentreY = *reinterpret_cast<float*>(pCanvas + nOffsetCanvasClipY) * 0.5f;

    fX = fCentreX + (fX - fCentreX) * fFactor;
    fY = fCentreY + (fY - fCentreY) * fFactor;
    fXScale *= fFactor;
    fYScale *= fFactor;
}

static void __fastcall DrawTileScaled(uint8_t* pThis, void*, void* pMaterial, float fX, float fY, float fXScale, float fYScale)
{
    if (nComicPanelScaling.load() >= 2)
        ScaleAboutCentre(pThis, fX, fY, fXScale, fYScale);

    shDrawTileScaled.fastcall<void>(pThis, nullptr, pMaterial, fX, fY, fXScale, fYScale);
}

static void __fastcall DrawTileJustified(uint8_t* pThis, void*, void* pMaterial, float fX, float fY, float fXScale, float fYScale, uint8_t nJustification)
{
    if (nComicPanelScaling.load() >= 2)
        ScaleAboutCentre(pThis, fX, fY, fXScale, fYScale);

    shDrawTileJustified.fastcall<void>(pThis, nullptr, pMaterial, fX, fY, fXScale, fYScale, nJustification);
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    // MOV EAX,[EBP+0xC] ... MOV [EAX],0x1E0 - the return of a screen height that is not one.
    auto patternScreenHeight = module_pattern(L"Engine.dll", "8B 45 0C 8B 4D F4 5F 5E C7 00 E0 01 00 00");
    if (!patternScreenHeight.empty())
        mhScreenHeight = safetyhook::create_mid(patternScreenHeight.get_first(14), ScreenHeight);
    else
        LogWarn("ComicPanels: screen height pattern not found");

    // FSTP [ESI+0x2BC] - the HUD icon scale, worked out from the font rather than the screen. The
    // store on its own also matches a particle emitter, so the FMUL that feeds it is part of the
    // pattern.
    auto patternIconScale = module_pattern(L"Engine.dll", "D8 0D ?? ?? ?? ?? D9 9E BC 02 00 00");
    if (!patternIconScale.empty())
        mhHudIconScale = safetyhook::create_mid(patternIconScale.get_first(12), HudIconScale);
    else
        LogWarn("ComicPanels: HUD icon scale pattern not found");

    if (auto p = GetProcAddress(hEngine, "?DrawTileScaled@UCanvas@@UAEXPAVUMaterial@@MMMM@Z"))
        shDrawTileScaled = safetyhook::create_inline(p, DrawTileScaled);

    if (auto p = GetProcAddress(hEngine, "?DrawTileJustified@UCanvas@@UAEXPAVUMaterial@@MMMME@Z"))
        shDrawTileJustified = safetyhook::create_inline(p, DrawTileJustified);

    BindInt(nComicPanelScaling, PREF_COMICPANELSCALING);
    BindFloat(fComicPanelScaleOverride, PREF_COMICPANELSCALE);
}

class ComicPanels
{
public:
    ComicPanels()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} ComicPanels;
