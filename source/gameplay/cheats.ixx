module;

#include <common.hxx>

export module cheats;

import common;
import settings;
import logging;

// The commands are gated by LevelInfo.bAllowCheat, which maps to bit 0x400 in LevelInfo's flag block.
// Force that flag on every frame so cheats remain available across level changes and travel.
static constexpr auto nOffsetLevelFlags = 0x380;
static constexpr uint32_t nFlagAllowCheat = 0x400;

// ULevel::GetLevelInfo is exported by name and called several times a frame, so the flag is
// continuously re-forced rather than set once during level load.
static SafetyHookInline shGetLevelInfo{};

static std::atomic<bool> bAllowCheats = false;

// Preserve each level's original value so disabling the setting restores it.
// A new LevelInfo pointer indicates a new level, so one saved value is sufficient.
static void* pKnownLevelInfo = nullptr;
static bool bStockAllowCheat = false;

static void* __fastcall GetLevelInfo(void* pThis, void*)
{
    auto pLevelInfo = shGetLevelInfo.fastcall<void*>(pThis, nullptr);
    if (!pLevelInfo)
        return pLevelInfo;

    auto& nFlags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pLevelInfo) + nOffsetLevelFlags);

    if (pLevelInfo != pKnownLevelInfo)
    {
        pKnownLevelInfo = pLevelInfo;
        bStockAllowCheat = (nFlags & nFlagAllowCheat) != 0;
    }

    if (bAllowCheats || bStockAllowCheat)
        nFlags |= nFlagAllowCheat;
    else
        nFlags &= ~nFlagAllowCheat;

    return pLevelInfo;
}

static void Init()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto p = GetProcAddress(hEngine, "?GetLevelInfo@ULevel@@QAEPAVALevelInfo@@XZ");
    if (!p)
    {
        LogWarn("AllowCheats: Engine.dll did not export GetLevelInfo, cheats stay as each map left them");
        return;
    }

    shGetLevelInfo = safetyhook::create_inline(p, GetLevelInfo);
    BindBool(bAllowCheats, PREF_ALLOWCHEATS);
}

class Cheats
{
public:
    Cheats()
    {
        MongooseFix::onEngineInitEvent() += []() { Init(); };
    }
} Cheats;
