module;

#include <common.hxx>

export module hudscale;

import common;
import logging;
import menuscale;
import mongoosemenu;

// For the HUD pass the canvas reports the 480 unit tall screen the HUD was drawn against and every
// quad out is multiplied back up; glyphs ride along, being quads through the same function. Works
// because every HUD tile and glyph reaches FCanvasUtil::DrawTile (exported; first four arguments
// are corners in absolute screen pixels), AHUD::eventPostRender is exported and called from one
// place, and UCanvas::Update runs once a frame ahead of the pass.
//
// Menus need the real ClipX, script hit testing against the real pixel mouse, so only menu text is
// scaled, in two places that must agree or centred labels drift: the quads a string emits, about
// the string start, and both outputs of the per character metrics primitive behind TextSize.
//
// UCanvas: +0x34 OrgX +0x38 OrgY +0x3C ClipX +0x40 ClipY +0x64 SizeX +0x68 SizeY +0x74 Viewport
// Text is point sampled, wrong when magnified, so smoothing goes back on for scaled strings.
static constexpr auto nOffsetOrgX = 0x34;
static constexpr auto nOffsetOrgY = 0x38;
static constexpr auto nOffsetViewport = 0x74;
static constexpr auto nOffsetRenderInterface = 0x154;   // UViewport
static constexpr auto nSmoothingSlot = 0xAC / 4;        // FRenderInterface::SetSmoothing(UBOOL)
static constexpr auto nOffsetZ = 0x4C;
static constexpr auto nOffsetDrawColour = 0x58;
static constexpr auto nOffsetClipX = 0x3C;
static constexpr auto nOffsetClipY = 0x40;
static constexpr auto nOffsetSizeX = 0x64;
static constexpr auto nOffsetSizeY = 0x68;

// execGetScreenHeight returns the same literal, which is right once the canvas agrees.
static constexpr auto fAuthoredWidth = 640.0f;
static constexpr auto fAuthoredHeight = 480.0f;

// __thiscall: the four corners are the first stack slots past the return address.
static constexpr auto nOffsetArgX1 = 0x04;

static SafetyHookInline shHudPostRender{};
static SafetyHookInline shGuiPreRender{};
static SafetyHookInline shGuiPostRender{};
static SafetyHookInline shDrawString{};
static SafetyHookInline shDrawLine{};
static SafetyHookInline shSetOrigin{};

// execDrawTile and execDrawTileClipped pass CurX+OrgX; DrawTileStretched, DrawTileScaled,
// DrawTileBound, DrawTileScaleBound, DrawTileJustified, DrawIcon and DrawPattern pass CurX and
// never read OrgX, which only shows now the interface centres itself (origin 480,0 at 3840 wide).
// All seven take (UMaterial*, X, Y, ...), so one mid hook at entry adds the origin.
static constexpr auto nOffsetArgX = 0x08;   // __thiscall: return address, UMaterial*, X, Y
static constexpr auto nOffsetArgY = 0x0C;

// 0xA0, DrawTileStretched, taken over whole rather than nudged; see below.
static constexpr int aOriginBlindSlots[] = { 0x84, 0x88, 0xA4, 0xA8, 0xAC, 0xB0 };
static SafetyHookMid amhOriginBlind[std::size(aOriginBlindSlots)]{};
static SafetyHookMid mhDrawTile{};
static SafetyHookMid mhDrawTriangle{};
static SafetyHookMid mhCharSize{};
static SafetyHookMid mhDrawPortal{};

// Set for the length of the HUD pass; atomic because the ini watcher reads the scale.
static std::atomic<bool> bHudPass = false;
static std::atomic<float> fHudScale = 1.0f;

static std::atomic<bool> bGuiPass = false;
static std::atomic<float> fGuiScale = 1.0f;

// The interface box is 640x480 times one scale, and neither script use of ClipX, ratios and the
// right edge of page wide draws, wants the screen. The real values are kept for the HUD pass, which
// lays out against the screen even inside this one.
static std::atomic<bool> bGuiClamped = false;
static std::atomic<float> fRealClipX = 0.0f;
static std::atomic<float> fRealClipY = 0.0f;
static bool bStringRun = false;
static float fStringX = 0.0f;
static float fStringY = 0.0f;

// The objectives band runs to the screen edges but its lines sit 50 units in from the page, so the
// widening is recorded and the text moves with it.
static constexpr auto fAuthoredBandHeight = 118.0f;
static bool bBandDrawn = false;
static float fBandHeight = 0.0f;

// Thread local: the loading thread animates while the main thread is still drawing, and a shared
// flag would scale the menu too. Above one only inside the pass, so flag and scale are one value.
static thread_local float fLoadingScale = 1.0f;

static void DrawTile(SafetyHookContext& ctx)
{
    auto pCorners = reinterpret_cast<float*>(ctx.esp + nOffsetArgX1);

    if (fLoadingScale > 1.0f)
    {
        pCorners[0] *= fLoadingScale;
        pCorners[1] *= fLoadingScale;
        pCorners[2] *= fLoadingScale;
        pCorners[3] *= fLoadingScale;
        return;
    }

    // Virtual coordinates, so the quad moves as well as grows.
    if (bHudPass.load())
    {
        const auto fScale = fHudScale.load();

        pCorners[0] *= fScale;
        pCorners[1] *= fScale;
        pCorners[2] *= fScale;
        pCorners[3] *= fScale;
        return;
    }

    // About the string start, so the position script chose survives.
    if (bStringRun)
    {
        const auto fScale = fGuiScale.load();

        pCorners[0] = fStringX + (pCorners[0] - fStringX) * fScale;
        pCorners[1] = fStringY + (pCorners[1] - fStringY) * fScale;
        pCorners[2] = fStringX + (pCorners[2] - fStringX) * fScale;
        pCorners[3] = fStringY + (pCorners[3] - fStringY) * fScale;
    }
}

// The backing quad scales through DrawTile, but the region the scene renders into comes from these
// four stack ints and does not, leaving a 320x240 picture in the corner. Hooked after the last
// parameter comes off the script stack, before either use.
static void DrawPortal(SafetyHookContext& ctx)
{
    if (!bHudPass.load())
        return;

    const auto fScale = fHudScale.load();

    for (auto nOffset : { 0x1c, 0x20, 0x24, 0x28 })
    {
        auto pValue = reinterpret_cast<int32_t*>(ctx.ebp - nOffset);
        *pValue = static_cast<int32_t>(*pValue * fScale);
    }
}

