module;

#include <common.hxx>
#include <cstdio>
#include <fstream>

export module mongoosemenu;

import common;
import display;

// An in game page for the ini, drawn with the engine's own text and box primitives.
//
// Laid out in the 640x480 units XIDInterf.u was authored in, times the canvas ratios, like a
// script Paint, so hudscale's interface pass moves and scales it with the real pages.
//
// Offsets from XIII's own Core.dll; other UE2 builds lay these out four bytes further along:
//   UObject::Class      +0x24  UObject::GetClass          mov eax,[ecx+0x24]
//   UObject::Name       +0x20  UObject::GetFName          mov ecx,[ecx+0x20]
//   UField::SuperField  +0x28  UStruct::IsChildOf         mov ecx,[ecx+0x28]
//   UField::Next        +0x2C  UStruct::Link              local_1c = [local_1c+0x2c]
//   UStruct::Children   +0x40  UStruct::Link              piVar6 = [this+0x40]
//   UProperty::Offset   +0x34  UStruct::Link              PropertiesSize = ElementSize*ArrayDim + [p+0x34]
// Offset is a dword here, not the word other builds pack it into.
static constexpr auto nOffsetObjectClass = 0x24;
static constexpr auto nOffsetFieldName = 0x20;
static constexpr auto nOffsetFieldSuper = 0x28;
static constexpr auto nOffsetFieldNext = 0x2C;
static constexpr auto nOffsetStructChildren = 0x40;
static constexpr auto nOffsetPropertyOffset = 0x34;

// UCanvas, taken from UCanvas::execDrawText.
static constexpr auto nOffsetFont = 0x28;
static constexpr auto nOffsetClipX = 0x3C;
static constexpr auto nOffsetClipY = 0x40;
static constexpr auto nOffsetCurX = 0x44;
static constexpr auto nOffsetCurY = 0x48;
static constexpr auto nOffsetStyle = 0x50;
static constexpr auto nOffsetColour = 0x58;
static constexpr auto nOffsetViewport = 0x74;

// UViewport::Interactions, the array UInteractionMaster::MasterProcessKeyEvent walks, then the
// screen size UCanvas::WrappedPrint clips against.
static constexpr auto nOffsetInteractions = 0x58;
static constexpr auto nOffsetViewportSizeX = 0x88;
static constexpr auto nOffsetViewportSizeY = 0x8C;

static constexpr auto fAuthoredWidth = 640.0f;
static constexpr auto fAuthoredHeight = 480.0f;

// XIIIWindow::DisplayHelpBar, PC branch: a 32 unit bar 30 in from each edge and 6 up.
static constexpr auto fBarHeight = 32.0f;
static constexpr auto fBarInset = 30.0f;
static constexpr auto fBarBottom = 6.0f;

// UCanvas::Style. 1 draws opaque, 5 blends the alpha in DrawColor.
static constexpr auto nStyleNormal = 1;
static constexpr auto nStyleAlpha = 5;

// UCanvas::WrappedPrint skips the draw at STY_None and still fills in the metrics.
static constexpr auto nStyleNone = 0;

static constexpr auto nFindName = 0;

struct FColour { uint8_t B, G, R, A; };

static constexpr FColour White{ 255, 255, 255, 255 };
static constexpr FColour Black{ 0, 0, 0, 255 };
static constexpr FColour Grey{ 127, 127, 127, 255 };

using FNameCtor_t = void(__thiscall*)(void*, const char*, int, bool);
using GetName_t = const char*(__thiscall*)(void*);
using WrappedPrint_t = void(__cdecl*)(void*, int, int*, int*, void*, int, const char*);
using DrawTileStretched_t = void(__thiscall*)(void*, void*, float, float, float, float);

struct FPlane { float X, Y, Z, W; };

using DrawTile_t = void(__thiscall*)(void*, void*, float, float, float, float, float, float, float,
    float, float, FPlane, FPlane);
using StaticFindObject_t = void*(__cdecl*)(void*, void*, const char*, int);
using PlayMenuSound_t = void(__thiscall*)(void*, void*, void*, void*, int32_t);

static FNameCtor_t pFNameCtor = nullptr;
static GetName_t pGetName = nullptr;
static WrappedPrint_t pWrappedPrint = nullptr;
static DrawTileStretched_t pDrawTileStretched = nullptr;
static DrawTile_t pCanvasDrawTile = nullptr;
static StaticFindObject_t pStaticFindObject = nullptr;

static SafetyHookInline shKeyEvent{};

template<class T>
static T Read(void* pBase, size_t nOffset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(pBase) + nOffset);
}

// Canvas::Font is null after the interface pass, and a font found by name is not necessarily one
// the text path can use, so borrow the font the menus drew their help bar with.
static void* pGuiFont = nullptr;

export void MongooseMenuNoteFont(void* pFont)
{
    if (pFont)
        pGuiFont = pFont;
}

static void* MenuFont(void* pCanvas)
{
    return pGuiFont ? pGuiFont : Read<void*>(pCanvas, nOffsetFont);
}

// UBoolProperty::BitMask, the same offset menuscale reads it at.
static constexpr auto nOffsetBoolBitMask = 0x64;

// Searched up the whole class chain: UObject::FindObjectField hashes one class's own fields, so
// an inherited var never resolves through it.
static void* FindProperty(void* pObject, const char* szName)
{
    if (!pObject || !pFNameCtor)
        return nullptr;

    int32_t nName = 0;
    pFNameCtor(&nName, szName, nFindName, false);

    if (!nName)
        return nullptr;

    for (auto pClass = Read<void*>(pObject, nOffsetObjectClass); pClass;
        pClass = Read<void*>(pClass, nOffsetFieldSuper))
    {
        for (auto pField = Read<void*>(pClass, nOffsetStructChildren); pField;
            pField = Read<void*>(pField, nOffsetFieldNext))
        {
            if (Read<int32_t>(pField, nOffsetFieldName) == nName)
                return pField;
        }
    }

    return nullptr;
}

static void* Field(void* pObject, const char* szName)
{
    auto pProperty = FindProperty(pObject, szName);

    return pProperty ? static_cast<uint8_t*>(pObject) + Read<int32_t>(pProperty, nOffsetPropertyOffset)
        : nullptr;
}

static void SetBool(void* pObject, const char* szName, bool bOn)
{
    auto pProperty = FindProperty(pObject, szName);
    if (!pProperty)
        return;

    auto pStorage = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pObject)
        + Read<int32_t>(pProperty, nOffsetPropertyOffset));
    const auto nMask = Read<uint32_t>(pProperty, nOffsetBoolBitMask);

    if (bOn)
        *pStorage |= nMask;
    else
        *pStorage &= ~nMask;
}

static const char* ClassName(void* pObject)
{
    if (!pObject || !pGetName)
        return "";

    auto pClass = Read<void*>(pObject, nOffsetObjectClass);
    return pClass ? pGetName(pClass) : "";
}

// By the whole chain, not the leaf: the interface object the game builds is a subclass.
static bool IsA(void* pObject, const char* szName)
{
    if (!pObject || !pGetName)
        return false;

    for (auto pClass = Read<void*>(pObject, nOffsetObjectClass); pClass;
        pClass = Read<void*>(pClass, nOffsetFieldSuper))
    {
        if (std::strcmp(pGetName(pClass), szName) == 0)
            return true;
    }

    return false;
}

// The interface is an Interaction on the viewport, so no sweep of the object table.
static void* RootWindow(void* pCanvas)
{
    auto pViewport = Read<void*>(pCanvas, nOffsetViewport);
    if (!pViewport)
        return nullptr;

    auto ppItems = Read<void**>(pViewport, nOffsetInteractions);
    const auto nCount = Read<int32_t>(pViewport, nOffsetInteractions + sizeof(void*));

    for (auto i = 0; i < nCount && ppItems; i++)
    {
        if (IsA(ppItems[i], "GUIController"))
            return ppItems[i];
    }

    return nullptr;
}

