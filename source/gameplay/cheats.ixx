module;

#include <common.hxx>
#include <cctype>

export module cheats;

import common;
import settings;
import logging;

// Cheat commands live on the PlayerController's CheatManager, which script only creates while
// LevelInfo.bAllowCheat is set, so the flag is forced on and the setting is enforced per command.
static constexpr auto nOffsetLevelFlags = 0x380;
static constexpr uint32_t nFlagAllowCheat = 0x400;

static SafetyHookInline shGetLevelInfo{};
static SafetyHookInline shScriptConsoleExec{};

static std::atomic<bool> bAllowCheats = false;

// UObject::GetFullName is "<class> <path>", and the class here is XIIICheatManager.
using tGetFullName = const char* (__fastcall*)(void*, void*, char*);
static tGetFullName pGetFullName = nullptr;

// The cheats are script toggles with no readable state, so turning the setting off replays what
// went through. ponytail: a cheat the game itself cancels leaves the tracking stale.
static void* pCheatManager = nullptr;
static void* pCheatOut = nullptr;
static void* pCheatExec = nullptr;
static std::vector<std::string> aOnCheats;
static std::atomic<bool> bClearPending = false;

static void Track(const char* szCommand)
{
    std::string sCheat(szCommand, std::strcspn(szCommand, " \t"));
    for (auto& c : sCheat)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const auto it = std::find(aOnCheats.begin(), aOnCheats.end(), sCheat);

    // Walk is Fly, Ghost and Amphibious's own way back, so those three are tracked under it.
    if (sCheat == "fly" || sCheat == "ghost" || sCheat == "amphibious")
    {
        if (std::find(aOnCheats.begin(), aOnCheats.end(), "walk") == aOnCheats.end())
            aOnCheats.emplace_back("walk");
    }
    else if (sCheat == "walk" || sCheat == "god" || sCheat == "dandd" || sCheat == "neuneu")
    {
        if (it != aOnCheats.end())
            aOnCheats.erase(it);
        else if (sCheat != "walk")
            aOnCheats.push_back(sCheat);
    }
}

static void ClearCheats()
{
    for (const auto& sCheat : aOnCheats)
        shScriptConsoleExec.fastcall<int>(pCheatManager, nullptr, sCheat.c_str(), pCheatOut, pCheatExec);

    aOnCheats.clear();
}

static void* __fastcall GetLevelInfo(void* pThis, void*)
{
    // The watcher fires on its own thread, so the replay waits for the game's.
    if (pCheatManager && bClearPending.exchange(false))
        ClearCheats();

    auto pLevelInfo = shGetLevelInfo.fastcall<void*>(pThis, nullptr);
    if (pLevelInfo)
        *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pLevelInfo) + nOffsetLevelFlags) |= nFlagAllowCheat;

    return pLevelInfo;
}

// The menu drives brightness, resolution and the like through the same object, so only the cheats
// themselves are tracked, and blocking one leaves the command to the rest of the exec chain.
static int __fastcall ScriptConsoleExec(void* pThis, void*, const char* szCommand, void* pOut, void* pExec)
{
    const auto szFullName = pGetFullName(pThis, nullptr, nullptr);
    const auto bCheatObject = szFullName && std::strstr(szFullName, "CheatManager ");

    if (bCheatObject && !bAllowCheats)
        return 0;

    const auto nResult = shScriptConsoleExec.fastcall<int>(pThis, nullptr, szCommand, pOut, pExec);

    if (bCheatObject && nResult)
    {
        // A fresh CheatManager means a fresh level, so the old tracking cannot be replayed.
        if (pThis != pCheatManager)
            aOnCheats.clear();

        pCheatManager = pThis;
        pCheatOut = pOut;
        pCheatExec = pExec;
        Track(szCommand);
    }

    return nResult;
}

static void Init()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hEngine || !hCore)
        return;

    auto pLevelInfo = GetProcAddress(hEngine, "?GetLevelInfo@ULevel@@QAEPAVALevelInfo@@XZ");
    auto pConsoleExec = GetProcAddress(hCore, "?ScriptConsoleExec@UObject@@UAEHPBDAAVFOutputDevice@@PAV1@@Z");
    pGetFullName = reinterpret_cast<tGetFullName>(GetProcAddress(hCore, "?GetFullName@UObject@@QBEPBDPAD@Z"));

    if (!pLevelInfo || !pConsoleExec || !pGetFullName)
    {
        LogWarn("AllowCheats: exports GetLevelInfo {}, ScriptConsoleExec {}, GetFullName {}",
            pLevelInfo != nullptr, pConsoleExec != nullptr, pGetFullName != nullptr);
        return;
    }

    shGetLevelInfo = safetyhook::create_inline(pLevelInfo, GetLevelInfo);
    shScriptConsoleExec = safetyhook::create_inline(pConsoleExec, ScriptConsoleExec);

    ApplyAndWatch([]()
    {
        const auto bNow = MongooseFixSettings.GetInt(PREF_ALLOWCHEATS) != 0;

        if (!bNow && bAllowCheats)
            bClearPending = true;

        bAllowCheats = bNow;
    });
}

class Cheats
{
public:
    Cheats()
    {
        MongooseFix::onEngineInitEvent() += []() { Init(); };
    }
} Cheats;