// UCanvas::DrawFrame draws the comic border as triangles, the one HUD primitive that is not a
// tile. Corners are the first six stack floats.
static void DrawTriangle(SafetyHookContext& ctx)
{
    if (!bHudPass.load())
        return;

    const auto fScale = fHudScale.load();
    auto pCorners = reinterpret_cast<float*>(ctx.esp + nOffsetArgX1);

    for (auto i = 0; i < 6; i++)
        pCorners[i] *= fScale;
}

// A stroke is one pixel wide whatever the box size and has no width to set, so it is repeated
// along its perpendicular. HUD coordinates are virtual and move as well as thicken, interface ones
// are already real.
static void __fastcall DrawLine(uint8_t* pThis, void*, float fX1, float fY1, float fX2, float fY2,
    uint32_t nColour, int nStyle)
{
    const auto bHud = bHudPass.load();
    const auto fScale = bHud ? fHudScale.load() : (bGuiPass.load() ? fGuiScale.load() : 1.0f);

    if (bHud)
    {
        fX1 *= fScale;
        fY1 *= fScale;
        fX2 *= fScale;
        fY2 *= fScale;
    }

    // The authored border is two pixels: UCanvas::DrawTile lays the ring down twice.
    const auto nStrokes = std::clamp(static_cast<int>(fScale * 2.0f + 0.5f), 1, 32);
    const auto fLength = std::sqrt((fX2 - fX1) * (fX2 - fX1) + (fY2 - fY1) * (fY2 - fY1));

    if (nStrokes <= 1 || fLength <= 0.0f)
    {
        shDrawLine.thiscall<void>(pThis, fX1, fY1, fX2, fY2, nColour, nStyle);
        return;
    }

    // Stepped either side, so the border grows about its own line rather than off one edge.
    const auto fStepX = -(fY2 - fY1) / fLength;
    const auto fStepY = (fX2 - fX1) / fLength;

    for (auto i = 0; i < nStrokes; i++)
    {
        const auto fOffset = (i - (nStrokes - 1) * 0.5f);
        shDrawLine.thiscall<void>(pThis, fX1 + fStepX * fOffset, fY1 + fStepY * fOffset,
            fX2 + fStepX * fOffset, fY2 + fStepY * fOffset, nColour, nStyle);
    }
}

// WorldToScreen projects through the viewport, not the canvas, so it answers real pixels whatever
// the canvas reports and the HUD pass would scale script's target boxes again. Handed pass units.
static SafetyHookInline shWorldToScreen{};

static void __fastcall WorldToScreen(uint8_t* pThis, void*, void* pStack, float* pResult)
{
    shWorldToScreen.thiscall<void>(pThis, pStack, pResult);

    if (!pResult || !bHudPass.load())
        return;

    const auto fScale = fHudScale.load();

    pResult[0] /= fScale;
    pResult[1] /= fScale;
}

static void __fastcall SetOrigin(uint8_t* pCanvas, void*, void* pStack, void* pResult)
{
    shSetOrigin.thiscall<void>(pCanvas, pStack, pResult);

    if (!bGuiPass.load() || !pCanvas)
        return;

    // Message box geometry is in pixels, but like every window it sets the origin from WinLeft as a
    // fraction of the page: 990 gives 2,851,680. Its panel, backdrop, controls and captions are all
    // absolute screen pixels, so zero is right and anything else strands the captions, which ignore
    // the origin.
    auto pOrgX = reinterpret_cast<float*>(pCanvas + nOffsetOrgX);
    auto pOrgY = reinterpret_cast<float*>(pCanvas + nOffsetOrgY);
    const auto fLimit = (std::max)(fRealClipX.load(), *reinterpret_cast<float*>(pCanvas + nOffsetClipX));

    if (fLimit > 0.0f && (std::abs(*pOrgX) > fLimit || std::abs(*pOrgY) > fLimit))
    {
        *pOrgX = 0.0f;
        *pOrgY = 0.0f;
    }
}

// Set for one execDrawMsgboxBackground; the first quad says which caller it is.
static bool bMsgboxPass = false;

// The framework's white backdrop goes down before the box sets its origin, so it takes the page one
// and lands a pillarbox right. Same rectangle every frame, so the panel identifies it.
static float aMsgboxPanel[4]{};
static int nMsgboxQuad = 0;
static bool bMsgboxSelfPlaced = false;

static void AddOrigin(SafetyHookContext& ctx)
{
    auto pCanvas = reinterpret_cast<uint8_t*>(ctx.ecx);
    if (!pCanvas)
        return;

    // The loading screen sets no origin, so it would inherit the last menu's pillarbox, in virtual
    // units, and land a screen over.
    if (fLoadingScale > 1.0f)
        return;

    auto pX = reinterpret_cast<float*>(ctx.esp + nOffsetArgX);
    auto pY = reinterpret_cast<float*>(ctx.esp + nOffsetArgY);

    // The first quad tells the callers apart: the pause menu's is a page wide surround bar, the
    // message box drops those and leads with the panel.
    auto pXL = reinterpret_cast<float*>(ctx.esp + nOffsetArgX + 8);
    auto pYL = reinterpret_cast<float*>(ctx.esp + nOffsetArgY + 8);

    if (bMsgboxPass)
    {
        if (nMsgboxQuad == 0)
        {
            const auto fClipX = *reinterpret_cast<float*>(pCanvas + nOffsetClipX);

            bMsgboxSelfPlaced = *pXL < fClipX * 0.9f;

            if (bMsgboxSelfPlaced)
            {
                aMsgboxPanel[0] = *pX;
                aMsgboxPanel[1] = *pY;
                aMsgboxPanel[2] = *pXL;
                aMsgboxPanel[3] = *pYL;
            }
        }

        nMsgboxQuad++;

        // menuscale already centres the box in screen coordinates, because the GUI natives drawing
        // its caption and buttons read WinLeft straight, so no offset.
        if (bMsgboxSelfPlaced)
            return;
    }
    else if (aMsgboxPanel[2] > 0.0f && *pX == aMsgboxPanel[0] && *pY == aMsgboxPanel[1]
        && *pXL == aMsgboxPanel[2] && *pYL == aMsgboxPanel[3])
    {
        return;
    }

    *pX += *reinterpret_cast<float*>(pCanvas + nOffsetOrgX);
    *pY += *reinterpret_cast<float*>(pCanvas + nOffsetOrgY);
}

