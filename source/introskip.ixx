module;

#include <common.hxx>

export module introskip;

import common;
import settings;
import logging;

// XIII.exe's WinMain plays the three logo movies itself, before the startup map, as three inlined
// copies of the same block:
//
//   Open("ubi") / Open("alien") / Open("Nvidia")   vtable +0x74 on UPCVideoPlayerDevice
//   Play()                                         +0x78
//   while (uiGetStatus()) Tick(0)                  +0x98 / +0x6C
//
// Failing the open is not enough - the chain does not read that as "done, move to the next" and
// sits on a black screen - so all three are jumped over, from the PUSH of "ubi" to the first
// instruction past the third Tick loop, 0x165 bytes on. Jump and PUSH are both five bytes.
static constexpr auto nIntroBlockLength = 0x165;

static std::unique_ptr<raw_mem> patchSkipIntro;

static void Init()
{
    // PUSH "ubi" / MOV EDX,[EBP-0x148] / MOV ECX,[EDX+0x60] / ... / CALL [EAX+0x74] - the first
    // Open. The later two blocks use a different register order, so this matches once.
    auto patternIntro = module_pattern(nullptr, "68 ? ? ? ? 8B 95 B8 FE FF FF 8B 4A 60 8B 85 B8 FE FF FF 8B 50 60 8B 02 FF 50 74");
    if (patternIntro.empty())
    {
        LogWarn("SkipIntroMovies: intro pattern not found, the logo movies still play");
        return;
    }

    const auto nRelative = static_cast<int32_t>(nIntroBlockLength - 5);
    const auto pRelative = reinterpret_cast<const uint8_t*>(&nRelative);

    patchSkipIntro = std::make_unique<raw_mem>(patternIntro.get_first(0),
        std::initializer_list<uint8_t>{ 0xE9, pRelative[0], pRelative[1], pRelative[2], pRelative[3] });

    BindPatch(*patchSkipIntro, PREF_SKIPINTROMOVIES);
}

class IntroSkip
{
public:
    IntroSkip()
    {
        MongooseFix::onInitEvent() += []() { Init(); };
    }
} IntroSkip;
