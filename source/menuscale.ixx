module;

#include <common.hxx>

export module menuscale;

import common;
import display;
import logging;

// XIDInterf.u lays the menus out in 640x480, wrongly twice: BeforePaint derives fRatioX=ClipX/640
// and fRatioY=ClipY/480 separately, stretching 4:3 art on 16:9, then clamps both to 800/640;
// XIIIWindow.InternalOn* and the four control classes pillarbox the rest with
// if (ClipX > 800) SetOrigin((ClipX-800)/2, ...), the mouse hit test included.
//
// All constant operands in the package bytecode, patched in place - nothing inserted, no jump
// moved:
//
//   BeforePaint's 800/600   1e9, so FClamp returns what it was handed.
//   800/600 elsewhere       640*scale, 480*scale - the centring those branches meant, and equal to
//                           ClipX/ClipY on 4:3, so the tests go false.
//   DisplayHelpBar          the one part in raw screen pixels (32 tall, 30 in from each edge, 6
//                           from the bottom, 28 pixel icons, 3 pixel gap). One byte each, so past
//                           a scale of about 8 they stop growing.
//   XIIIRootWindow's 448    480. 448 is an NTSC safe area squash that shipped on PC, multiplying
//                           nearly every menu Y by 0.9333.
//   bCenterInGame           on. Set on only three input menus; on, the centring branch, the
//                           control bounds and the mouse correction agree.
//   bCalculateSize          on. A dozen menus turn it off and assign a caption width of
//                           200/160/175, 640x480 pixels in a field measured in screen pixels, with
//                           no room in the byte for a scaled value.
//
// The first two are flags, reached through the object system; the rest are recorded and rewritten
// on every device reset. One scale for both axes comes free from hudscale.ixx making the canvas
// report the menu box.
//
// Patching happens in UFunction::PostLoad (vtable +0x28, body shared by no other Core.dll class),
// after the linker deserialises the bytecode and before anything can call it.
//
//   UStruct  +0x48  Script.Data, +0x4C Script.ArrayNum  (TArray<BYTE>)
static constexpr auto nOffsetScriptData = 0x48;
static constexpr auto nOffsetScriptNum = 0x4C;

static constexpr auto fAuthoredWidth = 640.0f;
static constexpr auto fAuthoredHeight = 480.0f;

// EX_IntConst 0x1D, EX_FloatConst 0x1E, each plus four little endian bytes. Which one a literal
// became is not obvious from the source - 800 in float arithmetic folds to a float, the 800 in
// "C.ClipX > 800" stays an int behind a cast - so both are looked for.
static constexpr uint8_t nIntConst = 0x1D;
static constexpr uint8_t nFloatConst = 0x1E;
static constexpr auto nConstSize = 5;

struct Constant
{
    uint8_t aBytes[nConstSize];
};

static Constant IntConst(int32_t nValue)
{
    Constant c{ { nIntConst } };
    std::memcpy(c.aBytes + 1, &nValue, sizeof(nValue));
    return c;
}

static Constant FloatConst(float fValue)
{
    Constant c{ { nFloatConst } };
    std::memcpy(c.aBytes + 1, &fValue, sizeof(fValue));
    return c;
}

// A plain byte scan. A five byte constant could in principle sit inside another instruction's
// operand, but never does in these functions, and a disassembler to rule it out is a lot of code.
template<class F>
static int ForEachConstant(uint8_t* pScript, int nSize, const Constant& from, F&& fn)
{
    auto nCount = 0;

    for (auto i = 0; i + nConstSize <= nSize; i++)
    {
        if (std::memcmp(pScript + i, from.aBytes, nConstSize) != 0)
            continue;

        fn(pScript + i);
        i += nConstSize - 1;
        nCount++;
    }

    return nCount;
}

// A number the resolution decides; the operand keeps whichever encoding it was compiled with.
enum class Live
{
    MenuWidth,
    MenuHeight,
    Pixels,
};

struct Site
{
    uint8_t* pConstant;
    Live eKind;
    bool bFloat;
    float fAuthored;
};

static std::mutex mtxSites;
static std::vector<Site> aSites;

// EX_IntConstByte behind a cast to float. One byte, so the original is kept and the scale clamped.
static constexpr uint8_t aCastByte[] = { 0x39, 0x3F, 0x2C };

