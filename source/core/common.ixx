module;

#include <common.hxx>
#include <stacktrace>

export module common;

// Modules register at static init and the loader fires the lists in order. Priority exists for the
// log header and the update prompt that blocks startup.
export class MongooseFix
{
public:
    template<typename... Args>
    class Event
    {
    public:
        static constexpr auto nDefaultPriority = 100;

        void operator+=(std::function<void(Args...)>&& handler)
        {
            add(std::move(handler), nDefaultPriority);
        }

        void add(std::function<void(Args...)>&& handler, int priority)
        {
            handlers.emplace_back(priority, std::move(handler));
        }

        // Lower priority first; ties keep registration order, hence the stable sort.
        void executeAll(Args... args) const
        {
            auto ordered = handlers;
            std::stable_sort(ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            for (auto& handler : ordered)
                handler.second(args...);
        }

    private:
        std::vector<std::pair<int, std::function<void(Args...)>>> handlers;
    };

    // Fires once the asi is loaded, before the engine has done anything.
    static Event<>& onInitEvent()
    {
        static Event<> e;
        return e;
    }

    // Modal prompts, on the main thread ahead of onInitEvent, so nothing loads until answered.
    static Event<>& onStartupPromptEvent()
    {
        static Event<> e;
        return e;
    }

    // The exe loads these well after the asi is mapped, so patches into them wait on their own
    // event rather than onInitEvent.
    static Event<>& onEngineInitEvent() { static Event<> e; return e; }
    static Event<>& onD3DDrvInitEvent() { static Event<> e; return e; }
    static Event<>& onGUIInitEvent()    { static Event<> e; return e; }
    static Event<>& onCoreInitEvent()   { static Event<> e; return e; }
    static Event<>& onWinDrvInitEvent() { static Event<> e; return e; }
    static Event<>& onWindowInitEvent() { static Event<> e; return e; }

    static Event<>& onIniFileChange()   { static Event<> e; return e; }
    static Event<>& onShutdownEvent()   { static Event<> e; return e; }
};

// Apply now and on every ini re-read. Everything user facing goes through this, so no setting is
// startup-only.
export inline void ApplyAndWatch(std::function<void()> fn)
{
    fn();
    MongooseFix::onIniFileChange() += std::function<void()>(std::move(fn));
}

export template<class T = std::filesystem::path>
T GetModulePath(HMODULE hModule)
{
    static constexpr auto INITIAL_BUFFER_SIZE = MAX_PATH;
    static constexpr auto MAX_ITERATIONS = 7;

    if constexpr (std::is_same_v<T, std::filesystem::path>)
    {
        std::u16string ret;
        auto bufferSize = INITIAL_BUFFER_SIZE;
        for (size_t i = 0; i < MAX_ITERATIONS; ++i)
        {
            ret.resize(bufferSize);
            auto written = GetModuleFileNameW(hModule, (LPWSTR)&ret[0], bufferSize);
            if (written < ret.length())
            {
                ret.resize(written);
                return std::filesystem::path(ret);
            }
            bufferSize *= 2;
        }
    }
    else
    {
        T ret;
        auto bufferSize = INITIAL_BUFFER_SIZE;
        for (size_t i = 0; i < MAX_ITERATIONS; ++i)
        {
            ret.resize(bufferSize);
            size_t written = 0;
            if constexpr (std::is_same_v<T, std::string>)
                written = GetModuleFileNameA(hModule, &ret[0], bufferSize);
            else
                written = GetModuleFileNameW(hModule, &ret[0], bufferSize);
            if (written < ret.length())
            {
                ret.resize(written);
                return ret;
            }
            bufferSize *= 2;
        }
    }
    return T();
}

export template<class T = std::filesystem::path>
T GetThisModulePath()
{
    HMODULE hm = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)&MongooseFix::onInitEvent, &hm);
    T r = GetModulePath<T>(hm);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return r.parent_path();
    else if constexpr (std::is_same_v<T, std::string>)
        return r.substr(0, r.find_last_of("/\\") + 1);
    else
        return r.substr(0, r.find_last_of(L"/\\") + 1);
}

export template<class T = std::filesystem::path>
T GetExeModulePath()
{
    T r = GetModulePath<T>(nullptr);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return r.parent_path();
    else if constexpr (std::is_same_v<T, std::string>)
        return r.substr(0, r.find_last_of("/\\") + 1);
    else
        return r.substr(0, r.find_last_of(L"/\\") + 1);
}

export template<class T = std::filesystem::path>
T GetExeModuleName()
{
    const T name = GetModulePath<T>(nullptr);
    if constexpr (std::is_same_v<T, std::filesystem::path>)
        return name.filename();
    else if constexpr (std::is_same_v<T, std::string>)
        return name.substr(name.find_last_of("/\\") + 1);
    else
        return name.substr(name.find_last_of(L"/\\") + 1);
}

// By process rather than by class name: XIII does not use the WWindowsViewportWindow class every
// other UE2 game does. EnumWindows is top level only, and an owner means a dialog.
export inline HWND FindGameWindow()
{
    struct Search { DWORD nProcessId; HWND hWindow; } search{ GetCurrentProcessId(), nullptr };

    EnumWindows([](HWND hWindow, LPARAM lParam) -> BOOL
    {
        auto& search = *reinterpret_cast<Search*>(lParam);

        DWORD nProcessId = 0;
        GetWindowThreadProcessId(hWindow, &nProcessId);

        if (nProcessId != search.nProcessId || !IsWindowVisible(hWindow) || GetWindow(hWindow, GW_OWNER))
            return TRUE;

        search.hWindow = hWindow;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));

    return search.hWindow;
}

