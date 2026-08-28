module;

#include <common.hxx>

export module fov;

import common;
import settings;
import logging;
import display;

// FOV be script property: no native clamp to widen, no config to write. Native part be moment
// camera get built. UGameEngine::Draw read view actor FovAngle, hand to FCameraSceneNode, once
// for world pass, once for pass interface draw over. Rewrite value in flight, script side stay
// untouched: nothing reach User.ini, nothing fight game own zoom.
//
// Value not replaced, rescaled. Weapon zoom levels authored as factors of stock 85 degrees, so
// scope opening at fixed number zoom different amount at every FOV setting. Go through tangent,
// every zoom keep magnification it had.
//
// FovAngle be horizontal, vertical half come from viewport, so wide screen only cut height off
// 4:3 image. Same tangent scaling widen back by aspect ratio: vertical angle hold still, one FOV
// number look same everywhere.
static constexpr auto fPi = 3.14159265358979323846;
static constexpr auto fStockAspect = 4.0 / 3.0;
static constexpr auto fMinFieldOfView = 1.0f;
static constexpr auto fMaxFieldOfView = 170.0f;

static std::atomic<float> fFieldOfView = fStockFieldOfView;

static SafetyHookMid mhWorldPass{};
static SafetyHookMid mhOverlayPass{};
static SafetyHookMid mhViewmodel{};

// How much wider than 4:3 tangent of horizontal half angle must be for vertical half angle to
// come out same.
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

// ECX hold float bits of FovAngle here, on way to being pushed as last argument of camera scene
// node.
static void ApplyToContext(SafetyHookContext& ctx)
{
    float fValue = 0.0f;
    std::memcpy(&fValue, &ctx.ecx, sizeof(fValue));

    fValue = ScaleFieldOfView(fValue);

    std::memcpy(&ctx.ecx, &fValue, sizeof(fValue));
}

// First person mesh not drawn through camera node: level renderer swap in own projection. Half
// angle be literal pushed straight into matrix, so no FOV setting to follow here, only same
// aspect correction camera pass get.
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
    // Overlay pass set render flag between two moves: that be what tell them apart.
    auto patternWorld = module_pattern(L"Engine.dll", "8B 46 30 8B 88 F8 01 00 00 8B 45 D0 51");
    auto patternOverlay = module_pattern(L"Engine.dll", "8B 46 30 C7 86 58 01 00 00 01 00 00 00 8B 88 F8 01 00 00 8B 45 D0 51");

    // FILD SizeX / FSTP [ESP] / PUSH 0.43633: last argument onto stack be half angle, 25 degrees,
    // ahead of call that build first person projection matrix.
    auto patternViewmodel = module_pattern(L"Engine.dll", "DB 86 88 00 00 00 D9 1C 24 68 BE 65 DF 3E E8");

    if (patternWorld.empty() && patternOverlay.empty())
    {
        LogWarn("FieldOfView: no camera pattern matched, the setting does nothing");
        return;
    }

    // Hooked one instruction past load, so register already hold value.
    if (!patternWorld.empty())
        mhWorldPass = safetyhook::create_mid(patternWorld.get_first(9), ApplyToContext);

    if (!patternOverlay.empty())
        mhOverlayPass = safetyhook::create_mid(patternOverlay.get_first(19), ApplyToContext);

    if (!patternViewmodel.empty())
        mhViewmodel = safetyhook::create_mid(patternViewmodel.get_first(14), ApplyToViewmodel);
    else
        LogWarn("FieldOfView: first person projection pattern not matched, the viewmodel keeps the 4:3 framing");

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
