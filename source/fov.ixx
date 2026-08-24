module;

#include <common.hxx>

export module fov;

import common;
import settings;
import logging;

// FOV is a script property, so there is no native clamp to widen and no config value to write.
// What is native is the moment the camera is built: UGameEngine::Draw reads the view actor's
// FovAngle and hands it to FCameraSceneNode, once for the world pass and once for the pass the
// interface is drawn over. Rewriting the value in flight leaves the script side untouched, so
// nothing gets saved into User.ini and nothing fights the game's own zoom logic.
//
// The value is not replaced outright, it is rescaled. The weapon zoom levels are authored as
// factors of the stock 85 degrees, and a scope that opened at a fixed number would zoom by a
// different amount for every FOV setting. Converting through the tangent keeps every zoom the
// same magnification it was.
static constexpr auto fPi = 3.14159265358979323846;
static constexpr auto fMinFieldOfView = 1.0f;
static constexpr auto fMaxFieldOfView = 170.0f;

static std::atomic<float> fFieldOfView = fStockFieldOfView;

static SafetyHookMid mhWorldPass{};
static SafetyHookMid mhOverlayPass{};

static float ScaleFieldOfView(float fSource)
{
    const auto fTarget = fFieldOfView.load();

    if (fSource <= 0.0f || std::abs(fTarget - fStockFieldOfView) < 0.01f)
        return fSource;

    const auto fRadians = [](double fDegrees) { return fDegrees * fPi / 180.0; };
    const auto fDegrees = [](double fRadians) { return fRadians * 180.0 / fPi; };

    const auto fZoom = std::tan(fRadians(fTarget) * 0.5) / std::tan(fRadians(fStockFieldOfView) * 0.5);
    const auto fResult = fDegrees(2.0 * std::atan(std::tan(fRadians(fSource) * 0.5) * fZoom));

    return std::clamp(static_cast<float>(fResult), fMinFieldOfView, fMaxFieldOfView);
}

// ECX holds the float bits of FovAngle at this point, on its way to being pushed as the last
// argument of the camera scene node.
static void ApplyToContext(SafetyHookContext& ctx)
{
    float fValue = 0.0f;
    std::memcpy(&fValue, &ctx.ecx, sizeof(fValue));

    fValue = ScaleFieldOfView(fValue);

    std::memcpy(&ctx.ecx, &fValue, sizeof(fValue));
}

static void InitEngine()
{
    // MOV EAX,[ESI+0x30] (Viewport->Actor) / MOV ECX,[EAX+0x1F8] (Actor->FovAngle) / MOV EAX,...
    // The overlay pass sets a render flag between the two moves, which is what tells them apart.
    auto patternWorld = module_pattern(L"Engine.dll", "8B 46 30 8B 88 F8 01 00 00 8B 45 D0 51");
    auto patternOverlay = module_pattern(L"Engine.dll", "8B 46 30 C7 86 58 01 00 00 01 00 00 00 8B 88 F8 01 00 00 8B 45 D0 51");

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