struct PixelSite
{
    uint8_t* pOperand;
    uint8_t nAuthored;
};

static std::vector<PixelSite> aPixelSites;

static void ApplyLiveSites()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (nWidth <= 0 || nHeight <= 0)
        return;

    // Parenthesised because Windows.h is included without NOMINMAX and min is a macro.
    const auto fScale = (std::min)(nWidth / fAuthoredWidth, nHeight / fAuthoredHeight);

    std::lock_guard g(mtxSites);

    for (const auto& site : aSites)
    {
        auto fValue = 0.0f;

        switch (site.eKind)
        {
        // The branches compare against the box the canvas reports, so the bound must make
        // (box - bound)/2 come out as (screen - box)/2.
        case Live::MenuWidth:
            fValue = std::clamp(2.0f * fAuthoredWidth * fScale - nWidth, 0.0f, fAuthoredWidth * fScale);
            break;
        case Live::MenuHeight:
            fValue = std::clamp(2.0f * fAuthoredHeight * fScale - nHeight, 0.0f, fAuthoredHeight * fScale);
            break;
        case Live::Pixels:     fValue = site.fAuthored * fScale; break;
        }

        const auto c = site.bFloat ? FloatConst(fValue) : IntConst(static_cast<int32_t>(fValue + 0.5f));
        injector::WriteMemoryRaw(site.pConstant, const_cast<uint8_t*>(c.aBytes), nConstSize, true);
    }

    for (const auto& site : aPixelSites)
    {
        const auto nScaled = std::clamp(static_cast<int>(site.nAuthored * fScale + 0.5f), 1, 255);
        injector::WriteMemory<uint8_t>(site.pOperand, static_cast<uint8_t>(nScaled), true);
    }
}

static int RecordPixels(uint8_t* pScript, int nSize, uint8_t nValue)
{
    std::lock_guard g(mtxSites);

    auto nCount = 0;

    for (auto i = 0; i + 4 <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aCastByte, sizeof(aCastByte)) != 0 || pScript[i + 3] != nValue)
            continue;

        aPixelSites.emplace_back(pScript + i + 3, nValue);
        i += 3;
        nCount++;
    }

    return nCount;
}

static int RecordLive(uint8_t* pScript, int nSize, int32_t nValue, Live eKind)
{
    std::lock_guard g(mtxSites);

    const auto fAuthored = static_cast<float>(nValue);

    auto nCount = ForEachConstant(pScript, nSize, IntConst(nValue),
        [eKind, fAuthored](uint8_t* p) { aSites.emplace_back(p, eKind, false, fAuthored); });

    nCount += ForEachConstant(pScript, nSize, FloatConst(fAuthored),
        [eKind, fAuthored](uint8_t* p) { aSites.emplace_back(p, eKind, true, fAuthored); });

    return nCount;
}

static int RewriteOnce(uint8_t* pScript, int nSize, int32_t nValue, float fReplacement)
{
    const auto cInt = IntConst(static_cast<int32_t>(fReplacement));
    const auto cFloat = FloatConst(fReplacement);

    auto nCount = ForEachConstant(pScript, nSize, IntConst(nValue),
        [&](uint8_t* p) { injector::WriteMemoryRaw(p, const_cast<uint8_t*>(cInt.aBytes), nConstSize, true); });

    nCount += ForEachConstant(pScript, nSize, FloatConst(static_cast<float>(nValue)),
        [&](uint8_t* p) { injector::WriteMemoryRaw(p, const_cast<uint8_t*>(cFloat.aBytes), nConstSize, true); });

    return nCount;
}

