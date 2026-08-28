module;

#include <common.hxx>

export module menuscale;

import common;
import display;
import logging;

// XIDInterf.u lay menus out in 640x480, wrong twice: BeforePaint make fRatioX=ClipX/640 and
// fRatioY=ClipY/480 apart, stretch 4:3 art on 16:9, then clamp both to 800/640; XIIIWindow.
// InternalOn* and the four control classes pillarbox rest with
// if (ClipX > 800) SetOrigin((ClipX-800)/2, ...), mouse hit test included.
//
// All be constant operands in package bytecode, patched in place, nothing inserted, no jump moved:
//
//   BeforePaint 800/600     1e9, so FClamp give back what it got.
//   800/600 elsewhere       640*scale, 480*scale, the centring those branches meant, same as
//                           ClipX/ClipY on 4:3, so tests go false.
//   DisplayHelpBar          the one part in raw screen pixels (32 tall, 30 in from each edge, 6
//                           from bottom, 28 pixel icons, 3 pixel gap). One byte each, so past
//                           scale of about 8 they stop growing.
//   XIIIRootWindow 448      480. 448 be NTSC safe area squash that shipped on PC, multiply nearly
//                           every menu Y by 0.9333.
//   bCenterInGame           on. Set on only three input menus; on, centring branch, control bounds
//                           and mouse correction agree.
//   bCalculateSize          on. Dozen menus turn it off and give caption width 200/160/175,
//                           640x480 pixels in field measured in screen pixels, no room in byte for
//                           scaled value.
//
// First two be flags, reached through object system; rest recorded and rewritten on every device
// reset. One scale for both axes come free from hudscale.ixx making canvas report menu box.
//
// Patching happen in UFunction::PostLoad (vtable +0x28, body shared by no other Core.dll class),
// after linker deserialise bytecode and before anything can call it.
//
//   UStruct  +0x48  Script.Data, +0x4C Script.ArrayNum  (TArray<BYTE>)
static constexpr auto nOffsetScriptData = 0x48;
static constexpr auto nOffsetScriptNum = 0x4C;

static constexpr auto fAuthoredWidth = 640.0f;
static constexpr auto fAuthoredHeight = 480.0f;

// Where every PC message box open, in 640x480 units. Not page centre: pause panel use same corner
// and both must agree.
static constexpr auto fMsgBoxOrgX = 220.0f;
static constexpr auto fMsgBoxOrgY = 130.0f;

// EX_IntConst 0x1D, EX_FloatConst 0x1E, each plus four little endian bytes. Which one a literal
// became not obvious from source: 800 in float arithmetic fold to float, the 800 in
// "C.ClipX > 800" stay int behind cast, so both looked for.
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

// Plain byte scan. Five byte constant could in principle sit inside another instruction operand,
// but never do in these functions, and disassembler to rule it out be lot of code.
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

// Number resolution decide; operand keep whichever encoding it compiled with.
enum class Live
{
    MenuWidth,
    MenuHeight,
    Pixels,
    ScreenWidth,
    ScreenHeight,
    MsgBoxLeft,
    MsgBoxTop,
};

struct Site
{
    uint8_t* pConstant;
    Live eKind;
    bool bFloat;
    float fAuthored;
    uint8_t aExpected[nConstSize];
};

static std::mutex mtxSites;
static std::vector<Site> aSites;

// EX_IntConstByte behind cast to float. One byte, so original kept and scale clamped.
static constexpr uint8_t aCastByte[] = { 0x39, 0x3F, 0x2C };

struct PixelSite
{
    uint8_t* pOperand;
    uint8_t nAuthored;
    uint8_t nExpected;
};

static std::vector<PixelSite> aPixelSites;

// Site stay ours only while it still hold bytes we last put there. Scans be unanchored and
// transforms below rewrite nodes in place, so recorded operand can land inside node a later
// transform moved, and package unload free buffer outright. Either way address stop meaning what it
// meant, and rewriting it on next device reset put float constant over whatever live there now,
// mangled operand VM then run.
//
// Callers hold mtxSites.
static void AddSite(uint8_t* p, Live eKind, bool bFloat, float fAuthored)
{
    Site site{ p, eKind, bFloat, fAuthored, {} };
    std::memcpy(site.aExpected, p, nConstSize);
    aSites.push_back(site);
}

static void AddPixelSite(uint8_t* p)
{
    aPixelSites.push_back(PixelSite{ p, *p, *p });
}

