module;

#include <common.hxx>
#include <FileWatch.hpp>
#include <variant>

export module settings;

import common;

export enum Pref
{
    PREF_DISPLAYMODE,
    PREF_RESOLUTIONX,
    PREF_RESOLUTIONY,
    PREF_VSYNC,
    PREF_MAXFRAMERATE,
    PREF_DIRECTXVERSION,
    PREF_ANISOTROPICFILTERING,
    PREF_MSAA,
    PREF_INTERNALRESX,
    PREF_INTERNALRESY,
    PREF_SCALINGFILTER,
    PREF_MOUSESENSITIVITY,
    PREF_MOUSESMOOTHING,
    PREF_PADSENSITIVITY,
    PREF_VIBRATION,
    PREF_PADLAYOUT,
    PREF_FIELDOFVIEW,
    PREF_SKIPINTROMOVIES,
    PREF_ALLOWCHEATS,

    COUNT,
};

// Script FOV base. Weapon zoom levels and the viewmodel offset are authored against it, so the FOV
// module scales from it.
export inline constexpr auto fStockFieldOfView = 85.0f;

export class CSettings
{
private:
    using PrefValue = std::variant<int32_t, float>;
    static inline std::array<PrefValue, static_cast<size_t>(Pref::COUNT)> mPrefs;

public:
    static inline void ReadIniSettings()
    {
        CIniReader iniReader("");

        // A failed open yields all defaults. The menu replaces the ini by rename and the watcher
        // fires around it, so a read landing in that window would reset every setting to stock.
        std::error_code ec;
        if (std::filesystem::exists(iniReader.GetIniPath(), ec)
            && iniReader.ReadInteger("Display", "DisplayMode", -1) == -1)
            return;

        // (0) windowed, (1) borderless, (2) fullscreen.
        mPrefs[PREF_DISPLAYMODE] = std::clamp(iniReader.ReadInteger("Display", "DisplayMode", 2), 0, 2);

        // 0 on either axis means the desktop.
        auto nResolutionX = iniReader.ReadInteger("Display", "ResolutionX", 0);
        auto nResolutionY = iniReader.ReadInteger("Display", "ResolutionY", 0);
        mPrefs[PREF_RESOLUTIONX] = nResolutionX < 1 ? 0 : std::clamp(nResolutionX, 320, 16384);
        mPrefs[PREF_RESOLUTIONY] = nResolutionY < 1 ? 0 : std::clamp(nResolutionY, 240, 16384);

        mPrefs[PREF_VSYNC] = std::clamp(iniReader.ReadInteger("Display", "VSync", 1), 0, 1);

        // 0 unlocks. Anything else is paced by the fix itself; the engine's own wait is bypassed.
        mPrefs[PREF_MAXFRAMERATE] = std::clamp(iniReader.ReadInteger("Display", "MaxFrameRate", 0), 0, 9999);

        // (0) Direct3D 8, (1) Direct3D 9.
        mPrefs[PREF_DIRECTXVERSION] = std::clamp(iniReader.ReadInteger("Graphics", "DirectXVersion", 0), 0, 1);

        mPrefs[PREF_ANISOTROPICFILTERING] = std::clamp(iniReader.ReadInteger("Graphics", "AnisotropicFiltering", 16), 0, 16);

        // 0, 2, 4 or 8; anything between rounds down to the next real level.
        auto nMSAA = std::clamp(iniReader.ReadInteger("Graphics", "MSAA", 0), 0, 8);
        mPrefs[PREF_MSAA] = nMSAA >= 8 ? 8 : nMSAA >= 4 ? 4 : nMSAA >= 2 ? 2 : 0;

        auto nInternalX = iniReader.ReadInteger("Display", "InternalResolutionX", 0);
        auto nInternalY = iniReader.ReadInteger("Display", "InternalResolutionY", 0);
        const auto bInternal = nInternalX > 0 && nInternalY > 0;
        // 8192, not the 16384 the ini used to document: past that the card dies rather than slows.
        mPrefs[PREF_INTERNALRESX] = bInternal ? std::clamp(nInternalX, 320, 8192) : 0;
        mPrefs[PREF_INTERNALRESY] = bInternal ? std::clamp(nInternalY, 240, 8192) : 0;
        mPrefs[PREF_SCALINGFILTER] = std::clamp(iniReader.ReadInteger("Display", "ScalingFilter", 1), 0, 1);

        mPrefs[PREF_MOUSESMOOTHING] = std::clamp(iniReader.ReadInteger("Gameplay", "MouseSmoothing", 0), 0, 1);

        auto fMouseSensitivity = iniReader.ReadFloat("Gameplay", "MouseLookSensitivity", 1.0f);
        mPrefs[PREF_MOUSESENSITIVITY] = fMouseSensitivity <= 0.0f ? 1.0f : std::clamp(fMouseSensitivity, 0.01f, 20.0f);

        auto fPadSensitivity = iniReader.ReadFloat("Controller", "ControllerLookSensitivity", 1.0f);
        mPrefs[PREF_PADSENSITIVITY] = fPadSensitivity <= 0.0f ? 1.0f : std::clamp(fPadSensitivity, 0.01f, 20.0f);

        mPrefs[PREF_VIBRATION] = std::clamp(iniReader.ReadInteger("Controller", "Vibration", 1), 0, 1);
        mPrefs[PREF_PADLAYOUT] = std::clamp(iniReader.ReadInteger("Controller", "Layout", 0), 0, 3);

        mPrefs[PREF_FIELDOFVIEW] = std::clamp(iniReader.ReadFloat("FieldOfView", "FieldOfView", fStockFieldOfView), 45.0f, 145.0f);

        mPrefs[PREF_SKIPINTROMOVIES] = std::clamp(iniReader.ReadInteger("General", "SkipIntro", 1), 0, 1);

        mPrefs[PREF_ALLOWCHEATS] = std::clamp(iniReader.ReadInteger("General", "AllowCheats", 0), 0, 1);

        static std::once_flag flag;
        std::call_once(flag, [&]()
        {
            if (!std::filesystem::exists(iniReader.GetIniPath()))
                return;

            static filewatch::FileWatch<std::string> watch(iniReader.GetIniPath().string(),
                [](const std::string&, const filewatch::Event change_type)
                {
                    // The menu replaces the file by rename, so the change arrives as a rename
                    // rather than a write.
                    if (change_type != filewatch::Event::modified
                        && change_type != filewatch::Event::added
                        && change_type != filewatch::Event::renamed_new)
                        return;

                    ReadIniSettings();
                    MongooseFix::onIniFileChange().executeAll();
                });
        });
    }

public:
    int32_t GetInt(Pref name) { return std::get<int32_t>(mPrefs[name]); }
    float GetFloat(Pref name) { return std::get<float>(mPrefs[name]); }
    void SetInt(Pref name, int32_t value) { mPrefs[name] = value; }
    void SetFloat(Pref name, float value) { mPrefs[name] = value; }
} MongooseFixSettings;

// T covers plain and atomic destinations.
export template<class T>
void BindBool(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = MongooseFixSettings.GetInt(pref) != 0; });
}

export template<class T>
void BindInt(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = MongooseFixSettings.GetInt(pref); });
}

export template<class T>
void BindFloat(T& target, Pref pref)
{
    ApplyAndWatch([&target, pref]() { target = MongooseFixSettings.GetFloat(pref); });
}

export void BindPatch(raw_mem& patch, Pref pref)
{
    ApplyAndWatch([&patch, pref]() { patch.Set(MongooseFixSettings.GetInt(pref) != 0); });
}