// Not all seven are exported, so the vtable is found by the three that are and the rest read out.
static void** FindCanvasVtable(HMODULE hEngine, void* pDrawTile, void* pUpdate, void* pSetClip)
{
    auto pBase = reinterpret_cast<uint8_t*>(hEngine);
    auto pDos = reinterpret_cast<IMAGE_DOS_HEADER*>(pBase);
    auto pNt = reinterpret_cast<IMAGE_NT_HEADERS32*>(pBase + pDos->e_lfanew);

    const size_t nSize = pNt->OptionalHeader.SizeOfImage;

    for (size_t i = 0; i + 0xC0 < nSize; i += sizeof(void*))
    {
        auto pSlot = reinterpret_cast<void**>(pBase + i);

        if (pSlot[0x74 / sizeof(void*)] == pDrawTile &&
            pSlot[0x70 / sizeof(void*)] == pUpdate &&
            pSlot[0xB8 / sizeof(void*)] == pSetClip)
        {
            return pSlot;
        }
    }

    return nullptr;
}

static void SmoothText(uint8_t* pCanvas)
{
    auto pViewport = *reinterpret_cast<uint8_t**>(pCanvas + nOffsetViewport);
    if (!pViewport)
        return;

    auto pRenderInterface = *reinterpret_cast<void**>(pViewport + nOffsetRenderInterface);
    if (!pRenderInterface)
        return;

    auto pVtable = *reinterpret_cast<void***>(pRenderInterface);
    reinterpret_cast<void(__thiscall*)(void*, int)>(pVtable[nSmoothingSlot])(pRenderInterface, 1);
}

// X and Y are the pen start, before the origin is added.
static int __cdecl DrawString(uint8_t* pCanvas, void* pFont, int nX, int nY, const char* szText,
    uint32_t nColour, int bClip, int bParseAmpersand)
{
    if (!pCanvas || (!bGuiPass.load() && !bHudPass.load()))
        return shDrawString.ccall<int>(pCanvas, pFont, nX, nY, szText, nColour, bClip, bParseAmpersand);

    SmoothText(pCanvas);

    if (!bGuiPass.load())
        return shDrawString.ccall<int>(pCanvas, pFont, nX, nY, szText, nColour, bClip, bParseAmpersand);

    MongooseMenuNoteFont(pFont);

    const auto fOrgX = *reinterpret_cast<float*>(pCanvas + nOffsetOrgX);
    const auto fOrgY = *reinterpret_cast<float*>(pCanvas + nOffsetOrgY);

    const auto fBox = fAuthoredWidth * fGuiScale.load();
    const auto fPillarX = bGuiClamped.load() ? (fRealClipX.load() - fBox) * 0.5f : 0.0f;

    if (bBandDrawn && !MongooseMenuOwnsText() && fPillarX > 0.0f && nY + fOrgY < fBandHeight)
        nX -= static_cast<int>(fPillarX);

    fStringX = nX + fOrgX;
    fStringY = nY + fOrgY;
    bStringRun = true;

    const auto nAdvance = shDrawString.ccall<int>(pCanvas, pFont, nX, nY, szText, nColour, bClip, bParseAmpersand);

    bStringRun = false;

    return static_cast<int>(nAdvance * fGuiScale.load());
}

// Tail of the per character metrics primitive behind every script text measurement. The width is
// still reachable by its stack slot, the height by EBP.
static void CharSize(SafetyHookContext& ctx)
{
    if (!bGuiPass.load())
        return;

    const auto fScale = fGuiScale.load();
    auto pWidth = *reinterpret_cast<int32_t**>(ctx.esp + 0x14);
    auto pHeight = reinterpret_cast<int32_t*>(ctx.ebp);

    *pWidth = static_cast<int32_t>(*pWidth * fScale);
    *pHeight = static_cast<int32_t>(*pHeight * fScale);
}

// Every pause panel dimension in execDrawMsgboxBackground is a literal, so the frame stays two
// pixels wide at any size: 2.0 thickness per border quad, 1.0 inset to straddle the edge, 2.0
// lengthening to close the corners, 2.0/4.0 for the surround bars. The thickness is a push
// immediate, written in place; the rest load from float constants Engine.dll shares module wide, so
// each load operand is repointed at a float of ours.
static constexpr uint8_t aPushFloat2[] = { 0x68, 0x00, 0x00, 0x00, 0x40 };  // PUSH 2.0f
static constexpr uint8_t nFAdd32 = 0x05;    // FADD dword ptr [imm32], after D8
static constexpr uint8_t nFSub32 = 0x25;    // FSUB dword ptr [imm32], after D8
static constexpr auto nEscapeFloat = 0xD8;
static constexpr uint8_t aReturn8[] = { 0xC2, 0x08, 0x00 };  // RET 0x8, end of body
static constexpr auto nMsgboxBodyLength = 0x600;

struct ScaledFloat
{
    float fAuthored;
    float fValue;
};

// Reached absolutely by the repointed loads, so it has to outlive every draw.
static ScaledFloat aMsgboxFloats[] = { { 1.0f, 1.0f }, { 2.0f, 2.0f }, { 4.0f, 4.0f } };
static std::vector<float*> aMsgboxThickness;
static float fMsgboxScale = 0.0f;

// None of the thirteen quads' numbers reach script; this is the only place to see them.
static SafetyHookInline shMsgboxBackground{};

static void __fastcall MsgboxBackground(uint8_t* pCanvas, void*, void* pStack, void* pResult)
{
    if (!pCanvas)
    {
        shMsgboxBackground.thiscall<void>(pCanvas, pStack, pResult);
        return;
    }

    bMsgboxPass = true;
    nMsgboxQuad = 0;

    shMsgboxBackground.thiscall<void>(pCanvas, pStack, pResult);

    bMsgboxPass = false;
}