static void ApplyLiveSites()
{
    const auto nWidth = nBackBufferWidth.load();
    const auto nHeight = nBackBufferHeight.load();

    if (nWidth <= 0 || nHeight <= 0)
        return;

    // Parenthesised because Windows.h included without NOMINMAX and min be macro.
    const auto fScale = (std::min)(nWidth / fAuthoredWidth, nHeight / fAuthoredHeight);

    std::lock_guard g(mtxSites);

    auto nDropped = 0;

    std::erase_if(aSites, [&](Site& site)
    {
        if (std::memcmp(site.pConstant, site.aExpected, nConstSize) != 0)
        {
            nDropped++;
            return true;
        }

        auto fValue = 0.0f;

        switch (site.eKind)
        {
        // Branches compare against box canvas report, so bound must make (box - bound)/2 come out
        // as (screen - box)/2. Past 24:9 screen be wider than two boxes and bound go negative,
        // which both comparison and halving take, so no floor: floored at zero menu sat half a box
        // in from left.
        case Live::MenuWidth:
            fValue = 2.0f * fAuthoredWidth * fScale - nWidth;
            break;
        case Live::MenuHeight:
            fValue = 2.0f * fAuthoredHeight * fScale - nHeight;
            break;
        case Live::Pixels:     fValue = site.fAuthored * fScale; break;

        case Live::ScreenWidth:  fValue = static_cast<float>(nWidth); break;
        case Live::ScreenHeight: fValue = static_cast<float>(nHeight); break;

        // Authored corner plus pillarbox GUI natives never apply.
        case Live::MsgBoxLeft:
            fValue = fMsgBoxOrgX * fScale + (nWidth - fAuthoredWidth * fScale) * 0.5f;
            break;
        case Live::MsgBoxTop:
            fValue = fMsgBoxOrgY * fScale + (nHeight - fAuthoredHeight * fScale) * 0.5f;
            break;
        }

        const auto c = site.bFloat ? FloatConst(fValue) : IntConst(static_cast<int32_t>(fValue + 0.5f));
        injector::WriteMemoryRaw(site.pConstant, const_cast<uint8_t*>(c.aBytes), nConstSize, true);
        std::memcpy(site.aExpected, c.aBytes, nConstSize);

        return false;
    });

    std::erase_if(aPixelSites, [&](PixelSite& site)
    {
        if (*site.pOperand != site.nExpected)
        {
            nDropped++;
            return true;
        }

        const auto nScaled = static_cast<uint8_t>(std::clamp(static_cast<int>(site.nAuthored * fScale + 0.5f), 1, 255));
        injector::WriteMemory<uint8_t>(site.pOperand, nScaled, true);
        site.nExpected = nScaled;

        return false;
    });

    if (nDropped)
        LogWarn("MenuScale: dropped {} live site(s) whose bytecode moved", nDropped);
}

static int RecordPixels(uint8_t* pScript, int nSize, uint8_t nValue)
{
    std::lock_guard g(mtxSites);

    auto nCount = 0;

    for (auto i = 0; i + 4 <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aCastByte, sizeof(aCastByte)) != 0 || pScript[i + 3] != nValue)
            continue;

        AddPixelSite(pScript + i + 3);
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
        [eKind, fAuthored](uint8_t* p) { AddSite(p, eKind, false, fAuthored); });

    nCount += ForEachConstant(pScript, nSize, FloatConst(fAuthored),
        [eKind, fAuthored](uint8_t* p) { AddSite(p, eKind, true, fAuthored); });

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

// XIIIEditCtrl size its text field as (WinWidth*640 - FirstBoxWidth)*fRatioX, but FirstBoxWidth
// already scaled, line above read (WinWidth*640*fRatioX - 16*fRatioX)/2, so ratio land twice. Past
// ratio of 2 it go negative and BeforePaint shorten until it fit loops never end: "Runaway loop
// detected". Paint second box be same expression.
//
// Prefix tree of float * (171, 0xAB) and float - (175, 0xAF), each <opcode> <a> <b>
// EX_EndFunctionParms, so reassociate to WinWidth*640*fRatioX - FirstBoxWidth be two opcodes
// swapped and operands after them exchanged, in place:
//
//   AB AF AB <WinWidth> <640> 16 <FirstBoxWidth> 16 <fRatioX>       16
//   AF AB AB <WinWidth> <640> 16 <fRatioX>       16 <FirstBoxWidth> 16
//
// Variables be EX_InstanceVariable plus pointer, five bytes each. 640 be int constant behind
// EX_PrimitiveCast to float, seven bytes, no float constant.
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

// Pages size own box from text they just measured:
//
//   C.TextSize(Caps(TitleText), W, H);
//   DrawStretchedTexture(C, X, 80*fRatioY, (W+40)*fRatioX, (H+10)*fScaleTo*fRatioY, myRoot.FondMenu);
//
// TextSize answer in screen pixels, so ratio already in W and H and box grow with its square:
// "Select your profile" come out 504 tall at 3840x2160 where 144 be right. Both terms share one
// shape (measured local, 640x480 margin behind cast to float, ratio), so one fix serve both: scale
// margin, put float 1 in place of trailing ratio. Same bytes, in place:
//
//   AB AB AE <H> <39 3F 2C 0A> 16 <fScaleTo> 16 <fRatioY> 16     height, 25 bytes
//   AB    AE <W> <39 3F 2C 28> 16 <fRatioX>  16                  width, 18 bytes
//
// Margin be one byte, so past scale of about three the wider one (80, on multiplayer profile page)
// clamp at 255 and that box come out slightly narrow.
static constexpr uint8_t nAddFloat = 0xAE;
static constexpr uint8_t nLocalVariable = 0x00;
static constexpr auto nMeasuredHeightLength = 25;
static constexpr auto nMeasuredWidthLength = 18;
static constexpr uint8_t nDivideFloat = 0xAC;

