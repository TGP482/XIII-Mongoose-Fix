module;

#include <common.hxx>
#include <DbgHelp.h>
#include <format>

#pragma comment(lib, "dbghelp.lib")

export module crashdump;

import common;
import logging;

// The engine wraps its main loop in a catch all, so a fault unwinds every guard and ends at
// Window.dll's crash box with the stack that caused it already gone. A vectored handler runs before
// any of that: the dump is taken on the faulting thread, at the faulting instruction.
//
// Only faults nothing recovers from are taken; anything a probe or a driver raises and handles is
// left alone. Fatal errors the engine raises itself never become an exception, so the box is hooked
// as well, though that dump is post unwind, but it is what there is.
static std::atomic_flag bDumped;
static SafetyHookInline shOnInitDialog{};

static bool Fatal(DWORD nCode)
{
    switch (nCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

// Into the game's own CrashDumps folder, which it creates and never writes to.
static std::filesystem::path DumpPath()
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    auto folder = GetExeModulePath<std::filesystem::path>() / "CrashDumps";

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);

    return folder / std::format("XIIIMongooseFix_{:04}{:02}{:02}_{:02}{:02}{:02}.dmp",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
}

static void WriteDump(EXCEPTION_POINTERS* pInfo)
{
    if (bDumped.test_and_set())
        return;

    const auto path = DumpPath();
    auto hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto bWritten = false;

    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION info{ GetCurrentThreadId(), pInfo, FALSE };

        bWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
                | MiniDumpWithUnloadedModules), pInfo ? &info : nullptr, nullptr, nullptr) != FALSE;

        CloseHandle(hFile);
    }

    LogWarn("CrashDump: {} CrashDumps\\{}", bWritten ? "dump written to" : "could not write",
        path.filename().string());
}

static LONG CALLBACK OnException(EXCEPTION_POINTERS* pInfo)
{
    const auto pRecord = pInfo ? pInfo->ExceptionRecord : nullptr;

    if (!pRecord || !Fatal(pRecord->ExceptionCode))
        return EXCEPTION_CONTINUE_SEARCH;

    LogWarn("CrashDump: exception 0x{:08X} at 0x{:08X}", pRecord->ExceptionCode,
        reinterpret_cast<uintptr_t>(pRecord->ExceptionAddress));

    WriteDump(pInfo);
    TerminateProcess(GetCurrentProcess(), pRecord->ExceptionCode);

    return EXCEPTION_CONTINUE_SEARCH;
}

static void __fastcall OnInitDialog(void*, void*)
{
    WriteDump(nullptr);
    TerminateProcess(GetCurrentProcess(), 1);
}

static void InitWindow()
{
    auto hWindow = GetModuleHandleW(L"Window.dll");
    if (!hWindow)
        return;

    if (auto p = GetProcAddress(hWindow, "?OnInitDialog@WCrashBoxDialog@@UAEXXZ"))
        shOnInitDialog = safetyhook::create_inline(p, OnInitDialog);

    if (!shOnInitDialog)
        LogWarn("CrashDump: Window.dll's WCrashBoxDialog::OnInitDialog could not be hooked, crashes still show the box");
}

class CrashDump
{
public:
    CrashDump()
    {
        MongooseFix::onInitEvent() += []()
        {
            if (!AddVectoredExceptionHandler(1, OnException))
                LogWarn("CrashDump: the exception handler could not be installed, faults are only caught at the box");
        };

        MongooseFix::onWindowInitEvent() += []() { InitWindow(); };
    }
} CrashDump;
