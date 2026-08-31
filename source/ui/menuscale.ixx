module;

#include <common.hxx>

export module menuscale;

import common;
import display;
import logging;

// XIDInterf.u lays the menus out in 640x480, wrong twice: BeforePaint makes fRatioX=ClipX/640 and
// fRatioY=ClipY/480 apart, stretching 4:3 art on 16:9, then clamps both to 800/640; XIIIWindow.
// InternalOn* and the four control classes pillarbox the rest with
// if (ClipX > 800) SetOrigin((ClipX-800)/2, ...), mouse hit test included.
//
// All constant operands in the package bytecode, patched in place, nothing inserted, no jump
// moved:
//
//   BeforePaint 800/600     1e9, so FClamp gives back what it got.
//   800/600 elsewhere       640*scale, 480*scale, the centring those branches meant, the same as
//                           ClipX/ClipY on 4:3, so the tests go false.
//   DisplayHelpBar          the one part in raw screen pixels (32 tall, 30 in from each edge, 6
//                           from the bottom, 28 pixel icons, 3 pixel gap). One byte each, so they
//                           stop growing past a scale of about 8.
//   XIIIRootWindow 448      480. 448 is the NTSC safe area squash that shipped on PC, multiplying
//                           nearly every menu Y by 0.9333.
//   bCenterInGame           on. Set on only three input menus. On, the centring branch, control
//                           bounds and mouse correction agree.
//   bCalculateSize          on. A dozen menus turn it off and give a caption width of 200/160/175,
//                           640x480 pixels in a field of screen pixels, no room in the byte for a
//                           scaled value.
//
// First two are flags, reached through the object system. Rest are recorded and rewritten on every
// device reset. One scale for both axes comes free from hudscale.ixx making the canvas report the
// menu box.
//
// Patched in UFunction::PostLoad (vtable +0x28, a body no other Core.dll class shares), after the
// linker deserialises the bytecode and before anything can call it.
//
//   UStruct  +0x48  Script.Data, +0x4C Script.ArrayNum  (TArray<BYTE>)
static constexpr auto nOffsetScriptData = 0x48;
static constexpr auto nOffsetScriptNum = 0x4C;

static constexpr auto fAuthoredWidth = 640.0f;
static constexpr auto fAuthoredHeight = 480.0f;

// Where every PC message box opens, in 640x480 units. Not the page centre, the pause panel uses
// the same corner and the two must agree.
static constexpr auto fMsgBoxOrgX = 220.0f;
static constexpr auto fMsgBoxOrgY = 130.0f;

// EX_IntConst 0x1D, EX_FloatConst 0x1E, each plus four little endian bytes. Which one a literal
// became is not obvious from the source. An 800 in float arithmetic folds to a float, the 800 in
// "C.ClipX > 800" stays an int behind a cast, so both are looked for.
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

// Plain byte scan. A five byte constant could sit inside another instruction's operand, but none
// do in these functions and a disassembler to rule it out is a lot of code.
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

// A number the resolution decides. The operand keeps whichever encoding it compiled with.
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

// EX_IntConstByte behind a cast to float. One byte, so the original is kept and the scale clamped.
static constexpr uint8_t aCastByte[] = { 0x39, 0x3F, 0x2C };

struct PixelSite
{
    uint8_t* pOperand;
    uint8_t nAuthored;
    uint8_t nExpected;
};

static std::vector<PixelSite> aPixelSites;