// The Options grid is not in array order, so the topmost control is found by WinTop, not index.
enum class Focus { None, Top, Middle, Bottom };

static void* FocusedControl(void* pRoot)
{
    auto pPageField = Field(pRoot, "ActivePage");
    auto pPage = pPageField ? *reinterpret_cast<void**>(pPageField) : nullptr;
    auto pField = pPage ? Field(pPage, "FocusedControl") : nullptr;

    return pField ? *reinterpret_cast<void**>(pField) : nullptr;
}

static Focus FocusPosition(void* pRoot)
{
    auto pPageField = Field(pRoot, "ActivePage");
    auto pPage = pPageField ? *reinterpret_cast<void**>(pPageField) : nullptr;
    auto pFocused = FocusedControl(pRoot);
    auto pControlsField = pPage ? Field(pPage, "Controls") : nullptr;

    if (!pFocused || !pControlsField)
        return Focus::None;

    auto ppControls = *reinterpret_cast<void***>(pControlsField);
    const auto nCount = Read<int32_t>(pControlsField, sizeof(void*));

    const auto Top = [](void* pControl) -> float
    {
        auto pField = Field(pControl, "WinTop");
        return pField ? *reinterpret_cast<float*>(pField) : 0.0f;
    };

    const auto fMine = Top(pFocused);
    auto fHighest = fMine;
    auto fLowest = fMine;

    for (auto i = 0; i < nCount && ppControls; i++)
    {
        if (!ppControls[i])
            continue;

        fHighest = (std::min)(fHighest, Top(ppControls[i]));
        fLowest = (std::max)(fLowest, Top(ppControls[i]));
    }

    if (fMine <= fHighest + 0.001f)
        return Focus::Top;

    return fMine >= fLowest - 0.001f ? Focus::Bottom : Focus::Middle;
}

static const char* ActivePageName(void* pRoot)
{
    auto pField = Field(pRoot, "ActivePage");
    if (!pField)
        return "";

    auto pPage = *reinterpret_cast<void**>(pField);
    return pPage ? ClassName(pPage) : "";
}

// Settings

enum class Kind { Toggle, Choice, Number, Scale };

// Declared here: the page pointer, its list and the swallowed keys all key off it.
static bool bPageOpen = false;

// Interface screen space, where the mouse is reported.
static float fScreenW = 0.0f;
static float fScreenH = 0.0f;

// What the render resolution is a percentage of. Never the viewport: with an internal resolution
// live it carries the render size, so the percentage reads as 100 and writes back out as off.
// internalres scales from the render device's output size.
static float fOutputW = 0.0f;
static float fOutputH = 0.0f;

static void ReadOutputSize()
{
    const auto nX = GetDisplayOutputWidth();
    const auto nY = GetDisplayOutputHeight();

    if (nX > 0 && nY > 0)
    {
        fOutputW = static_cast<float>(nX);
        fOutputH = static_cast<float>(nY);
        return;
    }

    // Before the first SetRes there is no output size yet.
    auto hWindow = FindGameWindow();
    hWindow = hWindow ? GetAncestor(hWindow, GA_ROOT) : nullptr;

    RECT client{};
    if (hWindow && GetClientRect(hWindow, &client) && client.right > 0 && client.bottom > 0)
    {
        fOutputW = static_cast<float>(client.right);
        fOutputH = static_cast<float>(client.bottom);
    }
}

// The ini's documented 16384 per axis takes the device out at a 4K window; 8192 is the real ceiling.
static constexpr auto nScaleStep = 10;
static constexpr auto nScaleFloor = 20;
static constexpr auto nScaleCeiling = 200;
static constexpr auto nMinAxisX = 320;
static constexpr auto nMinAxisY = 240;
static constexpr auto nMaxAxis = 8192;

// Even pixels: a half resolution pass trips over an odd target.
static int ScalePixels(float fBase, float fPercent)
{
    return static_cast<int>(fBase * fPercent / 100.0f + 0.5f) & ~1;
}

static bool ScaleFits(float fPercent)
{
    if (fOutputW <= 0.0f || fOutputH <= 0.0f)
        return true;

    const auto nX = ScalePixels(fOutputW, fPercent);
    const auto nY = ScalePixels(fOutputH, fPercent);

    return nX >= nMinAxisX && nY >= nMinAxisY && nX <= nMaxAxis && nY <= nMaxAxis;
}

// The renderer is picked when the device is made, so a change is next launch's.
static bool bRestartNotice = false;

struct Choice
{
    float fValue;
    const char* szLabel;
};

struct Setting
{
    const char* szSection;
    const char* szKey;
    const char* szLabel;
    Kind eKind;
    const Choice* pChoices;
    int nChoices;
    float fMin;
    float fMax;
    float fStep;
    int nDecimals;
    float fDefault;
    float fValue;

    // Only what the player moved is written back, so a percentage derived under one output size
    // is never rewritten as pixels under another.
    bool bChanged;
};

static constexpr Choice aDisplayMode[]{ { 0, "Windowed" }, { 1, "Borderless" }, { 2, "Fullscreen" } };
static constexpr Choice aWidth[]{ { 0, "Desktop" }, { 640, "640" }, { 800, "800" }, { 1024, "1024" },
    { 1280, "1280" }, { 1366, "1366" }, { 1600, "1600" }, { 1920, "1920" }, { 2560, "2560" }, { 3840, "3840" } };
static constexpr Choice aHeight[]{ { 0, "Desktop" }, { 480, "480" }, { 600, "600" }, { 720, "720" },
    { 768, "768" }, { 900, "900" }, { 1080, "1080" }, { 1440, "1440" }, { 2160, "2160" } };
static constexpr Choice aScalingFilter[]{ { 0, "Point" }, { 1, "Bilinear" } };
static constexpr Choice aMaxFrameRate[]{ { 0, "Unlocked" }, { 1, "Monitor" }, { 30, "30" }, { 60, "60" },
    { 72, "72" }, { 90, "90" }, { 120, "120" }, { 144, "144" }, { 165, "165" }, { 240, "240" }, { 360, "360" } };
static constexpr Choice aDirectX[]{ { 0, "DirectX 8" }, { 1, "DirectX 9" } };
static constexpr Choice aAniso[]{ { 0, "Off" }, { 2, "2x" }, { 4, "4x" }, { 8, "8x" }, { 16, "16x" } };
static constexpr Choice aMSAA[]{ { 0, "Off" }, { 2, "2x" }, { 4, "4x" }, { 8, "8x" } };
static constexpr Choice aLayout[]{ { 0, "Classic Halo" }, { 1, "Goofy Halo" }, { 2, "Classic XIII" }, { 3, "Goofy XIII" } };

#define CHOICE(a) a, static_cast<int>(std::size(a))

