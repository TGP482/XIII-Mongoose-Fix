module;

#include <common.hxx>

export module fpsfixes;

import common;
import logging;

// Cutscene focus turn clamps its per frame step: Clamp( delta, -speed*dt*182, speed*dt*182 ).
// Bounds are ints, ramp starts at zero. Above ~162 fps first frame truncates both to 0, step comes
// back 0, script's own reset zeroes the ramp. Deadlock, not slowness. One unit of room keeps the
// ramp alive; past that the limit is frame rate independent already.
// Scoped to the ticks carrying that idiom, and only to a Clamp with both bounds zero.
// docs/CutsceneCameraHighFps.md: full trace.
//
//   UObject      +0x0C FStateFrame*  +0x24 UClass*
//   FStateFrame  +0x1C UState*
//   UClass       +0x28 super
//   FFrame       +0x08 UObject*      +0x0C bytecode
static constexpr auto nOffsetStateFrame = 0x0C;
static constexpr auto nOffsetStateNode = 0x1C;
static constexpr auto nOffsetClass = 0x24;
static constexpr auto nOffsetSuper = 0x28;
static constexpr auto nOffsetFrameObject = 0x08;
static constexpr auto nOffsetFrameCode = 0x0C;
static constexpr uint8_t nOpcodeDebugInfo = 0x42;

static SafetyHookInline shClamp{};
static SafetyHookInline shActorTick{};
static SafetyHookInline shControllerTick{};
static SafetyHookInline shPlayerTick{};
static const char* (__thiscall* pGetName)(void*) = nullptr;
static void(__fastcall* pStep)(void*, void*, void*, void*) = nullptr;

static auto bFocusTurn = false;

static const char* StateName(void* pObject)
{
    auto pFrame = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(pObject) + nOffsetStateFrame);
    if (!pFrame)
        return "";

    auto pState = *reinterpret_cast<void**>(pFrame + nOffsetStateNode);
    return pState ? pGetName(pState) : "";
}

// Cached per class. Runs for every actor, every frame.
static bool IsFocusTurner(void* pObject)
{
    static std::map<void*, bool> known;

    auto pClass = *reinterpret_cast<void**>(static_cast<uint8_t*>(pObject) + nOffsetClass);
    if (!pClass)
        return false;

    if (auto it = known.find(pClass); it != known.end())
        return it->second;

    auto bMatch = false;
    for (auto pWalk = pClass; pWalk && !bMatch; pWalk = *reinterpret_cast<void**>(static_cast<uint8_t*>(pWalk) + nOffsetSuper))
    {
        const auto szName = pGetName(pWalk);
        bMatch = szName && (!strcmp(szName, "CineController2") || !strcmp(szName, "KelloAmbush") || !strcmp(szName, "RoofGrapnleDemonstrator"));
    }

    known.emplace(pClass, bMatch);
    return bMatch;
}

// Save, not clear: one of these ticks can drive another.
static int Tick(SafetyHookInline& sh, void* pThis, float fDelta, int nTickType, bool bTurner)
{
    const auto bOuter = std::exchange(bFocusTurn, bTurner);
    const auto nResult = sh.fastcall<int>(pThis, nullptr, fDelta, nTickType);
    bFocusTurn = bOuter;

    return nResult;
}

// Three int params plus the trailing debug opcode, same as the original. Bytecode pointer has to
// land where the VM expects it.
static void __fastcall Clamp(void* pThis, void*, uint8_t* pFrame, int32_t* pResult)
{
    if (!bFocusTurn)
        return shClamp.fastcall<void>(pThis, nullptr, pFrame, pResult);

    auto pObject = *reinterpret_cast<void**>(pFrame + nOffsetFrameObject);
    int32_t nValue = 0, nMin = 0, nMax = 0;

    pStep(pFrame, nullptr, pObject, &nValue);
    pStep(pFrame, nullptr, pObject, &nMin);
    pStep(pFrame, nullptr, pObject, &nMax);

    auto& pCode = *reinterpret_cast<uint8_t**>(pFrame + nOffsetFrameCode);
    if (*++pCode == nOpcodeDebugInfo)
        pStep(pFrame, nullptr, pObject, nullptr);

    if (nMin == 0 && nMax == 0)
    {
        nMin = -1;
        nMax = 1;
    }

    *pResult = std::clamp(nValue, nMin, nMax);
}

