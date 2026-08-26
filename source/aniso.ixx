module;

#include <common.hxx>

export module aniso;

import common;
import settings;
import display;
import logging;

// The renderer sets D3DTSS_MINFILTER and D3DTSS_MAGFILTER to D3DTEXF_LINEAR once in SetRes and
// then several draw paths write them again per material, so patching SetRes alone would be
// undone the moment anything drew. It never touches D3DTSS_MAXANISOTROPY at all, which defaults
// to 1, so asking for anisotropic filtering without also setting the level does nothing.
//
// One hook on SetTextureStageState covers every writer and lets the level change while the game
// is running without a device reset.
static constexpr auto D3DTSS_MINFILTER = 17;
static constexpr auto D3DTSS_MAXANISOTROPY = 21;

static constexpr auto D3DTEXF_LINEAR = 2;
static constexpr auto D3DTEXF_ANISOTROPIC = 3;

// IDirect3DDevice8::SetTextureStageState, vtable slot 63.
static constexpr auto nSetTextureStageStateSlot = 63;

static std::atomic<int> nAnisotropicFiltering = 0;

static void* pHookedDevice = nullptr;
static SafetyHookVmt vmtDevice{};
static SafetyHookVm vmSetTextureStageState{};

static HRESULT __stdcall SetTextureStageState(void* pDevice, uint32_t nStage, uint32_t nType, uint32_t nValue)
{
    const auto nLevel = nAnisotropicFiltering.load();

    // Only the minification filter is worth turning up. Anisotropic magnification is not a thing
    // any driver does differently from linear, and asking for it costs a validation failure on
    // some of them.
    if (nLevel > 1 && nType == D3DTSS_MINFILTER && nValue == D3DTEXF_LINEAR)
    {
        vmSetTextureStageState.stdcall<HRESULT>(pDevice, nStage, D3DTSS_MAXANISOTROPY, static_cast<uint32_t>(nLevel));
        nValue = D3DTEXF_ANISOTROPIC;
    }

    return vmSetTextureStageState.stdcall<HRESULT>(pDevice, nStage, nType, nValue);
}

static void HookDevice()
{
    auto pDevice = GetD3DDevice();
    if (!pDevice || pDevice == pHookedDevice)
        return;

    // A reset keeps the same object and the hook with it. A recreate does not, so the hook is
    // rebuilt, and the old one is torn down first. Building a new vmt hook over a live one would
    // take the fake vtable for the real one and then have the old hook put the real one back.
    vmSetTextureStageState = {};
    vmtDevice = {};

    vmtDevice = safetyhook::create_vmt(pDevice);
    vmSetTextureStageState = safetyhook::create_vm(vmtDevice, nSetTextureStageStateSlot, SetTextureStageState);
    pHookedDevice = pDevice;
}

static void ApplySetting()
{
    auto nLevel = MongooseFixSettings.GetInt(PREF_ANISOTROPICFILTERING);

    // The device already told the game its ceiling at Init and the game ignored it. Asking for
    // more than the hardware supports is a failed call per stage, per frame.
    const auto nMax = GetDeviceMaxAnisotropy();
    if (nMax > 1 && nLevel > nMax)
    {
        LogWarn("Aniso: {}x asked for, device supports {}x", nLevel, nMax);
        nLevel = nMax;
    }

    nAnisotropicFiltering = nLevel;
}

class Aniso
{
public:
    Aniso()
    {
        onDeviceResetEvent() += []()
        {
            HookDevice();
            ApplySetting();
        };

        // The level itself needs no device work, so a change takes effect on the next draw.
        MongooseFix::onIniFileChange() += []() { ApplySetting(); };
    }
} Aniso;