// Locals holding screen pixels. TextSize and StrLen answer through two out parameters, so parameter
// list ending <local> <local> 16 name both. Mixed with 640x480 numbers they scale twice.
static std::vector<uint32_t> aMeasured;

static void CollectMeasured(uint8_t* pScript, int nSize)
{
    aMeasured.clear();

    for (auto i = 0; i + 11 <= nSize; i++)
    {
        if (pScript[i] != nLocalVariable || pScript[i + 5] != nLocalVariable
            || pScript[i + 10] != nEndFunctionParms)
            continue;

        aMeasured.push_back(*reinterpret_cast<uint32_t*>(pScript + i + 1));
        aMeasured.push_back(*reinterpret_cast<uint32_t*>(pScript + i + 6));
    }
}

static bool Measured(const uint8_t* pOperand)
{
    return std::find(aMeasured.begin(), aMeasured.end(),
        *reinterpret_cast<const uint32_t*>(pOperand)) != aMeasured.end();
}

static int NeutraliseMeasuredRatio(uint8_t* pScript, int nSize)
{
    const auto cOne = FloatConst(1.0f);
    auto nCount = 0;

    for (auto i = 0; i + nMeasuredWidthLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (i + nMeasuredHeightLength <= nSize
            && p[0] == nMultiplyFloat && p[1] == nMultiplyFloat && p[2] == nAddFloat
            && p[3] == nLocalVariable && Measured(p + 4)
            && std::memcmp(p + 8, aCastByte, sizeof(aCastByte)) == 0
            && p[12] == nEndFunctionParms
            && p[13] == nInstanceVariable && p[18] == nEndFunctionParms
            && p[19] == nInstanceVariable && p[24] == nEndFunctionParms)
        {
            {
                std::lock_guard g(mtxSites);
                AddPixelSite(p + 11);
            }

            injector::WriteMemoryRaw(p + 19, const_cast<uint8_t*>(cOne.aBytes), nConstSize, true);
            i += nMeasuredHeightLength - 1;
            nCount++;
            continue;
        }

        if (p[0] == nMultiplyFloat && p[1] == nAddFloat
            && p[2] == nLocalVariable && Measured(p + 3)
            && std::memcmp(p + 7, aCastByte, sizeof(aCastByte)) == 0
            && p[11] == nEndFunctionParms
            && p[12] == nInstanceVariable && p[17] == nEndFunctionParms)
        {
            {
                std::lock_guard g(mtxSites);
                AddPixelSite(p + 10);
            }

            injector::WriteMemoryRaw(p + 12, const_cast<uint8_t*>(cOne.aBytes), nConstSize, true);
            i += nMeasuredWidthLength - 1;
            nCount++;
        }
    }

    return nCount;
}

// Message box be whole pixels drawn by GUI natives, which read WinLeft straight, no origin, no
// ratio, so pillarbox never reach it. Only its panel get offset added (hudscale AddOrigin), so
// panel centred, caption and buttons a pillarbox left.
//
// Cheaper to put box in screen coordinates than teach seven draw paths the offset, so
// AdjustPosition centre against screen, not 4:3 box it get handed:
//
//   0F <WinLeft> AC AF 19 <C> <skip> <size> <ClipX> <WinWidth> 16 <39 3F 2C 02> 16     33 bytes
//
// ClipX read be fourteen bytes, so be float constant plus zero. X first, Y second.
static constexpr uint8_t nLet = 0x0F;
static constexpr uint8_t nContext = 0x19;
static constexpr uint8_t nIntConstCast[] = { 0x39, 0x3F, 0x1D };
static constexpr auto nCentreLength = 33;
static constexpr auto nCentreReadAt = 8;

// Canvas field read and float constant plus zero both be fourteen bytes. Constant rewritten with
// resolution.
static void WriteLiveRead(uint8_t* pRead, Live eKind)
{
    uint8_t aFixed[14];
    aFixed[0] = nAddFloat;
    std::memcpy(aFixed + 1, FloatConst(0.0f).aBytes, nConstSize);
    std::memcpy(aFixed + 6, nIntConstCast, sizeof(nIntConstCast));
    std::memset(aFixed + 9, 0, 4);
    aFixed[13] = nEndFunctionParms;

    injector::WriteMemoryRaw(pRead, aFixed, sizeof(aFixed), true);

    std::lock_guard g(mtxSites);
    AddSite(pRead + 1, eKind, true, 0.0f);
}

