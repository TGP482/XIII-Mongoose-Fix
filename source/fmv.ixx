module;

#include <common.hxx>

export module fmv;

import common;
import display;
import logging;

// UD3DRenderDevice::Video_CopyTexToScreen puts the Bink frame on a screen filling quad in clip
// space, corners hard coded at the unit square, so a 4:3 movie stretches to the window. The
// corners are the last four of six immediate pushes before the call, y1 x1 y0 x0 by address.
// Backbuffer is cleared black every frame, so narrowing the quad leaves bars, not the last frame.
static constexpr auto fMovieAspect = 4.0f / 3.0f;

static constexpr auto nOffsetY1 = 1;
static constexpr auto nOffsetX1 = 6;
static constexpr auto nOffsetY0 = 11;
static constexpr auto nOffsetX0 = 16;

static uint8_t* pCorners = nullptr;

static void Apply()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (!pCorners || nWidth <= 0 || nHeight <= 0)
        return;

    const auto fScreen = static_cast<float>(nWidth) / nHeight;
    const auto fX = fScreen > fMovieAspect ? fMovieAspect / fScreen : 1.0f;
    const auto fY = fScreen < fMovieAspect ? fScreen / fMovieAspect : 1.0f;

    injector::WriteMemory<float>(pCorners + nOffsetY1, fY, true);
    injector::WriteMemory<float>(pCorners + nOffsetX1, fX, true);
    injector::WriteMemory<float>(pCorners + nOffsetY0, -fY, true);
    injector::WriteMemory<float>(pCorners + nOffsetX0, -fX, true);
}

static void Init()
{
    // PUSH 0 / PUSH 0, the top left texture coordinate, then the corners and the call.
    auto patternQuad = module_pattern(L"D3DDrv.dll",
        "6A 00 6A 00 68 00 00 80 3F 68 00 00 80 3F 68 00 00 80 BF 68 00 00 80 BF E8");

    if (patternQuad.empty())
    {
        LogWarn("Fmv: the movie quad was not found, movies stay stretched to the window");
        return;
    }

    pCorners = patternQuad.get_first<uint8_t>(4);

    onDeviceResetEvent() += []() { Apply(); };
}

class Fmv
{
public:
    Fmv()
    {
        MongooseFix::onD3DDrvInitEvent() += []() { Init(); };
    }
} Fmv;
