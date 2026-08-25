module;

#include <common.hxx>

export module comicpanels;

import common;
import display;
import logging;

// A real time panel is not drawn from the world - the world renders into URenderTargetMaterial and
// the panel is a tile of it. That material is one 256x256 atlas shared by every panel, handed out
// in 8 unit cells over a 32 bit mask, so a panel is a couple hundred texels however large it lands
// on screen. Hence the blocks.
//
// The atlas texture and the size the material reports are separate: FCanvasUtil::DrawTile divides
// texture coordinates by GetUSize/GetVSize, so leaving those at 256 keeps every number script has -
// AllocRect cells, tile coordinates - and only Update's rect, which addresses real texels, grows
// with the texture.
//
//   URenderTargetMaterial  +0x54 USize  +0x58 VSize  +0x64 FBaseTexture*  +0x68 32 dword cell mask
//   FBaseTexture           +0x08 revision  +0x0C width  +0x10 height
static constexpr auto nAuthoredAtlas = 256;
static constexpr auto nMaxAtlas = 4096;
static constexpr auto nOffsetTexture = 0x64;
static constexpr auto nIndexRevision = 2;
static constexpr auto nIndexWidth = 3;
static constexpr auto nIndexHeight = 4;

static SafetyHookMid mhRenderTargetUpdate{};

// Powers of two only: the atlas is a render target, the driver may refuse an odd size.
static int AtlasSize()
{
    auto nSize = nAuthoredAtlas;

    while (nSize < nMaxAtlas && nSize * 2 <= nBackBufferHeight.load())
        nSize *= 2;

    return nSize;
}

static void RenderTargetUpdate(SafetyHookContext& ctx)
{
    auto pMaterial = reinterpret_cast<uint8_t*>(ctx.ecx);
    if (!pMaterial)
        return;

    auto pTexture = *reinterpret_cast<int32_t**>(pMaterial + nOffsetTexture);
    if (!pTexture)
        return;

    const auto nSize = AtlasSize();

    // The revision is what makes the renderer throw the old texture away.
    if (pTexture[nIndexWidth] != nSize)
    {
        pTexture[nIndexWidth] = nSize;
        pTexture[nIndexHeight] = nSize;
        pTexture[nIndexRevision]++;

        LogInfo("ComicPanels: panel atlas {}x{}", nSize, nSize);
    }

    const auto nScale = nSize / nAuthoredAtlas;
    if (nScale <= 1)
        return;

    // X, Y, width, height: the first four stack arguments.
    for (auto nOffset = 0x04; nOffset <= 0x10; nOffset += 4)
    {
        auto pValue = reinterpret_cast<int32_t*>(ctx.esp + nOffset);
        *pValue *= nScale;
    }
}

static void InitEngine()
{
    auto hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine)
        return;

    auto pUpdate = GetProcAddress(hEngine,
        "?Update@URenderTargetMaterial@@UAEXHHHHABVFVector@@ABVFRotator@@MVFColor@@MPAVUMaterial@@@Z");

    if (!pUpdate)
    {
        LogWarn("ComicPanels: Engine.dll did not export URenderTargetMaterial::Update, real time panels stay a 256x256 atlas");
        return;
    }

    mhRenderTargetUpdate = safetyhook::create_mid(pUpdate, RenderTargetUpdate);

    if (!mhRenderTargetUpdate)
        LogWarn("ComicPanels: URenderTargetMaterial::Update could not be hooked, real time panels stay a 256x256 atlas");
}

class ComicPanels
{
public:
    ComicPanels()
    {
        MongooseFix::onEngineInitEvent() += []() { InitEngine(); };
    }
} ComicPanels;