static int __fastcall ActorTick(void* pThis, void*, float fDelta, int nTickType)
{
    return Tick(shActorTick, pThis, fDelta, nTickType, pThis && IsFocusTurner(pThis));
}

static int __fastcall ControllerTick(void* pThis, void*, float fDelta, int nTickType)
{
    return Tick(shControllerTick, pThis, fDelta, nTickType, pThis && IsFocusTurner(pThis));
}

// Own vtable override, reaches neither hook above. Turn lives in NoControl. Other states are the
// player driving the camera.
static int __fastcall PlayerTick(void* pThis, void*, float fDelta, int nTickType)
{
    return Tick(shPlayerTick, pThis, fDelta, nTickType, pThis && !strcmp(StateName(pThis), "NoControl"));
}

// MoveActor sweeps 2 units past the requested delta, then parks the actor 2 units short of the
// impact. Blocked move returns Delta minus 2. Anything under 2 units returns nothing. Fixed loss
// per call, call count scales with frame rate: 60 uu/s at 30 fps, 480 uu/s at 240, past walk
// speed. In a gap the pawn grazes a surface every frame, so unlocked it never advances.
// Walking and falling share one 1/30 accumulator, drawn position lerps the last two steps, same
// shape as the softbody and camera fixes in the Splinter Cell fix. Shared accumulator carries
// leftover time through a takeoff or a landing instead of dropping a step at each.
static constexpr auto fFixedStep = 1.0f / 30.0f;
static constexpr auto nMaxSteps = 8;
static constexpr auto nOffsetLocation = 0xCC;
static constexpr auto nOffsetPhysics = 0x38;
static constexpr uint8_t nPhysWalking = 1;
static constexpr uint8_t nPhysFalling = 2;

static SafetyHookInline shPhysWalking{};
static SafetyHookInline shPhysFalling{};
static int(__thiscall* pIsHumanControlled)(void*) = nullptr;

static auto bStepping = false;

struct FVector { float X, Y, Z; };

static FVector& Location(void* pPawn)
{
    return *reinterpret_cast<FVector*>(static_cast<uint8_t*>(pPawn) + nOffsetLocation);
}

static uint8_t Physics(void* pPawn)
{
    return *(static_cast<uint8_t*>(pPawn) + nOffsetPhysics);
}

static FVector Lerp(const FVector& a, const FVector& b, float t)
{
    return { a.X + t * (b.X - a.X), a.Y + t * (b.Y - a.Y), a.Z + t * (b.Z - a.Z) };
}

static void Advance(void* pPawn, float fDelta, int nIterations)
{
    static FVector vBack{}, vFront{}, vDrawn{};
    static auto fPending = 0.0f;
    static auto bHeld = false;

    // Anything else that moved the pawn wins: teleports, movers, script.
    auto& vLocation = Location(pPawn);
    if (!bHeld || vLocation.X != vDrawn.X || vLocation.Y != vDrawn.Y || vLocation.Z != vDrawn.Z)
    {
        vBack = vFront = vLocation;
        fPending = 0.0f;
    }
    else
    {
        vLocation = vFront;
    }

    bHeld = false;
    fPending += fDelta;

    auto nSteps = 0;
    while (fPending >= fFixedStep && nSteps < nMaxSteps)
    {
        fPending -= fFixedStep;
        nSteps++;
    }

    if (nSteps == nMaxSteps)
        fPending = 0.0f;

    bStepping = true;
    for (auto i = 0; i < nSteps; i++)
    {
        vBack = vFront;

        // State can flip inside a step. Pick the handler per step, not per frame.
        auto& sh = Physics(pPawn) == nPhysFalling ? shPhysFalling : shPhysWalking;
        sh.fastcall<void>(pPawn, nullptr, fFixedStep, nIterations);

        vFront = vLocation;
    }
    bStepping = false;

    // Other states drive the pawn themselves. Hand it back untouched.
    const auto nPhysics = Physics(pPawn);
    if (nPhysics != nPhysWalking && nPhysics != nPhysFalling)
    {
        fPending = 0.0f;
        return;
    }

    // ponytail: raw write, no collision hash update. Interpolated offset never exceeds one 1/30
    // step, hash buckets far coarser, entry stays correct. Route through MoveActor if a trace is
    // ever seen missing the player.
    vDrawn = vLocation = Lerp(vBack, vFront, fPending / fFixedStep);
    bHeld = true;
}