static void PrepareMsgboxBackground(HMODULE hEngine)
{
    auto pFunction = reinterpret_cast<uint8_t*>(
        GetProcAddress(hEngine, "?execDrawMsgboxBackground@UCanvas@@QAEXAAUFFrame@@QAX@Z"));

    if (!pFunction)
    {
        LogWarn("HudScale: Engine.dll did not export UCanvas::execDrawMsgboxBackground, the pause panel keeps a two pixel frame");
        return;
    }

    for (auto i = 0; i < nMsgboxBodyLength; i++)
    {
        auto p = pFunction + i;

        // Bounded by the function's own return, so nothing past it is ever repointed.
        if (std::memcmp(p, aReturn8, sizeof(aReturn8)) == 0)
            break;

        if (std::memcmp(p, aPushFloat2, sizeof(aPushFloat2)) == 0)
        {
            aMsgboxThickness.push_back(reinterpret_cast<float*>(p + 1));
            i += static_cast<int>(sizeof(aPushFloat2)) - 1;
            continue;
        }

        if (p[0] != nEscapeFloat || (p[1] != nFAdd32 && p[1] != nFSub32))
            continue;

        auto ppOperand = reinterpret_cast<float**>(p + 2);
        auto pConstant = *ppOperand;

        // Anything outside the module is already one of ours, from an earlier pass.
        if (IsBadReadPtr(pConstant, sizeof(float)))
            continue;

        for (auto& scaled : aMsgboxFloats)
        {
            if (*pConstant != scaled.fAuthored)
                continue;

            injector::WriteMemory<float*>(ppOperand, &scaled.fValue, true);
            break;
        }

        i += 5;
    }

    shMsgboxBackground = safetyhook::create_inline(pFunction, MsgboxBackground);

    if (!shMsgboxBackground)
        LogWarn("HudScale: UCanvas::execDrawMsgboxBackground could not be hooked, the pause panel bars stay offset");
}

static void ScaleMsgboxBackground(float fScale)
{
    if (fScale == fMsgboxScale || fScale <= 0.0f)
        return;

    fMsgboxScale = fScale;

    for (auto& scaled : aMsgboxFloats)
        scaled.fValue = scaled.fAuthored * fScale;

    for (auto pThickness : aMsgboxThickness)
        injector::WriteMemory<float>(pThickness, 2.0f * fScale, true);
}

static float InterfaceScale(uint8_t* pCanvas)
{
    const auto fClipX = *reinterpret_cast<float*>(pCanvas + nOffsetClipX);
    const auto fClipY = *reinterpret_cast<float*>(pCanvas + nOffsetClipY);

    return (std::min)(fClipX / fAuthoredWidth, fClipY / fAuthoredHeight);
}

// The objectives band and the pause panel's black surround bars are no part of the 4:3 page, and
// stopping them at the box leaves a strip of game showing at each end. The canvas cannot name them,
// every control setting its origin to its own corner and clipping to its own size, but the screen
// can: the box sits (screen - box)/2 from each edge, so a tile is page wide if it reaches that and
// is most of a page across, which no background is.
static constexpr auto fSpanningFraction = 0.5f;
static constexpr auto fEdgeSlack = 0.5f;

struct FPlane
{
    float X;
    float Y;
    float Z;
    float W;
};

static SafetyHookInline shCanvasDrawTile{};

static void __fastcall CanvasDrawTile(uint8_t* pCanvas, void*, void* pMaterial,
    float fX, float fY, float fXL, float fYL, float fU, float fV, float fUL, float fVL, float fZ,
    FPlane Colour, FPlane Fog)
{
    // The settings page owns the pointer while it is up and draws the cursor itself.
    if (MongooseMenuSuppressTile(pMaterial))
        return;

    const auto Draw = [&](float fLeft, float fWidth)
    {
        // The interface's cursor goes past here; the settings page redraws the same quad over itself
        // rather than drawing a cursor of its own.
        MongooseMenuNoteTile(pMaterial, fLeft, fY, fWidth, fYL, fU, fV, fUL, fVL, fZ,
            &Colour.X, &Fog.X);

        shCanvasDrawTile.thiscall<void>(pCanvas, pMaterial, fLeft, fY, fWidth, fYL,
            fU, fV, fUL, fVL, fZ, Colour, Fog);
    };

    const auto fScreenX = fRealClipX.load();
    const auto fBox = fAuthoredWidth * fGuiScale.load();

    if (!bGuiClamped.load() || bHudPass.load() || fBox <= 0.0f || fScreenX <= fBox
        || fXL < fBox * fSpanningFraction)
    {
        Draw(fX, fXL);
        return;
    }

    const auto fBoxLeft = (fScreenX - fBox) * 0.5f;
    const auto fBoxRight = fBoxLeft + fBox;

    const auto bTouchesLeft = fX <= fBoxLeft + fEdgeSlack;
    const auto bTouchesRight = fX + fXL >= fBoxRight - fEdgeSlack;

    if (!bTouchesLeft && !bTouchesRight)
    {
        Draw(fX, fXL);
        return;
    }

    const auto fLeft = bTouchesLeft ? 0.0f : fX;
    const auto fRight = bTouchesRight ? fScreenX : fX + fXL;

    // The full screen backdrop draws the same way, so the band is known by its height, 118 units of
    // the page.
    const auto fBand = fAuthoredBandHeight * fGuiScale.load();

    if (bTouchesLeft && bTouchesRight && fY <= 0.5f && std::abs(fYL - fBand) <= 2.0f)
    {
        bBandDrawn = true;
        fBandHeight = fYL;
    }

    Draw(fLeft, fRight - fLeft);
}

// DrawTileStretched takes the nine slice border as half the material's USize and VSize, 16 pixels
// at any resolution for a 32x32 style texture, and corner size, middle span and texture coordinates
// all derive from it, so there is no number to patch (script BorderOffsets is never read natively).
// The slices are emitted here instead, border scaled, source rectangles left alone.
static SafetyHookInline shDrawTileStretched{};

using MaterialSize_t = int(__thiscall*)(void*);