// XIIIEditCtrl sizes its text field as (WinWidth*640 - FirstBoxWidth)*fRatioX, but FirstBoxWidth
// is already scaled - the line above reads (WinWidth*640*fRatioX - 16*fRatioX)/2 - so the ratio
// lands twice. Past a ratio of 2 it goes negative and BeforePaint's shorten-until-it-fits loops
// never terminate: "Runaway loop detected". Paint's second box is the same expression.
//
// A prefix tree of float * (171, 0xAB) and float - (175, 0xAF), each <opcode> <a> <b>
// EX_EndFunctionParms, so reassociating to WinWidth*640*fRatioX - FirstBoxWidth is two opcodes
// swapped and the operands after them exchanged, in place:
//
//   AB AF AB <WinWidth> <640> 16 <FirstBoxWidth> 16 <fRatioX>       16
//   AF AB AB <WinWidth> <640> 16 <fRatioX>       16 <FirstBoxWidth> 16
//
// The variables are EX_InstanceVariable plus a pointer, five bytes each. 640 is an int constant
// behind an EX_PrimitiveCast to float, seven bytes, not a float constant.
static constexpr uint8_t nMultiplyFloat = 0xAB;
static constexpr uint8_t nSubtractFloat = 0xAF;
static constexpr uint8_t nInstanceVariable = 0x01;
static constexpr uint8_t nEndFunctionParms = 0x16;
static constexpr uint8_t aCastInt640[] = { 0x39, 0x3F, 0x1D, 0x80, 0x02, 0x00, 0x00 };
static constexpr auto nFieldWidthLength = 28;

static int ReassociateFieldWidth(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nFieldWidthLength <= nSize; i++)
    {
        auto pNode = pScript + i;

        if (pNode[0] != nMultiplyFloat || pNode[1] != nSubtractFloat || pNode[2] != nMultiplyFloat)
            continue;

        if (pNode[3] != nInstanceVariable || pNode[16] != nInstanceVariable || pNode[22] != nInstanceVariable)
            continue;

        if (std::memcmp(pNode + 8, aCastInt640, sizeof(aCastInt640)) != 0)
            continue;

        if (pNode[15] != nEndFunctionParms || pNode[21] != nEndFunctionParms || pNode[27] != nEndFunctionParms)
            continue;

        uint8_t aFixed[nFieldWidthLength];
        std::memcpy(aFixed, pNode, nFieldWidthLength);

        aFixed[0] = nSubtractFloat;
        aFixed[1] = nMultiplyFloat;
        std::memcpy(aFixed + 16, pNode + 22, nConstSize);
        std::memcpy(aFixed + 22, pNode + 16, nConstSize);

        injector::WriteMemoryRaw(pNode, aFixed, nFieldWidthLength, true);
        i += nFieldWidthLength - 1;
        nCount++;
    }

    return nCount;
}

// Three page titles size their own box out of the text they have just measured:
//
//   C.TextSize(Caps(TitleText), W, H);
//   DrawStretchedTexture(C, X, 80*fRatioY, (W+40)*fRatioX, (H+10)*fScaleTo*fRatioY, myRoot.FondMenu);
//
// TextSize answers in screen pixels, so the ratio is already in W and H and the box grows with its
// square - "Select your profile" comes out 504 tall at 3840x2160 where 144 is right. Both terms
// share a shape (measured local, 640x480 margin behind a cast to float, ratio), so one fix serves
// both: scale the margin, replace the trailing ratio with a float 1. Same bytes, in place:
//
//   AB AB AE <H> <39 3F 2C 0A> 16 <fScaleTo> 16 <fRatioY> 16     the height, 25 bytes
//   AB    AE <W> <39 3F 2C 28> 16 <fRatioX>  16                  the width, 18 bytes
//
// The margin is one byte, so past a scale of about three the wider one (80, on the multiplayer
// profile page) clamps at 255 and that box comes out slightly narrow.
static constexpr uint8_t nAddFloat = 0xAE;
static constexpr uint8_t nLocalVariable = 0x00;
static constexpr auto nMeasuredHeightLength = 25;
static constexpr auto nMeasuredWidthLength = 18;