// A site stays ours only while it still holds the bytes we last put there. Scans are unanchored
// and the transforms below rewrite nodes in place, so a recorded operand can land inside a node a
// later transform moved, and a package unload frees the buffer outright. Either way the address
// stops meaning what it meant, and rewriting it on the next device reset would put a float constant
// over whatever lives there now, leaving a mangled operand for the VM to run. An unloaded package
// takes its pages with it, so the address is probed before it is read.
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

    const auto fScale = (std::min)(nWidth / fAuthoredWidth, nHeight / fAuthoredHeight);

    std::lock_guard g(mtxSites);

    auto nDropped = 0;

    std::erase_if(aSites, [&](Site& site)
    {
        if (IsBadReadPtr(site.pConstant, nConstSize) || std::memcmp(site.pConstant, site.aExpected, nConstSize) != 0)
        {
            nDropped++;
            return true;
        }

        auto fValue = 0.0f;

        switch (site.eKind)
        {
        // Branches compare against the box the canvas reports, so the bound must make
        // (box - bound)/2 come out as (screen - box)/2. Past 24:9 the screen is wider than two
        // boxes and the bound goes negative, which both the comparison and the halving take. With a
        // floor at zero the menu sat half a box in from the left.
        case Live::MenuWidth:
            fValue = 2.0f * fAuthoredWidth * fScale - nWidth;
            break;
        case Live::MenuHeight:
            fValue = 2.0f * fAuthoredHeight * fScale - nHeight;
            break;
        case Live::Pixels:     fValue = site.fAuthored * fScale; break;

        case Live::ScreenWidth:  fValue = static_cast<float>(nWidth); break;
        case Live::ScreenHeight: fValue = static_cast<float>(nHeight); break;

        // Authored corner plus the pillarbox the GUI natives never apply.
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
        if (IsBadReadPtr(site.pOperand, 1) || *site.pOperand != site.nExpected)
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

// XIIIEditCtrl sizes its text field as (WinWidth*640 - FirstBoxWidth)*fRatioX, but FirstBoxWidth is
// already scaled, the line above reading (WinWidth*640*fRatioX - 16*fRatioX)/2, so the ratio lands
// twice. Past a ratio of 2 it goes negative and BeforePaint's shorten until it fits loop never ends,
// "Runaway loop detected". Paint's second box is the same expression.
//
// A prefix tree of float * (171, 0xAB) and float - (175, 0xAF), each <opcode> <a> <b>
// EX_EndFunctionParms. Reassociating to WinWidth*640*fRatioX - FirstBoxWidth is two opcodes swapped
// and the operands after them exchanged, in place:
//
//   AB AF AB <WinWidth> <640> 16 <FirstBoxWidth> 16 <fRatioX>       16
//   AF AB AB <WinWidth> <640> 16 <fRatioX>       16 <FirstBoxWidth> 16
//
// Variables are EX_InstanceVariable plus a pointer, five bytes each. 640 is an int constant behind
// an EX_PrimitiveCast to float, seven bytes, not a float constant.
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

// Pages size their own box from the text they just measured:
//
//   C.TextSize(Caps(TitleText), W, H);
//   DrawStretchedTexture(C, X, 80*fRatioY, (W+40)*fRatioX, (H+10)*fScaleTo*fRatioY, myRoot.FondMenu);
//
// TextSize answers in screen pixels, so the ratio is already in W and H and the box grows with its
// square. "Select your profile" comes out 504 tall at 3840x2160 where 144 is right. Both terms share
// one shape (measured local, 640x480 margin behind a cast to float, ratio), so one fix serves both:
// scale the margin, put a float 1 in place of the trailing ratio. Same bytes, in place:
//
//   AB AB AE <H> <39 3F 2C 0A> 16 <fScaleTo> 16 <fRatioY> 16     height, 25 bytes
//   AB    AE <W> <39 3F 2C 28> 16 <fRatioX>  16                  width, 18 bytes
//
// The margin is one byte, so past a scale of about three the wider one (80, multiplayer profile
// page) clamps at 255 and that box comes out slightly narrow.
static constexpr uint8_t nAddFloat = 0xAE;
static constexpr uint8_t nLocalVariable = 0x00;
static constexpr auto nMeasuredHeightLength = 25;
static constexpr auto nMeasuredWidthLength = 18;
static constexpr uint8_t nDivideFloat = 0xAC;

// Locals holding screen pixels. TextSize and StrLen answer through two out parameters, so a
// parameter list ending <local> <local> 16 names both. Mixed with 640x480 numbers they scale
// twice.
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

// A message box is whole pixels drawn by GUI natives, which read WinLeft straight, no origin and no
// ratio, so the pillarbox never reaches it. Only its panel gets the offset added (hudscale
// AddOrigin), leaving the panel centred and the caption and buttons a pillarbox left.
//
// Cheaper to put the box in screen coordinates than to teach seven draw paths the offset, so
// AdjustPosition centres against the screen rather than the 4:3 box it is handed:
//
//   0F <WinLeft> AC AF 19 <C> <skip> <size> <ClipX> <WinWidth> 16 <39 3F 2C 02> 16     33 bytes
//
// The ClipX read is fourteen bytes, so is a float constant plus zero. X first, Y second.
static constexpr uint8_t nLet = 0x0F;
static constexpr uint8_t nContext = 0x19;
static constexpr uint8_t nIntConstCast[] = { 0x39, 0x3F, 0x1D };
static constexpr auto nCentreLength = 33;
static constexpr auto nCentreReadAt = 8;

// A canvas field read and a float constant plus zero are both fourteen bytes. The constant is
// rewritten with the resolution.
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

// The in game box has no AdjustPosition, it keeps the InitBox argument in 4:3 box coordinates.
// InitBox opens by copying each parameter out, one statement each:
//
//   0F <WinWidth> <_Width>  ... <WinTop> <_OrgY>  <WinLeft> <_OrgX>          11 bytes each
//
// Third and fourth name the origins. Every read of them becomes the screen corner, a local read
// and a float constant both being five bytes.
static constexpr auto nCopyLength = 11;
static constexpr auto nOrgYStatement = 2;
static constexpr auto nOrgXStatement = 3;

// Message box size never left 640x480. Half the pages hand InitBox raw numbers, half multiply by
// fRatioX, and the box is screen pixels now, so the raw ones stay a sixth of their size at 4K,
// "This profile already exists" coming out one word to a line. The call site cannot tell the two
// units apart. Parameters are copied out one statement each, so every read of the four size ones
// becomes a constant the resolution drives. One size for every box, within a fifth of what the
// pages already use, and InternalOnPreDraw still grows the height to fit the message.
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

// The panel behind the pause menu and the message box takes its 9 slice margin in raw pixels, two
// float 10s side by side, DrawMsgboxBackground at the page's Paint, InitBox at the call site.
// Matched by shape, so the multiplayer pages the old per page rule missed get it too.
static int RecordMsgboxMargins(uint8_t* pScript, int nSize)
{
    const auto cTen = FloatConst(10.0f);
    auto nCount = 0;

    for (auto i = 0; i + 2 * nConstSize <= nSize; i++)
    {
        if (std::memcmp(pScript + i, cTen.aBytes, nConstSize) != 0
            || std::memcmp(pScript + i + nConstSize, cTen.aBytes, nConstSize) != 0)
            continue;

        {
            std::lock_guard g(mtxSites);
            AddSite(pScript + i, Live::Pixels, true, 10.0f);
            AddSite(pScript + i + nConstSize, Live::Pixels, true, 10.0f);
        }

        i += 2 * nConstSize - 1;
        nCount += 2;
    }

    return nCount;
}

// Comic pages pop onomatopoeia over whatever is highlighted, growing it from nothing:
//
//   DrawStretchedTexture(C, (205*fRatioX + 223) - 223*zoom, (23*fRatioY + 73) - 73*zoom,
//                        223*zoom, 73*zoom, tOnomatopee[0]);
//
// Size is raw pixels, so WOO! WOO! and SLAM! stay as drawn. The two margins pinning the corner are
// raw too while the corner is scaled, so the texture creeps as it grows. Wanted is
// ratio(205 + 223*(1 - zoom)).
//
// Reassociation, so the position fits in place. Sizes are float constants, rewritten with the
// resolution:
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

// XIIIWindow.DrawLabel grows its box to fit the caption:
//
//   C.StrLen(myL.sLabel, W, H);
//   if ((W + 16*fRatioX) > myL.XSize) { Offset = W + 16*fRatioX - myL.XSize; myL.XSize += Offset;
//                                       ... myL.XPos -= Offset; }
//
// W is screen pixels and XPos/XSize are 640x480, so Offset carries the ratio into both. The box is
// drawn (XSize-4)*fRatioX wide, ratio squared, and slides a screen width left. Every page that
// labels anything goes through it, the input pages' titles and key rows above all.
//
// In 640x480 terms: W/fRatioX + 16. The same nodes reassociated, fitting in place:
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

// A dozen page titles centre their caption in the 640x480 box the same wrong way:
//
//   C.SetPos((150 + (160-W)/2)*fRatioX, (47.5 - H/2)*fRatioY);
//
// The whole term is scaled, W and H with it, so the text leaves the box. At 3840x2160 the video
// page title lands off the top left corner. Wanted is 150*r + (160*r - W)/2, folding to
// (150 + 160/2)*r - W/2. Same node count, one multiply by the divisor padding the byte the fold
// frees:
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

// The Y half of the same line, and the pages that only have that half:
//
//   (47.5 - H/2)*fRatioY   ->   47.5*fRatioY - H/2
//
// Plain reassociation, so the byte count holds without padding:
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

// Same fault in SetObjectives, which keeps a running LineY in whole pixels and steps it by the
// height it just measured:
//
//   C.TextSize( MsgArray[i], W, H);
//   C.SetPos( 50*fRatioX, (LineY+6)*fRatioY + iObjDecalY);
//   LineY += 0.9*H;
//
// H is screen pixels, so LineY is too, and the ratio on it puts line two at 595 at 3840x2160 where
// 256 is right. LineY being an int makes the sum an int add behind a cast to float rather than the
// title boxes' float add. Same repair:
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

// reinterpret_cast is not a constant expression, so const, not constexpr.
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

// UField::SuperField, reaches a property without knowing which ancestor declared it.
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

// Turns a script bool on in the class defaults and in everything already built from them. A
// subclass copies its parent's defaults as it loads, so one that loaded first holds the old value,
// and so does every existing window. The optional float is a cached ratio, zeroed to force a
// resize.
static void SetDefaultBool(const char* szClass, const char* szProperty, const char* szStale = nullptr,
    bool bOn = true)
{
    auto pClass = FindObject(pAnyPackage, szClass);
    auto pProperty = pClass ? FindProperty(pClass, szProperty) : nullptr;

    // A class not loaded yet is no fault, and this runs twice a second.
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

            // Not GetDefaultObject, it asserts on a class whose defaults are not sized yet.
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

        // An existing window keeps what it was built with, so it is set too.
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

// XIIIValueControl lays itself out unlike every other control: two thirds caption, one third value,
// box inset 40 and 64, arrows 16 in from each end. Raw pixels, so the third holds no value once the
// row grows and nothing lines up with the combo above it.
//
// The combo works off a half it computes in script: caption to half-32r, box half-16r wide at half,
// arrows half-36r and width-12r. The same numbers here are constants, patched in place:
//
//   AB AB AB <2.0>                 2/3 becomes 0.393, half less 32r on the 300 wide row
//   AC AB AB <WinWidth> ... <2C 03>   box width: the third becomes a half
//   anchored <2C 10>               arrow: add becomes subtract, 4r back off the anchor
//   anchored <2C 28>               box corner: 40 becomes 16
//   <2C 40>                        box width: 64 becomes 0, the box runs to the arrow
//   loose <2C 10>                  right arrow 12r in; in BeforePaint 32r, recentring the text
//
// The anchor folds a 32r gap into a fraction, exact only at the width it was folded for. One page
// uses 288 and lands a few pixels off.
static constexpr float fValueAnchor = 1.18f;
static constexpr uint8_t aTwoThirds[] = { 0xAB, 0xAB, 0xAB, 0x1E, 0x00, 0x00, 0x00, 0x40 };
static constexpr uint8_t aThirdHead[] = { 0xAC, 0xAB, 0xAB, 0x01 };
static constexpr uint8_t aThirdDivisor[] = { 0x39, 0x3F, 0x2C, 0x03 };
static constexpr uint8_t aAnchorHead[] = { 0xAC, 0xAB, 0xAB, 0xAB, 0x1E };
static constexpr auto nAnchorLength = 34;
static constexpr auto nThirdLength = 26;
static constexpr auto nThirdDivisorAt = 22;

// Operand byte of each <39 3F 2C value>.
static std::vector<uint8_t*> FindCastBytes(uint8_t* pScript, int nSize, uint8_t nValue)
{
    std::vector<uint8_t*> aFound;

    for (auto i = 0; i + 4 <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aCastByte, sizeof(aCastByte)) == 0 && pScript[i + 3] == nValue)
        {
            aFound.push_back(pScript + i + 3);
            i += 3;
        }
    }

    return aFound;
}

static void WritePixelSite(uint8_t* pOperand, uint8_t nAuthored)
{
    injector::WriteMemory<uint8_t>(pOperand, nAuthored, true);

    std::lock_guard g(mtxSites);
    AddPixelSite(pOperand);
}

static int ReshapeValueControl(uint8_t* pScript, int nSize, bool bPaint)
{
    auto nCount = 0;

    for (auto i = 0; i + static_cast<int>(sizeof(aTwoThirds)) <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aTwoThirds, sizeof(aTwoThirds)) != 0)
            continue;

        const auto c = FloatConst(fValueAnchor);
        injector::WriteMemoryRaw(pScript + i + 3, const_cast<uint8_t*>(c.aBytes), nConstSize, true);
        i += sizeof(aTwoThirds) - 1;
        nCount++;
    }

    for (auto i = 0; i + nThirdLength <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aThirdHead, sizeof(aThirdHead)) != 0
            || std::memcmp(pScript + i + nThirdDivisorAt, aThirdDivisor, sizeof(aThirdDivisor)) != 0)
            continue;

        injector::WriteMemory<uint8_t>(pScript + i + nThirdDivisorAt + 3, 2, true);
        i += nThirdLength - 1;
        nCount++;
    }

    // Arrow first, while its 16 is the only anchored one. After it the loose pass below takes every
    // 16 left, and the corner takes 16 after that.
    for (auto i = 1; i + nAnchorLength + 4 <= nSize; i++)
    {
        if (std::memcmp(pScript + i, aAnchorHead, sizeof(aAnchorHead)) != 0
            || pScript[i - 1] != nAddFloat
            || std::memcmp(pScript + i + nAnchorLength, aCastByte, sizeof(aCastByte)) != 0
            || pScript[i + nAnchorLength + 3] != 16)
            continue;

        injector::WriteMemory<uint8_t>(pScript + i - 1, nSubtractFloat, true);
        WritePixelSite(pScript + i + nAnchorLength + 3, 4);
        i += nAnchorLength;
        nCount++;
    }

    for (auto pOperand : FindCastBytes(pScript, nSize, 16))
    {
        WritePixelSite(pOperand, bPaint ? 12 : 32);
        nCount++;
    }

    for (auto pOperand : FindCastBytes(pScript, nSize, 40))
    {
        WritePixelSite(pOperand, 16);
        nCount++;
    }

    for (auto pOperand : FindCastBytes(pScript, nSize, 64))
    {
        injector::WriteMemory<uint8_t>(pOperand, 0, true);
        nCount++;
    }

    return nCount;
}