static void __fastcall CanvasDrawTileStretched(uint8_t* pCanvas, void*, void* pMaterial,
    float fX, float fY, float fXL, float fYL)
{
    const auto fScale = bGuiClamped.load() ? fGuiScale.load() : 1.0f;

    if (!pCanvas || !pMaterial || fScale <= 1.0f || fXL <= 0.0f || fYL <= 0.0f)
    {
        shDrawTileStretched.thiscall<void>(pCanvas, pMaterial, fX, fY, fXL, fYL);
        return;
    }

    auto pMaterialVtable = *reinterpret_cast<void***>(pMaterial);
    const auto nUSize = reinterpret_cast<MaterialSize_t>(pMaterialVtable[0x70 / sizeof(void*)])(pMaterial);
    const auto nVSize = reinterpret_cast<MaterialSize_t>(pMaterialVtable[0x74 / sizeof(void*)])(pMaterial);

    if (nUSize <= 1 || nVSize <= 1)
    {
        shDrawTileStretched.thiscall<void>(pCanvas, pMaterial, fX, fY, fXL, fYL);
        return;
    }

    // Origin blind too, and no longer in the nudged list, so it adds the origin here.
    fX += *reinterpret_cast<float*>(pCanvas + nOffsetOrgX);
    fY += *reinterpret_cast<float*>(pCanvas + nOffsetOrgY);

    const auto fSourceX = nUSize * 0.5f;
    const auto fSourceY = nVSize * 0.5f;

    // Never more than half the control, or the two edges cross and the middle inverts.
    const auto fBorderX = (std::min)(fSourceX * fScale, fXL * 0.5f);
    const auto fBorderY = (std::min)(fSourceY * fScale, fYL * 0.5f);

    const auto fMiddleX = fXL - 2.0f * fBorderX;
    const auto fMiddleY = fYL - 2.0f * fBorderY;

    const auto nColour = *reinterpret_cast<uint32_t*>(pCanvas + nOffsetDrawColour);
    const FPlane Colour{
        ((nColour >> 16) & 0xFF) / 255.0f,
        ((nColour >> 8) & 0xFF) / 255.0f,
        (nColour & 0xFF) / 255.0f,
        ((nColour >> 24) & 0xFF) / 255.0f,
    };

    const FPlane Fog{ 0.0f, 0.0f, 0.0f, 0.0f };
    const auto fZ = *reinterpret_cast<float*>(pCanvas + nOffsetZ);

    auto pVtable = *reinterpret_cast<void***>(pCanvas);
    auto pDrawTile = reinterpret_cast<void(__thiscall*)(void*, void*, float, float, float, float,
        float, float, float, float, float, FPlane, FPlane)>(pVtable[0x74 / sizeof(void*)]);

    const auto Slice = [&](float fLeft, float fTop, float fWidth, float fHeight,
        float fU, float fV, float fUL, float fVL)
    {
        if (fWidth <= 0.0f || fHeight <= 0.0f)
            return;

        pDrawTile(pCanvas, pMaterial, fLeft, fTop, fWidth, fHeight, fU, fV, fUL, fVL, fZ, Colour, Fog);
    };

    const auto fRight = fX + fXL - fBorderX;
    const auto fBottom = fY + fYL - fBorderY;

    // Corners at their own source, edges from the texel just inside it, middle from the centre.
    Slice(fX,      fY,      fBorderX, fBorderY, 0.0f,     0.0f,     fSourceX, fSourceY);
    Slice(fRight,  fY,      fBorderX, fBorderY, fSourceX, 0.0f,     fSourceX, fSourceY);
    Slice(fX,      fBottom, fBorderX, fBorderY, 0.0f,     fSourceY, fSourceX, fSourceY);
    Slice(fRight,  fBottom, fBorderX, fBorderY, fSourceX, fSourceY, fSourceX, fSourceY);

    Slice(fX + fBorderX, fY,      fMiddleX, fBorderY, fSourceX, 0.0f,     1.0f,     fSourceY);
    Slice(fX + fBorderX, fBottom, fMiddleX, fBorderY, fSourceX, fSourceY, 1.0f,     fSourceY);
    Slice(fX,      fY + fBorderY, fBorderX, fMiddleY, 0.0f,     fSourceY, fSourceX, 1.0f);
    Slice(fRight,  fY + fBorderY, fBorderX, fMiddleY, fSourceX, fSourceY, fSourceX, 1.0f);

    Slice(fX + fBorderX, fY + fBorderY, fMiddleX, fMiddleY, fSourceX, fSourceY, 1.0f, 1.0f);
}

struct CanvasBox
{
    float fClipX;
    float fClipY;
    int32_t nSizeX;
    int32_t nSizeY;
};

static CanvasBox SaveBox(uint8_t* pCanvas)
{
    return {
        *reinterpret_cast<float*>(pCanvas + nOffsetClipX),
        *reinterpret_cast<float*>(pCanvas + nOffsetClipY),
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX),
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY),
    };
}

static void WriteBox(uint8_t* pCanvas, const CanvasBox& box)
{
    *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = box.fClipX;
    *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = box.fClipY;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = box.nSizeX;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = box.nSizeY;
}

static CanvasBox ClampToMenuBox(uint8_t* pCanvas, float fScale)
{
    const auto saved = SaveBox(pCanvas);

    const auto fBoxX = fAuthoredWidth * fScale;
    const auto fBoxY = fAuthoredHeight * fScale;

    *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = fBoxX;
    *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = fBoxY;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = static_cast<int32_t>(fBoxX);
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = static_cast<int32_t>(fBoxY);

    fRealClipX = saved.fClipX;
    fRealClipY = saved.fClipY;
    bGuiClamped = true;

    return saved;
}

static void RestoreBox(uint8_t* pCanvas, const CanvasBox& saved)
{
    bGuiClamped = false;
    WriteBox(pCanvas, saved);
}

// Rendering a portal leaves the canvas set up for the camera's view, and the scene runs its own HUD
// pass that ends by clearing the flag, so the rest of the outer HUD would go out unscaled.
static SafetyHookInline shDrawPortalExec{};