static int NeutraliseMeasuredRatio(uint8_t* pScript, int nSize)
{
    const auto cOne = FloatConst(1.0f);
    auto nCount = 0;

    for (auto i = 0; i + nMeasuredWidthLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (i + nMeasuredHeightLength <= nSize
            && p[0] == nMultiplyFloat && p[1] == nMultiplyFloat && p[2] == nAddFloat
            && p[3] == nLocalVariable
            && std::memcmp(p + 8, aCastByte, sizeof(aCastByte)) == 0
            && p[12] == nEndFunctionParms
            && p[13] == nInstanceVariable && p[18] == nEndFunctionParms
            && p[19] == nInstanceVariable && p[24] == nEndFunctionParms)
        {
            {
                std::lock_guard g(mtxSites);
                aPixelSites.emplace_back(p + 11, p[11]);
            }

            injector::WriteMemoryRaw(p + 19, const_cast<uint8_t*>(cOne.aBytes), nConstSize, true);
            i += nMeasuredHeightLength - 1;
            nCount++;
            continue;
        }

        if (p[0] == nMultiplyFloat && p[1] == nAddFloat
            && p[2] == nLocalVariable
            && std::memcmp(p + 7, aCastByte, sizeof(aCastByte)) == 0
            && p[11] == nEndFunctionParms
            && p[12] == nInstanceVariable && p[17] == nEndFunctionParms)
        {
            {
                std::lock_guard g(mtxSites);
                aPixelSites.emplace_back(p + 10, p[10]);
            }

            injector::WriteMemoryRaw(p + 12, const_cast<uint8_t*>(cOne.aBytes), nConstSize, true);
            i += nMeasuredWidthLength - 1;
            nCount++;
        }
    }

    return nCount;
}

// Same fault again in SetObjectives, which keeps a running LineY in whole pixels and steps it by
// the height it just measured:
//
//   C.TextSize( MsgArray[i], W, H);
//   C.SetPos( 50*fRatioX, (LineY+6)*fRatioY + iObjDecalY);
//   LineY += 0.9*H;
//
// H is screen pixels, so LineY is too, and the ratio on it puts line two at 595 at 3840x2160 where
// 256 is right. LineY being an int makes the sum an int add behind a cast to float rather than the
// title boxes' float add; the repair is the same:
//
//   AB 39 3F 92 <LineY> <2C 06> 16 <fRatioY> 16     18 bytes
static constexpr uint8_t nAddInt = 0x92;
static constexpr uint8_t nCastIntToFloat[] = { 0x39, 0x3F };
static constexpr uint8_t nIntConstByte = 0x2C;
static constexpr auto nLinePitchLength = 18;

static int NeutraliseLinePitch(uint8_t* pScript, int nSize)
{
    const auto cOne = FloatConst(1.0f);
    auto nCount = 0;

    for (auto i = 0; i + nLinePitchLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (p[0] != nMultiplyFloat
            || std::memcmp(p + 1, nCastIntToFloat, sizeof(nCastIntToFloat)) != 0
            || p[3] != nAddInt || p[4] != nLocalVariable
            || p[9] != nIntConstByte || p[11] != nEndFunctionParms
            || p[12] != nInstanceVariable || p[17] != nEndFunctionParms)
            continue;

        {
            std::lock_guard g(mtxSites);
            aPixelSites.emplace_back(p + 10, p[10]);
        }

        injector::WriteMemoryRaw(p + 12, const_cast<uint8_t*>(cOne.aBytes), nConstSize, true);
        i += nLinePitchLength - 1;
        nCount++;
    }

    return nCount;
}

using GetFullName_t = const char* (__thiscall*)(const void* pThis, char* szBuffer);

static GetFullName_t pGetFullName = nullptr;
static SafetyHookInline shFunctionPostLoad{};

static bool OwnedByAny(const char* szFullName, std::initializer_list<const char*> aOwners)
{
    for (const auto szOwner : aOwners)
        if (std::strstr(szFullName, szOwner))
            return true;

    return false;
}

using StaticFindObject_t = void* (__cdecl*)(void* pClass, void* pOuter, const char* szName, int bExactClass);
using IsChildOf_t = int (__thiscall*)(const void* pStruct, const void* pParent);

// reinterpret_cast is not a constant expression, so this is const rather than constexpr.
static void* const pAnyPackage = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
static constexpr auto nOffsetObjectClass = 0x24;    // UObject
static constexpr auto nOffsetPropertyOffset = 0x34; // UProperty
static constexpr auto nOffsetBoolBitMask = 0x64;    // UBoolProperty
static constexpr auto nOffsetClassDefaults = 0xD8;  // UClass, TArray<BYTE>

static StaticFindObject_t pStaticFindObject = nullptr;
static IsChildOf_t pIsChildOf = nullptr;
static void* pUClassClass = nullptr;
static void*** pppObjects = nullptr;
static int32_t* pnObjects = nullptr;

// UField::SuperField, for reaching a property without knowing which ancestor declared it.
static constexpr auto nOffsetSuperField = 0x28;