// Pages turn bCalculateSize off and hand the control a caption width in 640x480 units, which it
// reads as screen pixels. The sweep below turns the flag back on twice a second, so a page opens
// wrong and rights itself a moment later. Turned on where it is written instead: EX_LetBool,
// target, EX_False.
static constexpr uint8_t nBoolVariable = 0x2D;
static constexpr uint8_t nLetBool = 0x14;
static constexpr uint8_t nFalse = 0x28;
static constexpr uint8_t nTrue = 0x27;
static constexpr auto nBoolReadLength = 6;
static constexpr auto nContextPrefix = 10;

static int ForceCalculateSize(uint8_t* pScript, int nSize)
{
    auto nCount = 0;

    for (const auto szClass : { "XIIIComboControl", "XIIIValueControl", "XIIIEditCtrl", "XIIICheckBoxControl" })
    {
        auto pClass = FindObject(pAnyPackage, szClass);
        auto pProperty = pClass ? FindProperty(pClass, "bCalculateSize") : nullptr;

        // A class not loaded yet is no fault, the sweep still catches it.
        if (!pProperty)
            continue;

        for (auto i = 0; i + nBoolReadLength < nSize; i++)
        {
            if (pScript[i] != nBoolVariable || pScript[i + 1] != nInstanceVariable
                || std::memcmp(pScript + i + 2, &pProperty, sizeof(pProperty)) != 0
                || pScript[i + nBoolReadLength] != nFalse)
                continue;

            const auto bWrite = (i >= 1 && pScript[i - 1] == nLetBool)
                || (i >= nContextPrefix && pScript[i - nContextPrefix] == nLetBool);

            // Otherwise it is a comparison against false, meaning the opposite.
            if (!bWrite)
                continue;

            injector::WriteMemory<uint8_t>(pScript + i + nBoolReadLength, nTrue, true);
            i += nBoolReadLength;
            nCount++;
        }
    }

    return nCount;
}