static int CentreOnScreen(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nCentreLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (p[0] != nLet || p[1] != nInstanceVariable
            || p[6] != nDivideFloat || p[7] != nSubtractFloat
            || p[8] != nContext || p[9] != nLocalVariable
            || p[17] != nInstanceVariable || p[22] != nInstanceVariable
            || p[27] != nEndFunctionParms
            || std::memcmp(p + 28, aCastByte, sizeof(aCastByte)) != 0
            || p[31] != 2 || p[32] != nEndFunctionParms)
            continue;

        WriteLiveRead(p + nCentreReadAt, nCount == 0 ? Live::ScreenWidth : Live::ScreenHeight);

        i += nCentreLength - 1;
        nCount++;
    }

    return nCount;
}

// In game box have no AdjustPosition: it keep InitBox argument, in 4:3 box coordinates. InitBox
// open by copying each parameter out, one statement each:
//
//   0F <WinWidth> <_Width>  ... <WinTop> <_OrgY>  <WinLeft> <_OrgX>          11 bytes each
//
// Third and fourth name origins. Every read of them become screen corner; local read and float
// constant both be five bytes.
static constexpr auto nCopyLength = 11;
static constexpr auto nOrgYStatement = 2;
static constexpr auto nOrgXStatement = 3;

// Message box size never left 640x480. Half the pages hand InitBox raw numbers, half multiply by
// fRatioX, and box be screen pixels now, so raw ones stay a sixth of their size at 4K: "This
// profile already exists" come out one word to a line. Call site cannot tell the two units apart.
// Parameters copied out one statement each, so every read of the four size ones become constant
// resolution drive. One size for every box, within a fifth of what pages already use, and
// InternalOnPreDraw still grow height to fit message.
static constexpr auto fMsgBoxWidth = 400.0f;
static constexpr auto fMsgBoxHeight = 230.0f;
static constexpr auto fMsgBoxMargin = 10.0f;

// InitBox opening copies, in order: _Width, _Height, _OrgY, _OrgX, _LineWidth, _LineHeight.
static constexpr int aSizeStatement[] = { 0, 1, 4, 5 };
static constexpr float aSizeAuthored[] = { fMsgBoxWidth, fMsgBoxHeight, fMsgBoxMargin, fMsgBoxMargin };
static constexpr auto nSizeStatements = 6;

static int NormaliseMsgBox(uint8_t* pScript, int nSize)
{
    for (auto n = 0; n < nSizeStatements; n++)
    {
        auto p = pScript + n * nCopyLength;

        if ((n + 1) * nCopyLength > nSize || p[0] != nLet || p[1] != nInstanceVariable
            || p[6] != nLocalVariable)
            return 0;
    }

    uint32_t aParam[std::size(aSizeStatement)]{};

    for (size_t n = 0; n < std::size(aSizeStatement); n++)
        std::memcpy(&aParam[n], pScript + aSizeStatement[n] * nCopyLength + 7, sizeof(uint32_t));

    auto nCount = 0;

    for (auto i = 0; i + nConstSize <= nSize; i++)
    {
        if (pScript[i] != nLocalVariable)
            continue;

        for (size_t n = 0; n < std::size(aParam); n++)
        {
            if (std::memcmp(pScript + i + 1, &aParam[n], sizeof(uint32_t)) != 0)
                continue;

            injector::WriteMemoryRaw(pScript + i, const_cast<uint8_t*>(FloatConst(0.0f).aBytes),
                nConstSize, true);

            {
                std::lock_guard g(mtxSites);
                AddSite(pScript + i, Live::Pixels, true, aSizeAuthored[n]);
            }

            i += nConstSize - 1;
            nCount++;
            break;
        }
    }

    return nCount;
}

static int CentreInitBox(uint8_t* pScript, int nSize)
{
    uint32_t aOrg[2]{};

    for (auto n = 0; n <= nOrgXStatement; n++)
    {
        auto p = pScript + n * nCopyLength;

        if ((n + 1) * nCopyLength > nSize || p[0] != nLet || p[1] != nInstanceVariable
            || p[6] != nLocalVariable)
            return 0;

        if (n == nOrgYStatement || n == nOrgXStatement)
            std::memcpy(&aOrg[n - nOrgYStatement], p + 7, sizeof(uint32_t));
    }

    auto nCount = 0;

    for (auto i = 0; i + nConstSize <= nSize; i++)
    {
        if (pScript[i] != nLocalVariable)
            continue;

        for (auto n = 0; n < 2; n++)
        {
            if (std::memcmp(pScript + i + 1, &aOrg[n], sizeof(uint32_t)) != 0)
                continue;

            injector::WriteMemoryRaw(pScript + i, const_cast<uint8_t*>(FloatConst(0.0f).aBytes),
                nConstSize, true);

            {
                std::lock_guard g(mtxSites);
                AddSite(pScript + i, n == 0 ? Live::MsgBoxTop : Live::MsgBoxLeft, true, 0.0f);
            }

            i += nConstSize - 1;
            nCount++;
            break;
        }
    }

    return nCount;
}