static void* FindProperty(void* pClass, const char* szName);

static void* FindObject(void* pOuter, const char* szName)
{
    return pStaticFindObject ? pStaticFindObject(nullptr, pOuter, szName, 0) : nullptr;
}

static void* FindProperty(void* pClass, const char* szName)
{
    for (auto p = pClass; p; p = *reinterpret_cast<void**>(static_cast<uint8_t*>(p) + nOffsetSuperField))
    {
        if (auto pFound = FindObject(p, szName))
            return pFound;
    }

    return nullptr;
}

// Turns a script bool on in the class defaults and everything already built from them: a subclass
// copies its parent's defaults as it loads, so one that loaded first holds the old value, as does
// every existing window. The optional float is a cached ratio; zeroing it forces a resize.
static void SetDefaultBool(const char* szClass, const char* szProperty, const char* szStale = nullptr)
{
    auto pClass = FindObject(pAnyPackage, szClass);
    auto pProperty = pClass ? FindObject(pClass, szProperty) : nullptr;

    // A class the level did not load is not a fault, and this runs twice a second.
    if (!pProperty)
        return;

    auto pStale = szStale ? FindObject(pClass, szStale) : nullptr;
    const auto nStaleOffset = pStale
        ? *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pStale) + nOffsetPropertyOffset) : 0u;

    const auto nOffset = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperty) + nOffsetPropertyOffset);
    const auto nMask = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pProperty) + nOffsetBoolBitMask);

    for (auto i = 0; i < *pnObjects; i++)
    {
        auto pObject = static_cast<uint8_t*>((*pppObjects)[i]);

        // The object array is sparse.
        if (!pObject)
            continue;

        auto pObjectClass = *reinterpret_cast<void**>(pObject + nOffsetObjectClass);

        if (pObjectClass == pUClassClass)
        {
            if (!pIsChildOf(pObject, pClass))
                continue;

            // Not GetDefaultObject, which asserts on a class whose defaults are not sized yet.
            auto pDefaults = *reinterpret_cast<uint8_t**>(pObject + nOffsetClassDefaults);
            const auto nSize = *reinterpret_cast<int32_t*>(pObject + nOffsetClassDefaults + 4);

            if (!pDefaults || nSize < static_cast<int32_t>(nOffset + sizeof(uint32_t)))
                continue;

            *reinterpret_cast<uint32_t*>(pDefaults + nOffset) |= nMask;
            continue;
        }

        // An existing window keeps what it was constructed with, so it is set too.
        if (!pObjectClass || !pIsChildOf(pObjectClass, pClass))
            continue;

        *reinterpret_cast<uint32_t*>(pObject + nOffset) |= nMask;

        if (pStale)
            *reinterpret_cast<float*>(pObject + nStaleOffset) = 0.0f;
    }
}

// GUIStyles' BorderOffsets are absolute pixels, the same 16 at every resolution, and are what
// script reads when positioning text inside a control. The native draw ignores them and sizes from
// the material (handled canvas-side). Authored values are kept so a resolution change does not
// compound.
static constexpr auto nOffsetArrayDim = 0x30;   // UProperty
static constexpr auto nBorderCount = 4;

struct StyleBorders
{
    int32_t* pOffsets;
    int32_t aAuthored[nBorderCount];
};

static std::vector<StyleBorders> aStyleBorders;