// GUIStyles BorderOffsets is absolute pixels, the same 16 at every resolution, and what script
// reads when placing text inside a control. The native draw ignores them and sizes from the
// material (handled canvas side). Authored values are kept so a resolution change does not
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

    // Both spellings tried, so the class name is not what this hangs on.
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

// Menus are built as they open, so new windows have to be caught.
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

    // The box lays out in screen pixels now, the help bar it inherits in the 4:3 box, so it lands a
    // pillarbox left of the page's own bar, which says the same thing.
    for (const auto szBox : { "XIIIMsgBox", "XIIIMsgBoxInGame", "XIIILiveMsgBox" })
        SetDefaultBool(szBox, "bDisplayBar", nullptr, false);

    // XIIIComboControl caches the ratio it last sized itself at, cleared too.
    SetDefaultBool("XIIIComboControl", "bCalculateSize", "OldRatioX");
    SetDefaultBool("XIIIValueControl", "bCalculateSize");
    SetDefaultBool("XIIIEditCtrl", "bCalculateSize");
    SetDefaultBool("XIIICheckBoxControl", "bCalculateSize");

    ScaleStyleBorders();
}

// Once, after the interface package has loaded and a device exists.
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

    // Our own buffer, the engine's is one of four that rotate.
    char szFullName[1024]{};
    pGetFullName(pFunction, szFullName);

    // A live operand needs its value before the function it sits in can run.
    auto nTotal = 0;

    // The one function that makes the ratios and clamps them.
    if (std::strstr(szFullName, ".XIIIWindow.BeforePaint"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // The one place 800x600 is not a centring bound. The out of game bar lays out in an 800x600 box
    // at the top left instead of spanning the page. False at 640x480, so nailed false.
    else if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        nTotal += RewriteOnce(pScript, nSize, 800, 1e9f)
            + RewriteOnce(pScript, nSize, 600, 1e9f);
    }
    // Everywhere the 800x600 box is used to centre what is drawn inside it.
    else if (OwnedByAny(szFullName, { ".XIIIWindow.", ".XIIIGUIBaseButton.", ".XIIIGuiButton.", ".XIIIComboControl.", ".XIIIValueControl." }))
    {
        nTotal += RecordLive(pScript, nSize, 800, Live::MenuWidth)
            + RecordLive(pScript, nSize, 600, Live::MenuHeight);
    }

    // Bar height, edge margins, button icons.
    if (std::strstr(szFullName, ".XIIIWindow.DisplayHelpBar"))
    {
        auto nCount = 0;

        // Small numbers are EX_IntConstByte unless folded to float, so both are looked for.
        for (const uint8_t nPixels : { 2, 3, 6, 10, 28, 30, 32 })
        {
            nCount += RecordPixels(pScript, nSize, nPixels);
            nCount += RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        nTotal += nCount;
    }

    if (std::strstr(szFullName, ".XIIIRootWindow."))
        nTotal += RewriteOnce(pScript, nSize, 448, 480.0f);

    // Message box buttons are raw pixels, 88 wide, 30 tall, 5 up from the bottom, the gap between
    // them whatever is left, so they come out slivers bunched in the middle.
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

    // The offsets InitBox insets its own controls by are raw pixels too.
    if (std::strstr(szFullName, ".XIIIMsgBox.InitBox"))
    {
        nTotal += NormaliseMsgBox(pScript, nSize);

        for (const uint8_t nPixels : { 10, 30, 35 })
            nTotal += RecordPixels(pScript, nSize, nPixels);
    }

    // The caret under the name being typed and the dots at each end when the text is trimmed, raw
    // pixels, a couple of texels wide at any resolution. Every float constant here is one of their
    // sizes.
    if (std::strstr(szFullName, ".XIIIEditCtrl.Paint"))
    {
        nTotal += RecordLive(pScript, nSize, 8, Live::Pixels)
            + RecordLive(pScript, nSize, 2, Live::Pixels);
    }

    if (std::strstr(szFullName, ".XIIIEditCtrl."))
        nTotal += ReassociateFieldWidth(pScript, nSize);

    // Arrow width, its 4 down, box inset 6 and 12 down, raw pixels here and scaled on every other
    // control. Geometry first, it moves the constants the recording then holds on to.
    if (std::strstr(szFullName, ".XIIIValueControl.Paint")
        || std::strstr(szFullName, ".XIIIValueControl.BeforePaint"))
    {
        nTotal += ReshapeValueControl(pScript, nSize, std::strstr(szFullName, ".Paint") != nullptr);

        for (const uint8_t nPixels : { 4, 6, 8, 12 })
        {
            nTotal += RecordPixels(pScript, nSize, nPixels)
                + RecordLive(pScript, nSize, nPixels, Live::Pixels);
        }

        // Arrow width, the one float 16 left.
        nTotal += RecordLive(pScript, nSize, 16, Live::Pixels);
    }

    if (std::strstr(szFullName, "XIDInterf."))
        nTotal += ForceCalculateSize(pScript, nSize) + RecordMsgboxMargins(pScript, nSize);

    if (std::strstr(szFullName, ".XIIIMenuInGame.SetObjectives"))
        nTotal += NeutraliseLinePitch(pScript, nSize);

    // Every menu page that measures its own text. Shapes are specific and each gated on the local
    // really holding a measurement, so the whole package can be swept.
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

        // Package loads before the device, so these are written once there is one.
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