static void __fastcall DrawPortalExec(uint8_t* pCanvas, void*, void* pStack, void* pResult)
{
    if (!pCanvas || !bHudPass.load())
    {
        shDrawPortalExec.thiscall<void>(pCanvas, pStack, pResult);
        return;
    }

    const auto saved = SaveBox(pCanvas);

    shDrawPortalExec.thiscall<void>(pCanvas, pStack, pResult);

    WriteBox(pCanvas, saved);
    bHudPass = true;
}

// The loading thread draws to the viewport canvas against 640x480 like the HUD: 50 pixel sprites,
// caption 240 in from the right edge and 65 up from the bottom, font as authored. It runs mid load,
// so nothing shared is written, canvas and viewport included. Quads are multiplied on the way out;
// the two screen pixel inputs, spawn bounds and caption corner, are divided where they are read.
static constexpr auto nOffsetEngineClient = 0x4C;
static constexpr auto nOffsetClientViewports = 0x2C;
static constexpr auto nOffsetViewportCanvas = 0x68;
static constexpr auto nOffsetCurX = 0x44;
static constexpr auto nOffsetCurY = 0x48;

static SafetyHookInline shRenderAnimation{};
static SafetyHookMid mhSpawnBoundX{};
static SafetyHookMid mhSpawnBoundY{};
static SafetyHookMid mhCaptionPosition{};

// At the CDQ the viewport size is still in ECX, its margin not yet taken off.
static void SpawnBound(SafetyHookContext& ctx)
{
    if (fLoadingScale > 1.0f)
        ctx.ecx = static_cast<uintptr_t>(ctx.ecx / fLoadingScale);
}

// The pen is stored by now, ClipX - 240 and ClipY - 65 in screen pixels. The margins grow with the
// quads, so they are kept and the screen is divided.
static void CaptionPosition(SafetyHookContext& ctx)
{
    if (fLoadingScale <= 1.0f)
        return;

    auto pCanvas = reinterpret_cast<uint8_t*>(ctx.ebp);

    auto pCurX = reinterpret_cast<float*>(pCanvas + nOffsetCurX);
    auto pCurY = reinterpret_cast<float*>(pCanvas + nOffsetCurY);

    const auto fClipX = *reinterpret_cast<float*>(pCanvas + nOffsetClipX);
    const auto fClipY = *reinterpret_cast<float*>(pCanvas + nOffsetClipY);

    *pCurX = fClipX / fLoadingScale - (fClipX - *pCurX);
    *pCurY = fClipY / fLoadingScale - (fClipY - *pCurY);
}

static void __fastcall RenderAnimation(uint8_t* pThis, void*, uint8_t* pEngine)
{
    auto pClient = pEngine ? *reinterpret_cast<uint8_t**>(pEngine + nOffsetEngineClient) : nullptr;
    auto ppViewports = pClient ? *reinterpret_cast<uint8_t***>(pClient + nOffsetClientViewports) : nullptr;
    const auto nViewports = pClient
        ? *reinterpret_cast<int32_t*>(pClient + nOffsetClientViewports + sizeof(void*)) : 0;

    auto pViewport = (ppViewports && nViewports > 0) ? ppViewports[0] : nullptr;
    auto pCanvas = pViewport ? *reinterpret_cast<uint8_t**>(pViewport + nOffsetViewportCanvas) : nullptr;

    const auto fClipY = pCanvas ? *reinterpret_cast<float*>(pCanvas + nOffsetClipY) : 0.0f;

    if (fClipY <= fAuthoredHeight)
    {
        shRenderAnimation.thiscall<void>(pThis, pEngine);
        return;
    }

    fLoadingScale = fClipY / fAuthoredHeight;

    shRenderAnimation.thiscall<void>(pThis, pEngine);

    fLoadingScale = 1.0f;
}

static void __fastcall GuiPreRender(uint8_t* pMaster, void*, uint8_t* pCanvas)
{
    if (!pCanvas)
    {
        shGuiPreRender.thiscall<void>(pMaster, pCanvas);
        return;
    }

    // Catches menus built since the last sweep; throttled inside.
    RefreshInterfaceObjects();

    const auto fScale = InterfaceScale(pCanvas);
    fGuiScale = fScale;
    ScaleMsgboxBackground(fScale);

    const auto saved = ClampToMenuBox(pCanvas, fScale);

    bGuiPass = true;
    shGuiPreRender.thiscall<void>(pMaster, pCanvas);
    bGuiPass = false;

    RestoreBox(pCanvas, saved);
}

static void __fastcall GuiPostRender(uint8_t* pMaster, void*, uint8_t* pCanvas)
{
    if (!pCanvas)
    {
        shGuiPostRender.thiscall<void>(pMaster, pCanvas);
        return;
    }

    const auto fScale = InterfaceScale(pCanvas);
    fGuiScale = fScale;

    const auto saved = ClampToMenuBox(pCanvas, fScale);

    bBandDrawn = false;

    bGuiPass = true;
    shGuiPostRender.thiscall<void>(pMaster, pCanvas);

    // Inside the pass and the menu box, so the fix's own page is treated like a real one.
    MongooseMenuDraw(pCanvas);

    bGuiPass = false;

    RestoreBox(pCanvas, saved);
}