static void ScaleStyleBorders()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (nWidth <= 0 || nHeight <= 0)
        return;

    const auto fScale = (std::min)(nWidth / fAuthoredWidth, nHeight / fAuthoredHeight);

    // Both spellings are tried so the class name is not what this hinges on.
    void* pClass = nullptr;
    void* pBorders = nullptr;

    for (const auto szName : { "GUIStyles", "GUIStyle" })
    {
        pClass = FindObject(pAnyPackage, szName);
        pBorders = pClass ? FindProperty(pClass, "BorderOffsets") : nullptr;

        if (pBorders)
            break;
    }

    if (!pBorders)
    {
        static auto bSaid = false;

        if (!std::exchange(bSaid, true))
            LogWarn("MenuScale: the control styles' BorderOffsets was not found, control text stays where it was");

        return;
    }

    const auto nOffset = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pBorders) + nOffsetPropertyOffset);
    const auto nCount = (std::min)(static_cast<int>(*reinterpret_cast<uint16_t*>(
        static_cast<uint8_t*>(pBorders) + nOffsetArrayDim)), nBorderCount);

    for (auto i = 0; i < *pnObjects; i++)
    {
        auto pObject = static_cast<uint8_t*>((*pppObjects)[i]);

        if (!pObject)
            continue;

        auto pObjectClass = *reinterpret_cast<void**>(pObject + nOffsetObjectClass);

        if (pObjectClass == pUClassClass || !pObjectClass || !pIsChildOf(pObjectClass, pClass))
            continue;

        auto pOffsets = reinterpret_cast<int32_t*>(pObject + nOffset);
        auto it = std::find_if(aStyleBorders.begin(), aStyleBorders.end(),
            [pOffsets](const StyleBorders& s) { return s.pOffsets == pOffsets; });

        if (it == aStyleBorders.end())
        {
            StyleBorders entry{ pOffsets, {} };

            for (auto n = 0; n < nCount; n++)
                entry.aAuthored[n] = pOffsets[n];

            aStyleBorders.push_back(entry);
            it = aStyleBorders.end() - 1;
        }

        for (auto n = 0; n < nCount; n++)
            pOffsets[n] = static_cast<int32_t>(it->aAuthored[n] * fScale + 0.5f);
    }
}

// The pillarbox does not reach a message box: its children are plain engine components placed
// straight from WinLeft, not XIIIWindow controls that add it themselves, so the text lands a
// pillarbox left of its panel. AdjustPosition would have re-centred it but sits behind
// CurrentPF == 0, and this build reports 2.
//
// Only the children move. WinLeft feeds XIIIWindow's Paint through a draw that adds the centring
// origin and DrawMsgboxBackground through one that does not, so moving it would put those two a
// pillarbox apart; the panel gets the same offset on the draw side instead.
static std::vector<void*> aCentredBoxes;

static void CentreMessageBoxes()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (nWidth <= 0 || nHeight <= 0)
        return;

    const auto fScale = (std::min)(nWidth / fAuthoredWidth, nHeight / fAuthoredHeight);
    const auto fPillarX = (nWidth - fAuthoredWidth * fScale) * 0.5f;
    const auto fPillarY = (nHeight - fAuthoredHeight * fScale) * 0.5f;

    if (fPillarX <= 0.0f && fPillarY <= 0.0f)
        return;

    // None is a child of the others: the in game one extends XIIIWindow directly.
    void* aClasses[3]{};
    auto nClasses = 0;

    for (const auto szName : { "XIIIMsgBox", "XIIIMsgBoxInGame", "XIIILiveMsgBox" })
    {
        if (auto pFound = FindObject(pAnyPackage, szName))
            aClasses[nClasses++] = pFound;
    }

    if (!nClasses)
        return;

    auto pClass = aClasses[0];

    // A property belongs to its declaring class, so a lookup on a subclass finds nothing, and the
    // declarer's runtime name is not always the one in the sources - WinLeft is on GUIComponent,
    // Controls further down. So the search walks up from the box's own class.
    auto pWinLeft = FindProperty(pClass, "WinLeft");
    auto pWinTop = FindProperty(pClass, "WinTop");
    auto pControls = FindProperty(pClass, "Controls");

    if (!pWinLeft || !pWinTop || !pControls)
    {
        // Every frame, so once is enough. Which of the three is missing is the diagnosis.
        static auto bSaid = false;

        if (!std::exchange(bSaid, true))
            LogWarn("MenuScale: a message box's geometry was not found ({}, {}, {}), they stay off centre",
                pWinLeft != nullptr, pWinTop != nullptr, pControls != nullptr);

        return;
    }

    const auto nLeft = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pWinLeft) + nOffsetPropertyOffset);
    const auto nTop = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pWinTop) + nOffsetPropertyOffset);
    const auto nList = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pControls) + nOffsetPropertyOffset);

    const auto Shift = [&](uint8_t* pWindow)
    {
        *reinterpret_cast<float*>(pWindow + nLeft) += fPillarX;
        *reinterpret_cast<float*>(pWindow + nTop) += fPillarY;
    };

    for (auto i = 0; i < *pnObjects; i++)
    {
        auto pObject = static_cast<uint8_t*>((*pppObjects)[i]);

        if (!pObject)
            continue;

        auto pObjectClass = *reinterpret_cast<void**>(pObject + nOffsetObjectClass);

        if (pObjectClass == pUClassClass || !pObjectClass)
            continue;

        auto bIsBox = false;

        for (auto c = 0; c < nClasses && !bIsBox; c++)
            bIsBox = pObjectClass == aClasses[c];

        if (!bIsBox)
            continue;

        if (std::find(aCentredBoxes.begin(), aCentredBoxes.end(), pObject) != aCentredBoxes.end())
            continue;

        aCentredBoxes.push_back(pObject);

        // Controls is a TArray of component pointers: data, count, capacity.
        auto ppControls = *reinterpret_cast<uint8_t***>(pObject + nList);
        const auto nCount = *reinterpret_cast<int32_t*>(pObject + nList + 4);

        for (auto c = 0; ppControls && c < nCount; c++)
        {
            if (ppControls[c])
                Shift(ppControls[c]);
        }
    }
}

