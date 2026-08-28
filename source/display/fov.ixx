module;

#include <common.hxx>

export module fov;

import common;
import settings;
import logging;
import display;

// FOV is a script property: no native clamp to widen, no config to write. The only native moment
// is where the camera is built. UGameEngine::Draw reads the view actor's FovAngle and hands it to
// FCameraSceneNode, once for the world pass and once for the pass the interface draws over.
// Rewriting it in flight leaves the script side alone, so nothing reaches User.ini and nothing
// fights the game's own zoom.
//
// Rescaled, not replaced: weapon zoom levels are authored as factors of the stock 85 degrees, so a
// scope that opens at a fixed number would zoom by a different amount at every FOV setting. Going
// through the tangent keeps each zoom at the magnification it had.
//
// FovAngle is horizontal and the vertical half comes from the viewport, so a wide screen only cuts
// height off the 4:3 image. The same tangent scaling widens back by the aspect ratio, holding the
// vertical angle still, so one FOV number looks the same everywhere.
static constexpr auto fPi = 3.14159265358979323846;
static constexpr auto fStockAspect = 4.0 / 3.0;
static constexpr auto fMinFieldOfView = 1.0f;
static constexpr auto fMaxFieldOfView = 170.0f;

static std::atomic<float> fFieldOfView = fStockFieldOfView;

static SafetyHookMid mhWorldPass{};
static SafetyHookMid mhOverlayPass{};
static SafetyHookMid mhViewmodel{};
static SafetyHookMid mhWorldToScreen{};

// How much wider than 4:3 the horizontal half angle's tangent has to be for the vertical half
// angle to come out the same.
static double AspectZoom()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (nWidth <= 0 || nHeight <= 0)
        return 1.0;

    return (static_cast<double>(nWidth) / nHeight) / fStockAspect;
}

static float ScaleFieldOfView(float fSource)
{
    const auto fTarget = fFieldOfView.load();

    if (fSource <= 0.0f)
        return fSource;

    const auto fRadians = [](double fDegrees) { return fDegrees * fPi / 180.0; };
    const auto fDegrees = [](double fRadians) { return fRadians * 180.0 / fPi; };

    const auto fZoom = std::tan(fRadians(fTarget) * 0.5) / std::tan(fRadians(fStockFieldOfView) * 0.5) * AspectZoom();

    if (std::abs(fZoom - 1.0) < 0.0001)
        return fSource;

    const auto fResult = fDegrees(2.0 * std::atan(std::tan(fRadians(fSource) * 0.5) * fZoom));

    return std::clamp(static_cast<float>(fResult), fMinFieldOfView, fMaxFieldOfView);
}

// The register holds the float bits of FovAngle on the way to the camera scene node.
static void Apply(uintptr_t& nRegister)
{
    float fValue = 0.0f;
    std::memcpy(&fValue, &nRegister, sizeof(fValue));

    fValue = ScaleFieldOfView(fValue);

    std::memcpy(&nRegister, &fValue, sizeof(fValue));
}

static void ApplyToContext(SafetyHookContext& ctx)
{
    Apply(ctx.ecx);
}

// UInteraction::WorldToScreen builds a camera node of its own from the same FovAngle. Left
// unscaled, every box script puts around an actor lands off by that much.
static void ApplyToWorldToScreen(SafetyHookContext& ctx)
{
    Apply(ctx.edx);
}

// The first person mesh is not drawn through the camera node; the level renderer swaps in its own
// projection with the half angle as a literal pushed straight into the matrix. No FOV setting to
// follow here, only the aspect correction the camera pass gets.
static void ApplyToViewmodel(SafetyHookContext& ctx)
{
    const auto fZoom = AspectZoom();

    if (std::abs(fZoom - 1.0) < 0.0001)
        return;

    auto& fHalfAngle = *reinterpret_cast<float*>(ctx.esp);

    fHalfAngle = static_cast<float>(std::atan(std::tan(static_cast<double>(fHalfAngle)) * fZoom));
}

static void InitEngine()
{
    // MOV EAX,[ESI+0x30] (Viewport->Actor) / MOV ECX,[EAX+0x1F8] (Actor->FovAngle) / MOV EAX,...
    // The overlay pass sets a render flag between the two moves, which is what tells them apart.
    auto patternWorld = module_pattern(L"Engine.dll", "8B 46 30 8B 88 F8 01 00 00 8B 45 D0 51");
    auto patternOverlay = module_pattern(L"Engine.dll", "8B 46 30 C7 86 58 01 00 00 01 00 00 00 8B 88 F8 01 00 00 8B 45 D0 51");

    // FILD SizeX / FSTP [ESP] / PUSH 0.43633: the last argument pushed is the half angle, 25
    // degrees, ahead of the call that builds the first person projection matrix.
    auto patternViewmodel = module_pattern(L"Engine.dll", "DB 86 88 00 00 00 D9 1C 24 68 BE 65 DF 3E E8");

    if (patternWorld.empty() && patternOverlay.empty())
    {
        LogWarn("FieldOfView: no camera pattern matched, the setting does nothing");
        return;
    }

    // Hooked one instruction past the load, so the register already holds the value.
    if (!patternWorld.empty())
        mhWorldPass = safetyhook::create_mid(patternWorld.get_first(9), ApplyToContext);

    if (!patternOverlay.empty())
        mhOverlayPass = safetyhook::create_mid(patternOverlay.get_first(19), ApplyToContext);

    if (!patternViewmodel.empty())
        mhViewmodel = safetyhook::create_mid(patternViewmodel.get_first(14), ApplyToViewmodel);
    else
        LogWarn("FieldOfView: first person projection pattern not matched, the viewmodel keeps the 4:3 framing");

    // MOV EDX,[EBX+0x1F8] (PlayerController->FovAngle) / PUSH EDX, one instruction back.
    auto patternWorldToScreen = module_pattern(L"Engine.dll", "8B 93 F8 01 00 00 52 8B 55 D8 83 EC 0C");

    if (!patternWorldToScreen.empty())
        mhWorldToScreen = safetyhook::create_mid(patternWorldToScreen.get_first(6), ApplyToWorldToScreen);

    BindFloat(fFieldOfView, PREF_FIELDOFVIEW);
}

class FieldOfView
{
public:
    FieldOfView()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} FieldOfView;