// Comic pages pop onomatopoeia over whatever be highlighted, growing it from nothing:
//
//   DrawStretchedTexture(C, (205*fRatioX + 223) - 223*zoom, (23*fRatioY + 73) - 73*zoom,
//                        223*zoom, 73*zoom, tOnomatopee[0]);
//
// Size be raw pixels, so WOO! WOO! and SLAM! stay as drawn. The two margins pinning corner be raw
// too while corner be scaled, so texture creep as it grow. Want: ratio(205 + 223*(1 - zoom)).
//
// Reassociation, so position fit in place. Sizes be float constants, rewritten with resolution:
//
//   AF AE AB <205> <r> 16 <39 3F 2C DF> 16 AB <39 3F 2C DF> <zoom> 16 16          31 bytes
//   AB AF AE <205>       <39 3F 2C DF> 16 AB <39 3F 2C DF> <zoom> 16 16 <r> 16
static constexpr auto nPopPositionLength = 31;
static constexpr auto nPopLength = 86;

static bool PopPosition(const uint8_t* p)
{
    return p[0] == nSubtractFloat && p[1] == nAddFloat && p[2] == nMultiplyFloat
        && p[3] == nFloatConst && p[8] == nInstanceVariable && p[13] == nEndFunctionParms
        && std::memcmp(p + 14, aCastByte, sizeof(aCastByte)) == 0 && p[18] == nEndFunctionParms
        && p[19] == nMultiplyFloat
        && std::memcmp(p + 20, aCastByte, sizeof(aCastByte)) == 0
        && p[24] == nLocalVariable && p[29] == nEndFunctionParms && p[30] == nEndFunctionParms;
}

static void RewritePopPosition(uint8_t* p)
{
    uint8_t aFixed[nPopPositionLength];
    aFixed[0] = nMultiplyFloat;
    aFixed[1] = nSubtractFloat;
    aFixed[2] = nAddFloat;
    std::memcpy(aFixed + 3, p + 3, nConstSize);      // corner
    std::memcpy(aFixed + 8, p + 14, 4);              // margin
    aFixed[12] = nEndFunctionParms;
    aFixed[13] = nMultiplyFloat;
    std::memcpy(aFixed + 14, p + 20, 4);             // margin again
    std::memcpy(aFixed + 18, p + 24, nConstSize);    // zoom
    aFixed[23] = nEndFunctionParms;
    aFixed[24] = nEndFunctionParms;
    std::memcpy(aFixed + 25, p + 8, nConstSize);     // ratio
    aFixed[30] = nEndFunctionParms;

    injector::WriteMemoryRaw(p, aFixed, nPopPositionLength, true);
}

static int ScaleOnomatopoeia(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nPopLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (!PopPosition(p) || !PopPosition(p + nPopPositionLength))
            continue;

        // The two sizes, each float constant times zoom.
        if (p[62] != nMultiplyFloat || p[63] != nFloatConst || p[68] != nLocalVariable
            || p[73] != nEndFunctionParms
            || p[74] != nMultiplyFloat || p[75] != nFloatConst || p[80] != nLocalVariable
            || p[85] != nEndFunctionParms)
            continue;

        RewritePopPosition(p);
        RewritePopPosition(p + nPopPositionLength);

        {
            std::lock_guard g(mtxSites);

            for (const auto nAt : { 63, 75 })
            {
                auto fAuthored = 0.0f;
                std::memcpy(&fAuthored, p + nAt + 1, sizeof(fAuthored));
                AddSite(p + nAt, Live::Pixels, true, fAuthored);
            }
        }

        i += nPopLength - 1;
        nCount++;
    }

    return nCount;
}

// XIIIWindow.DrawLabel grow its box to fit caption:
//
//   C.StrLen(myL.sLabel, W, H);
//   if ((W + 16*fRatioX) > myL.XSize) { Offset = W + 16*fRatioX - myL.XSize; myL.XSize += Offset;
//                                       ... myL.XPos -= Offset; }
//
// W be screen pixels, XPos/XSize be 640x480, so Offset carry ratio into both: box drawn
// (XSize-4)*fRatioX wide, ratio squared, and slide a screen width left. Every page that label
// anything go through it, input pages titles and key rows above all.
//
// In 640x480 terms: W/fRatioX + 16. Same nodes reassociated, fit in place:
//
//   AE <W> AB <39 3F 2C 10> 16 <fRatioX>       16 16       18 bytes
//   AE AC  <W>              <fRatioX>       16 <39 3F 2C 10> 16
static constexpr auto nLabelWidenLength = 18;

