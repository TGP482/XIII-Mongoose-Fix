module;

#include <common.hxx>

export module comicpanels;

import common;
import settings;
import display;
import logging;

// The onomatopoeia sprites are drawn by script in Xiii.u through UCanvas. Level 2 scales the two
// canvas entry points those draws land in, and is off by default because it catches anything else
// drawn the same way. GetScreenHeight's 480 and the icon scale are left to hudscale.ixx.
static constexpr auto fAuthoredScreenHeight = 480.0f;
static constexpr auto nOffsetCanvasClipX = 0x3C;
static constexpr auto nOffsetCanvasClipY = 0x40;

static std::atomic<int> nComicPanelScaling = 1;
static std::atomic<float> fComicPanelScaleOverride = 0.0f;

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

// About the middle of the screen, so a centred panel stays centred as it grows.
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

    // Not hooked: hudscale.ixx already hooks the entry of both to add the canvas origin, and its
    // pass-wide scaling covers what level 2 did here.
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
