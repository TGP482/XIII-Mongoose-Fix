module;

#include <common.hxx>

export module dx9;

import common;
import settings;
import logging;

// d3d8to9 by Patrick Mours, BSD 3-Clause, compiled into the asi from source/d3d8to9. Its entry
// point stands in for the one D3DDrv.dll imports from d3d8.dll.
extern "C" void* __stdcall Direct3DCreate8(UINT nSDKVersion);

// Installed ahead of msaa, which hooks the same export and has to sit outside this one to see the
// object that comes back.
static constexpr auto nInitPriority = 50;

static SafetyHookInline shCreate{};

static void* __stdcall Create(UINT nSDKVersion)
{
    auto pResult = Direct3DCreate8(nSDKVersion);

    // No Direct3D 9 device to be had, so hand back the stock one rather than no renderer at all.
    if (!pResult)
    {
        LogWarn("Direct3D 9: d3d8to9 returned nothing, falling back to Direct3D 8");
        return shCreate.stdcall<void*>(nSDKVersion);
    }

    return pResult;
}

static void Init()
{
    if (MongooseFixSettings.GetInt(PREF_DIRECTXVERSION) != 1)
        return;

    auto hD3D8 = LoadLibraryW(L"d3d8.dll");
    auto pCreate = hD3D8 ? GetProcAddress(hD3D8, "Direct3DCreate8") : nullptr;

    if (!pCreate)
    {
        LogWarn("Direct3D 9: d3d8.dll did not export Direct3DCreate8, Direct3D 9 is off");
        return;
    }

    shCreate = safetyhook::create_inline(pCreate, Create);

    if (!shCreate)
    {
        LogWarn("Direct3D 9: Direct3DCreate8 could not be hooked, Direct3D 9 is off");
        return;
    }

    LogInfo("Direct3D 9: on");
}

class DX9
{
public:
    DX9()
    {
        // The device is made once, so this is startup only; an ini change lands on the next launch.
        MongooseFix::onD3DDrvInitEvent().add([]() { Init(); }, nInitPriority);
    }
} DX9;