static int FixLabelWiden(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nLabelWidenLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (p[0] != nAddFloat || p[1] != nLocalVariable || !Measured(p + 2)
            || p[6] != nMultiplyFloat
            || std::memcmp(p + 7, aCastByte, sizeof(aCastByte)) != 0
            || p[11] != nInstanceVariable
            || p[16] != nEndFunctionParms || p[17] != nEndFunctionParms)
            continue;

        uint8_t aFixed[nLabelWidenLength];
        aFixed[0] = nAddFloat;
        aFixed[1] = nDivideFloat;
        std::memcpy(aFixed + 2, p + 1, nConstSize);      // W
        std::memcpy(aFixed + 7, p + 11, nConstSize);     // fRatioX
        aFixed[12] = nEndFunctionParms;
        std::memcpy(aFixed + 13, p + 7, 4);              // the 16
        aFixed[17] = nEndFunctionParms;

        injector::WriteMemoryRaw(p, aFixed, nLabelWidenLength, true);
        i += nLabelWidenLength - 1;
        nCount++;
    }

    return nCount;
}

// Dozen page titles centre their caption in 640x480 box same wrong way:
//
//   C.SetPos((150 + (160-W)/2)*fRatioX, (47.5 - H/2)*fRatioY);
//
// Whole term scaled, W and H with it, so text leave box: at 3840x2160 video page title land off
// top left corner. Want 150*r + (160*r - W)/2, which fold to (150 + 160/2)*r - W/2. Same node
// count; one multiply of divisor pad the byte fold free:
//
//   AB AE <150> AC AF <39 3F 2C A0> <W> 16 <39 3F 2C 02> 16 16 <r> 16          31 bytes
//   AF AB <230> <r> 16 AB AC <W> <39 3F 2C 02> 16 <39 3F 2C 01> 16 16
static constexpr auto nMeasuredCentreLength = 31;

static int FoldMeasuredCentre(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nMeasuredCentreLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (p[0] != nMultiplyFloat || p[1] != nAddFloat || p[2] != nFloatConst
            || p[7] != nDivideFloat || p[8] != nSubtractFloat
            || std::memcmp(p + 9, aCastByte, sizeof(aCastByte)) != 0
            || p[13] != nLocalVariable || !Measured(p + 14)
            || p[18] != nEndFunctionParms
            || std::memcmp(p + 19, aCastByte, sizeof(aCastByte)) != 0
            || p[23] != nEndFunctionParms || p[24] != nEndFunctionParms
            || p[25] != nInstanceVariable || p[30] != nEndFunctionParms)
            continue;

        float fBase = 0.0f;
        std::memcpy(&fBase, p + 3, sizeof(fBase));
        const auto cFolded = FloatConst(fBase + p[12] * 0.5f);

        uint8_t aFixed[nMeasuredCentreLength];
        aFixed[0] = nSubtractFloat;
        aFixed[1] = nMultiplyFloat;
        std::memcpy(aFixed + 2, cFolded.aBytes, nConstSize);
        std::memcpy(aFixed + 7, p + 25, nConstSize);     // r
        aFixed[12] = nEndFunctionParms;
        aFixed[13] = nMultiplyFloat;
        aFixed[14] = nDivideFloat;
        std::memcpy(aFixed + 15, p + 13, nConstSize);    // W
        std::memcpy(aFixed + 20, p + 19, 4);             // the 2
        aFixed[24] = nEndFunctionParms;
        std::memcpy(aFixed + 25, aCastByte, sizeof(aCastByte));
        aFixed[28] = 1;
        aFixed[29] = nEndFunctionParms;
        aFixed[30] = nEndFunctionParms;

        injector::WriteMemoryRaw(p, aFixed, nMeasuredCentreLength, true);
        i += nMeasuredCentreLength - 1;
        nCount++;
    }

    return nCount;
}

// Y half of same line, and pages that only have that half:
//
//   (47.5 - H/2)*fRatioY   ->   47.5*fRatioY - H/2
//
// Plain reassociation, so byte count hold without padding:
//
//   AB AF <47.5> AC <H> <39 3F 2C 02> 16 16 <r> 16      25 bytes
//   AF AB <47.5> <r> 16 AC <H> <39 3F 2C 02> 16 16
static constexpr auto nMeasuredHalfLength = 25;