// startNewPhysics re-enters these from inside a step. Only the outermost call accumulates.
static bool Passthrough(void* pPawn, float fDelta)
{
    return bStepping || !pPawn || !(fDelta > 0.0f) || !pIsHumanControlled(pPawn);
}

static void __fastcall PhysWalking(void* pThis, void*, float fDelta, int nIterations)
{
    if (Passthrough(pThis, fDelta))
        return shPhysWalking.fastcall<void>(pThis, nullptr, fDelta, nIterations);

    Advance(pThis, fDelta, nIterations);
}

static void __fastcall PhysFalling(void* pThis, void*, float fDelta, int nIterations)
{
    if (Passthrough(pThis, fDelta))
        return shPhysFalling.fastcall<void>(pThis, nullptr, fDelta, nIterations);

    Advance(pThis, fDelta, nIterations);
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hEngine || !hCore)
        return;

    auto pPhysWalking = GetProcAddress(hEngine, "?physWalking@APawn@@QAEXMH@Z");
    auto pPhysFalling = GetProcAddress(hEngine, "?physFalling@APawn@@UAEXMH@Z");
    pIsHumanControlled = reinterpret_cast<decltype(pIsHumanControlled)>(GetProcAddress(hEngine, "?IsHumanControlled@APawn@@QAEHXZ"));

    if (!pPhysWalking || !pPhysFalling || !pIsHumanControlled)
    {
        LogWarn("FpsFixes: an export is missing, walking and falling stay frame rate dependent");
    }
    else
    {
        shPhysWalking = safetyhook::create_inline(pPhysWalking, PhysWalking);
        shPhysFalling = safetyhook::create_inline(pPhysFalling, PhysFalling);
    }

    auto pClamp = GetProcAddress(hCore, "?execClamp@UObject@@QAEXAAUFFrame@@QAX@Z");
    auto pActorTick = GetProcAddress(hEngine, "?Tick@AActor@@UAEHMW4ELevelTick@@@Z");
    auto pControllerTick = GetProcAddress(hEngine, "?Tick@AController@@UAEHMW4ELevelTick@@@Z");
    auto pPlayerTick = GetProcAddress(hEngine, "?Tick@APlayerController@@UAEHMW4ELevelTick@@@Z");
    pGetName = reinterpret_cast<decltype(pGetName)>(GetProcAddress(hCore, "?GetName@UObject@@QBEPBDXZ"));
    pStep = reinterpret_cast<decltype(pStep)>(GetProcAddress(hCore, "?Step@FFrame@@QAEXPAVUObject@@QAX@Z"));

    if (!pClamp || !pActorTick || !pControllerTick || !pPlayerTick || !pGetName || !pStep)
    {
        LogWarn("FpsFixes: an export is missing, the cutscene camera turn stays frame rate dependent");
        return;
    }

    shClamp = safetyhook::create_inline(pClamp, Clamp);
    shActorTick = safetyhook::create_inline(pActorTick, ActorTick);
    shControllerTick = safetyhook::create_inline(pControllerTick, ControllerTick);
    shPlayerTick = safetyhook::create_inline(pPlayerTick, PlayerTick);
}

class FpsFixes
{
public:
    FpsFixes()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} FpsFixes;