// Same order and wording as the ini.
static Setting aSettings[]
{
    { "Display", "DisplayMode", "Display Mode", Kind::Choice, CHOICE(aDisplayMode), 0, 0, 0, 0, 1, 1 },
    { "Display", "ResolutionX", "Window Width", Kind::Choice, CHOICE(aWidth), 0, 0, 0, 0, 0, 0 },
    { "Display", "ResolutionY", "Window Height", Kind::Choice, CHOICE(aHeight), 0, 0, 0, 0, 0, 0 },
    // A percentage of the window: the ini keeps both axes, the page keeps the percentage.
    { "Display", "InternalResolutionX", "Render Resolution", Kind::Scale, nullptr, 0, 0, 0, 0, 0, 100, 100 },
    { "Display", "ScalingFilter", "Scaling Filter", Kind::Choice, CHOICE(aScalingFilter), 0, 0, 0, 0, 1, 1 },
    { "Display", "VSync", "V-Sync", Kind::Toggle, nullptr, 0, 0, 1, 1, 0, 1, 1 },
    { "Display", "MaxFrameRate", "Max Frame Rate", Kind::Choice, CHOICE(aMaxFrameRate), 0, 0, 0, 0, 1, 1 },
    { "Graphics", "DirectXVersion", "DirectX Version", Kind::Choice, CHOICE(aDirectX), 0, 0, 0, 0, 1, 1 },
    { "Graphics", "AnisotropicFiltering", "Anisotropic Filtering", Kind::Choice, CHOICE(aAniso), 0, 0, 0, 0, 16, 16 },
    { "Graphics", "MSAA", "MSAA", Kind::Choice, CHOICE(aMSAA), 0, 0, 0, 0, 0, 0 },
    { "Gameplay", "MouseLookSensitivity", "Mouse Look Sensitivity", Kind::Number, nullptr, 0, 0.1f, 20.0f, 0.1f, 2, 3.0f, 3.0f },
    { "Gameplay", "MouseSmoothing", "Mouse Smoothing", Kind::Toggle, nullptr, 0, 0, 1, 1, 0, 0, 0 },
    { "FieldOfView", "FieldOfView", "Field of View", Kind::Number, nullptr, 0, 45.0f, 145.0f, 1.0f, 2, 91.31f, 91.31f },
    { "Controller", "ControllerLookSensitivity", "Controller Look Sensitivity", Kind::Number, nullptr, 0, 0.1f, 20.0f, 0.1f, 2, 2.5f, 2.5f },
    { "Controller", "Vibration", "Vibration", Kind::Toggle, nullptr, 0, 0, 1, 1, 0, 1, 1 },
    { "Controller", "Layout", "Controller Layout", Kind::Choice, CHOICE(aLayout), 0, 0, 0, 0, 0, 0 },
    { "General", "SkipIntro", "Skip Intro", Kind::Toggle, nullptr, 0, 0, 1, 1, 0, 1, 1 },
    { "General", "AllowCheats", "Allow Cheats", Kind::Toggle, nullptr, 0, 0, 1, 1, 0, 0, 0 },
};

#undef CHOICE

static constexpr auto nRowCount = static_cast<int>(std::size(aSettings));

static std::string FormatValue(const Setting& s)
{
    char szText[64]{};

    if (s.eKind == Kind::Toggle)
        return s.fValue != 0.0f ? "On" : "Off";

    if (s.eKind == Kind::Scale)
    {
        std::snprintf(szText, sizeof(szText), "%d%% (%dx%d)", static_cast<int>(s.fValue),
            ScalePixels(fOutputW, s.fValue), ScalePixels(fOutputH, s.fValue));
        return szText;
    }

    if (s.eKind == Kind::Choice)
    {
        for (auto i = 0; i < s.nChoices; i++)
        {
            if (std::abs(s.pChoices[i].fValue - s.fValue) < 0.5f)
                return s.pChoices[i].szLabel;
        }

        std::snprintf(szText, sizeof(szText), "%d", static_cast<int>(s.fValue));
        return szText;
    }

    std::snprintf(szText, sizeof(szText), "%.*f", s.nDecimals, s.fValue);
    return szText;
}

static void StepValue(Setting& s, int nDirection)
{
    if (s.eKind == Kind::Toggle)
    {
        s.fValue = nDirection > 0 ? 0.0f : 1.0f;
        return;
    }

    if (s.eKind == Kind::Number)
    {
        s.fValue = std::clamp(s.fValue + s.fStep * nDirection, s.fMin, s.fMax);
        return;
    }

    if (s.eKind == Kind::Scale)
    {
        const auto fNext = std::clamp(s.fValue + nScaleStep * nDirection,
            static_cast<float>(nScaleFloor), static_cast<float>(nScaleCeiling));

        if (ScaleFits(fNext))
            s.fValue = fNext;

        return;
    }

    // Nearest listed value first, so a hand edited ini still steps from where it sits.
    auto nIndex = 0;
    auto fBest = std::abs(s.pChoices[0].fValue - s.fValue);

    for (auto i = 1; i < s.nChoices; i++)
    {
        const auto fDistance = std::abs(s.pChoices[i].fValue - s.fValue);
        if (fDistance < fBest)
        {
            fBest = fDistance;
            nIndex = i;
        }
    }

    s.fValue = s.pChoices[std::clamp(nIndex + nDirection, 0, s.nChoices - 1)].fValue;
}

static void Step(Setting& s, int nDirection)
{
    const auto fWas = s.fValue;
    StepValue(s, nDirection);

    if (s.fValue == fWas)
        return;

    s.bChanged = true;

    if (std::strcmp(s.szKey, "DirectXVersion") == 0)
        bRestartNotice = true;
}

// Ini

static std::filesystem::path IniPath()
{
    return CIniReader("").GetIniPath();
}

static void LoadValues()
{
    CIniReader ini("");

    for (auto& s : aSettings)
    {
        s.fValue = ini.ReadFloat(s.szSection, s.szKey, s.fDefault);
        s.bChanged = false;

        if (s.eKind == Kind::Scale)
        {
            // 0 in the ini means the window, 100%.
            const auto fPercent = (s.fValue > 0.0f && fOutputW > 0.0f)
                ? std::round(s.fValue / fOutputW * 100.0f / nScaleStep) * nScaleStep
                : 100.0f;

            s.fValue = std::clamp(fPercent, static_cast<float>(nScaleFloor),
                static_cast<float>(nScaleCeiling));

            if (!ScaleFits(s.fValue))
                s.fValue = 100.0f;
        }
    }
}

// CIniReader::Write would rewrite the file from the parsed pairs and lose each value's comment,
// so only the number between the equals sign and the comment is replaced. Written once on the way
// out: the watcher re-applies every change, and a per keypress display mode resets the device.
static bool bDirty = false;

static std::string ValueText(const Setting& s, bool bHeight = false)
{
    char szText[64]{};

    if (s.eKind == Kind::Number)
    {
        std::snprintf(szText, sizeof(szText), "%.*f", s.nDecimals, s.fValue);
        return szText;
    }

    if (s.eKind == Kind::Scale)
    {
        const auto fAxis = bHeight ? fOutputH : fOutputW;
        std::snprintf(szText, sizeof(szText), "%d",
            s.fValue == 100.0f ? 0 : ScalePixels(fAxis, s.fValue));
        return szText;
    }

    std::snprintf(szText, sizeof(szText), "%d", static_cast<int>(s.fValue));
    return szText;
}

// The scale row owns both axes, so the height key is matched by name, not by row.
static const Setting* ScaleForHeightKey(const std::string& sSection, const std::string& sKey)
{
    for (const auto& s : aSettings)
    {
        if (s.eKind == Kind::Scale && sSection == s.szSection && sKey == "InternalResolutionY")
            return &s;
    }

    return nullptr;
}