static int SwapMeasuredCentre(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (auto i = 0; i + nMeasuredHalfLength <= nSize; i++)
    {
        auto p = pScript + i;

        if (p[0] != nMultiplyFloat || p[1] != nSubtractFloat || p[2] != nFloatConst
            || p[7] != nDivideFloat
            || p[8] != nLocalVariable || !Measured(p + 9)
            || std::memcmp(p + 13, aCastByte, sizeof(aCastByte)) != 0
            || p[17] != nEndFunctionParms || p[18] != nEndFunctionParms
            || p[19] != nInstanceVariable || p[24] != nEndFunctionParms)
            continue;

        uint8_t aFixed[nMeasuredHalfLength];
        aFixed[0] = nSubtractFloat;
        aFixed[1] = nMultiplyFloat;
        std::memcpy(aFixed + 2, p + 2, nConstSize);      // 47.5
        std::memcpy(aFixed + 7, p + 19, nConstSize);     // r
        aFixed[12] = nEndFunctionParms;
        aFixed[13] = nDivideFloat;
        std::memcpy(aFixed + 14, p + 8, nConstSize);     // H
        std::memcpy(aFixed + 19, p + 13, 4);             // the 2
        aFixed[23] = nEndFunctionParms;
        aFixed[24] = nEndFunctionParms;

        injector::WriteMemoryRaw(p, aFixed, nMeasuredHalfLength, true);
        i += nMeasuredHalfLength - 1;
        nCount++;
    }

    return nCount;
}

// Same fault again in SetObjectives, which keep running LineY in whole pixels and step it by
// height it just measured:
//
//   C.TextSize( MsgArray[i], W, H);
//   C.SetPos( 50*fRatioX, (LineY+6)*fRatioY + iObjDecalY);
//   LineY += 0.9*H;
//
// H be screen pixels, so LineY be too, and ratio on it put line two at 595 at 3840x2160 where 256
// be right. LineY being int make the sum int add behind cast to float, not title boxes float add;
// repair be same:
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
            AddPixelSite(p + 10);
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

// reinterpret_cast be no constant expression, so const, not constexpr.
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

// UField::SuperField, for reaching property without knowing which ancestor declared it.
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

// Turn script bool on in class defaults and everything already built from them: subclass copy
// parent defaults as it load, so one that loaded first hold old value, same for every existing
// window. Optional float be cached ratio; zero it, resize happen.
static void SetDefaultBool(const char* szClass, const char* szProperty, const char* szStale = nullptr,
    bool bOn = true)
{
    auto pClass = FindObject(pAnyPackage, szClass);
    auto pProperty = pClass ? FindProperty(pClass, szProperty) : nullptr;

    // Class level did not load be no fault, and this run twice a second.
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

        // Object array be sparse.
        if (!pObject)
            continue;

        auto pObjectClass = *reinterpret_cast<void**>(pObject + nOffsetObjectClass);

        if (pObjectClass == pUClassClass)
        {
            if (!pIsChildOf(pObject, pClass))
                continue;

            // Not GetDefaultObject, which assert on class whose defaults not sized yet.
            auto pDefaults = *reinterpret_cast<uint8_t**>(pObject + nOffsetClassDefaults);
            const auto nSize = *reinterpret_cast<int32_t*>(pObject + nOffsetClassDefaults + 4);

            if (!pDefaults || nSize < static_cast<int32_t>(nOffset + sizeof(uint32_t)))
                continue;

            if (bOn)
                *reinterpret_cast<uint32_t*>(pDefaults + nOffset) |= nMask;
            else
                *reinterpret_cast<uint32_t*>(pDefaults + nOffset) &= ~nMask;

            continue;
        }

        // Existing window keep what it built with, so it get set too.
        if (!pObjectClass || !pIsChildOf(pObjectClass, pClass))
            continue;

        if (bOn)
            *reinterpret_cast<uint32_t*>(pObject + nOffset) |= nMask;
        else
            *reinterpret_cast<uint32_t*>(pObject + nOffset) &= ~nMask;

        if (pStale)
            *reinterpret_cast<float*>(pObject + nStaleOffset) = 0.0f;
    }
}

// GUIStyles BorderOffsets be absolute pixels, same 16 at every resolution, and be what script read
// when placing text inside control. Native draw ignore them and size from material (handled canvas
// side). Authored values kept so resolution change not compound.
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

    // Both spellings tried so class name not what this hang on.
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

// Menus built as they open, so new windows must be caught.
export void RefreshInterfaceObjects()
{
    if (!pStaticFindObject || !pIsChildOf || !pUClassClass || !pppObjects || !pnObjects)
        return;

    static uint32_t nLastSweep = 0;
    const auto nNow = GetTickCount();

    if (nLastSweep && nNow - nLastSweep < 500)
        return;

    nLastSweep = nNow ? nNow : 1;

    SetDefaultBool("XIIIWindow", "bCenterInGame");

    // Box lay out in screen pixels now, but help bar it inherit lay out in 4:3 box, so it land a
    // pillarbox left of page own bar, which say same thing.
    for (const auto szBox : { "XIIIMsgBox", "XIIIMsgBoxInGame", "XIIILiveMsgBox" })
        SetDefaultBool(szBox, "bDisplayBar", nullptr, false);

    // XIIIComboControl cache ratio it last sized itself at, so that cleared too.
    SetDefaultBool("XIIIComboControl", "bCalculateSize", "OldRatioX");
    SetDefaultBool("XIIIValueControl", "bCalculateSize");
    SetDefaultBool("XIIIEditCtrl", "bCalculateSize");
    SetDefaultBool("XIIICheckBoxControl", "bCalculateSize");

    ScaleStyleBorders();
}