// Menus are built as they are opened, so new windows have to be caught.
export void RefreshInterfaceObjects()
{
    if (!pStaticFindObject || !pIsChildOf || !pUClassClass || !pppObjects || !pnObjects)
        return;

    // Has to run before a new box's first draw or it shows in the corner for a frame, and the
    // object count cannot be watched because the engine reuses freed slots. So: every frame, made
    // cheap with three pointer compares per object rather than IsChildOf.
    CentreMessageBoxes();

    static uint32_t nLastSweep = 0;
    const auto nNow = GetTickCount();

    if (nLastSweep && nNow - nLastSweep < 500)
        return;

    nLastSweep = nNow ? nNow : 1;

    SetDefaultBool("XIIIWindow", "bCenterInGame");

    // XIIIComboControl caches the ratio it last sized itself at, so that is cleared too.
    SetDefaultBool("XIIIComboControl", "bCalculateSize", "OldRatioX");
    SetDefaultBool("XIIIValueControl", "bCalculateSize");
    SetDefaultBool("XIIIEditCtrl", "bCalculateSize");
    SetDefaultBool("XIIICheckBoxControl", "bCalculateSize");

    ScaleStyleBorders();
}

// Once, after the interface package is loaded and the device exists.
static void ApplyObjectFixes()
{
    if (!pStaticFindObject || !pIsChildOf || !pUClassClass || !pppObjects || !pnObjects)
    {
        LogWarn("MenuScale: the object system is not available, the menus stay in the corner");
        return;
    }

    RefreshInterfaceObjects();
}