static void SaveValues()
{
    if (!bDirty)
        return;

    bDirty = false;

    const auto path = IniPath();

    std::ifstream in(path);
    if (!in)
        return;

    std::vector<std::string> aLines;
    std::string sLine;
    while (std::getline(in, sLine))
    {
        if (!sLine.empty() && sLine.back() == '\r')
            sLine.pop_back();
        aLines.push_back(sLine);
    }
    in.close();

    std::string sSection;

    for (auto& line : aLines)
    {
        const auto nFirst = line.find_first_not_of(" \t");
        if (nFirst == std::string::npos)
            continue;

        if (line[nFirst] == '[')
        {
            const auto nEnd = line.find(']', nFirst);
            if (nEnd != std::string::npos)
                sSection = line.substr(nFirst + 1, nEnd - nFirst - 1);
            continue;
        }

        const auto nEquals = line.find('=');
        if (nEquals == std::string::npos)
            continue;

        auto sKey = line.substr(nFirst, nEquals - nFirst);
        const auto nKeyEnd = sKey.find_last_not_of(" \t");
        sKey = nKeyEnd == std::string::npos ? std::string() : sKey.substr(0, nKeyEnd + 1);

        const Setting* pSetting = nullptr;
        auto bHeight = false;

        for (const auto& s : aSettings)
        {
            if (sSection == s.szSection && sKey == s.szKey)
            {
                pSetting = &s;
                break;
            }
        }

        if (!pSetting)
        {
            pSetting = ScaleForHeightKey(sSection, sKey);
            bHeight = pSetting != nullptr;
        }

        if (!pSetting || !pSetting->bChanged)
            continue;

        // Whatever follows the value stays put, comment column included.
        const auto sValue = ValueText(*pSetting, bHeight);
        const auto nComment = line.find("//", nEquals + 1);
        const auto nStart = line.find_first_not_of(" \t", nEquals + 1);
        const auto nHead = (nStart != std::string::npos && (nComment == std::string::npos || nStart < nComment))
            ? nStart : nEquals + 1;

        std::string sNew(sValue);

        if (nComment != std::string::npos && nComment > nHead)
            sNew.append(nComment - nHead > sNew.size() ? nComment - nHead - sNew.size() : 1, ' ');

        line = line.substr(0, nHead) + sNew
            + (nComment == std::string::npos ? std::string() : line.substr(nComment));
    }

    // Truncating in place lets the watcher read the ini half written and default the missing keys.
    // Written beside it and renamed over, so a reader sees either the old file or the new one.
    auto temp = path;
    temp += ".tmp";

    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out)
            return;

        for (const auto& line : aLines)
            out << line << "\n";

        if (!out.flush())
            return;
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(temp);
}

// Drawing

struct Rect { float fX, fY, fW, fH; };

struct Draw
{
    void* pCanvas;
    float fRatioX;
    float fRatioY;
    void* pFondMenu;
    void* pFondNoir;

    // Canvas units to the screen the mouse is reported in: the interface box is centred, the
    // cursor is clamped against the whole screen, so the two differ by the pillarbox.
    float fPillarX;
    float fPillarY;
};

static FColour Colour(void* pCanvas)
{
    return Read<FColour>(pCanvas, nOffsetColour);
}

static void SetColour(void* pCanvas, FColour colour)
{
    *reinterpret_cast<FColour*>(static_cast<uint8_t*>(pCanvas) + nOffsetColour) = colour;
}

static void Tile(const Draw& d, void* pMaterial, float fX, float fY, float fW, float fH,
    FColour colour, int nStyle)
{
    if (!pMaterial || !pDrawTileStretched)
        return;

    auto pStyle = static_cast<uint8_t*>(d.pCanvas) + nOffsetStyle;
    const auto nOldStyle = *pStyle;
    const auto oldColour = Colour(d.pCanvas);

    *pStyle = static_cast<uint8_t>(nStyle);
    SetColour(d.pCanvas, colour);

    pDrawTileStretched(d.pCanvas, pMaterial, fX * d.fRatioX, fY * d.fRatioY,
        fW * d.fRatioX, fH * d.fRatioY);

    *pStyle = nOldStyle;
    SetColour(d.pCanvas, oldColour);
}

// The comic outline every menu box has: black texture two units proud on each side, laid first,
// as XIIIMenuAudioClientWindow frames its panel.
static constexpr auto fOutline = 2.0f;

static void Box(const Draw& d, float fX, float fY, float fW, float fH, FColour colour = White,
    int nStyle = nStyleNormal)
{
    Tile(d, d.pFondNoir, fX - fOutline, fY - fOutline, fW + fOutline * 2.0f, fH + fOutline * 2.0f,
        Black, nStyleNormal);

    Tile(d, d.pFondMenu, fX, fY, fW, fH, colour, nStyle);
}

// The interface pass nudges strings drawn over the in game objectives band left by the pillarbox.
// This page is laid out in the menu box, so it flags its own strings and skips that nudge.
static bool bOwnText = false;

export bool MongooseMenuOwnsText()
{
    return bOwnText;
}

// STY_None measures without drawing, the call execTextSize makes, so this agrees with the menus
// centring their own captions. Canvas units.
static void Measure(void* pCanvas, const char* szText, float& fW, float& fH)
{
    fW = 0.0f;
    fH = 0.0f;

    auto pFont = MenuFont(pCanvas);
    if (!pFont || !pWrappedPrint)
        return;

    auto pCurX = reinterpret_cast<float*>(static_cast<uint8_t*>(pCanvas) + nOffsetCurX);
    auto pCurY = reinterpret_cast<float*>(static_cast<uint8_t*>(pCanvas) + nOffsetCurY);
    const auto fSavedX = *pCurX;
    const auto fSavedY = *pCurY;

    int nW = 0;
    int nH = 0;

    pWrappedPrint(pCanvas, nStyleNone, &nW, &nH, pFont, 0, szText);

    *pCurX = fSavedX;
    *pCurY = fSavedY;

    fW = static_cast<float>(nW);
    fH = static_cast<float>(nH);
}

// X and Y in authored 640x480 space; centring in canvas units, where the metrics are.
static void Text(const Draw& d, float fX, float fY, const char* szText, FColour colour, bool bCentre = false)
{
    auto pFont = MenuFont(d.pCanvas);
    if (!pFont || !pWrappedPrint)
        return;

    auto fLeft = fX * d.fRatioX;

    if (bCentre)
    {
        float fW = 0.0f, fH = 0.0f;
        Measure(d.pCanvas, szText, fW, fH);
        fLeft -= fW * 0.5f;
    }

    auto pCurX = reinterpret_cast<float*>(static_cast<uint8_t*>(d.pCanvas) + nOffsetCurX);
    auto pCurY = reinterpret_cast<float*>(static_cast<uint8_t*>(d.pCanvas) + nOffsetCurY);
    auto pStyle = static_cast<uint8_t*>(d.pCanvas) + nOffsetStyle;
    const auto nOldStyle = *pStyle;
    const auto oldColour = Colour(d.pCanvas);

    *pCurX = fLeft;
    *pCurY = fY * d.fRatioY;
    *pStyle = nStyleNormal;
    SetColour(d.pCanvas, colour);

    int nW = 0;
    int nH = 0;

    bOwnText = true;
    pWrappedPrint(d.pCanvas, nStyleNormal, &nW, &nH, pFont, 0, szText);
    bOwnText = false;

    *pStyle = nOldStyle;
    SetColour(d.pCanvas, oldColour);
}

// Help bar button

// Recomputed, not read: menuscale rewrites DisplayHelpBar's pixel figures live and they never
// reach script as values.
static Rect HelpBar(void* pCanvas)
{
    const auto fClipX = Read<float>(pCanvas, nOffsetClipX);
    const auto fClipY = Read<float>(pCanvas, nOffsetClipY);
    const auto fScale = fClipY / fAuthoredHeight;

    return { fBarInset * fScale, fClipY - (fBarHeight + fBarBottom) * fScale,
        fClipX - 2.0f * fBarInset * fScale, fBarHeight * fScale };
}

static constexpr auto szButtonText = "Mongoose Fix Settings";
static constexpr auto szWatermark = "Mongoose Fix " rsc_Version;

static Rect ButtonRect(void* pCanvas)
{
    const auto bar = HelpBar(pCanvas);

    float fW = 0.0f, fH = 0.0f;
    Measure(pCanvas, szButtonText, fW, fH);

    const auto fPad = bar.fH * 0.35f;
    const auto fWidth = fW + fPad * 2.0f;

    return { bar.fX + (bar.fW - fWidth) * 0.5f, bar.fY, fWidth, bar.fH };
}

static bool Inside(const Rect& r, float fX, float fY)
{
    return fX >= r.fX && fX <= r.fX + r.fW && fY >= r.fY && fY <= r.fY + r.fH;
}

// The interface updates MouseX and hit tests in its own key handler, not during its render, so
// nothing around the render stops the menu underneath following the cursor. The page swallows both
// mouse axes and tracks the pointer from the same deltas, with the sign per axis measured from one
// event each while the page is closed.
static float fOwnX = -1.0f;
static float fOwnY = -1.0f;
static float fSignX = 0.0f;
static float fSignY = 0.0f;

