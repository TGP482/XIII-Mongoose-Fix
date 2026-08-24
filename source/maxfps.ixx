module;

#include <common.hxx>

export module maxfps;

import common;
import settings;
import logging;

// The frame rate is capped by UGameEngine::GetMaxTickRate returning a flat 120 whenever there is
// no net driver, which the main loop turns into an appSleep at the end of every frame. Both the
// loop inside Engine.dll and the one inside XIII.exe read the same virtual, so replacing what it
// returns covers the game however it was launched.
//
// The multiplayer answers are left alone: they are the net tick rate, not a frame rate, and
// running the net driver unlocked is not the same request.
static constexpr auto fStockMaxTickRate = 120.0f;

static std::atomic<int> nMaxFrameRate = 0;

static SafetyHookInline shGetMaxTickRate{};

static float __fastcall GetMaxTickRate(void* pThis, void*)
{
    const auto fOriginal = shGetMaxTickRate.fastcall<float>(pThis, nullptr);

    if (fOriginal != fStockMaxTickRate)
        return fOriginal;

    // 0 unlocks. The caller already treats anything at or below zero as "do not sleep", so this
    // needs no patch of its own.
    return static_cast<float>(nMaxFrameRate.load());
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto pGetMaxTickRate = GetProcAddress(hEngine, "?GetMaxTickRate@UGameEngine@@UAEMXZ");
    if (!pGetMaxTickRate)
    {
        LogWarn("MaxFrameRate: Engine.dll did not export GetMaxTickRate, the frame rate cap is untouched");
        return;
    }

    shGetMaxTickRate = safetyhook::create_inline(pGetMaxTickRate, GetMaxTickRate);
    BindInt(nMaxFrameRate, PREF_MAXFRAMERATE);
}

class MaxFrameRate
{
public:
    MaxFrameRate()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} MaxFrameRate;