static void __fastcall HudPostRender(uint8_t* pHud, void*, uint8_t* pCanvas)
{
    // The HUD lays out against the whole screen, so inside the interface pass the real values go
    // back for its length.
    const auto bRestoreBox = pCanvas && bGuiClamped.load();

    if (bRestoreBox)
    {
        *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = fRealClipX.load();
        *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = fRealClipY.load();
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = static_cast<int32_t>(fRealClipX.load());
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = static_cast<int32_t>(fRealClipY.load());
    }

    const auto ReClamp = [&]()
    {
        if (!bRestoreBox)
            return;

        const auto fBox = fGuiScale.load();
        *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = fAuthoredWidth * fBox;
        *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = fAuthoredHeight * fBox;
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = static_cast<int32_t>(fAuthoredWidth * fBox);
        *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = static_cast<int32_t>(fAuthoredHeight * fBox);
    };

    const auto fClipY = pCanvas ? *reinterpret_cast<float*>(pCanvas + nOffsetClipY) : 0.0f;

    // Nothing to gain below the height the art was drawn at.
    if (fClipY <= fAuthoredHeight)
    {
        shHudPostRender.thiscall<void>(pHud, pCanvas);
        ReClamp();
        return;
    }

    const auto fScale = fClipY / fAuthoredHeight;

    const auto fClipX = *reinterpret_cast<float*>(pCanvas + nOffsetClipX);
    const auto nSizeX = *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX);
    const auto nSizeY = *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY);

    *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = fClipX / fScale;
    *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = fAuthoredHeight;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = static_cast<int32_t>(nSizeX / fScale);
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = static_cast<int32_t>(fAuthoredHeight);

    // The HUD sets no origin, so it inherits the interface's and execDrawTile adds it before
    // anything here sees the quad: the loading screen picture is a page wide tile in this pass, and
    // the menu's centring origin would put it a page right.
    auto pOrgX = reinterpret_cast<float*>(pCanvas + nOffsetOrgX);
    auto pOrgY = reinterpret_cast<float*>(pCanvas + nOffsetOrgY);
    const auto fOrgX = *pOrgX;
    const auto fOrgY = *pOrgY;

    *pOrgX = 0.0f;
    *pOrgY = 0.0f;

    fHudScale = fScale;
    bHudPass = true;

    shHudPostRender.thiscall<void>(pHud, pCanvas);

    bHudPass = false;

    *pOrgX = fOrgX;
    *pOrgY = fOrgY;

    *reinterpret_cast<float*>(pCanvas + nOffsetClipX) = fClipX;
    *reinterpret_cast<float*>(pCanvas + nOffsetClipY) = fClipY;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeX) = nSizeX;
    *reinterpret_cast<int32_t*>(pCanvas + nOffsetSizeY) = nSizeY;

    ReClamp();
}

// UGUIController::NativePostRender clamps the cursor to the canvas size, the menu box during the
// interface pass, so it stops at the pillarbox. Hooked past where the cursor's own scale is taken,
// which wants the box: clamped X in [EBP-0x14], clamped Y in EDI, raw pair at ESI+0x90, both redone
// against the screen.
static SafetyHookMid mhCursorClamp{};

static void CursorClamp(SafetyHookContext& ctx)
{
    const auto nWidth = static_cast<int32_t>(fRealClipX.load());
    const auto nHeight = static_cast<int32_t>(fRealClipY.load());

    // Zero until the first interface pass, the only caller.
    if (nWidth <= 0 || nHeight <= 0)
        return;

    auto pMouse = reinterpret_cast<int32_t*>(ctx.esi + 0x90);

    *reinterpret_cast<int32_t*>(ctx.ebp - 0x14) = std::clamp(pMouse[0], 0, nWidth);
    ctx.edi = std::clamp(pMouse[1], 0, nHeight);
}