static void ReadMouse(void* pRoot, float& fX, float& fY)
{
    fX = -1.0f;
    fY = -1.0f;

    auto pX = Field(pRoot, "MouseX");
    auto pY = Field(pRoot, "MouseY");

    if (!pX || !pY)
        return;

    fX = static_cast<float>(*reinterpret_cast<int32_t*>(pX));
    fY = static_cast<float>(*reinterpret_cast<int32_t*>(pY));
}

static void MousePosition(void* pRoot, float& fX, float& fY)
{
    if (bPageOpen && fOwnX >= 0.0f)
    {
        fX = fOwnX;
        fY = fOwnY;
        return;
    }

    ReadMouse(pRoot, fX, fY);
}

// The page

static constexpr auto fPanelX = 40.0f;
static constexpr auto fPanelY = 20.0f;
static constexpr auto fPanelW = 560.0f;
static constexpr auto fPanelH = 440.0f;
static constexpr auto fTitleX = 200.0f;
static constexpr auto fTitleY = 2.0f;
static constexpr auto fTitleW = 240.0f;
static constexpr auto fTitleH = 30.0f;

// Row pitch and box heights are the Bot Challenge page's, the widest spaced list the game ships.
static constexpr auto fRowTop = 54.0f;
static constexpr auto fListBottom = fPanelY + fPanelH - 34.0f;
static constexpr auto fHeaderHeight = 28.0f;
static constexpr auto fSettingHeight = 38.0f;
static constexpr auto fControlY = 5.0f;
static constexpr auto fControlH = 26.0f;
static constexpr auto fTextInset = 5.0f;

static constexpr auto fHeaderX = 52.0f;
static constexpr auto fLabelX = 52.0f;
static constexpr auto fLabelW = 230.0f;
static constexpr auto fLabelTextX = 64.0f;
static constexpr auto fOnX = 372.0f;
static constexpr auto fOffX = 464.0f;
static constexpr auto fSmallW = 84.0f;
static constexpr auto fArrowLeftX = 352.0f;
static constexpr auto fValueX = 372.0f;
static constexpr auto fValueW = 176.0f;
static constexpr auto fArrowRightX = 556.0f;
static constexpr auto fArrowHitX = 344.0f;
static constexpr auto fArrowHitW = 36.0f;

// A caption for each ini section, then its settings. Headers are lines only, skipped by the focus.
struct Entry
{
    const char* szHeader;
    int nSetting;
};

static std::vector<Entry> aEntries;

static const char* CategoryName(const char* szSection)
{
    return std::strcmp(szSection, "FieldOfView") == 0 ? "Field of View" : szSection;
}

static void BuildEntries()
{
    if (!aEntries.empty())
        return;

    const char* szLast = nullptr;

    for (auto i = 0; i < nRowCount; i++)
    {
        if (!szLast || std::strcmp(szLast, aSettings[i].szSection) != 0)
        {
            szLast = aSettings[i].szSection;
            aEntries.push_back({ CategoryName(szLast), -1 });
        }

        aEntries.push_back({ nullptr, i });
    }
}

static int nFocus = 0;
static int nTop = 0;

static int EntryCount()
{
    return static_cast<int>(aEntries.size());
}

static float EntryHeight(int nEntry)
{
    return aEntries[nEntry].szHeader ? fHeaderHeight : fSettingHeight;
}

static float RowY(int nRow)
{
    auto fY = fRowTop;

    for (auto i = nTop; i < nRow; i++)
        fY += EntryHeight(i);

    return fY;
}

// One past the last entry that still fits under the panel.
static int LastVisible()
{
    auto fY = fRowTop;
    auto i = nTop;

    for (; i < EntryCount(); i++)
    {
        const auto fHeight = EntryHeight(i);

        if (fY + fHeight > fListBottom)
            break;

        fY += fHeight;
    }

    return (std::max)(i, nTop + 1);
}

static Rect Authored(const Draw& d, float fX, float fY, float fW, float fH)
{
    return { fX * d.fRatioX + d.fPillarX, fY * d.fRatioY + d.fPillarY,
        fW * d.fRatioX, fH * d.fRatioY };
}

static int ClickDirection(const Draw& d, const Setting& s, float fRow, float fX, float fY)
{
    const auto fY0 = fRow + fControlY;

    if (s.eKind == Kind::Toggle)
    {
        if (Inside(Authored(d, fOnX, fY0, fSmallW, fControlH), fX, fY))
            return -1;

        return Inside(Authored(d, fOffX, fY0, fSmallW, fControlH), fX, fY) ? 1 : 0;
    }

    if (Inside(Authored(d, fArrowHitX, fY0, fArrowHitW, fControlH), fX, fY))
        return -1;

    return Inside(Authored(d, fArrowRightX - 10.0f, fY0, fArrowHitW, fControlH), fX, fY) ? 1 : 0;
}

static bool bMouseMoved = false;

// One more stop on the Options page: the keyboard reaches it off the ends of that page's grid.
static bool bButtonFocused = false;

// Arrow keys only; hover is decided fresh each frame, so a resting mouse cannot stick it on.
static bool bButtonKeyFocus = false;

// The three the menus use: MnCurseur as the highlight moves, MnValid on accept, MnAnnul on cancel.
static constexpr auto szSoundCursor = "XIIIsound.Interface.MnCurseur";
static constexpr auto szSoundAccept = "XIIIsound.Interface.MnValid";
static constexpr auto szSoundCancel = "XIIIsound.Interface.MnAnnul";

static void PlaySound(void* pCanvas, const char* szName);

// XIIIGUIBaseButton binds MouseEnter to OnActivate, so a control relights only on reactivation,
// not when the pointer crosses back. Dimming is undone by hand, so keep the control and its state.
static void* pDimmedControl = nullptr;
static auto bDimmedWasLit = false;

// XIIITextureButton::MouseEnter by hand: the flag, the onomatopoeia's zoom and the tick, none of
// which fire again for a control the page never deactivated.
static void Relight(void* pCanvas, void* pControl)
{
    SetBool(pControl, "bDisplayTex", true);
    SetBool(pControl, "bZoomIn", true);

    if (auto pZoom = Field(pControl, "zoom"))
        *reinterpret_cast<float*>(pZoom) = 0.0f;

    PlaySound(pCanvas, szSoundCursor);
}

// Relights only if the page still has the same control: the pad can move the focus with one
// dimmed, and lighting the old one back up would leave two lit.
static void Dim(void* pCanvas, void* pFocused, bool bWant)
{
    auto pControl = bWant ? pFocused : nullptr;

    if (pDimmedControl == pControl)
        return;

    if (pDimmedControl && bDimmedWasLit && pDimmedControl == pFocused)
        Relight(pCanvas, pDimmedControl);

    pDimmedControl = pControl;
    bDimmedWasLit = false;

    if (!pControl)
        return;

    if (auto pProperty = FindProperty(pControl, "bDisplayTex"))
    {
        const auto nMask = Read<uint32_t>(pProperty, nOffsetBoolBitMask);
        const auto nOffset = Read<int32_t>(pProperty, nOffsetPropertyOffset);
        bDimmedWasLit = (*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pControl) + nOffset) & nMask) != 0;
    }

    SetBool(pControl, "bDisplayTex", false);
}