// Once, after interface package loaded and device exist.
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

    // Own buffer: engine one be one of four that rotate.
    char szFullName[1024]{};
    pGetFullName(pFunction, szFullName);

    // Live operand need its value before function it sit in can run.
    auto nTotal = 0;

    // The one function that both make ratios and clamp them.
    if (std::strstr(szFullName, ".XIIIWindow.BeforePaint"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // The one place 800x600 be no centring bound: out of game bar lay out in 800x600 box at top
    // left instead of spanning page. False at 640x480, so nailed false.
    else if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // Everywhere 800x600 box used to centre what drawn inside it.
    else if (OwnedByAny(szFullName, { ".XIIIWindow.", ".XIIIGUIBaseButton.", ".XIIIGuiButton.", ".XIIIComboControl.", ".XIIIValueControl." }))
    {
        nTotal += RecordLive(pScript, nSize, 800, Live::MenuWidth)
            + RecordLive(pScript, nSize, 600, Live::MenuHeight);
    }

    // Bar height, edge margins, button icons.
    if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        auto nCount = 0;

        // Small numbers be EX_IntConstByte unless folded to float, so both looked for.
        for (const uint8_t nPixels : { 2, 3, 6, 10, 28, 30, 32 })
        {
            nCount += RecordPixels(pScript, nSize, nPixels);
            nCount += RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        nTotal += nCount;
    }

    if (std::strstr(szFullName, ".XIIIRootWindow."))
        nTotal += RewriteOnce(pScript, nSize, 448, 480.0f);

    // Message box buttons be raw pixels, 88 wide, 30 tall, 5 up from bottom, and gap between them
    // be whatever left, so they come out slivers bunched in middle.
    if ((std::strstr(szFullName, ".LayoutButtons") || std::strstr(szFullName, ".AdjustPosition"))
        && OwnedByAny(szFullName, { ".XIIIMsgBox.", ".XIIIMsgBoxInGame.", ".XIIILiveMsgBox." }))
    {
        auto nCount = 0;

        for (const uint8_t nPixels : { 5, 30, 88 })
        {
            nCount += RecordPixels(pScript, nSize, nPixels);
            nCount += RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        nTotal += nCount;
    }

    if (std::strstr(szFullName, ".AfterPaint") && std::strstr(szFullName, "XIDInterf."))
        nTotal += ScaleOnomatopoeia(pScript, nSize);

    if (std::strstr(szFullName, ".XIIIMsgBox.AdjustPosition"))
        nTotal += CentreOnScreen(pScript, nSize);

    if (std::strstr(szFullName, ".XIIIMsgBoxInGame.InitBox"))
        nTotal += CentreInitBox(pScript, nSize);

    // Offsets InitBox inset own controls by be raw pixels too.
    if (std::strstr(szFullName, ".XIIIMsgBox.InitBox"))
    {
        nTotal += NormaliseMsgBox(pScript, nSize);

        for (const uint8_t nPixels : { 10, 30, 35 })
            nTotal += RecordPixels(pScript, nSize, nPixels);
    }

    // Caret under name being typed, and dots at each end when text trimmed: raw pixels, couple of
    // texels wide at any resolution. Every float constant here be one of their sizes.
    if (std::strstr(szFullName, ".XIIIEditCtrl.Paint"))
    {
        nTotal += RecordLive(pScript, nSize, 8, Live::Pixels)
            + RecordLive(pScript, nSize, 2, Live::Pixels);
    }

    if (std::strstr(szFullName, ".XIIIEditCtrl."))
        nTotal += ReassociateFieldWidth(pScript, nSize);

    // Pause panel native take its surround bar width as argument and script hand it raw 10, the one
    // number on page still in 640x480 pixels.
    if (std::strstr(szFullName, ".XIIIMenuInGame.Paint"))
    {
        // Native take floats, so this one may be whole EX_FloatConst.
        nTotal += RecordPixels(pScript, nSize, 10)
            + RecordLive(pScript, nSize, 10, Live::Pixels);
    }

    if (std::strstr(szFullName, ".XIIIMenuInGame.SetObjectives"))
        nTotal += NeutraliseLinePitch(pScript, nSize);

    // Every menu page that measure own text. Shapes be specific and each gated on local really
    // holding a measurement, so whole package can be swept.
    if (std::strstr(szFullName, "XIDInterf."))
    {
        CollectMeasured(pScript, nSize);

        if (!aMeasured.empty())
        {
            nTotal += NeutraliseMeasuredRatio(pScript, nSize)
                + FoldMeasuredCentre(pScript, nSize)
                + SwapMeasuredCentre(pScript, nSize);

            if (std::strstr(szFullName, ".XIIIWindow.DrawLabel"))
                nTotal += FixLabelWiden(pScript, nSize);
        }
    }

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

        // Package load before device, so these written once there be one.
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
