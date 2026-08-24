module;

#include <common.hxx>
#include <FileWatch.hpp>
#include <variant>

export module settings;

import common;

export enum Pref
{
    PREF_RESOLUTIONX,
    PREF_RESOLUTIONY,
    PREF_VSYNC,
    PREF_MAXFRAMERATE,
    PREF_ANISOTROPICFILTERING,
    PREF_MOUSESENSITIVITY,
    PREF_MOUSESMOOTHING,
    PREF_FIELDOFVIEW,
    PREF_COMICPANELSCALING,
    PREF_COMICPANELSCALE,

    COUNT,
};

// The value the ini shipped with for FieldOfView, and the value the game's own script uses as
// its base. Anything derived from FOV (weapon zoom levels, the viewmodel offset) is authored
// against this number, so it is the reference the FOV module scales from.
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

        // 0 on either axis means "whatever the desktop is", which is what almost everyone wants
        // and what the game cannot work out for itself.
        auto nResolutionX = iniReader.ReadInteger("Display", "ResolutionX", 0);
        auto nResolutionY = iniReader.ReadInteger("Display", "ResolutionY", 0);
        mPrefs[PREF_RESOLUTIONX] = nResolutionX < 1 ? 0 : std::clamp(nResolutionX, 320, 16384);
        mPrefs[PREF_RESOLUTIONY] = nResolutionY < 1 ? 0 : std::clamp(nResolutionY, 240, 16384);

        mPrefs[PREF_VSYNC] = std::clamp(iniReader.ReadInteger("Display", "VSync", 1), 0, 1);

        // 0 unlocks. Anything above 0 is handed to the engine's own tick rate governor, which
        // is a sleep rather than a spin, so a cap is cheaper than none.
        mPrefs[PREF_MAXFRAMERATE] = std::clamp(iniReader.ReadInteger("Display", "MaxFrameRate", 0), 0, 9999);

        mPrefs[PREF_ANISOTROPICFILTERING] = std::clamp(iniReader.ReadInteger("Graphics", "AnisotropicFiltering", 16), 0, 16);

        mPrefs[PREF_MOUSESMOOTHING] = std::clamp(iniReader.ReadInteger("Input", "MouseSmoothing", 0), 0, 1);

        auto fMouseSensitivity = iniReader.ReadFloat("Input", "MouseSensitivity", 1.0f);
        mPrefs[PREF_MOUSESENSITIVITY] = fMouseSensitivity <= 0.0f ? 1.0f : std::clamp(fMouseSensitivity, 0.01f, 10.0f);

        mPrefs[PREF_FIELDOFVIEW] = std::clamp(iniReader.ReadFloat("FieldOfView", "FieldOfView", fStockFieldOfView), 45.0f, 145.0f);

        mPrefs[PREF_COMICPANELSCALING] = std::clamp(iniReader.ReadInteger("Interface", "ComicPanelScaling", 1), 0, 2);

        // 0 asks for the factor to be worked out from the height the art was drawn for.
        auto fComicPanelScale = iniReader.ReadFloat("Interface", "ComicPanelScale", 0.0f);
        mPrefs[PREF_COMICPANELSCALE] = fComicPanelScale <= 0.0f ? 0.0f : std::clamp(fComicPanelScale, 0.25f, 4.0f);

        // The watcher is what makes every setting live. It is installed once, on the first read.
        static std::once_flag flag;
        std::call_once(flag, [&]()
        {
            if (!std::filesystem::exists(iniReader.GetIniPath()))
                return;

            static filewatch::FileWatch<std::string> watch(iniReader.GetIniPath().string(),
                [](const std::string&, const filewatch::Event change_type)
                {
                    if (change_type != filewatch::Event::modified)
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

// Reading a setting into a variable now and on every re-read is what nearly every module wants
// to do with nearly every setting, so it is one call. T covers plain and atomic destinations.
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

// A byte patch written while the setting is on and put back while it is off.
export void BindPatch(raw_mem& patch, Pref pref)
{
    ApplyAndWatch([&patch, pref]() { patch.Set(MongooseFixSettings.GetInt(pref) != 0); });
}