static void DrawPage(const Draw& d, float fMouseX, float fMouseY)
{
    Box(d, fPanelX, fPanelY, fPanelW, fPanelH);
    Box(d, fTitleX, fTitleY, fTitleW, fTitleH);
    Text(d, fTitleX + fTitleW * 0.5f, fTitleY + fTextInset, szButtonText, Black, true);

    const auto nEnd = LastVisible();

    for (auto i = nTop; i < nEnd; i++)
    {
        const auto& entry = aEntries[i];
        const auto fY = RowY(i);

        if (entry.szHeader)
        {
            Text(d, fHeaderX, fY + fTextInset, entry.szHeader, Black);
            continue;
        }

        auto& s = aSettings[entry.nSetting];

        // Hover focuses a row, as the game's own controls do, but only once the mouse has moved:
        // a resting cursor would drag the focus back on every arrow press.
        if (bMouseMoved && Inside(Authored(d, fPanelX, fY, fPanelW, EntryHeight(i)), fMouseX, fMouseY))
            nFocus = i;

        if (i == nFocus)
            Tile(d, d.pFondNoir, fPanelX + 8.0f, fY, fPanelW - 16.0f, EntryHeight(i),
                { 255, 255, 255, 60 }, nStyleAlpha);

        const auto fRow = fY + fControlY;

        Box(d, fLabelX, fRow, fLabelW, fControlH);
        Text(d, fLabelTextX, fRow + fTextInset, s.szLabel, Black);

        if (s.eKind == Kind::Toggle)
        {
            const auto bOn = s.fValue != 0.0f;

            Box(d, fOnX, fRow, fSmallW, fControlH, bOn ? White : Grey);
            Text(d, fOnX + fSmallW * 0.5f, fRow + fTextInset, "On", Black, true);

            Box(d, fOffX, fRow, fSmallW, fControlH, bOn ? Grey : White);
            Text(d, fOffX + fSmallW * 0.5f, fRow + fTextInset, "Off", Black, true);
            continue;
        }

        Text(d, fArrowLeftX, fRow + fTextInset, "<", Black);
        Box(d, fValueX, fRow, fValueW, fControlH);
        Text(d, fValueX + fValueW * 0.5f, fRow + fTextInset, FormatValue(s).c_str(), Black, true);
        Text(d, fArrowRightX, fRow + fTextInset, ">", Black);
    }

    Text(d, 320.0f, fPanelY + fPanelH - 24.0f,
        "Up and Down scroll, Left and Right change, Enter applies, Escape closes", Black, true);
}

static void DrawNotice(const Draw& d)
{
    static constexpr auto fNoticeX = 160.0f;
    static constexpr auto fNoticeY = 196.0f;
    static constexpr auto fNoticeW = 320.0f;
    static constexpr auto fNoticeH = 76.0f;

    Box(d, fNoticeX, fNoticeY, fNoticeW, fNoticeH);
    Text(d, fNoticeX + fNoticeW * 0.5f, fNoticeY + 14.0f, "This setting requires a restart", Black, true);
    Text(d, fNoticeX + fNoticeW * 0.5f, fNoticeY + 44.0f, "Press any key", Grey, true);
}

// The interface draws its cursor before this page, so it would sit under the panel. The quad the
// controller drew is recorded and replayed on top, keeping the game's material, size and hot spot.
struct CursorTile
{
    void* pMaterial;
    float aArgs[8];
    float fZ;
    FPlane colour;
    FPlane fog;
    bool bValid;
};

static CursorTile cursorTile{};
static void* pCursorMaterial = nullptr;

// The interface hides its cursor while the pad is live, so the page's copy goes too.
static std::atomic<bool> bPadInput = false;

export void MongooseMenuNotePadInput(bool bPad)
{
    bPadInput = bPad;
}

// The quad's offset from the pointer, learned on any frame the mouse is not parked. Parked, only
// its corner is wrong.
static float fHotX = 0.0f;
static float fHotY = 0.0f;
static auto bHotKnown = false;

// Colour and fog come across as four floats each: the interface pass has its own FPlane and two of
// that name in one translation unit is ambiguous.
//
// The replay goes back through UCanvas::DrawTile, the call being suppressed, so it says so.
static bool bReplayingCursor = false;

export bool MongooseMenuSuppressTile(void* pMaterial)
{
    return bPageOpen && !bReplayingCursor && pMaterial && pMaterial == pCursorMaterial;
}

export void MongooseMenuNoteTile(void* pMaterial, float fX, float fY, float fXL, float fYL,
    float fU, float fV, float fUL, float fVL, float fZ, const float* pColour, const float* pFog)
{
    if (!pMaterial || pMaterial != pCursorMaterial || !pColour || !pFog)
        return;

    cursorTile = { pMaterial, { fX, fY, fXL, fYL, fU, fV, fUL, fVL }, fZ,
        { pColour[0], pColour[1], pColour[2], pColour[3] },
        { pFog[0], pFog[1], pFog[2], pFog[3] }, true };
}

static void NoteHotSpot(const Draw& d, float fMouseX, float fMouseY)
{
    if (!cursorTile.bValid || fMouseX < 0.0f)
        return;

    fHotX = cursorTile.aArgs[0] - (fMouseX - d.fPillarX);
    fHotY = cursorTile.aArgs[1] - (fMouseY - d.fPillarY);
    bHotKnown = true;
}

static void DrawCursor(const Draw& d, float fMouseX, float fMouseY)
{
    if (!cursorTile.bValid || !bHotKnown || !pCanvasDrawTile || fMouseX < 0.0f || bPadInput.load())
        return;

    const auto& t = cursorTile;

    bReplayingCursor = true;

    pCanvasDrawTile(d.pCanvas, t.pMaterial, fMouseX - d.fPillarX + fHotX, fMouseY - d.fPillarY + fHotY,
        t.aArgs[2], t.aArgs[3], t.aArgs[4], t.aArgs[5], t.aArgs[6], t.aArgs[7], t.fZ, t.colour, t.fog);

    bReplayingCursor = false;
}

static bool ClickPage(const Draw& d, float fX, float fY)
{

    for (auto i = nTop; i < LastVisible(); i++)
    {
        const auto& entry = aEntries[i];

        if (entry.szHeader)
            continue;

        const auto fRow = RowY(i);

        if (!Inside(Authored(d, fPanelX, fRow, fPanelW, EntryHeight(i)), fX, fY))
            continue;

        nFocus = i;

        auto& s = aSettings[entry.nSetting];
        const auto nDirection = ClickDirection(d, s, fRow, fX, fY);

        if (nDirection == 0)
            return false;

        Step(s, nDirection);
        bDirty = true;
        return true;
    }

    return false;
}

// The tick the menus play as the highlight moves. AActor::PlayMenu is native final, so it is
// called as its exec does, arguments already resolved.
//
//   AActor::GetLevel   actor  +0x80
//   AActor::execPlayMenu      level +0x40, then +0x50 is the subsystem, whose vtable slot 0xA0 is
//                             PlayMenu(Actor, Actor, Sound, Type) with Type 32 for a menu sound.
static constexpr auto nOffsetActorLevel = 0x80;
static constexpr auto nOffsetLevelOwner = 0x40;
static constexpr auto nOffsetLevelAudio = 0x50;
static constexpr auto nAudioPlayMenuSlot = 0xA0 / sizeof(void*);
static constexpr auto nSoundTypeMenu = 32;

static void* const pAnyPackage = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

static void* FindSound(const char* szName)
{
    static std::map<std::string, void*> aSounds;

    auto it = aSounds.find(szName);
    if (it != aSounds.end())
        return it->second;

    auto pSound = pStaticFindObject ? pStaticFindObject(nullptr, pAnyPackage, szName, 0) : nullptr;

    aSounds.emplace(szName, pSound);
    return pSound;
}

static void PlaySound(void* pCanvas, const char* szName)
{
    auto pSound = FindSound(szName);

    auto pViewport = pCanvas ? Read<void*>(pCanvas, nOffsetViewport) : nullptr;
    auto pField = pViewport ? Field(pViewport, "Actor") : nullptr;
    auto pActor = pField ? *reinterpret_cast<void**>(pField) : nullptr;

    if (!pSound || !pActor)
        return;

    auto pLevel = Read<void*>(pActor, nOffsetActorLevel);
    auto pOwner = pLevel ? Read<void*>(pLevel, nOffsetLevelOwner) : nullptr;
    auto pAudio = pOwner ? Read<void*>(pOwner, nOffsetLevelAudio) : nullptr;

    if (!pAudio)
        return;

    auto ppVtable = *reinterpret_cast<void***>(pAudio);
    reinterpret_cast<PlayMenuSound_t>(ppVtable[nAudioPlayMenuSlot])(pAudio, pActor, pActor, pSound,
        nSoundTypeMenu);
}