static void InitGUI()
{
    // MOV ECX,[EBP+8] / MOV EDX,[ECX] / MOV [EBP-0x1c],EDI / CALL [EDX+0x70]: both clamps done,
    // cursor material about to be measured.
    auto patternCursorClamp = module_pattern(L"GUI.dll", "8B 4D 08 8B 11 89 7D E4 FF 52 70");

    if (!patternCursorClamp.empty())
        mhCursorClamp = safetyhook::create_mid(patternCursorClamp.get_first(), CursorClamp);

    if (!mhCursorClamp)
        LogWarn("HudScale: the menu cursor clamp was not found, the cursor stops at the pillarbox");
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto pPostRender = GetProcAddress(hEngine, "?eventPostRender@AHUD@@QAEXPAVUCanvas@@@Z");
    auto pDrawTile = GetProcAddress(hEngine, "?DrawTile@FCanvasUtil@@QAEXMMMMMMMMMPAVUMaterial@@VFColor@@@Z");

    if (!pPostRender || !pDrawTile)
    {
        LogWarn("HudScale: Engine.dll did not export AHUD::eventPostRender or FCanvasUtil::DrawTile, the HUD stays at its texture size");
        return;
    }

    shHudPostRender = safetyhook::create_inline(pPostRender, HudPostRender);
    mhDrawTile = safetyhook::create_mid(pDrawTile, DrawTile);

    if (!shHudPostRender || !mhDrawTile)
    {
        LogWarn("HudScale: the HUD pass could not be hooked, the HUD stays at its texture size");
        return;
    }

    if (auto pDrawTriangle = GetProcAddress(hEngine,
        "?DrawTriangle@FCanvasUtil@@QAEXMMMMMMMMMMMPAVUMaterial@@VFColor@@@Z"))
        mhDrawTriangle = safetyhook::create_mid(pDrawTriangle, DrawTriangle);

    if (!mhDrawTriangle)
        LogWarn("HudScale: FCanvasUtil::DrawTriangle could not be hooked, the comic border stays 640x480");

    auto pGuiPreRender = GetProcAddress(hEngine, "?MasterProcessPreRender@UInteractionMaster@@QAEXPAVUCanvas@@@Z");
    auto pGuiPostRender = GetProcAddress(hEngine, "?MasterProcessPostRender@UInteractionMaster@@QAEXPAVUCanvas@@@Z");

    // SUB ESP,0x48 / PUSH EBX / PUSH EBP / MOV EBP,[ESP+0x58] / PUSH ESI / LEA EBX,[EBP+0x2C]: the
    // function that turns a string into quads. Not exported, unlike everything around it.
    auto patternDrawString = module_pattern(L"Engine.dll", "83 EC 48 53 55 8B 6C 24 58 56 8D 5D 2C 57 8B CB");

    // MOV ECX,[ESI+0xC] / MOV [EBP],ECX / POP EDI / POP ESI / POP EBP / POP EBX / RET: hooked at
    // the POPs, where both out pointers are still reachable. The height argument slot is scratch by
    // then, so EBP is the only copy left.
    auto patternCharSize = module_pattern(L"Engine.dll", "8B 4E 0C 89 4D 00 5F 5E 5D 5B C3");

    if (!pGuiPreRender || !pGuiPostRender || patternDrawString.empty() || patternCharSize.empty())
    {
        LogWarn("HudScale: the interface text path was not found, menu text stays at its font size");
        return;
    }

    auto pCanvasDrawTile = GetProcAddress(hEngine, "?DrawTile@UCanvas@@UAEXPAVUMaterial@@MMMMMMMMMVFPlane@@1@Z");
    auto pCanvasUpdate = GetProcAddress(hEngine, "?Update@UCanvas@@UAEXXZ");
    auto pCanvasSetClip = GetProcAddress(hEngine, "?SetClip@UCanvas@@UAEXHHHH@Z");

    if (auto pVtable = (pCanvasDrawTile && pCanvasUpdate && pCanvasSetClip)
        ? FindCanvasVtable(hEngine, pCanvasDrawTile, pCanvasUpdate, pCanvasSetClip) : nullptr)
    {
        for (size_t i = 0; i < std::size(aOriginBlindSlots); i++)
            amhOriginBlind[i] = safetyhook::create_mid(pVtable[aOriginBlindSlots[i] / sizeof(void*)], AddOrigin);
    }
    else
    {
        LogWarn("HudScale: the canvas vtable was not found, menu panels ignore the centring origin");
    }

    if (auto pWorldToScreen = GetProcAddress(hEngine, "?execWorldToScreen@UInteraction@@QAEXAAUFFrame@@QAX@Z"))
        shWorldToScreen = safetyhook::create_inline(pWorldToScreen, WorldToScreen);

    if (auto pSetOrigin = GetProcAddress(hEngine, "?execSetOrigin@UCanvas@@QAEXAAUFFrame@@QAX@Z"))
        shSetOrigin = safetyhook::create_inline(pSetOrigin, SetOrigin);

    PrepareMsgboxBackground(hEngine);

    if (pCanvasDrawTile)
        shCanvasDrawTile = safetyhook::create_inline(pCanvasDrawTile, CanvasDrawTile);

    if (auto pStretched = GetProcAddress(hEngine, "?DrawTileStretched@UCanvas@@UAEXPAVUMaterial@@MMMM@Z"))
        shDrawTileStretched = safetyhook::create_inline(pStretched, CanvasDrawTileStretched);

    if (!shDrawTileStretched)
        LogWarn("HudScale: UCanvas::DrawTileStretched could not be hooked, control frames stay 16 pixels wide");

    if (!shCanvasDrawTile)
        LogWarn("HudScale: UCanvas::DrawTile could not be hooked, the menu's full width bars stop at the pillarbox");

    if (auto pRenderAnimation = GetProcAddress(hEngine,
        "?RenderAnimation@UEngLoadingAnimationThread@@UAEXPAVUEngine@@@Z"))
        shRenderAnimation = safetyhook::create_inline(pRenderAnimation, RenderAnimation);

    // MOV ECX,[EBX+0x88] / CDQ / SUB ECX,300, and SizeY with 200: where the sprites grow from.
    auto patternSpawnX = module_pattern(L"Engine.dll", "8B 8B 88 00 00 00 99 81 E9 2C 01 00 00 F7 F9");
    auto patternSpawnY = module_pattern(L"Engine.dll", "8B 8B 8C 00 00 00 99 81 E9 C8 00 00 00 F7 F9");

    // FLD ClipX / FSUB 240 / FSTP CurX, and the same for ClipY and 65: the caption corner.
    auto patternCaption = module_pattern(L"Engine.dll",
        "D9 45 3C 8D 54 24 20 D8 25 ? ? ? ? 52 6A 00 D9 5D 44 D9 45 40 D8 25 ? ? ? ? D9 5D 48 C7 45 58");

    if (!patternSpawnX.empty() && !patternSpawnY.empty() && !patternCaption.empty())
    {
        mhSpawnBoundX = safetyhook::create_mid(patternSpawnX.get_first(6), SpawnBound);
        mhSpawnBoundY = safetyhook::create_mid(patternSpawnY.get_first(6), SpawnBound);
        mhCaptionPosition = safetyhook::create_mid(patternCaption.get_first(31), CaptionPosition);
    }

    // Scaled quads with an unscaled spawn point or corner land off screen. All or none.
    if (!mhSpawnBoundX || !mhSpawnBoundY || !mhCaptionPosition)
        shRenderAnimation = {};

    if (!shRenderAnimation)
        LogWarn("HudScale: the loading animation was not found, the loading screen stays 640x480");

    // MOV ECX,[EBP+8] / LEA EAX,[EBP-0x2c] / MOV [EBP-0x2c],0x5a: the default FOV going onto the
    // script stack, 0x31 short of the point where all nine parameters are read.
    auto patternDrawPortal = module_pattern(L"Engine.dll", "8B 4D 08 8D 45 D4 C7 45 D4 5A 00 00 00");

    if (!patternDrawPortal.empty())
        mhDrawPortal = safetyhook::create_mid(patternDrawPortal.get_first(0x31), DrawPortal);

    if (!mhDrawPortal)
        LogWarn("HudScale: UCanvas::execDrawPortal was not found, the camera view stays 320x240");

    if (auto pDrawPortal = GetProcAddress(hEngine, "?execDrawPortal@UCanvas@@QAEXAAUFFrame@@QAX@Z"))
        shDrawPortalExec = safetyhook::create_inline(pDrawPortal, DrawPortalExec);

    if (!shDrawPortalExec)
        LogWarn("HudScale: UCanvas::execDrawPortal could not be hooked, the HUD drops out of scale behind a camera view");

    if (auto pDrawLine = GetProcAddress(hEngine, "?DrawLine@FCanvasUtil@@QAEXMMMMVFColor@@W4ERenderStyle@@@Z"))
        shDrawLine = safetyhook::create_inline(pDrawLine, DrawLine);
    else
        LogWarn("HudScale: Engine.dll did not export FCanvasUtil::DrawLine, box borders stay one pixel wide");

    shGuiPreRender = safetyhook::create_inline(pGuiPreRender, GuiPreRender);
    shGuiPostRender = safetyhook::create_inline(pGuiPostRender, GuiPostRender);
    shDrawString = safetyhook::create_inline(patternDrawString.get_first(), DrawString);
    mhCharSize = safetyhook::create_mid(patternCharSize.get_first(6), CharSize);

    if (!shGuiPreRender || !shGuiPostRender || !shDrawString || !mhCharSize)
        LogWarn("HudScale: the interface text path could not be hooked, menu text stays at its font size");
}

class HudScale
{
public:
    HudScale()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
        MongooseFix::onGUIInitEvent() += []() { InitGUI(); };
    }
} HudScale;