export inline bool IsModuleUAL(HMODULE mod)
{
    return GetProcAddress(mod, "IsUltimateASILoader") != nullptr;
}

// UAL calls InitializeASI itself and other loaders do not, so the stack is walked to tell them
// apart.
export bool IsUALPresent()
{
    for (const auto& entry : std::stacktrace::current())
    {
        HMODULE hModule = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)entry.native_handle(), &hModule))
        {
            if (IsModuleUAL(hModule))
                return true;
        }
    }
    return false;
}

export class CallbackHandler
{
public:
    static inline void RegisterCallback(std::function<void()>&& fn)
    {
        fn();
    }

    static inline void RegisterCallback(std::wstring_view module_name, std::function<void()>&& fn)
    {
        if (module_name.empty() || GetModuleHandleW(module_name.data()) != nullptr)
        {
            fn();
        }
        else
        {
            RegisterDllNotification();
            GetOnModuleLoadCallbackList().emplace(module_name, std::forward<std::function<void()>>(fn));
        }
    }

private:
    struct Comparator
    {
        bool operator()(const std::wstring& a, const std::wstring& b) const
        {
            std::wstring l(a.length(), ' '), r(b.length(), ' ');
            std::transform(a.begin(), a.end(), l.begin(), tolower);
            std::transform(b.begin(), b.end(), r.begin(), tolower);
            return l < r;
        }
    };

    static inline std::map<std::wstring, std::function<void()>, Comparator>& GetOnModuleLoadCallbackList()
    {
        static std::map<std::wstring, std::function<void()>, Comparator> list;
        return list;
    }

    static inline void invokeOnModuleLoad(std::wstring_view module_name)
    {
        auto& list = GetOnModuleLoadCallbackList();
        auto it = list.find(module_name.data());
        if (it != list.end())
            it->second();
    }

    typedef NTSTATUS(NTAPI* _LdrRegisterDllNotification)(ULONG, PVOID, PVOID, PVOID);

    typedef struct _LDR_DLL_LOADED_NOTIFICATION_DATA
    {
        ULONG Flags;
        PUNICODE_STRING FullDllName;
        PUNICODE_STRING BaseDllName;
        PVOID DllBase;
        ULONG SizeOfImage;
    } LDR_DLL_LOADED_NOTIFICATION_DATA, LDR_DLL_UNLOADED_NOTIFICATION_DATA, *PLDR_DLL_LOADED_NOTIFICATION_DATA;

    typedef union _LDR_DLL_NOTIFICATION_DATA
    {
        LDR_DLL_LOADED_NOTIFICATION_DATA Loaded;
        LDR_DLL_UNLOADED_NOTIFICATION_DATA Unloaded;
    } LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

    static inline void CALLBACK LdrDllNotification(ULONG reason, PLDR_DLL_NOTIFICATION_DATA data, PVOID)
    {
        static constexpr auto LDR_DLL_NOTIFICATION_REASON_LOADED = 1;
        if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED)
            invokeOnModuleLoad(data->Loaded.BaseDllName->Buffer);
    }

    static inline void RegisterDllNotification()
    {
        LdrRegisterDllNotification = (_LdrRegisterDllNotification)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
        if (LdrRegisterDllNotification && !cookie)
            LdrRegisterDllNotification(0, LdrDllNotification, 0, &cookie);
    }

    static inline _LdrRegisterDllNotification LdrRegisterDllNotification;
    static inline void* cookie;
public:
    static inline std::once_flag flag;
};

// First pattern that matches, so one patch body can cover more than one build.
export template <size_t count = 1, typename... Args>
hook::pattern find_pattern(Args... args)
{
    hook::pattern pattern;
    ((pattern = hook::pattern(args), !pattern.count_hint(count).empty()) || ...);
    return pattern;
}

// One module only: XIII spreads its code over five binaries, so a program-wide scan is slower and
// ambiguous. An unloaded module gives an empty pattern, never a scan of whatever else is mapped.
//
// One wildcard byte is one "?": Hooking.Patterns counts each question mark as a byte, so "??" is
// two wildcards and never matches.
export inline hook::pattern module_pattern(const wchar_t* module_name, std::string_view bytes)
{
    auto hModule = GetModuleHandleW(module_name);
    return hModule ? hook::module_pattern(hModule, bytes) : hook::pattern();
}

// Settings are live, so every patch has to be reversible at any moment, from any thread.
export class raw_mem
{
public:
    raw_mem(injector::memory_pointer_tr addr, std::initializer_list<uint8_t> bytes, bool offset_back = false)
    {
        ptr = addr.as_int() - (offset_back ? bytes.size() : 0);
        new_code.assign(std::move(bytes));
        old_code.resize(new_code.size());
        ReadMemoryRaw(ptr, old_code.data(), old_code.size(), true);
    }

    void Write()
    {
        std::lock_guard g(mtx);
        WriteMemoryRaw(ptr, new_code.data(), new_code.size(), true);
    }

    void Restore()
    {
        std::lock_guard g(mtx);
        WriteMemoryRaw(ptr, old_code.data(), old_code.size(), true);
    }

    void Set(bool bOn)
    {
        bOn ? Write() : Restore();
    }

    size_t Size() const { return old_code.size(); }

private:
    // injector's scoped_unprotect does no locking, and the ini watcher thread can re-apply a
    // patch while the engine thread is applying the same one.
    //
    // ponytail: one lock for all patches. Split it only if patch writes ever get hot.
    static inline std::mutex mtx;

    injector::memory_pointer ptr;
    std::vector<uint8_t> old_code;
    std::vector<uint8_t> new_code;
};