// Frame entry points

// What the last frame saw, so a key event hit tests the same geometry.
static Draw lastDraw{};
static Rect lastButton{};
// Both the Options page and the pause menu carry the caption. The pause menu is a plain vertical
// list, not a grid, so Down reaches the caption only off its genuine bottom.
static bool bOnPause = false;
static bool bOnHost = false;

static Draw MakeDraw(void* pCanvas, void* pRoot)
{
    Draw d{};
    d.pCanvas = pCanvas;

    const auto fClipX = Read<float>(pCanvas, nOffsetClipX);
    const auto fClipY = Read<float>(pCanvas, nOffsetClipY);

    d.fRatioX = fClipX / fAuthoredWidth;
    d.fRatioY = fClipY / fAuthoredHeight;

    if (auto pViewport = Read<void*>(pCanvas, nOffsetViewport))
    {
        const auto fScreenX = static_cast<float>(Read<int32_t>(pViewport, nOffsetViewportSizeX));
        const auto fScreenY = static_cast<float>(Read<int32_t>(pViewport, nOffsetViewportSizeY));

        d.fPillarX = (std::max)((fScreenX - fClipX) * 0.5f, 0.0f);
        d.fPillarY = (std::max)((fScreenY - fClipY) * 0.5f, 0.0f);

        fScreenW = fScreenX;
        fScreenH = fScreenY;
    }

    ReadOutputSize();

    if (auto pField = Field(pRoot, "FondMenu"))
        d.pFondMenu = *reinterpret_cast<void**>(pField);

    if (auto pField = Field(pRoot, "tFondNoir"))
        d.pFondNoir = *reinterpret_cast<void**>(pField);

    if (auto pField = Field(pRoot, "MouseCursors"))
    {
        auto ppCursors = *reinterpret_cast<void***>(pField);
        pCursorMaterial = ppCursors ? ppCursors[0] : nullptr;
    }

    return d;
}

export void MongooseMenuDraw(void* pCanvas)
{
    if (!pCanvas || !pDrawTileStretched)
        return;

    auto pRoot = RootWindow(pCanvas);

    if (!pRoot)
        return;

    const auto szPage = ActivePageName(pRoot);

    BuildEntries();

    const auto d = MakeDraw(pCanvas, pRoot);

    if (d.fRatioX <= 0.0f || d.fRatioY <= 0.0f)
        return;

    lastDraw = d;
    bOnPause = std::strcmp(szPage, "XIIIMenuInGame") == 0;
    bOnHost = bOnPause || std::strcmp(szPage, "XIIIMenuOptions") == 0;

    float fMouseX = -1.0f, fMouseY = -1.0f;
    MousePosition(pRoot, fMouseX, fMouseY);

    static auto fWasX = -1.0f;
    static auto fWasY = -1.0f;
    bMouseMoved = fMouseX != fWasX || fMouseY != fWasY;
    fWasX = fMouseX;
    fWasY = fMouseY;

    if (!bPageOpen)
        NoteHotSpot(d, fMouseX, fMouseY);

    static auto nLastFocus = -1;

    if (bPageOpen && nFocus != nLastFocus)
    {
        if (nLastFocus >= 0)
            PlaySound(pCanvas, szSoundCursor);

        nLastFocus = nFocus;
    }
    else if (!bPageOpen)
    {
        nLastFocus = -1;
    }

    if (bPageOpen)
    {
        DrawPage(d, fMouseX, fMouseY);

        if (bRestartNotice)
            DrawNotice(d);

        DrawCursor(d, fMouseX, fMouseY);
        return;
    }

    const auto bar = HelpBar(pCanvas);

    // The profile page leaves the left of its bar empty, so the build name goes there.
    if (std::strcmp(szPage, "XIIIMenuSelectProfile") == 0)
    {
        Text(d, bar.fX / d.fRatioX, (bar.fY + bar.fH * 0.25f) / d.fRatioY, szWatermark, Grey);
        return;
    }

    if (!bOnHost)
        return;

    lastButton = ButtonRect(pCanvas);

    if (lastButton.fW <= bar.fH)
        return;

    const auto bHover = Inside(lastButton, fMouseX - d.fPillarX, fMouseY - d.fPillarY);

    if (bMouseMoved)
        bButtonKeyFocus = false;

    if (bHover && !bButtonFocused)
        PlaySound(d.pCanvas, szSoundCursor);

    bButtonFocused = bHover || bButtonKeyFocus;

    // Every frame, not just on focus: the page keeps its control lit through SetFocus, so the
    // pointer leaving is not enough to put it out.
    Dim(d.pCanvas, FocusedControl(pRoot), bButtonFocused);

    Box(d, lastButton.fX / d.fRatioX, lastButton.fY / d.fRatioY,
        lastButton.fW / d.fRatioX, lastButton.fH / d.fRatioY, bButtonFocused ? Grey : White);

    Text(d, (lastButton.fX + lastButton.fW * 0.5f) / d.fRatioX,
        (lastButton.fY + lastButton.fH * 0.25f) / d.fRatioY, szButtonText,
        Black, true);

    // The caption is drawn after the interface's cursor, so the cursor goes back on top.
    DrawCursor(d, fMouseX, fMouseY);
}

static void FollowFocus(int nStep)
{
    // Headers are lines, not stops.
    for (auto i = 0; i < EntryCount(); i++)
    {
        nFocus = std::clamp(nFocus, 0, EntryCount() - 1);

        if (!aEntries[nFocus].szHeader)
            break;

        nFocus += nStep;

        if (nFocus < 0 || nFocus >= EntryCount())
            nFocus -= nStep * 2;
    }

    nFocus = std::clamp(nFocus, 0, EntryCount() - 1);

    // One line of context above the focus so its category caption stays visible.
    nTop = (std::min)(nTop, (std::max)(0, nFocus - 1));

    while (nFocus >= LastVisible() && nTop < EntryCount() - 1)
        nTop++;
}

// IK_MouseX and IK_MouseY, the pair Interactions.uc numbers 0xE4 and 0xE5.
static constexpr auto nMouseAxisX = 0xE4;
static constexpr auto nMouseAxisY = 0xE5;

static bool IsMouseAxis(int nKey)
{
    return nKey == nMouseAxisX || nKey == nMouseAxisY;
}