static void InitObjectSystem()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hCore)
        return;

    pStaticFindObject = reinterpret_cast<StaticFindObject_t>(GetProcAddress(hCore, "?StaticFindObject@UObject@@SAPAV1@PAVUClass@@PAV1@PBDH@Z"));
    pIsChildOf = reinterpret_cast<IsChildOf_t>(GetProcAddress(hCore, "?IsChildOf@UStruct@@QBEHPBV1@@Z"));
    pUClassClass = GetProcAddress(hCore, "?PrivateStaticClass@UClass@@0V1@A");

    if (auto p = GetProcAddress(hCore, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A"))
    {
        auto pArray = reinterpret_cast<uint8_t*>(p);
        pppObjects = reinterpret_cast<void***>(pArray);
        pnObjects = reinterpret_cast<int32_t*>(pArray + sizeof(void*));
    }

    if (!pStaticFindObject || !pIsChildOf || !pUClassClass || !pppObjects)
        LogWarn("MenuScale: Core.dll did not export the object system, the menus stay in the corner");
}

static void __fastcall FunctionPostLoad(uint8_t* pFunction, void*)
{
    shFunctionPostLoad.thiscall<void>(pFunction);

    auto pScript = *reinterpret_cast<uint8_t**>(pFunction + nOffsetScriptData);
    const auto nSize = *reinterpret_cast<int32_t*>(pFunction + nOffsetScriptNum);

    if (!pScript || nSize < nConstSize)
        return;

    // Own buffer: the engine's is one of four that rotate.
    char szFullName[1024]{};
    pGetFullName(pFunction, szFullName);

    // A live operand needs its value before the function it sits in can run.
    auto nTotal = 0;

    // The one function that both derives the ratios and clamps them.
    if (std::strstr(szFullName, ".XIIIWindow.BeforePaint"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // The one place 800x600 is not a centring bound: out of game the bar is laid out in an 800x600
    // box at the top left instead of spanning the page. False at 640x480, so it is nailed false.
    else if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // Everywhere the 800x600 box is used to centre what was drawn inside it.
    else if (OwnedByAny(szFullName, { ".XIIIWindow.", ".XIIIGUIBaseButton.", ".XIIIGuiButton.", ".XIIIComboControl.", ".XIIIValueControl." }))
    {
        nTotal += RecordLive(pScript, nSize, 800, Live::MenuWidth)
            + RecordLive(pScript, nSize, 600, Live::MenuHeight);
    }

    // Bar height, edge margins, button icons.
    if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        auto nCount = 0;

        // Small numbers are EX_IntConstByte unless folded to a float, so both are looked for.
        for (const uint8_t nPixels : { 2, 3, 6, 10, 28, 30, 32 })
        {
            nCount += RecordPixels(pScript, nSize, nPixels);
            nCount += RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        nTotal += nCount;
    }

    if (std::strstr(szFullName, ".XIIIRootWindow."))
        nTotal += RewriteOnce(pScript, nSize, 448, 480.0f);

    // Button width comes from the ratio but height from a raw 30, so they come out as slivers.
    if (std::strstr(szFullName, ".LayoutButtons")
        && OwnedByAny(szFullName, { ".XIIIMsgBox.", ".XIIIMsgBoxInGame.", ".XIIILiveMsgBox." }))
    {
        auto nCount = 0;

        for (const uint8_t nPixels : { 5, 30 })
        {
            nCount += RecordPixels(pScript, nSize, nPixels);
            nCount += RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        nTotal += nCount;
    }

    if (std::strstr(szFullName, ".XIIIEditCtrl."))
        nTotal += ReassociateFieldWidth(pScript, nSize);

    // The pause panel's native takes its surround bar width as an argument and script hands it a
    // raw 10, the one number on the page still in 640x480 pixels.
    if (std::strstr(szFullName, ".XIIIMenuInGame.Paint"))
    {
        // The native takes floats, so this one may be a whole EX_FloatConst.
        nTotal += RecordPixels(pScript, nSize, 10)
            + RecordLive(pScript, nSize, 10, Live::Pixels);
    }

    if (std::strstr(szFullName, ".XIIIMenuInGame.SetObjectives"))
        nTotal += NeutraliseLinePitch(pScript, nSize);

    // The three pages that draw a title box around text they measured themselves.
    if (OwnedByAny(szFullName, { ".XIIIMenuSelectProfile.", ".XIIIMenuCreateProfile.", ".XIIIMenuMultiProfile." }))
        nTotal += NeutraliseMeasuredRatio(pScript, nSize);

    if (nTotal)
        ApplyLiveSites();
}

static void InitCore()
{
    auto hCore = GetModuleHandleW(L"Core.dll");
    if (!hCore)
        return;

    auto pPostLoad = GetProcAddress(hCore, "?PostLoad@UFunction@@UAEXXZ");
    pGetFullName = reinterpret_cast<GetFullName_t>(GetProcAddress(hCore, "?GetFullName@UObject@@QBEPBDPAD@Z"));

    if (!pPostLoad || !pGetFullName)
    {
        LogWarn("MenuScale: Core.dll did not export UFunction::PostLoad or UObject::GetFullName, the menus stay capped at 800x600");
        return;
    }

    InitObjectSystem();

    shFunctionPostLoad = safetyhook::create_inline(pPostLoad, FunctionPostLoad);

    if (!shFunctionPostLoad)
        LogWarn("MenuScale: UFunction::PostLoad could not be hooked, the menus stay capped at 800x600");
}

class MenuScale
{
public:
    MenuScale()
    {
        MongooseFix::onCoreInitEvent() += []() { InitCore(); };

        // The package loads before the device, so these are written once there is one.
        MongooseFix::onD3DDrvInitEvent() += []()
        {
            onDeviceResetEvent() += []()
            {
                static std::once_flag flag;
                std::call_once(flag, ApplyObjectFixes);

                ApplyLiveSites();
            };
        };
    }
} MenuScale;
