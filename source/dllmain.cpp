#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <mutex>

import common;
import settings;
import logging;
import display;
import aniso;
import msaa;
import crtgamma;
import dx9;
import internalres;
import fov;
import maxfps;
import fpsfixes;
import rawmouse;
import controller;
import comicpanels;
import crashdump;
import blurfix;
import menuscale;
import mongoosemenu;
import hudscale;
import fmv;
import introskip;
import exitfix;
import cheats;
import updatecheck;

void Init()
{
    CSettings::ReadIniSettings();
    MongooseFix::onStartupPromptEvent().executeAll();
    MongooseFix::onInitEvent().executeAll();
}

extern "C"
{
    void __declspec(dllexport) InitializeASI()
    {
        std::call_once(CallbackHandler::flag, []()
        {
            CallbackHandler::RegisterCallback(Init);
            CallbackHandler::RegisterCallback(L"Engine.dll", []() { MongooseFix::onEngineInitEvent().executeAll(); });
            CallbackHandler::RegisterCallback(L"D3DDrv.dll", []() { MongooseFix::onD3DDrvInitEvent().executeAll(); });
            CallbackHandler::RegisterCallback(L"GUI.dll", []() { MongooseFix::onGUIInitEvent().executeAll(); });
            CallbackHandler::RegisterCallback(L"Core.dll", []() { MongooseFix::onCoreInitEvent().executeAll(); });
            CallbackHandler::RegisterCallback(L"WinDrv.dll", []() { MongooseFix::onWinDrvInitEvent().executeAll(); });
            CallbackHandler::RegisterCallback(L"Window.dll", []() { MongooseFix::onWindowInitEvent().executeAll(); });
        });
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        // Ultimate ASI Loader calls InitializeASI itself. Nothing else does.
        if (!IsUALPresent()) { InitializeASI(); }
    }
    if (reason == DLL_PROCESS_DETACH)
    {
        MongooseFix::onShutdownEvent().executeAll();
    }
    return TRUE;
}