// True swallows the key. Codes are the ones XIDInterf tests for: 0x01 left mouse, 0x0D enter,
// 0x1B escape, 0x08 backspace, 0x25 to 0x28 the arrows, 0xEC and 0xED the wheel.
static bool HandleKey(void* pCanvas, int nKey, int nAction, float fDelta)
{
    static constexpr auto nPress = 1;
    static constexpr auto nHold = 2;
    static constexpr auto nWheelUp = 0xEC;
    static constexpr auto nWheelDown = 0xED;

    // Only the arrows repeat; a held Accept or Cancel would fire over and over.
    const auto bRepeats = nKey == 0x25 || nKey == 0x26 || nKey == 0x27 || nKey == 0x28;

    auto pRoot = pCanvas ? RootWindow(pCanvas) : nullptr;

    if (bPageOpen && IsMouseAxis(nKey))
    {
        const auto fSign = nKey == nMouseAxisX ? fSignX : fSignY;
        auto& fOwn = nKey == nMouseAxisX ? fOwnX : fOwnY;
        const auto fLimit = nKey == nMouseAxisX ? fScreenW : fScreenH;

        if (fOwn >= 0.0f && fSign != 0.0f)
            fOwn = std::clamp(fOwn + fDelta * fSign, 0.0f, (std::max)(fLimit - 1.0f, 0.0f));

        return true;
    }

    if (nAction != nPress && !(bRepeats && nAction == nHold))
    {
        if (!bPageOpen)
            return false;

        if (bRestartNotice)
            return true;

        if (nKey == nWheelUp || nKey == nWheelDown)
        {
            const auto nStep = nKey == nWheelUp ? -1 : 1;
            nFocus += nStep;
            FollowFocus(nStep);
            return true;
        }

        return nKey == 0x01;
    }

    float fMouseX = -1.0f, fMouseY = -1.0f;
    if (pRoot)
        MousePosition(pRoot, fMouseX, fMouseY);

    const auto fCanvasX = fMouseX - lastDraw.fPillarX;
    const auto fCanvasY = fMouseY - lastDraw.fPillarY;

    if (!bPageOpen)
    {
        // Options control order: 0 is the parental lock across the top, 1 to 3 the row under it,
        // so up off the top and down off the bottom both land on the caption.
        if (bOnHost && !bButtonFocused && (nKey == 0x26 || nKey == 0x28))
        {
            const auto position = FocusPosition(pRoot);
            const auto bOffTheEnd = bOnPause ? position == Focus::Bottom : position != Focus::Top;

            if ((nKey == 0x26 && position == Focus::Top) || (nKey == 0x28 && bOffTheEnd))
            {
                bButtonFocused = true;
                bButtonKeyFocus = true;
                PlaySound(pCanvas, szSoundCursor);
                return true;
            }
        }

        if (bOnHost && bButtonFocused)
        {
            // Off the caption, and the page's grid takes the key from where it left off.
            if (nKey == 0x26 || nKey == 0x28)
            {
                bButtonFocused = false;
                bButtonKeyFocus = false;
                Dim(pCanvas, pRoot ? FocusedControl(pRoot) : nullptr, false);
                PlaySound(pCanvas, szSoundCursor);
                return true;
            }

            if (nKey != 0x0D && nKey != 0x01)
                return false;
        }
        else if (nKey != 0x01 || !bOnHost || !Inside(lastButton, fCanvasX, fCanvasY))
        {
            return false;
        }

        bButtonFocused = false;
        bButtonKeyFocus = false;
        Dim(pCanvas, pRoot ? FocusedControl(pRoot) : nullptr, false);
        PlaySound(pCanvas, szSoundAccept);
        LoadValues();
        nFocus = 0;
        nTop = 0;
        FollowFocus(1);
        ReadMouse(pRoot, fOwnX, fOwnY);
        bPageOpen = true;
        return true;
    }

    if (bRestartNotice)
    {
        bRestartNotice = false;
        PlaySound(pCanvas, szSoundAccept);
        return true;
    }

    switch (nKey)
    {
    // Enter and the pad's A keep the changes, Escape and B throw them away, as the game's other
    // option pages do.
    case 0x0D:
        SaveValues();
        PlaySound(pCanvas, szSoundAccept);
        return true;
    case 0x1B:
    case 0x08:
        bDirty = false;
        LoadValues();
        PlaySound(pCanvas, szSoundCancel);
        bPageOpen = false;
        return true;
    case 0x26:
        nFocus--;
        FollowFocus(-1);
        return true;
    case 0x28:
        nFocus++;
        FollowFocus(1);
        return true;
    case 0x25:
    case 0x27:
        if (!aEntries[nFocus].szHeader)
        {
            auto& s = aSettings[aEntries[nFocus].nSetting];
            Step(s, nKey == 0x25 ? -1 : 1);
            PlaySound(pCanvas, szSoundCursor);
            bDirty = true;
        }
        return true;
    case 0x01:
        if (ClickPage(lastDraw, fMouseX, fMouseY))
            PlaySound(pCanvas, szSoundCursor);

        return true;
    default:
        // Anything else carries on: a pad button is still a Joy key here, and controller.ixx can
        // only turn it into its keyboard equivalent further down while the key is travelling.
        return false;
    }
}

// The pad never reaches MasterProcessKeyEvent: controller.ixx turns a button into its keyboard
// equivalent, hands it to UGUIController::NativeKeyEvent, and offers it here first.
export bool MongooseMenuPadKey(int nKey, int nAction)
{
    return lastDraw.pCanvas && HandleKey(lastDraw.pCanvas, nKey, nAction, 0.0f);
}

static int __fastcall KeyEvent(void* pMaster, void*, int nKey, int nAction, float fDelta, void* pViewport)
{
    // The canvas hangs off the viewport.
    static constexpr auto nOffsetViewportCanvas = 0x68;

    auto pCanvas = pViewport ? Read<void*>(pViewport, nOffsetViewportCanvas) : nullptr;

    if (pCanvas && HandleKey(pCanvas, nKey, nAction, fDelta))
        return 1;

    // One event per axis gives the sign the controller applies.
    const auto bCalibrate = pCanvas && IsMouseAxis(nKey) && fDelta != 0.0f
        && (nKey == nMouseAxisX ? fSignX : fSignY) == 0.0f;

    auto pRoot = bCalibrate ? RootWindow(pCanvas) : nullptr;

    float fWasX = -1.0f, fWasY = -1.0f;
    if (pRoot)
        ReadMouse(pRoot, fWasX, fWasY);

    const auto nResult = shKeyEvent.thiscall<int>(pMaster, nKey, nAction, fDelta, pViewport);

    if (pRoot && fWasX >= 0.0f)
    {
        float fNowX = 0.0f, fNowY = 0.0f;
        ReadMouse(pRoot, fNowX, fNowY);

        const auto fMoved = nKey == nMouseAxisX ? fNowX - fWasX : fNowY - fWasY;

        if (fMoved != 0.0f)
        {
            auto& fSign = nKey == nMouseAxisX ? fSignX : fSignY;
            fSign = fMoved / fDelta > 0.0f ? 1.0f : -1.0f;
        }
    }

    return nResult;
}

static void Init()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    auto hEngine = GetModuleHandleW(L"Engine.dll");

    if (!hCore || !hEngine)
        return;

    pFNameCtor = reinterpret_cast<FNameCtor_t>(GetProcAddress(hCore, "??0FName@@QAE@PBDW4EFindName@@_N@Z"));
    pGetName = reinterpret_cast<GetName_t>(GetProcAddress(hCore, "?GetName@UObject@@QBEPBDXZ"));
    pWrappedPrint = reinterpret_cast<WrappedPrint_t>(GetProcAddress(hEngine, "?WrappedPrint@UCanvas@@AAAXW4ERenderStyle@@AAH1PAVUFont@@HPBD@Z"));
    pDrawTileStretched = reinterpret_cast<DrawTileStretched_t>(GetProcAddress(hEngine, "?DrawTileStretched@UCanvas@@UAEXPAVUMaterial@@MMMM@Z"));
    pCanvasDrawTile = reinterpret_cast<DrawTile_t>(GetProcAddress(hEngine, "?DrawTile@UCanvas@@UAEXPAVUMaterial@@MMMMMMMMMVFPlane@@1@Z"));
    pStaticFindObject = reinterpret_cast<StaticFindObject_t>(GetProcAddress(hCore, "?StaticFindObject@UObject@@SAPAV1@PAVUClass@@PAV1@PBDH@Z"));

    auto pKeyEvent = GetProcAddress(hEngine, "?MasterProcessKeyEvent@UInteractionMaster@@QAEHW4EInputKey@@W4EInputAction@@MPAVUViewport@@@Z");

    if (!pFNameCtor || !pGetName || !pWrappedPrint || !pDrawTileStretched || !pKeyEvent)
    {
        pDrawTileStretched = nullptr;
        return;
    }

    shKeyEvent = safetyhook::create_inline(pKeyEvent, KeyEvent);

    if (!shKeyEvent)
    {
        pDrawTileStretched = nullptr;
        return;
    }

    LoadValues();
}

class MongooseMenu
{
public:
    MongooseMenu()
    {
        MongooseFix::onEngineInitEvent() += []() { Init(); };
    }
} MongooseMenu;
