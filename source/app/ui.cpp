#include "ui.hpp"

#include "crash.hpp"
#include "image.hpp"
#include "logging.hpp"
#include "platform.hpp"
#include "sound.hpp"
#include "text.hpp"
#include "units.hpp"

#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ui {
namespace {

constexpr float Pi = 3.14159265f;
constexpr float Depth = 0.5f;
// Every shape is many small triangles, so citro2d gets room for plenty.
constexpr size_t MaxObjects = 8192;

C3D_RenderTarget *topTarget = nullptr;
C3D_RenderTarget *bottomTarget = nullptr;
C2D_TextBuf textBuffer = nullptr;
bool closing = false;
bool onBottom = false;
uint64_t startTime = 0;
bool ptmu = false;

// citro2d draws the system font at 30 / cellHeight; matching that keeps the
// measurements below identical to what ends up on screen.
float fontScale = 1.0f;
float fontLineFeed = 30.0f;
float asciiAdvance[128];
bool asciiReady = false;

constexpr const char *Ellipsis = "...";

// The type scales in ui.hpp were set before measuring: at them the system
// font came out about 1.4 times smaller than the cap heights measured from
// the system applets (a tab label 9 px tall instead of 14). Every size is
// scaled by this one factor, so measuring and drawing always agree.
constexpr float TextBoost = 1.4f;

// Text up to this much too wide is drawn narrower instead of scrolling a
// few pixels or losing its end. The console's font runs a little wider
// than Azahar's, so text that fits in the emulator can miss on hardware.
constexpr float MaxSqueeze = 0.88f;

// --- Textures ------------------------------------------------------------------

// The icon atlas: white with alpha, tinted when drawn.
C3D_Tex atlasTexture;
bool atlasReady = false;
bool atlasTried = false; // romfs is mounted after ui::init, so load lazily
Tex3DS_SubTexture atlasSub[64];
size_t atlasSubNext = 0;

// The one title icon on screen at a time. It is static so the texture
// pointer citro2d remembers between draws never dangles.
constexpr u16 IconTextureSize = 64;
C3D_Tex iconTexture;
bool iconTextureReady = false;
std::vector<uint16_t> iconPixels;
const Tex3DS_SubTexture IconSubTexture = {
    48, 48, 0.0f, 1.0f, 48.0f / IconTextureSize, 1.0f - 48.0f / IconTextureSize};
const C2D_Image IconImage = {&iconTexture, &IconSubTexture};

// The picture viewer's texture, big enough for a whole top screen.
constexpr u16 PictureTextureWidth = 512;
constexpr u16 PictureTextureHeight = 256;
C3D_Tex pictureTexture;
bool pictureTextureReady = false;
Tex3DS_SubTexture pictureSubTexture;
C2D_Image pictureImage = {&pictureTexture, &pictureSubTexture};

// What both screens showed when a dialog opened, dimmed behind it.
C3D_Tex backdropTextures[2];
bool backdropReady[2] = {false, false};
const Tex3DS_SubTexture BackdropSubs[2] = {
    {400, 240, 0.0f, 1.0f, 400.0f / 512, 1.0f - 240.0f / 256},
    {320, 240, 0.0f, 1.0f, 320.0f / 512, 1.0f - 240.0f / 256}};
// Dialog frames are not captured again, so dialogs never dim themselves.
bool inDialog = false;
bool lastFrameWasDialog = false;

// The notification on the top screen, if any, and when it appeared.
Notice notice;
bool noticeActive = false;
uint64_t noticeStart = 0;
std::function<void()> frameHook;
void drawNotice();

// A new screen fades in over the last frame of the one before, so what both
// show stays put. Touch is ignored until the screen has been open ScreenIn
// ms and the stylus has lifted, so a touch held from the last screen does
// not press the control under it.
constexpr uint64_t ScreenIn = 150;
constexpr uint64_t FadeIn = 200;
uint64_t touchLockStart = 0;
bool touchLocked = false;
uint64_t fadeStart = 0;
bool fading = false;
std::string lastScreen;
// The screen just entered, logged by name once its title is drawn.
std::string screenToLog;
// That last frame, kept as RGB8 so gradients do not band as it fades. The
// first screen has none and fades in from the plain background.
C3D_Tex fromTextures[2];
bool fromReady[2] = {false, false};
bool fromCapture = false;
bool frameShown = false;

// --- Font ----------------------------------------------------------------------

void loadFontMetrics()
{
    CFNT_s *font = fontGetSystemFont();
    fontScale = 30.0f / fontGetGlyphInfo(font)->cellHeight;
    fontLineFeed = fontGetInfo(font)->lineFeed;
}

float advance(uint32_t codePoint)
{
    if (codePoint < 128 && asciiReady) {
        return asciiAdvance[codePoint];
    }
    fontGlyphPos_s glyph;
    C2D_FontCalcGlyphPos(nullptr, &glyph,
                         C2D_FontGlyphIndexFromCodePoint(nullptr, codePoint),
                         0, 1.0f, 1.0f);
    return glyph.xAdvance * fontScale;
}

fontGlyphPos_s glyphPos(uint32_t codePoint)
{
    fontGlyphPos_s glyph;
    C2D_FontCalcGlyphPos(nullptr, &glyph,
                         C2D_FontGlyphIndexFromCodePoint(nullptr, codePoint),
                         0, 1.0f, 1.0f);
    return glyph;
}

// The x offset that centres value's ink rather than its advance width:
// half the difference between the last glyph's right bearing and the
// first glyph's left bearing.
float inkOffset(const std::string &value, float scale)
{
    if (value.empty()) {
        return 0;
    }
    const fontGlyphPos_s first = glyphPos(static_cast<unsigned char>(value.front()));
    const fontGlyphPos_s last = glyphPos(static_cast<unsigned char>(value.back()));
    const float trailing = last.xAdvance - (last.xOffset + last.width);
    return (trailing - first.xOffset) / 2 * fontScale * scale * TextBoost;
}

void prepareAscii()
{
    for (uint32_t codePoint = 0; codePoint < 128; ++codePoint) {
        asciiAdvance[codePoint] = advance(codePoint);
    }
    asciiReady = true;
}

struct Glyph {
    size_t offset;
    size_t length;
    uint32_t codePoint;
    float width;
};

std::vector<Glyph> glyphs(const std::string &value, float scale)
{
    scale *= TextBoost;
    std::vector<Glyph> output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size();) {
        uint32_t codePoint = 0xfffd;
        size_t length = text::utf8SequenceLength(value, index, &codePoint);
        if (length == 0) {
            length = 1;
            codePoint = 0xfffd;
        }
        output.push_back({index, length, codePoint,
                          advance(codePoint) * scale});
        index += length;
    }
    return output;
}

bool isBreakAfter(uint32_t codePoint)
{
    return codePoint == ' ' || codePoint == '/' || codePoint == '?' ||
           codePoint == '&' || codePoint == '=' || codePoint == '-' ||
           codePoint == '_' || codePoint == '.' || codePoint == ',';
}

std::string rtrimSpaces(std::string value)
{
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

// Removes characters from the end until value + "..." fits.
std::string withEllipsis(const std::string &value, float scale,
                         float maxWidth)
{
    const float ellipsisWidth = textWidth(Ellipsis, scale);
    const std::vector<Glyph> all = glyphs(value, scale);
    float width = 0.0f;
    size_t end = 0;
    for (const Glyph &glyph : all) {
        if (width + glyph.width + ellipsisWidth > maxWidth) {
            break;
        }
        width += glyph.width;
        end = glyph.offset + glyph.length;
    }
    return rtrimSpaces(value.substr(0, end)) + Ellipsis;
}

// drawText with the glyphs narrowed to squeeze of their width.
void drawSqueezed(const std::string &value, float x, float y, float scale,
                  float squeeze, u32 textColor, u32 flags)
{
    C2D_Text textObject;
    C2D_TextParse(&textObject, textBuffer, value.c_str());
    C2D_TextOptimize(&textObject);
    C2D_DrawText(&textObject, flags | C2D_WithColor, x, y, Depth,
                 scale * TextBoost * squeeze, scale * TextBoost, textColor);
}

// One line no wider than maxWidth: as it is, a little narrower, or cut
// with "..." when even that is not enough.
void drawFitted(const std::string &value, float x, float y, float scale,
                u32 textColor, float maxWidth, u32 flags = 0)
{
    const float width = textWidth(value, scale);
    if (width <= maxWidth + 0.5f) {
        drawSqueezed(value, x, y, scale, 1, textColor, flags);
    } else if (width * MaxSqueeze <= maxWidth) {
        drawSqueezed(value, x, y, scale, maxWidth / width, textColor, flags);
    } else {
        drawSqueezed(withEllipsis(value, scale, maxWidth), x, y, scale, 1,
                     textColor, flags);
    }
}

// --- Colour --------------------------------------------------------------------

u32 lerp(u32 a, u32 b, float t)
{
    t = std::max(0.0f, std::min(1.0f, t));
    u32 out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        const float from = static_cast<float>((a >> shift) & 0xff);
        const float to = static_cast<float>((b >> shift) & 0xff);
        out |= static_cast<u32>(from + (to - from) * t + 0.5f) << shift;
    }
    return out;
}

u32 withAlpha(u32 value, float alpha)
{
    const u32 a = static_cast<u32>(((value >> 24) & 0xff) *
                                   std::max(0.0f, std::min(1.0f, alpha)));
    return (value & 0x00ffffff) | (a << 24);
}

u32 darker(u32 value, float by)
{
    return lerp(value, value & 0xff000000, by);
}

u32 colorAt(const Stop *stops, size_t count, float t)
{
    if (count == 0) {
        return 0;
    }
    if (t <= stops[0].at) {
        return stops[0].color;
    }
    for (size_t index = 1; index < count; ++index) {
        if (t <= stops[index].at) {
            const float span = stops[index].at - stops[index - 1].at;
            return lerp(stops[index - 1].color, stops[index].color,
                        span > 0 ? (t - stops[index - 1].at) / span : 1.0f);
        }
    }
    return stops[count - 1].color;
}

// --- Shapes --------------------------------------------------------------------

void loadAtlas();
bool atlasTriedLoad();

// citro2d tints images through procedural textures, which Azahar does not
// emulate. Shapes and icons instead take their colour from the vertices and
// their coverage from the atlas's alpha, the way citro2d draws text. Set
// after every textured quad, because citro2d sets up its own when the mode
// changes; it then stays for the rest of the batch.
void useVertexColor()
{
    C3D_TexEnv *env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR,
                  GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, GPU_TEXTURE0,
                  GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvInit(C3D_GetTexEnv(1));
    C3D_TexEnvInit(C3D_GetTexEnv(2));
    C3D_TexEnvInit(C3D_GetTexEnv(3));
}

// One quad cut from the atlas between pixel corners (u0, v0) and (u1, v1),
// coloured per corner.
void atlasQuad(float x, float y, float w, float h, float u0, float v0,
               float u1, float v1, u32 topLeft, u32 topRight, u32 bottomLeft,
               u32 bottomRight)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    atlasTriedLoad();
    if (!atlasReady) {
        C2D_DrawRectangle(x, y, Depth, w, h, topLeft, topRight, bottomLeft,
                          bottomRight);
        return;
    }
    const float size = static_cast<float>(IconAtlasSize);
    Tex3DS_SubTexture sub = {static_cast<u16>(std::max(1.0f, w)),
                             static_cast<u16>(std::max(1.0f, h)), u0 / size,
                             1.0f - v0 / size, u1 / size, 1.0f - v1 / size};
    const C2D_Image image = {&atlasTexture, &sub};
    C2D_DrawParams params = {{x, y, w, h}, {0, 0}, Depth, 0};
    C2D_ImageTint tint;
    tint.corners[C2D_TopLeft] = {topLeft, 1.0f};
    tint.corners[C2D_TopRight] = {topRight, 1.0f};
    tint.corners[C2D_BotLeft] = {bottomLeft, 1.0f};
    tint.corners[C2D_BotRight] = {bottomRight, 1.0f};
    C2D_DrawImage(image, &params, &tint);
    useVertexColor();
}

// The middle of the disc is solid white, so it fills plain areas.
constexpr float SolidU = ShapeSize / 2.0f - 2;
constexpr float SolidV = ShapeRow + ShapeSize / 2.0f - 2;

void solidQuad(float x, float y, float w, float h, u32 top, u32 bottom)
{
    atlasQuad(x, y, w, h, SolidU, SolidV, SolidU + 4, SolidV + 4, top, top,
              bottom, bottom);
}

void rect(float x, float y, float w, float h, u32 fill)
{
    solidQuad(x, y, w, h, fill, fill);
}

// A quarter of the disc (ring < 0) or of ring n, scaled to radius, for the
// corner whose square starts at (x, y); only rows from..to (0..1) of it.
void cornerQuad(unsigned which, int ring, float x, float y, float radius,
                u32 top, u32 bottom, float from = 0, float to = 1)
{
    const float half = ShapeSize / 2.0f;
    const float cellX = (ring + 1) * static_cast<float>(ShapeSize);
    const float cellY = static_cast<float>(ShapeRow);
    const bool right = which == corner::TopRight || which == corner::BottomRight;
    const bool lower = which == corner::BottomLeft || which == corner::BottomRight;
    const float u0 = cellX + (right ? half : 0);
    const float v0 = cellY + (lower ? half : 0);
    atlasQuad(x, y + radius * from, radius, radius * (to - from), u0,
              v0 + half * from, u0 + half, v0 + half * to, top, top, bottom,
              bottom);
}

int ringFor(float width, float radius)
{
    const float ratio = radius > 0 ? width / radius : 1;
    int best = 0;
    for (int index = 1; index < RingCount; ++index) {
        if (std::fabs(RingRatios[index] - ratio) <
            std::fabs(RingRatios[best] - ratio)) {
            best = index;
        }
    }
    return best;
}

// Fills a rounded rectangle with a vertical gradient: bands of plain quads
// split at every stop, and anti-aliased corners from the disc.
void fillShape(const Rect &area, float radius, unsigned corners,
               const Stop *stops, size_t count)
{
    if (area.w <= 0 || area.h <= 0) {
        return;
    }
    const float r = std::max(0.0f, std::min(radius, std::min(area.w, area.h) / 2));
    float ys[16];
    size_t used = 0;
    const auto add = [&](float y) {
        if (used < 16) {
            ys[used++] = std::max(0.0f, std::min(area.h, y));
        }
    };
    add(0);
    add(r);
    add(area.h - r);
    add(area.h);
    for (size_t index = 0; index < count && used < 16; ++index) {
        add(stops[index].at * area.h);
    }
    std::sort(ys, ys + used);
    for (size_t index = 1; index < used; ++index) {
        const float y0 = ys[index - 1];
        const float y1 = ys[index];
        if (y1 - y0 < 0.05f) {
            continue;
        }
        const u32 c0 = colorAt(stops, count, y0 / area.h);
        const u32 c1 = colorAt(stops, count, y1 / area.h);
        // Corners use the same bands as the middle, so they follow every
        // stop of the gradient.
        unsigned leftCorner = 0;
        unsigned rightCorner = 0;
        float cornerY = 0;
        if (r > 0 && y1 <= r + 0.01f) {
            leftCorner = corners & corner::TopLeft;
            rightCorner = corners & corner::TopRight;
        } else if (r > 0 && y0 >= area.h - r - 0.01f) {
            leftCorner = corners & corner::BottomLeft;
            rightCorner = corners & corner::BottomRight;
            cornerY = area.h - r;
        }
        const float from = (y0 - cornerY) / std::max(r, 0.01f);
        const float to = (y1 - cornerY) / std::max(r, 0.01f);
        const float left = leftCorner ? r : 0;
        const float right = rightCorner ? r : 0;
        if (leftCorner) {
            cornerQuad(leftCorner, -1, area.x, area.y + cornerY, r, c0, c1,
                       from, to);
        }
        if (rightCorner) {
            cornerQuad(rightCorner, -1, area.x + area.w - r, area.y + cornerY,
                       r, c0, c1, from, to);
        }
        solidQuad(area.x + left, area.y + y0, area.w - left - right, y1 - y0,
                  c0, c1);
    }
}

void quad(float x0, float y0, float x1, float y1, float x2, float y2,
          float x3, float y3, u32 fill)
{
    C2D_DrawTriangle(x0, y0, fill, x1, y1, fill, x2, y2, fill, Depth);
    C2D_DrawTriangle(x0, y0, fill, x2, y2, fill, x3, y3, fill, Depth);
}

// Draws a real picture (not a shape): citro2d must set up its own image
// combiner again, which drawing any plain shape first makes it do.
void plainImage(const C2D_Image &image, float x, float y)
{
    C2D_DrawRectSolid(0, 0, Depth, 0, 0, 0);
    C2D_DrawImageAt(image, x, y, Depth);
}

// Clips nest: a marquee inside a list row stays inside the list too.
std::vector<Rect> clips;

void applyClip(const Rect &area)
{
    const int screenWidth = onBottom ? 320 : 400;
    const int left = std::max(0, static_cast<int>(240 - (area.y + area.h)));
    const int top = std::max(0, static_cast<int>(screenWidth - (area.x + area.w)));
    const int right = std::min(240, static_cast<int>(240 - area.y));
    const int bottom = std::min(screenWidth, static_cast<int>(screenWidth - area.x));
    C3D_SetScissor(GPU_SCISSOR_NORMAL, left, top, std::max(left, right),
                   std::max(top, bottom));
}

// Draws only inside area (and any clip already set) on the current screen,
// until unclip().
void clip(const Rect &area)
{
    C2D_Flush();
    Rect shown = area;
    if (!clips.empty()) {
        const Rect &outer = clips.back();
        const float x0 = std::max(outer.x, area.x);
        const float y0 = std::max(outer.y, area.y);
        const float x1 = std::min(outer.x + outer.w, area.x + area.w);
        const float y1 = std::min(outer.y + outer.h, area.y + area.h);
        shown = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    clips.push_back(shown);
    applyClip(shown);
}

void unclip()
{
    C2D_Flush();
    if (!clips.empty()) {
        clips.pop_back();
    }
    if (clips.empty()) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        applyClip(clips.back());
    }
}

// 0..1, rising and falling every period milliseconds.
float pulse(float period)
{
    const float phase = static_cast<float>(now() % static_cast<uint64_t>(period)) /
                        period;
    return 0.5f + 0.5f * std::sin(phase * 2 * Pi);
}

std::string titleCase(const std::string &value)
{
    std::string output = value;
    bool start = true;
    for (char &character : output) {
        if (start && character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
        start = character == ' ';
    }
    return output;
}

// --- Backdrop --------------------------------------------------------------------

bool makeTexture(C3D_Tex &texture, u16 width, u16 height, GPU_TEXCOLOR format)
{
    if (!C3D_TexInit(&texture, width, height, format)) {
        return false;
    }
    std::fill_n(static_cast<uint8_t *>(texture.data), texture.size, 0);
    C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

// Copies the screen as last shown into fromTextures[index]. The framebuffer
// and an RGB8 texture both keep pixels as blue, green, red bytes.
bool captureFrom(int index, gfxScreen_t screen, u16 width)
{
    if (gfxGetScreenFormat(screen) != GSP_BGR8_OES) {
        return false;
    }
    u16 fbWidth = 0;
    u16 fbHeight = 0;
    const uint8_t *framebuffer =
        gfxGetFramebuffer(screen, GFX_LEFT, &fbWidth, &fbHeight);
    if (framebuffer == nullptr || fbWidth != 240 || fbHeight != width) {
        return false;
    }
    if (!fromReady[index]) {
        if (!makeTexture(fromTextures[index], 512, 256, GPU_RGB8)) {
            return false;
        }
        fromReady[index] = true;
    }
    uint8_t *texels = static_cast<uint8_t *>(fromTextures[index].data);
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t *column = framebuffer + x * 240 * 3;
        for (uint32_t y = 0; y < 240; ++y) {
            std::memcpy(texels + image::tiledIndex(x, y, 512) * 3,
                        column + (239 - y) * 3, 3);
        }
    }
    C3D_TexFlush(&fromTextures[index]);
    return true;
}

void captureScreen(int index, gfxScreen_t screen, u16 width)
{
    if (gfxGetScreenFormat(screen) != GSP_BGR8_OES) {
        return;
    }
    u16 fbWidth = 0;
    u16 fbHeight = 0;
    const uint8_t *framebuffer =
        gfxGetFramebuffer(screen, GFX_LEFT, &fbWidth, &fbHeight);
    if (framebuffer == nullptr || fbWidth != 240 || fbHeight != width) {
        return;
    }
    if (!backdropReady[index] &&
        !makeTexture(backdropTextures[index], 512, 256, GPU_RGB565)) {
        return;
    }
    // The framebuffer is the screen turned on its side: columns of 240
    // pixels from the bottom up, three bytes each in BGR order.
    std::vector<uint16_t> pixels(static_cast<size_t>(width) * 240);
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t *column = framebuffer + x * 240 * 3;
        for (uint32_t y = 0; y < 240; ++y) {
            const uint8_t *pixel = column + (239 - y) * 3;
            pixels[y * width + x] = static_cast<uint16_t>(
                ((pixel[2] >> 3) << 11) | ((pixel[1] >> 2) << 5) |
                (pixel[0] >> 3));
        }
    }
    image::tileImage(pixels.data(), width, 240,
                     static_cast<uint16_t *>(backdropTextures[index].data), 512);
    C3D_TexFlush(&backdropTextures[index]);
    backdropReady[index] = true;
}

// The captured screen, or a plain background when there is none.
void drawBackdrop(bool bottom)
{
    const int index = bottom ? 1 : 0;
    if (backdropReady[index]) {
        const C2D_Image image = {&backdropTextures[index], &BackdropSubs[index]};
        plainImage(image, 0, 0);
    } else if (bottom) {
        drawBottomBackground();
    } else {
        drawTopBackground();
    }
}

// A screen's plain background at alpha, over whatever is drawn.
void drawBackground(bool bottom, float alpha)
{
    const u32 a = withAlpha(bottom ? color::BottomA : color::TopA, alpha);
    const u32 b = withAlpha(bottom ? color::BottomB : color::TopB, alpha);
    C2D_DrawRectangle(0, 0, Depth, bottom ? BottomWidth : TopWidth,
                      ScreenHeight, a, a, b, b);
}

// The screen before, at alpha, over the new one.
void drawFrom(int index, float alpha)
{
    const C2D_Image image = {&fromTextures[index], &BackdropSubs[index]};
    C2D_ImageTint tint;
    C2D_AlphaImageTint(&tint, alpha);
    C2D_DrawRectSolid(0, 0, Depth, 0, 0, 0);
    C2D_DrawImageAt(image, 0, 0, Depth, &tint);
    // Drawn now, with citro2d's own combiner: queued past useVertexColor it
    // would take the tint's colour, which is black.
    C2D_Flush();
    useVertexColor();
}

void loadAtlas()
{
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load("romfs:/ui/icons.png", &width, &height,
                                &channels, 4);
    if (pixels == nullptr) {
        return;
    }
    // White with the icon's coverage as alpha, so a tint colours it.
    if (width == IconAtlasSize && height == IconAtlasSize &&
        makeTexture(atlasTexture, IconAtlasSize, IconAtlasSize, GPU_LA8)) {
        uint16_t *texels = static_cast<uint16_t *>(atlasTexture.data);
        for (uint32_t y = 0; y < IconAtlasSize; ++y) {
            for (uint32_t x = 0; x < IconAtlasSize; ++x) {
                texels[image::tiledIndex(x, y, IconAtlasSize)] =
                    static_cast<uint16_t>(
                        0xff00 | pixels[(y * IconAtlasSize + x) * 4 + 3]);
            }
        }
        C3D_TexFlush(&atlasTexture);
        atlasReady = true;
    }
    stbi_image_free(pixels);
}

bool atlasTriedLoad()
{
    if (!atlasTried) {
        atlasTried = true;
        loadAtlas();
    }
    return atlasReady;
}

// --- Status bar ------------------------------------------------------------------

struct Status {
    uint64_t checked = 0;
    int bars = 0;
    bool connected = false;
    int battery = 5;
    bool charging = false;
    int64_t sdFree = -1; // -1 when the card cannot be read
};
Status status;

// Below this the free space is shown as a warning.
constexpr int64_t LowSdSpace = 256LL * 1024 * 1024;

void refreshStatus()
{
    const uint64_t time = now();
    if (status.checked != 0 && time - status.checked < 2000) {
        return;
    }
    status.checked = time;
    // A cheap call: the FS module keeps the free cluster count.
    status.sdFree = platform::sdFreeBytes();
    status.connected = platform::wifiStatus() == platform::Wifi::Connected;
    status.bars = status.connected ? osGetWifiStrength() : 0;
    if (ptmu) {
        u8 level = 5;
        u8 charging = 0;
        if (R_SUCCEEDED(PTMU_GetBatteryLevel(&level))) {
            status.battery = level;
        }
        if (R_SUCCEEDED(PTMU_GetBatteryChargeState(&charging))) {
            status.charging = charging != 0;
        }
    }
}

// --- Buttons ---------------------------------------------------------------------

struct ButtonLook {
    Stop body[5];
    size_t stops;
    u32 edgeTop;
    u32 edgeBottom;
    u32 highlight;
    u32 label;
};

ButtonLook lookFor(Style style)
{
    using color::rgb;
    switch (style) {
    case Style::Chosen:
        return {{{0, rgb(0xFFF6B8)}, {0.4f, rgb(0xFFED7F)}, {0.8f, rgb(0xE4D766)},
                 {1, rgb(0xC9BF49)}},
                4, rgb(0xC8A23A), rgb(0xA88A2A), rgb(0xFFFBD9), rgb(0xE07A10)};
    case Style::Current:
        return {{{0, rgb(0x9CC8D6)}, {0.3f, rgb(0x80B6C8)}, {0.75f, rgb(0x6C9DAB)},
                 {1, rgb(0x557E86)}},
                4, rgb(0x4E7A86), rgb(0x3D626B), rgb(0xC9E6EF), rgb(0xFFFFFF)};
    case Style::Danger:
        return {{{0, rgb(0xFF6A4E)}, {0.45f, rgb(0xFE3014)}, {0.7f, rgb(0xD8250F)},
                 {1, rgb(0xB31B0B)}},
                4, rgb(0x9A1A0C), rgb(0x7A1206), rgb(0xFFB2A0), rgb(0xFFFFFF)};
    case Style::Normal:
    case Style::Disabled:
        break;
    }
    return {{{0, rgb(0xFEFCF5)}, {0.45f, rgb(0xF4F1E6)}, {0.78f, rgb(0xE0DCCB)},
             {0.92f, rgb(0xCFC9B0)}, {1, rgb(0xB6B192)}},
            5, rgb(0xB3AA83), rgb(0x8F8659), rgb(0xFFFFFF), color::InkButton};
}

void drawButtonBody(const Rect &area, float radius, Style style, bool pressed)
{
    if (style == Style::Disabled) {
        strokeRound(area, radius, 1.5f, color::rgb(0xD3C88F));
        return;
    }
    ButtonLook look = lookFor(style);
    Rect body = area;
    if (pressed) {
        body.y += 1;
        for (size_t index = 0; index < look.stops; ++index) {
            look.body[index].color = darker(look.body[index].color, 0.1f);
        }
    } else {
        drawShadow({area.x, area.y + 2.5f, area.w, area.h}, radius, 3,
                   color::rgb(0x504620, 0x40));
    }
    const Stop edge[] = {{0, look.edgeTop}, {0.8f, look.edgeTop},
                         {1, look.edgeBottom}};
    fillShape({body.x - 1, body.y - 1, body.w + 2, body.h + 2.5f}, radius + 1,
              corner::All, edge, 3);
    fillShape(body, radius, corner::All, look.body, look.stops);
    rect(body.x + radius * 0.7f, body.y + 0.6f, body.w - radius * 1.4f, 1,
         withAlpha(look.highlight, 0.85f));
    if (style == Style::Chosen) {
        // Orange brackets at both ends, as on the system's option buttons.
        const u32 bracket = color::rgb(0xE89436);
        const float r = radius + 2;
        const float width = 4;
        const Rect outer = {body.x - 2, body.y - 2, body.w + 4, body.h + 4};
        const int ring = ringFor(width, r);
        cornerQuad(corner::TopLeft, ring, outer.x, outer.y, r, bracket, bracket);
        cornerQuad(corner::BottomLeft, ring, outer.x, outer.y + outer.h - r, r,
                   bracket, bracket);
        rect(outer.x, outer.y + r, width, outer.h - 2 * r, bracket);
        rect(outer.x + r, outer.y, 3, width, bracket);
        rect(outer.x + r, outer.y + outer.h - width, 3, width, bracket);
        cornerQuad(corner::TopRight, ring, outer.x + outer.w - r, outer.y, r,
                   bracket, bracket);
        cornerQuad(corner::BottomRight, ring, outer.x + outer.w - r,
                   outer.y + outer.h - r, r, bracket, bracket);
        rect(outer.x + outer.w - width, outer.y + r, width, outer.h - 2 * r,
             bracket);
        rect(outer.x + outer.w - r - 3, outer.y, 3, width, bracket);
        rect(outer.x + outer.w - r - 3, outer.y + outer.h - width, 3, width,
             bracket);
    }
}

// A label that fits the button: smaller, or on two lines, before it is cut.
void drawLabel(const std::string &label, const Rect &area, float scale,
               u32 textColor)
{
    const float room = area.w - 14;
    float use = scale;
    if (textWidth(label, use) > room) {
        use = std::max(scale * 0.82f, room / std::max(1.0f, textWidth(label, 1.0f)));
    }
    const float width = textWidth(label, use);
    if (width <= room) {
        drawCentered(label, area, use, textColor);
        return;
    }
    if (width * MaxSqueeze <= room) {
        drawSqueezed(label, area.x + area.w / 2,
                     area.y + (area.h - lineHeight(use)) / 2 + 1, use,
                     room / width, textColor, C2D_AlignCenter);
        return;
    }
    // Two lines, small enough that both fit the button's height.
    float small = scale * 0.82f;
    while (small > 0.36f && lineHeight(small) * 0.92f * 2 > area.h - 2) {
        small -= 0.02f;
    }
    const std::vector<std::string> lines = wrap(label, small, area.w - 12, 2);
    const float step = lineHeight(small) * 0.92f;
    float y = area.y + area.h / 2 - step * lines.size() / 2;
    for (const std::string &line : lines) {
        drawText(line, area.x + area.w / 2, y, small, textColor,
                 C2D_AlignCenter);
        y += step;
    }
}

// --- Dialog drawing --------------------------------------------------------------

constexpr Rect DialogBox = {10, 12, 300, 216};
constexpr float StripHeight = 39;
constexpr float DangerStripHeight = 30;
constexpr float StripRadius = 13; // the dialog box's corners

std::vector<Rect> stripButtons(const Rect &box, size_t count, float height)
{
    std::vector<Rect> rects;
    const float width = box.w / std::max<size_t>(1, count);
    for (size_t index = 0; index < count; ++index) {
        rects.push_back({box.x + index * width, box.y + box.h - height, width,
                         height});
    }
    return rects;
}

void drawDialogBox(const Rect &box)
{
    for (int ring = 0; ring < 6; ++ring) {
        fillRound({box.x - ring * 1.6f, box.y - ring * 1.6f + 1,
                   box.w + ring * 3.2f, box.h + ring * 3.2f},
                  13 + ring * 1.6f, color::rgb(0x000000, 0x12));
    }
    fillRound(box, 13,
              {{0, color::rgb(0xFFFDF0)}, {0.12f, color::rgb(0xF6F4EB)},
               {0.5f, color::rgb(0xE6E5DE)}, {0.88f, color::rgb(0xF2F0E6)},
               {1, color::rgb(0xFFFDF0)}});
}

void drawStrip(const Rect &box, const std::vector<std::string> &labels,
               int selected, int pressed, bool dark, float height,
               int dangerLabel)
{
    const std::vector<Rect> rects = stripButtons(box, labels.size(), height);
    for (size_t index = 0; index < rects.size(); ++index) {
        const Rect &area = rects[index];
        unsigned corners = 0;
        if (index == 0) {
            corners |= corner::BottomLeft;
        }
        if (index + 1 == rects.size()) {
            corners |= corner::BottomRight;
        }
        Stop stops[4] = {{0, color::rgb(0xFFFDF2)}, {0.45f, color::rgb(0xFFF0B4)},
                         {0.8f, color::rgb(0xFAE29A)}, {1, color::rgb(0xAE8D67)}};
        if (dark) {
            stops[0] = {0, color::rgb(0xB9B7B4)};
            stops[1] = {0.2f, color::rgb(0x9D9B98)};
            stops[2] = {0.7f, color::rgb(0x57585A)};
            stops[3] = {1, color::rgb(0x333435)};
        }
        if (static_cast<int>(index) == pressed) {
            for (Stop &stop : stops) {
                stop.color = darker(stop.color, 0.12f);
            }
        }
        fillShape(area, StripRadius, corners, stops, 4);
        if (index > 0) {
            rect(area.x, area.y, 1, area.h, color::rgb(0x191817));
        }
        const u32 textColor = dark ? color::rgb(0xFFFFFF)
                            : static_cast<int>(index) == dangerLabel
                                ? color::Bad
                                : color::InkDialog;
        // One line per button, shrinking, then a little narrower; a label
        // still too long goes on two lines rather than losing its end.
        float scale = type::Tab;
        const float room = area.w - 12;
        while (scale > 0.42f && textWidth(labels[index], scale) > room) {
            scale -= 0.02f;
        }
        const float width = textWidth(labels[index], scale);
        if (width <= room) {
            drawCentered(labels[index], area, scale, textColor);
        } else if (width * MaxSqueeze <= room) {
            drawSqueezed(labels[index], area.x + area.w / 2,
                         area.y + (area.h - lineHeight(scale)) / 2 + 1, scale,
                         room / width, textColor, C2D_AlignCenter);
        } else {
            const std::vector<std::string> lines =
                wrap(labels[index], scale, area.w - 10, 2);
            const float step = lineHeight(scale) * 0.92f;
            float y = area.y + (area.h - step * lines.size()) / 2 + 1;
            for (const std::string &line : lines) {
                drawFitted(line, area.x + area.w / 2, y, scale, textColor,
                           area.w - 10, C2D_AlignCenter);
                y += step;
            }
        }
        if (static_cast<int>(index) == selected) {
            // Concentric with the strip's rounded corners, square elsewhere.
            const Rect ring = area.inset(3);
            strokeRound(ring, StripRadius - 3, 3, color::Focus, corners);
        }
    }
    rect(box.x, box.y + box.h - height, box.w, 1, color::rgb(0x555553));
}

} // namespace

// --- Basics ----------------------------------------------------------------------

bool Rect::contains(const touchPosition &touch) const
{
    return touch.px >= x && touch.px < x + w && touch.py >= y &&
           touch.py < y + h;
}

bool init()
{
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
        return false;
    }
    if (!C2D_Init(MaxObjects)) {
        C3D_Fini();
        return false;
    }

    C2D_Prepare();
    topTarget = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    textBuffer = C2D_TextBufNew(8192);

    if (topTarget == nullptr || bottomTarget == nullptr ||
        textBuffer == nullptr) {
        shutdown();
        return false;
    }

    // C2D_TextBufNew maps the system font, so its metrics are available now.
    loadFontMetrics();
    prepareAscii();
    ptmu = R_SUCCEEDED(ptmuInit());
    startTime = osGetTime();
    hidSetRepeatParameters(20, 6);
    return true;
}

void shutdown()
{
    for (int index = 0; index < 2; ++index) {
        if (backdropReady[index]) {
            C3D_TexDelete(&backdropTextures[index]);
            backdropReady[index] = false;
        }
        if (fromReady[index]) {
            C3D_TexDelete(&fromTextures[index]);
            fromReady[index] = false;
        }
    }
    if (atlasReady) {
        C3D_TexDelete(&atlasTexture);
        atlasReady = false;
    }
    if (pictureTextureReady) {
        C3D_TexDelete(&pictureTexture);
        pictureTextureReady = false;
    }
    if (iconTextureReady) {
        C3D_TexDelete(&iconTexture);
        iconTextureReady = false;
        iconPixels.clear();
    }
    if (ptmu) {
        ptmuExit();
        ptmu = false;
    }
    if (textBuffer != nullptr) {
        C2D_TextBufDelete(textBuffer);
        textBuffer = nullptr;
    }
    C2D_Fini();
    C3D_Fini();
    topTarget = nullptr;
    bottomTarget = nullptr;
}

bool keepRunning()
{
    if (!closing && !aptMainLoop()) {
        closing = true;
    }
    return !closing;
}

uint64_t now()
{
    return osGetTime() - startTime;
}

bool Input::tapped(const Rect &area) const
{
    return (down & KEY_TOUCH) != 0 && area.contains(touch);
}

void lockTouch()
{
    touchLockStart = now();
    touchLocked = true;
}

void enterScreen(const std::string &id)
{
    lockTouch();
    // Read the status bar again, so the SD card's free space is right as
    // soon as a copy, delete or install brings us back.
    status.checked = 0;
    // The same screen shown again (after a dialog, or a key that redraws
    // it) does not fade.
    if (id != lastScreen) {
        lastScreen = id;
        crash::noteScreen(id);
        if (!screenToLog.empty()) { // drew no title
            logging::info("screen", {{"id", screenToLog}});
        }
        screenToLog = id;
        fromCapture = frameShown && captureFrom(0, GFX_TOP, 400) &&
                      captureFrom(1, GFX_BOTTOM, 320);
        fadeStart = now();
        fading = true;
    }
}

#ifdef THREEDSDL_SCRIPTED_INPUT
// Test builds only (make EXTRA_DEFINES=-DTHREEDSDL_SCRIPTED_INPUT): taps and
// buttons come from lines appended to sdmc:/3ds/3dsDL/input.txt, so Azahar
// can be driven without having focus. A line is "tap X Y" in bottom screen
// pixels or "key NAME" (A, B, X, Y, L, R, START, SELECT, UP, DOWN, LEFT,
// RIGHT). Each is held for a few frames, then released.
namespace scripted {

struct Step {
    u32 key = 0; // KEY_TOUCH for a tap
    u16 x = 0;
    u16 y = 0;
};

std::vector<Step> steps;
size_t next = 0;
long offset = 0;
uint64_t lastRead = 0;
int frame = -1; // of the current step, -1 when idle
Step current;

u32 keyNamed(const std::string &name)
{
    static const std::pair<const char *, u32> Keys[] = {
        {"A", KEY_A},         {"B", KEY_B},          {"X", KEY_X},
        {"Y", KEY_Y},         {"L", KEY_L},          {"R", KEY_R},
        {"START", KEY_START}, {"SELECT", KEY_SELECT}, {"UP", KEY_DUP},
        {"DOWN", KEY_DDOWN},  {"LEFT", KEY_DLEFT},   {"RIGHT", KEY_DRIGHT}};
    for (const auto &key : Keys) {
        if (name == key.first) {
            return key.second;
        }
    }
    return 0;
}

void readNew()
{
    FILE *file = std::fopen("sdmc:/3ds/3dsDL/input.txt", "rb");
    if (file == nullptr) {
        return;
    }
    std::fseek(file, 0, SEEK_END);
    if (std::ftell(file) < offset) {
        offset = 0; // the file was started again
    }
    std::fseek(file, offset, SEEK_SET);
    char line[64];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        char word[16] = {};
        char name[16] = {};
        unsigned x = 0;
        unsigned y = 0;
        Step step;
        if (std::sscanf(line, "%15s %u %u", word, &x, &y) == 3 &&
            std::strcmp(word, "tap") == 0) {
            step.key = KEY_TOUCH;
            step.x = static_cast<u16>(x);
            step.y = static_cast<u16>(y);
        } else if (std::sscanf(line, "%15s %15s", word, name) == 2 &&
                   std::strcmp(word, "key") == 0) {
            step.key = keyNamed(name);
        }
        if (step.key != 0) {
            steps.push_back(step);
        }
    }
    offset = std::ftell(file);
    std::fclose(file);
}

void apply(Input &input, touchPosition &last)
{
    if (frame < 0) {
        if (now() - lastRead >= 250) {
            lastRead = now();
            readNew();
        }
        if (next >= steps.size()) {
            return;
        }
        current = steps[next++];
        frame = 0;
    }
    // Held for frames 0-3, released on 4, then a pause before the next.
    if (frame < 4) {
        input.held |= current.key;
        if (frame == 0) {
            input.down |= current.key;
            input.repeat |= current.key;
        }
        if (current.key == KEY_TOUCH) {
            last.px = current.x;
            last.py = current.y;
        }
    } else if (frame == 4) {
        input.up |= current.key;
    }
    if (++frame > 10) {
        frame = -1;
    }
}

} // namespace scripted
#endif

Input readInput()
{
    static touchPosition last {};
    hidScanInput();
    Input input;
    input.down = hidKeysDown();
    input.repeat = hidKeysDownRepeat();
    input.held = hidKeysHeld();
    input.up = hidKeysUp();
    if (input.held & KEY_TOUCH) {
        hidTouchRead(&last);
    }
#ifdef THREEDSDL_SCRIPTED_INPUT
    scripted::apply(input, last);
#endif
    input.touch = last;
    if (touchLocked && !(input.held & KEY_TOUCH) &&
        now() - touchLockStart >= ScreenIn) {
        touchLocked = false;
    }
    if (touchLocked) {
        input.down &= ~KEY_TOUCH;
        input.repeat &= ~KEY_TOUCH;
        input.held &= ~KEY_TOUCH;
        input.up &= ~KEY_TOUCH;
    }
    return input;
}

void beginFrame()
{
    if (frameHook) {
        frameHook();
    }
    lastFrameWasDialog = inDialog;
    clips.clear();
    C2D_TextBufClear(textBuffer);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    // Shapes are drawn in order, like a painter's canvas, without depth.
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C2D_TargetClear(topTarget, color::TopB);
    C2D_TargetClear(bottomTarget, color::BottomB);
    C2D_SceneBegin(topTarget);
    onBottom = false;
    drawTopBackground();
}

void beginTop()
{
    C2D_SceneBegin(topTarget);
    onBottom = false;
}

void beginBottom()
{
    C2D_SceneBegin(bottomTarget);
    onBottom = true;
    drawBottomBackground();
}

void endFrame()
{
    const uint64_t age = now() - fadeStart;
    if (fading && age >= FadeIn) {
        fading = false;
    }
    if (fading) {
        // The last screen fades to the plain background in the first half
        // and the new one comes up from it in the second. The first screen
        // has nothing to fade out, so it takes the whole time coming up.
        const float t = static_cast<float>(age) / FadeIn;
        const bool leaving = fromCapture && t < 0.5f;
        const float p = leaving ? t * 2 : fromCapture ? t * 2 - 1 : t;
        const float alpha = 1 - p * p * (3 - 2 * p);
        for (int index = 1; index >= 0; --index) {
            C2D_SceneBegin(index == 1 ? bottomTarget : topTarget);
            drawBackground(index == 1, leaving ? 1 : alpha);
            if (leaving) {
                drawFrom(index, alpha);
            }
        }
        onBottom = false;
    }
    drawNotice();
    C3D_FrameEnd(0);
    frameShown = true;
}

void drawTopBackground()
{
    C2D_DrawRectangle(0, 0, Depth, TopWidth, ScreenHeight, color::TopA,
                      color::TopA, color::TopB, color::TopB);
}

void drawBottomBackground()
{
    C2D_DrawRectangle(0, 0, Depth, BottomWidth, ScreenHeight, color::BottomA,
                      color::BottomA, color::BottomB, color::BottomB);
}

// --- Text ------------------------------------------------------------------------

float textWidth(const std::string &value, float scale)
{
    float width = 0.0f;
    for (const Glyph &glyph : glyphs(value, scale)) {
        width += glyph.width;
    }
    return width;
}

float lineHeight(float scale)
{
    return std::ceil(scale * TextBoost * fontScale * fontLineFeed);
}

std::string fitEnd(const std::string &value, float scale, float maxWidth)
{
    if (textWidth(value, scale) <= maxWidth) {
        return value;
    }
    return withEllipsis(value, scale, maxWidth);
}

std::string fitMiddle(const std::string &value, float scale, float maxWidth)
{
    if (textWidth(value, scale) <= maxWidth) {
        return value;
    }
    const std::vector<Glyph> all = glyphs(value, scale);
    const float budget = maxWidth - textWidth(Ellipsis, scale);
    if (budget <= 0.0f || all.empty()) {
        return Ellipsis;
    }

    float headWidth = 0.0f;
    size_t head = 0;
    while (head < all.size() && headWidth + all[head].width <= budget * 0.4f) {
        headWidth += all[head].width;
        ++head;
    }
    float tailWidth = 0.0f;
    size_t tail = all.size();
    while (tail > head &&
           headWidth + tailWidth + all[tail - 1].width <= budget) {
        tailWidth += all[tail - 1].width;
        --tail;
    }

    const size_t headEnd = head == 0 ? 0 : all[head - 1].offset +
                                               all[head - 1].length;
    const size_t tailStart = tail < all.size() ? all[tail].offset
                                               : value.size();
    return value.substr(0, headEnd) + Ellipsis + value.substr(tailStart);
}

std::vector<std::string> wrap(const std::string &value, float scale,
                              float maxWidth, size_t maxLines)
{
    std::vector<std::string> lines;
    if (maxLines == 0) {
        return lines;
    }

    const std::vector<Glyph> all = glyphs(value, scale);
    std::string line;
    float width = 0.0f;
    size_t breakAt = 0; // bytes of line up to the last break opportunity
    bool cut = false;

    const auto push = [&](const std::string &finished) {
        if (lines.size() == maxLines) {
            cut = true;
            return;
        }
        lines.push_back(rtrimSpaces(finished));
    };

    for (const Glyph &glyph : all) {
        if (cut) {
            break;
        }
        if (glyph.codePoint == '\n') {
            push(line);
            line.clear();
            width = 0.0f;
            breakAt = 0;
            continue;
        }
        if (glyph.codePoint == ' ' && line.empty()) {
            continue;
        }

        if (width + glyph.width > maxWidth && !line.empty()) {
            std::string rest;
            if (breakAt > 0 && breakAt < line.size()) {
                rest = line.substr(breakAt);
                line.erase(breakAt);
            }
            push(line);
            line = rest;
            while (!line.empty() && line.front() == ' ') {
                line.erase(line.begin());
            }
            width = textWidth(line, scale);
            breakAt = 0;
            if (cut) {
                break;
            }
            if (glyph.codePoint == ' ' && line.empty()) {
                continue;
            }
        }

        line.append(value, glyph.offset, glyph.length);
        width += glyph.width;
        if (isBreakAfter(glyph.codePoint)) {
            breakAt = line.size();
        }
    }
    if (!cut && !line.empty()) {
        push(line);
    }

    if (cut && !lines.empty()) {
        lines.back() = withEllipsis(lines.back(), scale, maxWidth);
    }
    return lines;
}

void drawText(const std::string &value, float x, float y, float scale,
              u32 textColor, u32 flags)
{
    C2D_Text textObject;
    C2D_TextParse(&textObject, textBuffer, value.c_str());
    C2D_TextOptimize(&textObject);
    C2D_DrawText(&textObject, flags | C2D_WithColor, x, y, Depth,
                 scale * TextBoost, scale * TextBoost, textColor);
}

namespace {

// Wrapped lines where the last one, when text is cut off, holds everything
// that did not fit, to scroll instead of ending in "...".
struct ScrollingLines {
    std::vector<std::string> lines;
    bool lastScrolls = false;
};

ScrollingLines wrapScrolling(const std::string &value, float scale,
                             float maxWidth, size_t maxLines)
{
    ScrollingLines out;
    const std::vector<std::string> all = wrap(value, scale, maxWidth, 4096);
    if (all.size() <= maxLines || maxLines == 0) {
        out.lines = all;
        return out;
    }
    // Where the last shown line starts in value; wrap's lines are pieces of
    // it with the spaces at the breaks left out.
    size_t position = 0;
    for (size_t index = 0; index + 1 < maxLines; ++index) {
        const size_t found = value.find(all[index], position);
        if (found == std::string::npos) {
            out.lines = wrap(value, scale, maxWidth, maxLines);
            return out;
        }
        position = found + all[index].size();
        out.lines.push_back(all[index]);
    }
    std::string rest = value.substr(position);
    for (char &c : rest) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    const size_t first = rest.find_first_not_of(' ');
    out.lines.push_back(first == std::string::npos ? "" : rest.substr(first));
    out.lastScrolls = true;
    return out;
}

} // namespace

float drawWrapped(const std::string &value, float x, float y, float scale,
                  u32 textColor, float maxWidth, size_t maxLines, u32 flags)
{
    const float step = lineHeight(scale);
    const ScrollingLines wrapped = wrapScrolling(value, scale, maxWidth, maxLines);
    for (size_t index = 0; index < wrapped.lines.size(); ++index) {
        if (wrapped.lastScrolls && index + 1 == wrapped.lines.size()) {
            drawMarquee(wrapped.lines[index], x, y, scale, textColor, maxWidth,
                        flags);
        } else {
            drawText(wrapped.lines[index], x, y, scale, textColor, flags);
        }
        y += step;
    }
    return y;
}

void drawCentered(const std::string &value, const Rect &area, float scale,
                  u32 textColor, bool left)
{
    const float y = area.y + (area.h - lineHeight(scale)) / 2 + 1;
    if (left) {
        drawText(value, area.x, y, scale, textColor);
    } else {
        drawText(value, area.x + area.w / 2, y, scale, textColor,
                 C2D_AlignCenter);
    }
}

namespace {

// When each scrolling text was first shown, so it starts from its beginning
// and a newly highlighted row does not join a scroll halfway.
struct MarqueeStart {
    uint64_t key;
    uint64_t start;
    uint64_t seen;
};
std::vector<MarqueeStart> marquees;

constexpr float MarqueeSpeed = 32;     // pixels a second
constexpr uint64_t MarqueeWait = 1400; // ms at the start
constexpr uint64_t MarqueeRest = 1100; // ms at the end before starting over

uint64_t marqueeKey(const std::string &value, float x, float y)
{
    uint64_t hash = 1469598103934665603ULL;
    for (const char c : value) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 1099511628211ULL;
    }
    hash ^= static_cast<uint64_t>(x * 4) << 20;
    hash ^= static_cast<uint64_t>(y * 4) << 40;
    return hash ^ (onBottom ? 1 : 0);
}

float marqueeOffset(uint64_t key, float overflow)
{
    const uint64_t time = now();
    MarqueeStart *found = nullptr;
    for (MarqueeStart &entry : marquees) {
        if (entry.key == key) {
            found = &entry;
            break;
        }
    }
    if (found == nullptr || time - found->seen > 400) {
        // Forget texts that are no longer on screen.
        marquees.erase(std::remove_if(marquees.begin(), marquees.end(),
                                      [&](const MarqueeStart &entry) {
                                          return time - entry.seen > 1000 ||
                                                 entry.key == key;
                                      }),
                       marquees.end());
        marquees.push_back({key, time, time});
        found = &marquees.back();
    }
    found->seen = time;
    const uint64_t travel = static_cast<uint64_t>(overflow / MarqueeSpeed * 1000);
    const uint64_t cycle = MarqueeWait + travel + MarqueeRest;
    const uint64_t at = (time - found->start) % cycle;
    if (at < MarqueeWait) {
        return 0;
    }
    if (at >= MarqueeWait + travel) {
        return overflow;
    }
    return static_cast<float>(at - MarqueeWait) / 1000.0f * MarqueeSpeed;
}

} // namespace

void drawMarquee(const std::string &value, float x, float y, float scale,
                 u32 textColor, float maxWidth, u32 flags)
{
    const float width = textWidth(value, scale);
    if (width <= maxWidth + 0.5f) {
        drawText(value, x, y, scale, textColor, flags);
        return;
    }
    if (width * MaxSqueeze <= maxWidth) {
        drawSqueezed(value, x, y, scale, maxWidth / width, textColor, flags);
        return;
    }
    const u32 align = flags & C2D_AlignMask;
    const float left = align == C2D_AlignCenter ? x - maxWidth / 2
                     : align == C2D_AlignRight  ? x - maxWidth
                                                : x;
    const float offset = marqueeOffset(marqueeKey(value, left, y),
                                       width - maxWidth);
    // Only the glyphs in view are drawn: citro2d's C2D_DrawText never checks
    // its vertex buffer, and a line of thousands overruns it and hangs the GPU.
    size_t from = 0;
    size_t to = 0;
    float start = 0.0f; // where the first drawn glyph starts in the line
    float at = 0.0f;
    for (const Glyph &glyph : glyphs(value, scale)) {
        if (at >= offset + maxWidth) {
            break;
        }
        if (at + glyph.width <= offset) {
            from = glyph.offset + glyph.length;
            start = at + glyph.width;
        }
        to = glyph.offset + glyph.length;
        at += glyph.width;
    }
    clip({left, y - 2, maxWidth, lineHeight(scale) + 6});
    drawText(value.substr(from, to - from), left - offset + start, y, scale,
             textColor, flags & ~C2D_AlignMask);
    unclip();
}

void drawCenteredMarquee(const std::string &value, const Rect &area,
                         float scale, u32 textColor, bool left)
{
    const float y = area.y + (area.h - lineHeight(scale)) / 2 + 1;
    if (left) {
        drawMarquee(value, area.x, y, scale, textColor, area.w);
    } else {
        drawMarquee(value, area.x + area.w / 2, y, scale, textColor, area.w,
                    C2D_AlignCenter);
    }
}

// --- Shapes ----------------------------------------------------------------------

void fillRound(const Rect &area, float radius, u32 fill, unsigned corners)
{
    const Stop stops[] = {{0, fill}, {1, fill}};
    fillShape(area, radius, corners, stops, 2);
}

void fillRound(const Rect &area, float radius,
               std::initializer_list<Stop> stops, unsigned corners)
{
    fillShape(area, radius, corners, stops.begin(), stops.size());
}

void strokeRound(const Rect &area, float radius, float width, u32 stroke,
                 unsigned corners, bool openBottom)
{
    radius = std::max(0.0f, std::min(radius, std::min(area.w, area.h) / 2));
    const float tl = corners & corner::TopLeft ? radius : 0;
    const float tr = corners & corner::TopRight ? radius : 0;
    const float bl = !openBottom && (corners & corner::BottomLeft) ? radius : 0;
    const float br = !openBottom && (corners & corner::BottomRight) ? radius : 0;

    rect(area.x + tl, area.y, area.w - tl - tr, width, stroke);
    rect(area.x, area.y + tl, width, area.h - tl - bl, stroke);
    rect(area.x + area.w - width, area.y + tr, width, area.h - tr - br, stroke);
    if (!openBottom) {
        rect(area.x + bl, area.y + area.h - width, area.w - bl - br, width,
             stroke);
    }
    const int ring = ringFor(width, radius);
    if (tl > 0) {
        cornerQuad(corner::TopLeft, ring, area.x, area.y, tl, stroke, stroke);
    }
    if (tr > 0) {
        cornerQuad(corner::TopRight, ring, area.x + area.w - tr, area.y, tr,
                   stroke, stroke);
    }
    if (bl > 0) {
        cornerQuad(corner::BottomLeft, ring, area.x, area.y + area.h - bl, bl,
                   stroke, stroke);
    }
    if (br > 0) {
        cornerQuad(corner::BottomRight, ring, area.x + area.w - br,
                   area.y + area.h - br, br, stroke, stroke);
    }
}

void drawShadow(const Rect &area, float radius, float spread, u32 shadow)
{
    const int layers = std::max(1, static_cast<int>(spread));
    for (int layer = layers; layer >= 1; --layer) {
        const float grow = static_cast<float>(layer);
        fillRound({area.x - grow * 0.5f, area.y - grow * 0.3f,
                   area.w + grow, area.h + grow},
                  radius + grow * 0.5f,
                  withAlpha(shadow, 1.0f / (layers + 1)));
    }
}

void drawDots(float y, float left, float right)
{
    for (float x = left; x <= right; x += 8) {
        atlasQuad(x - 1.4f, y - 1.4f, 2.8f, 2.8f, 0, ShapeRow, ShapeSize,
                  ShapeRow + ShapeSize, color::Dot, color::Dot, color::Dot,
                  color::Dot);
    }
}

// --- Icons and pictures ------------------------------------------------------------

void drawIcon(Icon icon, float x, float y, float size, u32 tint)
{
    if (!atlasTriedLoad() || icon == Icon::None) {
        return;
    }
    int pick = 0; // the smallest atlas size at least as big as asked
    for (int index = 0; index < IconSizeCount; ++index) {
        if (IconSizes[index] >= size - 0.5f) {
            pick = index;
        }
    }
    const IconCell cell = IconCells[pick][static_cast<int>(icon)];
    const float cellSize = static_cast<float>(IconSizes[pick]);
    Tex3DS_SubTexture &sub = atlasSub[atlasSubNext++ % 64];
    sub = {static_cast<u16>(cellSize), static_cast<u16>(cellSize),
           cell.x / static_cast<float>(IconAtlasSize),
           1.0f - cell.y / static_cast<float>(IconAtlasSize),
           (cell.x + cellSize) / IconAtlasSize,
           1.0f - (cell.y + cellSize) / IconAtlasSize};
    const C2D_Image image = {&atlasTexture, &sub};
    C2D_ImageTint shade;
    C2D_PlainImageTint(&shade, tint, 1.0f);
    C2D_DrawImageAt(image, x, y, Depth, &shade, size / cellSize,
                    size / cellSize);
    useVertexColor();
}

void drawImage(const C2D_Image &image, float x, float y)
{
    plainImage(image, x, y);
}

const C2D_Image *showIcon(const std::vector<uint16_t> &pixels)
{
    if (pixels.size() != 48 * 48) {
        return nullptr;
    }
    if (!iconTextureReady) {
        if (!makeTexture(iconTexture, IconTextureSize, IconTextureSize,
                         GPU_RGB565)) {
            return nullptr;
        }
        iconTextureReady = true;
    }
    if (pixels != iconPixels) {
        // Frames are drawn with C3D_FRAME_SYNCDRAW, so the GPU is done with
        // the previous icon by the time this runs between frames.
        image::tileImage(pixels.data(), 48, 48,
                         static_cast<uint16_t *>(iconTexture.data),
                         IconTextureSize);
        C3D_TexFlush(&iconTexture);
        iconPixels = pixels;
    }
    return &IconImage;
}

void drawTitleIcon(const std::vector<uint16_t> &pixels, float x, float y,
                   Icon fallback)
{
    drawShadow({x, y + 1.5f, 48, 48}, 8, 2, color::rgb(0x403810, 0x50));
    fillRound({x - 1, y - 1, 50, 50}, 9, color::rgb(0xB3AA83));
    if (const C2D_Image *image = showIcon(pixels)) {
        fillRound({x, y, 48, 48}, 8, color::rgb(0xFFFFFF));
        plainImage(*image, x, y);
        return;
    }
    fillRound({x, y, 48, 48}, 8,
              {{0, color::rgb(0x4A4F59)}, {1, color::rgb(0x1E2127)}});
    drawIcon(fallback, x + 9, y + 9, 30, color::rgb(0x8FD1FF));
}

const C2D_Image *showPicture(const std::vector<uint16_t> &pixels,
                             uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || width > PictureTextureWidth ||
        height > PictureTextureHeight || pixels.size() != width * height) {
        return nullptr;
    }
    if (!pictureTextureReady) {
        if (!makeTexture(pictureTexture, PictureTextureWidth,
                         PictureTextureHeight, GPU_RGB565)) {
            return nullptr;
        }
        pictureTextureReady = true;
    }
    std::fill_n(static_cast<u16 *>(pictureTexture.data),
                PictureTextureWidth * PictureTextureHeight, 0);
    image::tileImage(pixels.data(), width, height,
                     static_cast<uint16_t *>(pictureTexture.data),
                     PictureTextureWidth);
    C3D_TexFlush(&pictureTexture);
    pictureSubTexture = {static_cast<u16>(width), static_cast<u16>(height),
                         0.0f, 1.0f,
                         static_cast<float>(width) / PictureTextureWidth,
                         1.0f - static_cast<float>(height) /
                                    PictureTextureHeight};
    return &pictureImage;
}

// --- Components ------------------------------------------------------------------

void drawStatusBar()
{
    refreshStatus();
    // Wi-Fi strength, four rising bars.
    for (int bar = 0; bar < 4; ++bar) {
        const float height = 4 + bar * 3;
        rect(3 + bar * 5, 16 - height, 4, height,
             bar < status.bars + (status.connected ? 1 : 0)
                 ? color::rgb(0x28B6F2)
                 : color::rgb(0xC9C2A0));
    }
    const Rect pill = {27, 2, 106, 15};
    if (status.connected) {
        fillRound(pill, 5, color::rgb(0x31AAE0));
        fillRound(pill.inset(1), 4,
                  {{0, color::rgb(0xA8DEF7)}, {0.5f, color::rgb(0x63C4F1)},
                   {1, color::rgb(0x2DA8DE)}});
        drawCentered("Connected", pill, 0.32f, color::rgb(0xFFFFFF));
    } else {
        fillRound(pill, 5, color::rgb(0xC8C8C8));
        fillRound(pill.inset(1), 4,
                  {{0, color::rgb(0xEFEFEF)}, {1, color::rgb(0xD6D6D6)}});
        drawCentered("Not Connected", pill, 0.3f, color::rgb(0x8B8B8B));
    }

    const time_t seconds = static_cast<time_t>(platform::now());
    tm parts {};
    gmtime_r(&seconds, &parts);
    static const char *const Days[] = {"Sun", "Mon", "Tue", "Wed",
                                       "Thu", "Fri", "Sat"};
    char clock[40];
    std::snprintf(clock, sizeof(clock), "%d/%d (%s)  %d:%02d",
                  parts.tm_mon + 1, parts.tm_mday, Days[parts.tm_wday % 7],
                  parts.tm_hour, parts.tm_min);
    drawText(clock, 364, 1, 0.4f, color::rgb(0x323232), C2D_AlignRight);

    // The SD card's free space, on every screen. It sits by the connection
    // pill, softer than the clock, so the two never read as one line.
    if (status.sdFree >= 0) {
        // A touch smaller than the clock and centred on its line: on the
        // console's font "SD 76.8 GiB" at the clock's size is wider than
        // the gap. shortBytes keeps it to that width on any card.
        constexpr float Scale = 0.37f;
        const float left = pill.x + pill.w + 6;
        const float clockLeft = 364 - textWidth(clock, 0.4f);
        drawFitted("SD " +
                       units::shortBytes(static_cast<uint64_t>(status.sdFree)),
                   left, 1 + (lineHeight(0.4f) - lineHeight(Scale)) / 2, Scale,
                   status.sdFree < LowSdSpace ? color::Caution : color::InkSoft,
                   clockLeft - 8 - left);
    }

    // Battery, four cells.
    const Rect battery = {371, 3, 24, 13};
    fillRound(battery, 2.5f, color::rgb(0x222222));
    fillRound(battery.inset(1.5f), 1.5f, color::rgb(0xFFFFFF));
    rect(battery.x + battery.w, battery.y + 4, 2, 5, color::rgb(0x222222));
    const int cells = std::max(1, std::min(4, (status.battery * 4 + 4) / 5));
    for (int cell = 0; cell < cells; ++cell) {
        rect(battery.x + 3 + cell * 4.6f, battery.y + 3, 3.8f, 7,
             status.charging ? color::rgb(0xFFA656) : color::rgb(0x4489DD));
    }
}

void nameScreen(const std::string &name)
{
    if (!screenToLog.empty()) {
        logging::info("screen", {{"name", name}});
        screenToLog.clear();
    }
}

void drawTopChrome(const std::string &title, Icon icon, u32 iconColor,
                   const std::string &order)
{
    nameScreen(title);
    drawTopBackground();
    drawStatusBar();
    const float textW = textWidth(title, type::Title);
    const float iconW = icon == Icon::None ? 0 : 28;
    const float maxW = 360 - iconW;
    const float width = iconW + std::min(textW, maxW);
    float x = 200 - width / 2;
    if (icon != Icon::None) {
        drawIcon(icon, x, 26, 22, iconColor);
        x += iconW;
    }
    drawMarquee(title, x, 37 - lineHeight(type::Title) / 2 + 1, type::Title,
                color::Accent, maxW);
    if (order.empty()) {
        drawDots(57);
        return;
    }
    const float right = TopPanel.x + TopPanel.w - 1;
    drawText(order, right, 57 - lineHeight(type::Tiny) / 2 + 1, type::Tiny,
             color::InkSoft, C2D_AlignRight);
    drawDots(57, 18, right - textWidth(order, type::Tiny) - 8);
}

void drawPanel(const Rect &area)
{
    fillRound(area, 5, color::Panel);
}

void drawRecess(const Rect &area)
{
    fillRound(area, 8,
              {{0, color::rgb(0xC9C08A)}, {0.04f, color::Recess},
               {1, color::Recess}});
}

float drawPanelText(const std::string &value, const Rect &area, float scale,
                    u32 textColor, size_t maxLines)
{
    // Smaller, down to four fifths of scale, until every line fits the
    // panel; only then does the last line scroll.
    float use = scale;
    ScrollingLines wrapped;
    for (bool first = true;; first = false, use -= 0.02f) {
        const size_t room = std::max<size_t>(
            1, static_cast<size_t>((area.h - 8) / (lineHeight(use) * 1.05f)));
        wrapped = wrapScrolling(value, use, area.w - 32,
                                first ? std::min(maxLines, room) : room);
        if (!wrapped.lastScrolls || use - 0.02f < scale * 0.8f) {
            break;
        }
    }
    const float step = lineHeight(use) * 1.05f;
    float y = area.y + (area.h - step * wrapped.lines.size()) / 2;
    for (size_t index = 0; index < wrapped.lines.size(); ++index) {
        if (wrapped.lastScrolls && index + 1 == wrapped.lines.size()) {
            drawMarquee(wrapped.lines[index], area.x + area.w / 2, y, use,
                        textColor, area.w - 32, C2D_AlignCenter);
        } else {
            drawText(wrapped.lines[index], area.x + area.w / 2, y, use,
                     textColor, C2D_AlignCenter);
        }
        y += step;
    }
    return y;
}

void drawButton(const Rect &area, const std::string &label, Style style,
                float scale, Icon icon, u32 iconColor, bool pressed,
                u32 labelColor)
{
    const float radius = area.h >= 34 ? 9 : area.h >= 28 ? 8 : 7;
    drawButtonBody(area, radius, style, pressed);
    u32 textColor = labelColor != 0 ? labelColor : lookFor(style).label;
    if (style == Style::Disabled) {
        textColor = color::rgb(0xCFC48A);
    }
    Rect inside = area;
    if (pressed) {
        inside.y += 1;
    }
    if (icon == Icon::None) {
        drawLabel(label, inside, scale, textColor);
        return;
    }
    // Like a label alone, it gets a little smaller before it is squeezed or
    // scrolls, and keeps the same margin on both sides.
    const float iconSize = std::min(20.0f, area.h - 10);
    const float room = area.w - 18 - iconSize - 6;
    float use = scale;
    if (textWidth(label, use) > room) {
        use = std::max(scale * 0.85f,
                       room / std::max(1.0f, textWidth(label, 1.0f)));
    }
    const float width = iconSize + 6 + std::min(room, textWidth(label, use));
    const float x = area.x + std::max(9.0f, (area.w - width) / 2);
    drawIcon(icon, x, inside.y + (area.h - iconSize) / 2, iconSize, iconColor);
    drawCenteredMarquee(label,
                        {x + iconSize + 6, inside.y,
                         area.x + area.w - 9 - (x + iconSize + 6), area.h},
                        use, textColor, true);
}

void drawTile(const Rect &area, const std::string &label, Icon icon,
              u32 iconColor, bool pressed)
{
    drawButtonBody(area, 10, Style::Normal, pressed);
    const float lift = pressed ? 1 : 0;
    constexpr float TileScale = 0.5f;
    const std::vector<std::string> lines = wrap(label, TileScale, area.w - 14, 2);
    const float step = lineHeight(TileScale) * 0.88f;
    const float block = 30 + 3 + step * lines.size();
    float y = area.y + (area.h - block) / 2 + lift;
    drawIcon(icon, area.x + area.w / 2 - 15, y, 30, iconColor);
    y += 33;
    for (const std::string &line : lines) {
        drawText(line, area.x + area.w / 2, y, TileScale, color::InkButton,
                 C2D_AlignCenter);
        y += step;
    }
}

void drawFocus(const Rect &area, float radius, float gap, float width)
{
    const float glow = 0.55f + 0.45f * pulse(1200);
    const Rect ring = {area.x - gap, area.y - gap, area.w + 2 * gap,
                       area.h + 2 * gap};
    strokeRound({ring.x - 1, ring.y - 1, ring.w + 2, ring.h + 2}, radius + gap + 1,
                1.5f, withAlpha(color::FocusGlow, glow * 0.8f));
    strokeRound(ring, radius + gap, width, color::Focus);
    strokeRound(ring.inset(width), std::max(0.0f, radius + gap - width), 1,
                withAlpha(color::FocusGlow, glow * 0.6f));
}

namespace {

constexpr float TabWidth = 126;
constexpr float WideTabWidth = 150;
constexpr float KeyScale = 0.36f;
constexpr float GlyphScale = 0.56f;

// The system font's own A, B, X or Y button (U+E000 to U+E003) as UTF-8,
// or "" for other keys and for fonts without them, such as Azahar's.
std::string buttonGlyph(const std::string &key)
{
    static const char Keys[] = "ABXY";
    if (key.size() != 1) {
        return "";
    }
    const char *at = std::strchr(Keys, key[0]);
    if (at == nullptr || key[0] == '\0') {
        return "";
    }
    const uint32_t codePoint = 0xE000 + static_cast<uint32_t>(at - Keys);
    if (C2D_FontGlyphIndexFromCodePoint(nullptr, codePoint) ==
        fontGetInfo(fontGetSystemFont())->alterCharIndex) {
        return "";
    }
    return {static_cast<char>(0xE0 | (codePoint >> 12)),
            static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)),
            static_cast<char>(0x80 | (codePoint & 0x3f))};
}

// The key cap's width and the gap after it.
float keyCapWidth(const std::string &key)
{
    if (key.empty()) {
        return 0;
    }
    const std::string glyph = buttonGlyph(key);
    if (!glyph.empty()) {
        return textWidth(glyph, GlyphScale) + 4;
    }
    // A single letter gets a 16 px circle: its ink is an even number of
    // pixels each way, so it centres exactly.
    const bool round = key.size() == 1;
    return std::max(round ? 16.0f : 15.0f,
                    textWidth(key, KeyScale) + (round ? 7 : 11)) +
           5;
}

} // namespace

Rect tabRect(Tab tab)
{
    switch (tab) {
    case Tab::Left:
        return {0, 209, TabWidth, 31};
    case Tab::Right:
        return {320 - TabWidth, 209, TabWidth, 31};
    case Tab::Full:
        break;
    }
    return {0, 209, 320, 31};
}

Rect tabRect(Tab tab, const std::string &label, const std::string &key)
{
    Rect area = tabRect(tab);
    if (tab == Tab::Right) {
        // An action too long for the tab even at drawTab's smallest size
        // grows it towards the middle, so the two tabs stay alike otherwise.
        const float wanted = std::min(
            WideTabWidth, std::max(TabWidth, textWidth(label, 0.47f) +
                                                 keyCapWidth(key) + 22));
        area.x -= wanted - area.w;
        area.w = wanted;
    }
    return area;
}

namespace {

// Draws key's cap at x on line (glyph or a cap of capColor with the letter
// in letterColor) and returns the x after it.
float drawKeyCap(const std::string &key, float x, const Rect &line,
                 u32 capColor, u32 letterColor)
{
    const std::string glyph = buttonGlyph(key);
    const bool round = key.size() == 1;
    const float capSize = round ? 16 : 15;
    const float keyWidth = keyCapWidth(key);
    if (!glyph.empty()) {
        drawText(glyph, x,
                 std::round(line.y + (line.h - lineHeight(GlyphScale)) / 2 + 1),
                 GlyphScale, capColor);
        return x + keyWidth;
    }
    if (key.empty()) {
        return x;
    }
    const float capWidth = keyWidth - 5;
    // Centred on the same line as the label beside it.
    const Rect cap = {x, line.y + (line.h - 15) / 2, capWidth, capSize};
    fillRound(cap, round ? capSize / 2 : 4, capColor);
    // Centred on the ink, as side bearings differ by letter. Raised half a
    // pixel, and rounded to whole pixels so it stays sharp. Placed as in a
    // 15 px cap; the 16 px circle adds its row below.
    drawText(key,
             std::round(cap.x + (cap.w - textWidth(key, KeyScale)) / 2 +
                        inkOffset(key, KeyScale)),
             std::round(cap.y - 0.5f + (15 - lineHeight(KeyScale)) / 2 + 1),
             KeyScale, letterColor);
    return x + keyWidth;
}

} // namespace

void drawTab(Tab tab, const std::string &label, const std::string &key,
             bool pressed)
{
    const Rect area = tabRect(tab, label, key);
    const unsigned corners = tab == Tab::Left    ? corner::TopRight
                           : tab == Tab::Right ? corner::TopLeft
                                               : corner::Top;
    // fillRound keeps a corner to half the height.
    const float radius = std::min(22.0f, area.h / 2);
    fillRound({area.x - (tab == Tab::Left ? 0 : 2), area.y - 2,
               area.w + (tab == Tab::Full ? 4 : 2), area.h + 2},
              24, color::rgb(0x000000, 0x30), corners);
    fillRound(area, radius, color::rgb(0xC4C4C4), corners);
    Stop body[] = {{0, color::rgb(0x8A8A8A)}, {0.12f, color::rgb(0x7C7C7C)},
                   {0.6f, color::rgb(0x707070)}, {1, color::rgb(0x666666)}};
    if (pressed) {
        for (Stop &stop : body) {
            stop.color = darker(stop.color, 0.2f);
        }
    }
    fillShape({area.x, area.y + 2, area.w, area.h - 2}, 21, corners, body, 4);

    // Label, with the button that also does this as a white key cap.
    const float keyWidth = keyCapWidth(key);
    // Shrink a long label so it and its key cap stay on the flat part of
    // the tab, clear of the rounded corner; past that it is squeezed, so
    // it never runs off the tab.
    const float room = area.w - 22 - keyWidth;
    float labelScale = type::Tab;
    while (labelScale > 0.45f && textWidth(label, labelScale) > room) {
        labelScale -= 0.02f;
    }
    const float squeeze =
        std::min(1.0f, room / std::max(1.0f, textWidth(label, labelScale)));
    const float labelWidth = textWidth(label, labelScale) * squeeze;
    // Centred on the tab, a little away from its rounded inner corner, and
    // on the middle of the grey body below the highlight.
    const float inset = tab == Tab::Full ? 0 : 6;
    const float centre = tab == Tab::Right ? area.x + inset + (area.w - inset) / 2
                                           : area.x + (area.w - inset) / 2;
    // Whole pixels, so the key cap and its letter line up exactly.
    float x = std::round(centre - (labelWidth + keyWidth) / 2);
    const Rect line = {x, area.y + 3.5f, labelWidth + keyWidth, area.h - 3};
    x = drawKeyCap(key, x, line, color::rgb(0xFFFFFF), color::rgb(0x6C6C6C));
    drawSqueezed(label, x, line.y + (line.h - lineHeight(labelScale)) / 2 + 1,
                 labelScale, squeeze, color::rgb(0xFFFFFF), 0);
}

void drawShoulderKeys(const ShoulderKey &left, const ShoulderKey &right)
{
    // Built as the nav tabs are, small enough for the strip under the panel.
    const float top = TopPanel.y + TopPanel.h;
    const float height = ScreenHeight - top;
    const float radius = height / 2;
    constexpr float Cap = 11;
    constexpr float IconSize = 11;
    // The console's system font stands taller than Azahar's at one scale,
    // so the words and the cap's letter are sized to fit, not fixed.
    const auto fit = [](float scale, float room) {
        return scale * std::min(1.0f, room / lineHeight(scale));
    };
    const float labelScale = fit(type::Tiny, height - 1);
    const float letterScale = fit(0.3f, Cap - 1);
    constexpr float Gap = 4;
    constexpr float Outer = 5; // from the screen's edge to the cap
    constexpr float Inner = 9; // clear of the rounded corner
    const float middle = top + 1 + (height - 1) / 2;
    const auto drawCap = [&](const char *letter, float x) {
        fillRound({x, middle - Cap / 2, Cap, Cap}, Cap / 2, color::rgb(0xFFFFFF));
        drawText(letter,
                 std::round(x + (Cap - textWidth(letter, letterScale)) / 2 +
                            inkOffset(letter, letterScale)),
                 std::round(middle - lineHeight(letterScale) / 2 + 1),
                 letterScale, color::rgb(0x6C6C6C));
        return x + Cap + Gap;
    };
    const auto drawIconPart = [&](const ShoulderKey &key, float x) {
        if (key.icon == Icon::None) {
            return x;
        }
        drawIcon(key.icon, x, middle - IconSize / 2, IconSize, key.iconColor);
        return x + IconSize + Gap;
    };
    const auto drawLabel = [&](const ShoulderKey &key, float x) {
        if (key.label.empty()) {
            return x;
        }
        drawText(key.label, x, std::round(middle - lineHeight(labelScale) / 2 + 1),
                 labelScale, color::rgb(0xFFFFFF));
        return x + textWidth(key.label, labelScale) + Gap;
    };
    const auto draw = [&](const ShoulderKey &key, const char *letter, bool onLeft) {
        if (key.label.empty() && key.icon == Icon::None) {
            return;
        }
        float content = Cap;
        if (key.icon != Icon::None) {
            content += Gap + IconSize;
        }
        if (!key.label.empty()) {
            content += Gap + textWidth(key.label, labelScale);
        }
        const float width = std::round(Outer + content + Inner);
        const Rect area = {onLeft ? 0 : TopWidth - width, top, width, height};
        const unsigned corners = onLeft ? corner::TopRight : corner::TopLeft;
        fillRound({area.x - (onLeft ? 0 : 1), area.y - 1, area.w + 1, area.h + 1},
                  radius + 1, color::rgb(0x000000, 0x30), corners);
        fillRound(area, radius, color::rgb(0xC4C4C4), corners);
        const Stop body[] = {{0, color::rgb(0x8A8A8A)},
                             {0.12f, color::rgb(0x7C7C7C)},
                             {0.6f, color::rgb(0x707070)},
                             {1, color::rgb(0x666666)}};
        fillShape({area.x, area.y + 1, area.w, area.h - 1}, radius, corners, body, 4);
        // The cap is outermost, nearest the button.
        if (onLeft) {
            drawLabel(key, drawIconPart(key, drawCap(letter, area.x + Outer)));
        } else {
            drawCap(letter, drawIconPart(key, drawLabel(key, area.x + Inner)));
        }
    };
    draw(left, "L", true);
    draw(right, "R", false);
}

float drawTag(float x, float y, const std::string &label, u32 fill)
{
    const float width = textWidth(label, 0.4f) + 16;
    fillRound({x, y, width, 20}, 5, fill);
    drawCentered(label, {x, y, width, 20}, 0.4f, color::rgb(0xFFFFFF));
    return width;
}

float drawBadge(float right, float y, const std::string &label, u32 fill)
{
    const float width = std::max(34.0f, textWidth(label, 0.34f) + 10);
    fillRound({right - width, y, width, 16}, 4, fill);
    drawCentered(label, {right - width, y, width, 16}, 0.34f,
                 color::rgb(0xFFFFFF));
    return width;
}

float drawTagRow(float x, float y, float tagWidth, const std::string &tag,
                 const std::string &value, u32 fill)
{
    fillRound({x, y, tagWidth, 20}, 5, fill);
    drawCentered(fitEnd(tag, 0.4f, tagWidth - 6), {x, y, tagWidth, 20}, 0.4f,
                 color::rgb(0xFFFFFF));
    drawCenteredMarquee(value, {x + tagWidth + 10, y, 366 - tagWidth - 40, 20},
                        type::Small, color::Ink, true);
    return y + 26;
}

float drawFacts(const std::vector<Fact> &facts, const Rect &area,
                float minTagWidth)
{
    constexpr float Row = 20;
    constexpr float Pitch = 26;
    float tagWidth = minTagWidth;
    for (const Fact &fact : facts) {
        tagWidth = std::max(tagWidth, textWidth(fact.tag, 0.4f) + 16);
    }
    tagWidth = std::min(tagWidth, 150.0f);
    const float left = area.x + 18;
    const float valueWidth = area.w - 36 - tagWidth - 10;
    float y = area.y + (area.h - (Pitch * facts.size() - (Pitch - Row))) / 2;
    for (const Fact &fact : facts) {
        fillRound({left, y, tagWidth, Row}, 5, fact.fill);
        drawCentered(fitEnd(fact.tag, 0.4f, tagWidth - 6), {left, y, tagWidth, Row},
                     0.4f, color::rgb(0xFFFFFF));
        drawCenteredMarquee(fact.value, {left + tagWidth + 10, y, valueWidth, Row},
                            type::Small, color::Ink, true);
        y += Pitch;
    }
    return y;
}

void drawEmblem(Icon icon, u32 iconColor, const Rect &area)
{
    constexpr float Size = 64;
    drawIcon(icon, area.x + (area.w - Size) / 2, area.y + (area.h - Size) / 2,
             Size, iconColor);
}

void drawProgressBar(const Rect &area, float fraction)
{
    const float radius = area.h / 2;
    fillRound(area, radius,
              {{0, color::rgb(0xB5AC77)}, {0.3f, color::rgb(0xCFC795)},
               {1, color::rgb(0xD6CF9F)}});
    const auto fill = [&](float x, float width) {
        if (width < 1) {
            return;
        }
        fillRound({x, area.y, width, area.h}, std::min(radius, width / 2),
                  {{0, color::rgb(0xFFE27A)}, {0.4f, color::rgb(0xFFCC22)},
                   {1, color::rgb(0xFF9900)}});
        rect(x + std::min(radius, width / 2), area.y + 1.5f,
             std::max(0.0f, width - 2 * std::min(radius, width / 2)), 1.5f,
             color::rgb(0xFFFFFF, 0x80));
    };
    if (fraction < 0.0f) {
        const float width = area.w * 0.28f;
        const float phase = static_cast<float>(now() % 1400) / 1400.0f;
        const float x = area.x + (area.w + width) * phase - width;
        const float left = std::max(area.x, x);
        const float right = std::min(area.x + area.w, x + width);
        fill(left, right - left);
        return;
    }
    fill(area.x, area.w * std::min(1.0f, fraction));
}

void drawSpinner(float centerX, float centerY, float radius)
{
    const int lead = static_cast<int>(now() / 100) % 8;
    for (int index = 0; index < 8; ++index) {
        const float angle = index * Pi / 4 - Pi / 2;
        const int age = (lead - index + 8) % 8;
        const u32 fill = withAlpha(color::Ink, 1.0f - age * 0.11f);
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        const float inner = radius * 0.42f;
        const float half = radius * 0.13f;
        // A bar from inner to radius, half wide on each side.
        quad(centerX + c * inner - s * half, centerY + s * inner + c * half,
             centerX + c * radius - s * half, centerY + s * radius + c * half,
             centerX + c * radius + s * half, centerY + s * radius - c * half,
             centerX + c * inner + s * half, centerY + s * inner - c * half,
             fill);
    }
}

// --- Dialogs ---------------------------------------------------------------------

void captureBackdrop()
{
    // A dialog opened mid-fade would otherwise fade in too, over a backdrop
    // caught half faded.
    fading = false;
    captureScreen(0, GFX_TOP, 400);
    captureScreen(1, GFX_BOTTOM, 320);
}

// --- Notifications -------------------------------------------------------------

namespace {

constexpr uint64_t NoticeIn = 320;
constexpr uint64_t NoticeHold = 6000;
constexpr uint64_t NoticeOut = 260;

// Overshoots a little and settles, like the HOME Menu's banners.
float easeOutBack(float t)
{
    const float c = 1.4f;
    const float u = t - 1;
    return 1 + (c + 1) * u * u * u + c * u * u;
}

void drawNotice()
{
    if (!noticeActive) {
        return;
    }
    const uint64_t age = now() - noticeStart;
    if (age >= NoticeIn + NoticeHold + NoticeOut) {
        noticeActive = false;
        return;
    }
    // A dialog dims a copy of the screens, so the banner waits under it.
    if (inDialog) {
        return;
    }
    float shown = 1;
    if (age < NoticeIn) {
        shown = easeOutBack(static_cast<float>(age) / NoticeIn);
    } else if (age > NoticeIn + NoticeHold) {
        const float t = static_cast<float>(age - NoticeIn - NoticeHold) / NoticeOut;
        shown = 1 - t * t;
    }
    constexpr float Width = 336;
    constexpr float Height = 50;
    constexpr float Rest = 22; // just under the status bar
    const float x = (TopWidth - Width) / 2;
    const float y = -Height - 8 + (Rest + Height + 8) * shown;

    C2D_SceneBegin(topTarget);
    onBottom = false;
    const Rect card = {x, y, Width, Height};
    drawShadow({x, y + 2, Width, Height}, 12, 4, color::rgb(0x403810, 0x48));
    fillRound(card, 12, color::rgb(0xC8BF96));
    fillRound(card.inset(1), 11,
              {{0, color::rgb(0xFFFFFF)}, {0.6f, color::rgb(0xFAF8EF)},
               {1, color::rgb(0xEFEBDA)}});

    const Rect badge = {x + 8, y + 7, 36, 36};
    fillRound(badge, 9, notice.iconColor);
    fillRound({badge.x, badge.y, badge.w, badge.h / 2}, 9,
              color::rgb(0xFFFFFF, 0x38), corner::Top);
    drawIcon(notice.icon, badge.x + 6, badge.y + 6, 24, color::rgb(0xFFFFFF));

    const float textX = badge.x + badge.w + 10;
    const float textWidthMax = x + Width - 12 - textX;
    drawMarquee(notice.title, textX, y + 6, type::Small, color::InkStrong,
                textWidthMax);
    drawMarquee(notice.text, textX, y + 27, type::Tiny, color::Ink,
                textWidthMax);

    // How long it stays, as a line that runs out.
    if (age >= NoticeIn && age <= NoticeIn + NoticeHold) {
        const float left = 1 - static_cast<float>(age - NoticeIn) / NoticeHold;
        rect(textX, y + Height - 5, textWidthMax * left, 2,
             withAlpha(notice.iconColor, 0.55f));
    }
}

} // namespace

void notify(const Notice &value)
{
    // One that is down stays down, so notices in quick succession do not
    // bounce; its line starts again.
    const uint64_t age = now() - noticeStart;
    const bool down = noticeActive && !inDialog && age >= NoticeIn &&
                      age <= NoticeIn + NoticeHold;
    notice = value;
    noticeActive = true;
    noticeStart = down ? now() - NoticeIn : now();
    sound::play(sound::Effect::Notify);
}

void setFrameHook(std::function<void()> hook)
{
    frameHook = std::move(hook);
}

namespace {

Icon toneIcon(Tone tone)
{
    switch (tone) {
    case Tone::Error:
    case Tone::Warning:
        return Icon::Warning;
    case Tone::Success:
        return Icon::Check;
    case Tone::Info:
        break;
    }
    return Icon::AppMark;
}

u32 toneColor(Tone tone)
{
    switch (tone) {
    case Tone::Error:
        return color::Bad;
    case Tone::Warning:
        return color::IconOrange;
    case Tone::Success:
        return color::Good;
    case Tone::Info:
        break;
    }
    return color::Accent;
}

u32 paragraphColor(u32 requested)
{
    // The older dark theme's colours, mapped onto the khaki panel.
    if (requested == color::Text) {
        return color::Ink;
    }
    return requested;
}

void drawDialogTop(const Dialog &dialog)
{
    if (dialog.drawTop) {
        dialog.drawTop();
        return;
    }
    if (dialog.body.empty() && dialog.icon == nullptr) {
        drawBackdrop(false);
        return;
    }
    drawTopChrome(titleCase(dialog.header), toneIcon(dialog.tone),
                  toneColor(dialog.tone));
    drawPanel();
    float y = TopPanel.y + 12;
    float left = TopPanel.x + 18;
    float width = TopPanel.w - 36;
    if (dialog.icon != nullptr) {
        drawShadow({left, y + 1.5f, 48, 48}, 8, 2, color::rgb(0x403810, 0x50));
        plainImage(*dialog.icon, left, y);
        left += 60;
        width -= 60;
    }
    // Paragraphs top-down, the first in body size, the rest smaller, all
    // a little smaller still when they would not fit in the panel.
    const float bottom = TopPanel.y + TopPanel.h - 8;
    float shrink = 1;
    for (; shrink > 0.8f; shrink -= 0.05f) {
        float height = 0;
        bool first = true;
        for (const Paragraph &paragraph : dialog.body) {
            if (paragraph.text.empty()) {
                continue;
            }
            const float scale = (first ? type::Body : type::Small) * shrink;
            height += wrap(paragraph.text, scale, width, paragraph.maxLines + 1)
                              .size() *
                          lineHeight(scale) +
                      5;
            first = false;
        }
        if (y + height <= bottom) {
            break;
        }
    }
    bool first = true;
    for (const Paragraph &paragraph : dialog.body) {
        if (paragraph.text.empty()) {
            continue;
        }
        const float scale = (first ? type::Body : type::Small) * shrink;
        const size_t room = static_cast<size_t>(
            std::max(0.0f, (bottom - y) / lineHeight(scale)));
        const size_t lines = std::min(paragraph.maxLines + 1, room);
        if (lines == 0) {
            break;
        }
        y = drawWrapped(paragraph.text, left, y, scale,
                        paragraphColor(paragraph.textColor), width, lines) +
            5;
        if (y > TopPanel.y + 62) {
            left = TopPanel.x + 18;
            width = TopPanel.w - 36;
        }
        first = false;
    }
}

} // namespace

int choose(const Dialog &dialog)
{
    if (!lastFrameWasDialog) {
        captureBackdrop();
    }
    inDialog = true;
    lockTouch();
    const std::vector<std::string> labels =
        dialog.buttons.empty() ? std::vector<std::string>{"OK"}
                               : dialog.buttons;
    const int count = static_cast<int>(labels.size());
    // A dangerous choice between two gets the system's red button, with
    // the other choice on a dark strip under it.
    const bool redLayout = dialog.dangerous >= 0 && count == 2;
    const int safe = redLayout ? 1 - dialog.dangerous : -1;
    const Rect redButton = {DialogBox.x + 18, DialogBox.y + DialogBox.h -
                                                  DangerStripHeight - 48,
                            DialogBox.w - 36, 36};
    int selection = std::max(0, std::min(dialog.initial, count - 1));
    int pressed = -1;
    int result = -1;
    if (dialog.tone == Tone::Error) {
        sound::play(sound::Effect::Error);
        logging::error("error",
                       {{"header", dialog.header},
                        {"title", dialog.title},
                        {"text", dialog.body.empty() ? "" : dialog.body[0].text}});
    }

    const auto hit = [&](const touchPosition &touch) {
        if (redLayout) {
            if (redButton.contains(touch)) {
                return dialog.dangerous;
            }
            const Rect strip = {DialogBox.x, DialogBox.y + DialogBox.h -
                                                 DangerStripHeight,
                                DialogBox.w, DangerStripHeight};
            return strip.contains(touch) ? safe : -1;
        }
        const std::vector<Rect> rects =
            stripButtons(DialogBox, labels.size(), StripHeight);
        for (int index = 0; index < count; ++index) {
            if (rects[index].contains(touch)) {
                return index;
            }
        }
        return -1;
    };

    while (keepRunning()) {
        const Input input = readInput();
        const int before = selection;
        if (redLayout) {
            if (input.repeat & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
                selection = selection == dialog.dangerous ? safe
                                                          : dialog.dangerous;
            }
        } else {
            if (input.repeat & KEY_LEFT) {
                selection = (selection + count - 1) % count;
            }
            if (input.repeat & KEY_RIGHT) {
                selection = (selection + 1) % count;
            }
        }
        if (selection != before) {
            sound::play(sound::Effect::Move);
        }
        if (input.down & KEY_B) {
            result = dialog.cancelButton;
            break;
        }
        if (input.down & KEY_A) {
            result = selection;
            break;
        }
        if (input.down & KEY_TOUCH) {
            pressed = hit(input.touch);
        }
        if ((input.up & KEY_TOUCH) && pressed >= 0) {
            if (hit(input.touch) == pressed) {
                result = pressed;
                break;
            }
            pressed = -1;
        }
        const int showPressed =
            (input.held & KEY_TOUCH) && hit(input.touch) == pressed ? pressed
                                                                     : -1;

        beginFrame();
        drawDialogTop(dialog);
        beginBottom();
        drawBackdrop(true);
        rect(0, 0, BottomWidth, ScreenHeight, color::rgb(0x141208, 0x7A));
        drawDialogBox(DialogBox);

        const float stripTop =
            DialogBox.y + DialogBox.h - (redLayout ? DangerStripHeight + 56
                                                   : StripHeight);
        const Rect message = {DialogBox.x + 16, DialogBox.y + 8,
                              DialogBox.w - 32, stripTop - DialogBox.y - 12};
        const std::vector<std::string> lines =
            wrap(dialog.title, type::Body, message.w, 6);
        const float step = lineHeight(type::Body) * 1.08f;
        float y = message.y + (message.h - step * lines.size()) / 2;
        for (const std::string &line : lines) {
            drawText(line, message.x + message.w / 2, y, type::Body,
                     color::InkDialog, C2D_AlignCenter);
            y += step;
        }

        if (redLayout) {
            drawButton(redButton, labels[dialog.dangerous], Style::Danger,
                       type::Button, Icon::None, 0,
                       showPressed == dialog.dangerous);
            if (selection == dialog.dangerous) {
                drawFocus(redButton, 9);
            }
            drawStrip(DialogBox, {labels[safe]}, selection == safe ? 0 : -1,
                      showPressed == safe ? 0 : -1, true, DangerStripHeight,
                      -1);
        } else {
            drawStrip(DialogBox, labels, selection, showPressed, false,
                      StripHeight, dialog.dangerous);
        }
        endFrame();
    }
    inDialog = false;
    if (keepRunning()) {
        // Cancel, or the only button of a message, sounds like going back.
        sound::play(result == dialog.cancelButton && count > 1
                        ? sound::Effect::Back
                        : sound::Effect::Select);
    }
    return keepRunning() ? result : -1;
}

void message(const std::string &title, const std::string &text, Tone tone,
             const std::string &detail)
{
    Dialog dialog;
    dialog.header = title;
    dialog.tone = tone;
    // Short news goes in the box; longer explanations go on the top screen
    // with the title in the box.
    if (text.size() <= 110) {
        dialog.title = text.empty() ? title : text;
        if (!detail.empty()) {
            dialog.body.push_back({detail, color::InkSoft, 0.5f, 5});
        }
    } else {
        dialog.title = title;
        dialog.body.push_back({text, color::Ink, 0.5f, 6});
        if (!detail.empty()) {
            dialog.body.push_back({detail, color::InkSoft, 0.5f, 3});
        }
    }
    dialog.buttons = {"OK"};
    choose(dialog);
}

// --- Screens of buttons ------------------------------------------------------------

namespace {

// The focusable things on a screen: controls, then the left and right tab.
struct Target {
    Rect rect;
    int choice;
    bool tab;
};

int nearest(const std::vector<Target> &targets, int from, u32 key)
{
    if (from < 0 || from >= static_cast<int>(targets.size())) {
        return targets.empty() ? -1 : 0;
    }
    const Rect &a = targets[from].rect;
    const float ax = a.x + a.w / 2;
    const float ay = a.y + a.h / 2;
    const bool across = key == KEY_LEFT || key == KEY_RIGHT;
    // Targets in line with this one (overlapping it across the direction
    // of travel) come first, so Right from Back reaches the tab beside it
    // rather than a tile that is closer but higher up.
    const auto inLine = [&](const Rect &b) {
        return across ? b.y < a.y + a.h && b.y + b.h > a.y
                      : b.x < a.x + a.w && b.x + b.w > a.x;
    };
    int best = -1;
    bool bestInLine = false;
    float bestScore = 1e9f;
    for (int index = 0; index < static_cast<int>(targets.size()); ++index) {
        if (index == from) {
            continue;
        }
        const Rect &b = targets[index].rect;
        const float dx = b.x + b.w / 2 - ax;
        const float dy = b.y + b.h / 2 - ay;
        float primary = 0;
        float secondary = 0;
        if (key == KEY_RIGHT) {
            primary = dx;
            secondary = std::fabs(dy);
        } else if (key == KEY_LEFT) {
            primary = -dx;
            secondary = std::fabs(dy);
        } else if (key == KEY_DOWN) {
            primary = dy;
            secondary = std::fabs(dx);
        } else {
            primary = -dy;
            secondary = std::fabs(dx);
        }
        if (primary <= 4) {
            continue;
        }
        const float score = primary + secondary * 2;
        const bool lined = inLine(b);
        if ((lined && !bestInLine) ||
            (lined == bestInLine && score < bestScore)) {
            bestScore = score;
            bestInLine = lined;
            best = index;
        }
    }
    return best < 0 ? from : best;
}

} // namespace

ScreenResult runScreen(const ScreenSpec &spec, int &focus)
{
    // Known by its layout, not its labels, which can change in place
    // (e.g. "Check at Startup: On").
    std::string id = "screen|" + spec.back + "|" + spec.action;
    for (const Control &control : spec.controls) {
        id += "|" + std::to_string(static_cast<int>(control.rect.x)) + "," +
              std::to_string(static_cast<int>(control.rect.y)) + "," +
              std::to_string(static_cast<int>(control.icon));
    }
    enterScreen(id);
    // Controls first, then the tabs. The ring and the D-pad only reach the
    // controls: tabs answer to their keys and to touch, as the system's
    // bottom bar does, since a ring on a shape that runs off the screen
    // reads as a stray line.
    std::vector<Target> targets;
    std::vector<Target> controls;
    int controlCount = 0;
    int current = -1;
    // Built again when spec.update enables or disables a control; the ring
    // stays on the same control when it still can.
    const auto build = [&](int keep) {
        targets.clear();
        for (size_t index = 0; index < spec.controls.size(); ++index) {
            if (spec.controls[index].enabled) {
                targets.push_back({spec.controls[index].rect,
                                   static_cast<int>(index), false});
            }
        }
        if (spec.closeBar) {
            targets.push_back({tabRect(Tab::Full), ScreenBack, true});
        } else {
            if (!spec.back.empty()) {
                targets.push_back({tabRect(Tab::Left), ScreenBack, true});
            }
            if (!spec.action.empty()) {
                targets.push_back(
                    {tabRect(Tab::Right, spec.action, spec.actionKey),
                     ScreenAction, true});
            }
        }
        controls.clear();
        for (const Target &target : targets) {
            if (!target.tab) {
                controls.push_back(target);
            }
        }
        controlCount = static_cast<int>(controls.size());
        current = controlCount > 0 ? 0 : -1;
        for (int index = 0; index < controlCount; ++index) {
            if (controls[index].choice == keep) {
                current = index;
            }
        }
    };
    build(focus);
    int pressed = -1;
    ScreenResult result;

    const auto targetAt = [&](const touchPosition &touch) {
        for (size_t index = 0; index < targets.size(); ++index) {
            if (targets[index].rect.contains(touch)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    };

    while (keepRunning()) {
        if (spec.update && spec.update()) {
            build(current >= 0 ? controls[current].choice : focus);
            pressed = -1;
        }
        const Input input = readInput();
        // Behind a Close bar there is nothing to go back to, so B does not
        // quit; only START does.
        if ((input.down & KEY_B) && !spec.closeBar) {
            result.choice = ScreenBack;
            break;
        }
        if (spec.closeBar && (input.down & KEY_START)) {
            result.choice = ScreenBack;
            break;
        }
        if (!spec.action.empty() && (input.down & spec.actionKeys)) {
            result.choice = ScreenAction;
            break;
        }
        if (input.down & spec.extraKeys) {
            result.choice = ScreenKey;
            result.key = input.down & spec.extraKeys;
            break;
        }
        const int before = current;
        for (const u32 key : {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT}) {
            if ((input.repeat & key) && current >= 0) {
                current = nearest(controls, current, key);
            }
        }
        if (current != before) {
            sound::play(sound::Effect::Move);
        }
        if ((input.down & KEY_A) && current >= 0) {
            result.choice = controls[current].choice;
            break;
        }
        if (input.down & KEY_TOUCH) {
            pressed = targetAt(input.touch);
            // Controls come first in targets, at the same index.
            if (pressed >= 0 && pressed < controlCount) {
                current = pressed;
            }
        }
        if ((input.up & KEY_TOUCH) && pressed >= 0) {
            if (targetAt(input.touch) == pressed) {
                result.choice = targets[pressed].choice;
                break;
            }
            pressed = -1;
        }
        const int showPressed =
            (input.held & KEY_TOUCH) && pressed >= 0 &&
                    targets[pressed].rect.contains(input.touch)
                ? pressed
                : -1;
        // drawTop may describe the control under the ring, so keep focus
        // current while the screen is open.
        if (current >= 0) {
            focus = controls[current].choice;
        }

        beginFrame();
        if (spec.drawTop) {
            spec.drawTop();
        }
        beginBottom();
        if (spec.drawBottom) {
            spec.drawBottom();
        }
        for (size_t index = 0; index < targets.size(); ++index) {
            const Target &target = targets[index];
            if (target.tab) {
                continue;
            }
            const Control &control = spec.controls[target.choice];
            const bool down = showPressed == static_cast<int>(index);
            if (control.tile) {
                drawTile(control.rect, control.label, control.icon,
                         control.iconColor, down);
            } else {
                drawButton(control.rect, control.label, control.style,
                           control.scale, control.icon, control.iconColor,
                           down, control.labelColor);
            }
        }
        for (size_t index = 0; index < spec.controls.size(); ++index) {
            const Control &control = spec.controls[index];
            if (!control.enabled) {
                drawButton(control.rect, control.label, Style::Disabled,
                           control.scale);
            }
        }
        const auto tabDown = [&](int choice) {
            return showPressed >= 0 && targets[showPressed].choice == choice;
        };
        if (spec.closeBar) {
            drawTab(Tab::Full, "Close", "START", tabDown(ScreenBack));
        } else {
            if (!spec.back.empty()) {
                drawTab(Tab::Left, spec.back, "B", tabDown(ScreenBack));
            }
            if (!spec.action.empty()) {
                drawTab(Tab::Right, spec.action, spec.actionKey,
                        tabDown(ScreenAction));
            }
        }
        if (current >= 0) {
            const Control &control = spec.controls[controls[current].choice];
            drawFocus(control.rect, control.tile ? 10
                                    : control.rect.h >= 34 ? 9
                                                           : 8);
        }
        // Badges sit on the top right corner, as the HOME Menu marks new
        // icons, over the focus ring so it never hides them.
        for (const Control &control : spec.controls) {
            if (control.badge.empty()) {
                continue;
            }
            const Rect &area = control.rect;
            const float width =
                std::max(16.0f, textWidth(control.badge, 0.34f) + 10);
            const float right = std::min(area.x + area.w + 6, BottomWidth - 2);
            const float top = std::max(1.5f, area.y - 9);
            fillRound({right - width - 1.5f, top - 1.5f, width + 3, 19}, 5.5f,
                      color::rgb(0xFFFFFF));
            const Rect badge = {right - width, top, width, 16};
            fillRound(badge, 4, control.badgeColor);
            drawCentered(control.badge, badge, 0.34f, color::rgb(0xFFFFFF));
        }
        endFrame();
    }
    if (current >= 0) {
        focus = controls[current].choice;
    }
    if (result.choice == ScreenBack) {
        sound::play(sound::Effect::Back);
    } else if (result.choice != ScreenClosed) {
        sound::play(sound::Effect::Select);
    }
    return result;
}

int menu(const std::vector<MenuItem> &items,
         const std::function<void(int)> &drawTop, const std::string &back)
{
    ScreenSpec spec;
    spec.back = back;
    const size_t count = items.size();
    if (count <= 4) {
        const float pitch = 46;
        const float top = 6 + (196 - pitch * count) / 2 + 4;
        for (size_t index = 0; index < count; ++index) {
            Control control;
            control.rect = {26, top + index * pitch, 268, 38};
            control.label = items[index].label;
            control.enabled = items[index].enabled;
            control.labelColor = items[index].danger ? color::Bad : 0;
            spec.controls.push_back(control);
        }
    } else {
        const size_t rows = (count + 1) / 2;
        const float pitch = std::min(40.0f, 196.0f / rows);
        // The gaps give way before the buttons go under 24 px.
        const float height = std::max(pitch - 8, std::min(24.0f, pitch - 3));
        const float top = 6 + (196 - pitch * rows) / 2 + 4;
        for (size_t index = 0; index < count; ++index) {
            Control control;
            control.rect = {index % 2 == 0 ? 16.0f : 164.0f,
                            top + (index / 2) * pitch, 140, height};
            control.label = items[index].label;
            control.scale = type::Small + 0.06f;
            control.enabled = items[index].enabled;
            control.labelColor = items[index].danger ? color::Bad : 0;
            spec.controls.push_back(control);
        }
    }
    int focus = 0;
    for (size_t index = 0; index < count; ++index) {
        if (items[index].enabled) {
            focus = static_cast<int>(index);
            break;
        }
    }
    spec.drawTop = [&]() {
        if (drawTop) {
            drawTop(focus);
        }
    };
    const ScreenResult result = runScreen(spec, focus);
    return result.choice >= 0 ? result.choice : -1;
}

int chooseOption(const std::string &title,
                 const std::vector<std::string> &options, int current,
                 Icon icon, u32 iconColor)
{
    // Choosing an option saves it and goes back, so there is no OK.
    const int chosen =
        std::max(0, std::min(current, static_cast<int>(options.size()) - 1));
    ScreenSpec spec;
    spec.back = "Back";
    const float pitch = 39;
    const float height = pitch * options.size() + 8;
    const float top = std::max(22.0f, 24 + (180 - height) / 2);
    for (size_t index = 0; index < options.size(); ++index) {
        Control control;
        control.rect = {30, top + 8 + index * pitch, 260, 30};
        control.label = options[index];
        control.scale = type::Body + 0.04f;
        control.style = static_cast<int>(index) == chosen ? Style::Chosen
                                                          : Style::Normal;
        spec.controls.push_back(control);
    }
    spec.drawTop = [&]() {
        drawTopChrome(title, icon, iconColor);
        drawPanel();
        // The options below show the current one as chosen.
        drawEmblem(icon, iconColor);
    };
    // The title is on the top screen already.
    spec.drawBottom = [&]() { drawRecess({18, top, 284, height}); };
    int focus = chosen;
    const ScreenResult result = runScreen(spec, focus);
    return result.choice >= 0 ? result.choice : -1;
}

// --- Scrolling -------------------------------------------------------------------

bool Scroller::update(const Input &input, const Rect &area, float content,
                      float viewport)
{
    const float maximum = std::max(0.0f, content - viewport);
    bool tap = false;
    longPressed_ = false;

    if ((input.down & KEY_TOUCH) && area.contains(input.touch)) {
        dragging_ = true;
        moved_ = false;
        longPressFired_ = false;
        following_ = false;
        startY_ = lastY_ = input.touch.py;
        velocity_ = 0;
        downTime_ = now();
        pressPoint = input.touch;
    }
    if (dragging_ && (input.held & KEY_TOUCH)) {
        const float y = input.touch.py;
        if (std::fabs(y - startY_) > 6) {
            moved_ = true;
        }
        if (moved_) {
            float delta = lastY_ - y;
            // Past either end the content follows the finger at half speed.
            if (offset_ < 0 || offset_ > maximum) {
                delta *= 0.45f;
            }
            offset_ += delta;
            velocity_ = velocity_ * 0.4f + (lastY_ - y) * 0.6f;
        } else if (!longPressFired_ && now() - downTime_ > 500) {
            longPressFired_ = true;
            longPressed_ = true;
        }
        lastY_ = y;
        target_ = offset_;
    } else if (dragging_) {
        dragging_ = false;
        if (!moved_ && !longPressFired_) {
            tap = true;
            tapPoint = pressPoint;
        }
    }

    if (!dragging_) {
        if (std::fabs(velocity_) > 0.15f) {
            offset_ += velocity_;
            velocity_ *= 0.95f;
            target_ = offset_;
        } else {
            velocity_ = 0;
            if (following_) {
                offset_ += (target_ - offset_) * 0.35f;
                if (std::fabs(target_ - offset_) < 0.3f) {
                    offset_ = target_;
                    following_ = false;
                }
            }
        }
        // Spring back from either end.
        if (offset_ < 0) {
            offset_ += (0 - offset_) * 0.3f;
            velocity_ = 0;
            if (offset_ > -0.3f) {
                offset_ = 0;
            }
        } else if (offset_ > maximum) {
            offset_ += (maximum - offset_) * 0.3f;
            velocity_ = 0;
            if (offset_ - maximum < 0.3f) {
                offset_ = maximum;
            }
        }
    }
    return tap;
}

void Scroller::reveal(float top, float height, float viewport)
{
    float goal = offset_;
    if (top < offset_) {
        goal = top;
    } else if (top + height > offset_ + viewport) {
        goal = top + height - viewport;
    }
    if (goal != offset_) {
        target_ = std::max(0.0f, goal);
        following_ = true;
        velocity_ = 0;
    }
}

// --- Lists -----------------------------------------------------------------------

namespace {
ListResult runListQuietly(const ListSpec &spec, int &cursor, float *offset);
}

ListResult runList(const ListSpec &spec, int &cursor, float *offset)
{
    const ListResult result = runListQuietly(spec, cursor, offset);
    if (result.key == KEY_B) {
        sound::play(sound::Effect::Back);
    } else if (result.key != 0) {
        sound::play(sound::Effect::Select);
    }
    return result;
}

namespace {

ListResult runListQuietly(const ListSpec &spec, int &cursor, float *offset)
{
    enterScreen("list|" + spec.title + "|" + spec.heading + "|" + spec.back +
                "|" + spec.action);
    constexpr float RowHeight = 25;
    constexpr float Pitch = 31;
    const bool headed = !spec.heading.empty();
    // Under the heading, measured, so its descenders clear the recess with
    // the console's font too.
    const float listTop = headed ? std::ceil(3 + lineHeight(type::Small)) : 6.0f;
    const Rect recess = {8, listTop, 304, 204 - listTop};
    const Rect backTab = tabRect(Tab::Left);
    const Rect actionTab = tabRect(Tab::Right, spec.action, spec.actionKey);
    const Rect inner = {recess.x, recess.y + 4, recess.w, recess.h - 8};
    // The heading's actions, laid out leftwards from the recess's right end.
    struct Placed {
        Rect ink;
        Rect hit; // wider and taller than its ink, to be easy to hit
        u32 key;  // reported for a tap
    };
    std::vector<Placed> placed(headed ? spec.headingActions.size() : 0);
    u32 headingKeys = 0;
    float headingRight = recess.x + recess.w - 2;
    for (size_t index = placed.size(); index-- > 0;) {
        const HeadingAction &action = spec.headingActions[index];
        const float width =
            keyCapWidth(action.key) + textWidth(action.label, type::Small);
        const Rect ink = {headingRight - width, 0, width, listTop - 2};
        placed[index] = {ink, {ink.x - 8, 0, ink.w + 14, listTop + 2},
                         action.keys & (~action.keys + 1)};
        headingKeys |= action.keys;
        headingRight = ink.x - 14; // clear of the next one's hit area
    }
    const auto headingAt = [&](const touchPosition &point) {
        for (size_t index = 0; index < placed.size(); ++index) {
            if (placed[index].hit.contains(point)) {
                return static_cast<int>(index);
            }
        }
        return -1;
    };
    // L and R page through the rows unless the list gives them a job.
    const u32 taken = spec.actionKeys | headingKeys | spec.extraKeys;
    const u32 pageUpKeys = (KEY_LEFT | KEY_L) & ~taken;
    const u32 pageDownKeys = (KEY_RIGHT | KEY_R) & ~taken;
    const float viewport = inner.h;
    const float rowLeft = recess.x + 7;
    // Worked out again whenever spec.update changes the rows.
    int count = 0;
    float content = 0;
    bool scrollable = false;
    float rowWidth = 0;
    const auto layout = [&]() {
        count = static_cast<int>(spec.items.size());
        content = count * Pitch + 2;
        scrollable = content > viewport;
        rowWidth = recess.w - 14 - (scrollable ? 12 : 0);
    };
    layout();

    cursor = count == 0 ? -1 : std::max(0, std::min(cursor, count - 1));
    Scroller scroller;
    if (offset != nullptr && *offset >= 0) {
        // Where it was, moved only as far as the highlighted row needs.
        const float top = cursor * Pitch;
        const float start = std::min(*offset, std::max(0.0f, content - viewport));
        scroller.setOffset(
            std::max(0.0f, std::max(std::min(start, top), top + Pitch - viewport)));
    } else if (cursor > 0) {
        const float top = cursor * Pitch;
        scroller.setOffset(std::max(0.0f, std::min(top - viewport / 2 + Pitch / 2,
                                                   content - viewport)));
    }
    // Hands back where the rows were, however the list closes.
    struct KeepOffset {
        float *into;
        const Scroller &from;
        ~KeepOffset()
        {
            if (into != nullptr) {
                *into = from.offset();
            }
        }
    } keepOffset {offset, scroller};
    int tabPressed = 0; // -1 back, 1 action, 2 + n the heading's nth action
    bool touched = false; // the person has moved the highlight or the rows
    const u32 actionKey = spec.actionKeys & (~spec.actionKeys + 1);

    const auto rowAt = [&](const touchPosition &point) {
        const float y = point.py - inner.y + scroller.offset();
        const int row = static_cast<int>(y / Pitch);
        if (y < 0 || row >= count || std::fmod(y, Pitch) > RowHeight + 2) {
            return -1;
        }
        return row;
    };

    // The scrollbar's thumb, and the column it runs in, which takes a drag
    // straight to any part of a long list.
    const float trackX = recess.x + recess.w - 12;
    const Rect barColumn = {trackX - 6, inner.y, BottomWidth - trackX + 6, inner.h};
    const auto thumb = [&]() {
        const float height = std::max(18.0f, (inner.h - 8) * viewport / content);
        const float travel = inner.h - 8 - height;
        const float position =
            std::max(0.0f, std::min(1.0f, scroller.offset() / (content - viewport)));
        return Rect{trackX, inner.y + 4 + travel * position, 10, height};
    };
    float grab = -1; // where the thumb is held, from its top; -1 when not held

    while (keepRunning()) {
        // A list that fills in while it is open keeps what is on screen in
        // place: the row at the top of the view stays there, however many
        // rows arrive above it, and the highlight stays on its row. Only a
        // list nobody has touched stays at its first row as rows arrive.
        if (spec.update) {
            const auto keyAt = [&](int row) {
                return row >= 0 && row < count ? spec.items[row].key : "";
            };
            const bool untouched = !touched && cursor <= 0 && scroller.offset() <= 0;
            const int topRow = std::max(
                0, std::min(count - 1, static_cast<int>(scroller.offset() / Pitch)));
            const std::string topKey = untouched ? "" : keyAt(topRow);
            const float within = scroller.offset() - topRow * Pitch;
            const std::string cursorKey = untouched ? "" : keyAt(cursor);
            const int before = cursor;
            if (spec.update()) {
                layout();
                const auto rowOf = [&](const std::string &key) {
                    for (int index = 0; !key.empty() && index < count; ++index) {
                        if (spec.items[index].key == key) {
                            return index;
                        }
                    }
                    return -1;
                };
                const int top = rowOf(topKey);
                if (top >= 0) {
                    scroller.shift(top * Pitch + within - scroller.offset());
                }
                const int found = rowOf(cursorKey);
                if (cursor != before && cursor >= 0 && cursor < count) {
                    // The screen moved it, to a row it was waiting for.
                    scroller.reveal(cursor * Pitch, Pitch, viewport);
                } else if (found >= 0) {
                    cursor = found;
                } else {
                    cursor = count == 0 ? -1
                                        : std::max(0, std::min(cursor, count - 1));
                    if (untouched && !scroller.dragging()) {
                        scroller.setOffset(0);
                    }
                }
            }
        }
        const bool loading = spec.loading && spec.loading();
        const Input input = readInput();
        if (count > 0) {
            const int pageRows = std::max(1, static_cast<int>(viewport / Pitch) - 1);
            int target = cursor;
            if (input.repeat & KEY_UP) {
                target = cursor > 0 ? cursor - 1 : count - 1;
            }
            if (input.repeat & KEY_DOWN) {
                target = cursor + 1 < count ? cursor + 1 : 0;
            }
            if (input.repeat & pageUpKeys) {
                target = std::max(0, cursor - pageRows);
            }
            if (input.repeat & pageDownKeys) {
                target = std::min(count - 1, cursor + pageRows);
            }
            if (target != cursor) {
                cursor = target;
                touched = true;
                scroller.reveal(cursor * Pitch, Pitch, viewport);
                sound::play(sound::Effect::Move);
            }
        }
        if (input.down & KEY_B) {
            return {KEY_B, cursor};
        }
        if ((input.down & KEY_A) && count > 0) {
            return {KEY_A, cursor};
        }
        if (input.down & spec.actionKeys) {
            return {input.down & spec.actionKeys, cursor};
        }
        if (input.down & headingKeys) {
            return {input.down & headingKeys, cursor};
        }
        if (input.down & spec.extraKeys) {
            return {input.down & spec.extraKeys, cursor};
        }

        // Tabs press on touch and act on release.
        if (input.down & KEY_TOUCH) {
            const int heading = headingAt(input.touch);
            tabPressed = !spec.back.empty() && backTab.contains(input.touch) ? -1
                         : !spec.action.empty() && actionTab.contains(input.touch)
                             ? 1
                         : heading >= 0 ? 2 + heading
                                        : 0;
        }
        if ((input.up & KEY_TOUCH) && tabPressed != 0) {
            const Rect area = tabPressed < 0    ? backTab
                              : tabPressed == 1 ? actionTab
                                                : placed[tabPressed - 2].hit;
            const int which = tabPressed;
            tabPressed = 0;
            if (area.contains(input.touch)) {
                if (which < 0) {
                    return {KEY_B, cursor};
                }
                return {which == 1 ? actionKey : placed[which - 2].key, cursor};
            }
        }

        if ((input.down & KEY_TOUCH) && scrollable &&
            barColumn.contains(input.touch)) {
            // Held where it was touched, or by its middle when the touch
            // lands on the track.
            const Rect bar = thumb();
            grab = input.touch.py >= bar.y && input.touch.py < bar.y + bar.h
                       ? input.touch.py - bar.y
                       : bar.h / 2;
        }
        if (grab >= 0 && (input.held & KEY_TOUCH) && scrollable) {
            const Rect bar = thumb();
            const float travel = inner.h - 8 - bar.h;
            const float position =
                travel > 0 ? (input.touch.py - grab - inner.y - 4) / travel : 0;
            scroller.setOffset(std::max(0.0f, std::min(1.0f, position)) *
                               (content - viewport));
            touched = true;
        } else {
            grab = -1;
        }

        const Rect rows =
            scrollable ? Rect{inner.x, inner.y, barColumn.x - inner.x, inner.h} : inner;
        if (scroller.update(input, rows, content, viewport)) {
            const int row = rowAt(scroller.tapPoint);
            if (row >= 0 && row != cursor) {
                // The first tap only highlights; the next one opens.
                cursor = row;
                touched = true;
                sound::play(sound::Effect::Move);
            } else if (row >= 0) {
                cursor = row;
                return {KEY_A, cursor};
            }
        }
        if (scroller.moved()) {
            touched = true; // dragged or flicked
        }
        if (scroller.longPressed() && spec.longPressKey != 0) {
            const int row = rowAt(scroller.pressPoint);
            if (row >= 0) {
                cursor = row;
                return {spec.longPressKey, cursor};
            }
        }
        const int held = scroller.dragging() && !scroller.moved()
                             ? rowAt(scroller.pressPoint)
                             : -1;

        beginFrame();
        if (spec.drawTop) {
            spec.drawTop(cursor);
        } else {
            drawTopChrome(spec.title);
        }
        beginBottom();
        if (!placed.empty()) {
            drawMarquee(spec.heading, recess.x + 2, 2, type::Small,
                        color::rgb(0x63593F), placed.front().ink.x - recess.x - 12);
            for (size_t index = 0; index < placed.size(); ++index) {
                const Placed &action = placed[index];
                const bool down = tabPressed == 2 + static_cast<int>(index) &&
                                  (input.held & KEY_TOUCH) &&
                                  action.hit.contains(input.touch);
                const u32 ink = down ? color::InkStrong : color::rgb(0x63593F);
                const Rect line = {action.ink.x, 1, action.ink.w, listTop - 3};
                const float x = drawKeyCap(spec.headingActions[index].key,
                                           action.ink.x, line, ink,
                                           color::rgb(0xF4EFD6));
                drawText(spec.headingActions[index].label, x, 2, type::Small, ink);
            }
        } else if (headed) {
            drawMarquee(spec.heading, 160, 2, type::Small, color::rgb(0x63593F),
                        296, C2D_AlignCenter);
        }
        drawRecess(recess);
        if (count == 0 && loading) {
            drawSpinner(recess.x + recess.w / 2, recess.y + recess.h / 2, 14);
        } else if (count == 0) {
            drawPanelText(spec.emptyText, recess, type::Small, color::InkSoft);
        }

        clip(inner);
        const int first = std::max(0, static_cast<int>(scroller.offset() / Pitch) - 1);
        for (int index = first; index < count; ++index) {
            const float y = inner.y + 2 + index * Pitch - scroller.offset();
            if (y > inner.y + inner.h) {
                break;
            }
            const ListItem &item = spec.items[index];
            const Rect row = {rowLeft, y, rowWidth, RowHeight};
            const bool down = index == held;
            fillRound({row.x - 1, row.y - 1, row.w + 2, row.h + 2.5f}, 9,
                      {{0, color::rgb(0xC3BA92)}, {1, color::rgb(0xA69C6E)}});
            if (item.marked) {
                fillRound(row, 8, {{0, color::rgb(0xF3FAFF)},
                                   {1, color::rgb(0xD8EEF8)}});
            } else {
                fillRound(row, 8, {{0, down ? color::rgb(0xE6E2D2)
                                            : color::rgb(0xFEFCF4)},
                                   {1, down ? color::rgb(0xD8D3BF)
                                            : color::rgb(0xEEEBDF)}});
            }
            float textX = row.x + 8;
            if (item.icon != Icon::None) {
                drawIcon(item.icon, textX, row.y + 4, 17, item.iconColor);
                textX += 23;
            }
            float right = row.x + row.w - 8;
            if (item.progress >= 0) {
                // Under the name and whatever is beside it.
                const Rect track = {textX, row.y + row.h - 4.5f, right - textX, 2.5f};
                fillRound(track, 1.25f, color::rgb(0xE2DBBB));
                const float fill = track.w * std::max(0.0f, std::min(1.0f, item.progress));
                if (fill > 0) {
                    fillRound({track.x, track.y, fill, track.h}, 1.25f,
                              {{0, color::rgb(0xFFCC22)}, {1, color::rgb(0xFF9900)}});
                }
            }
            if (!item.badge.empty()) {
                right -= drawBadge(right, row.y + 4.5f, item.badge,
                                   item.badgeColor) + 6;
            } else if (!item.detail.empty()) {
                const std::string detail =
                    fitEnd(item.detail, type::Tiny, 110);
                drawCentered(detail, {right - textWidth(detail, type::Tiny),
                                      row.y, 120, row.h},
                             type::Tiny, color::InkSoft, true);
                right -= textWidth(detail, type::Tiny) + 8;
            }
            if (item.flag != Icon::None) {
                drawIcon(item.flag, right - 13, row.y + 6, 13, item.flagColor);
                right -= 13 + 6;
            }
            // Only the highlighted row scrolls, as on the system's lists.
            if (index == cursor && !scroller.dragging()) {
                drawCenteredMarquee(item.label,
                                    {textX, row.y, right - textX, row.h},
                                    type::Row, item.labelColor, true);
            } else {
                // Fitted the same way, so a row keeps its look as the
                // ring arrives.
                drawFitted(item.label, textX,
                           row.y + (row.h - lineHeight(type::Row)) / 2 + 1,
                           type::Row, item.labelColor, right - textX);
            }
            if (index == cursor && !scroller.dragging()) {
                drawFocus(row, 8, 2.5f, 2.5f);
            }
        }
        unclip();

        if (scrollable) {
            rect(trackX + 4, inner.y + 4, 2, inner.h - 8, color::rgb(0xA79D70));
            const Rect bar = thumb();
            fillRound({bar.x - 1, bar.y - 1, bar.w + 2, bar.h + 2}, 5,
                      color::rgb(0x9A916A));
            fillRound(bar, 4,
                      grab >= 0 ? color::rgb(0xDDD8C4) : color::rgb(0xFBF9F0));
        }

        if (!spec.back.empty()) {
            drawTab(Tab::Left, spec.back, "B",
                    tabPressed < 0 && (input.held & KEY_TOUCH) &&
                        backTab.contains(input.touch));
        }
        if (!spec.action.empty()) {
            drawTab(Tab::Right, spec.action, spec.actionKey,
                    tabPressed > 0 && (input.held & KEY_TOUCH) &&
                        actionTab.contains(input.touch));
        }
        if (loading && count > 0 && spec.hint.empty()) {
            // Between the tabs while more rows are still on their way.
            drawSpinner(160, 224, 8);
        } else if (!spec.hint.empty()) {
            // In the gap between the tabs, which a wide action tab narrows.
            const float gapLeft = backTab.x + backTab.w;
            const float gapRight = spec.action.empty() ? 320 - gapLeft : actionTab.x;
            drawMarquee(spec.hint, (gapLeft + gapRight) / 2, 220, type::Tiny,
                        color::Counter, std::max(0.0f, gapRight - gapLeft - 8),
                        C2D_AlignCenter);
        }
        endFrame();
    }
    return {0, cursor};
}

} // namespace

// --- Progress, busy and scanner ------------------------------------------------------

const Rect ProgressCancelButton = {70, 148, 180, 36};

void drawProgressPanel(const ProgressView &view)
{
    drawPanel();
    const float left = TopPanel.x + 18;
    float nameX = left;
    if (!view.titleIcon.empty()) {
        drawTitleIcon(view.titleIcon, left, TopPanel.y + 12);
        nameX += 60;
    } else {
        drawIcon(view.nameIcon, left, TopPanel.y + 16, 30, view.nameIconColor);
        nameX += 40;
    }
    const float nameWidth = TopPanel.x + TopPanel.w - 18 - nameX;
    drawMarquee(view.name, nameX, TopPanel.y + 13, 0.62f, color::InkStrong,
                nameWidth);
    if (!view.note.empty()) {
        drawMarquee(view.note, nameX, TopPanel.y + 41, type::Small, color::Good,
                    nameWidth);
    } else {
        drawMarquee(view.source, nameX, TopPanel.y + 41, type::Small,
                    color::InkSoft, nameWidth);
    }
    drawProgressBar({left, TopPanel.y + 72, TopPanel.w - 36, 18}, view.fraction);
    drawText(view.amounts, left, TopPanel.y + 98, type::Body, color::Ink);
    if (view.fraction >= 0) {
        char percent[8];
        std::snprintf(percent, sizeof(percent), "%d%%",
                      static_cast<int>(view.fraction * 100));
        drawText(percent, TopPanel.x + TopPanel.w - 18, TopPanel.y + 98,
                 type::Body, color::InkStrong, C2D_AlignRight);
    }
    // The speed and its tag on the left, and the time left across from
    // them, under the percentage; the speed gives way to the others.
    const float right = TopPanel.x + TopPanel.w - 18;
    const float leftWidth =
        view.left.empty() ? 0 : std::min(textWidth(view.left, type::Small), 160.0f);
    if (leftWidth > 0) {
        drawFitted(view.left, right, TopPanel.y + 126, type::Small,
                   color::InkStrong, leftWidth, C2D_AlignRight);
    }
    const float tagWidth = view.tag.empty() ? 0 : textWidth(view.tag, 0.4f) + 16;
    const float room = right - left - (leftWidth > 0 ? leftWidth + 12 : 0) -
                       (tagWidth > 0 ? tagWidth + 8 : 0);
    const float speedWidth =
        view.speed.empty() ? 0 : std::min(textWidth(view.speed, type::Small), room);
    drawMarquee(view.speed, left, TopPanel.y + 126, type::Small, color::Ink, room);
    if (tagWidth > 0) {
        drawTag(left + (speedWidth > 0 ? speedWidth + 8 : 0), TopPanel.y + 124,
                view.tag);
    }
}

void drawProgress(const ProgressView &view)
{
    beginFrame();
    drawTopChrome(view.title, view.icon, view.iconColor);
    drawProgressPanel(view);

    beginBottom();
    // Three lines of the message at most, in a panel that fills the screen
    // when there is no Cancel button under it.
    const Rect panel = {13, 10, 294, view.cancellable ? 128.0f : 220.0f};
    drawPanel(panel);
    // A message that ends in a folder ("Saving to sdmc:/Downloads/") breaks
    // before the path instead of inside it; the path scrolls if it is long.
    std::vector<std::string> lines;
    bool pathLine = false;
    const size_t path = view.message.find(" sdmc:/");
    if (path != std::string::npos &&
        textWidth(view.message, type::Body) > 266) {
        lines = {view.message.substr(0, path), view.message.substr(path + 1)};
        pathLine = true;
    } else {
        lines = wrap(view.message, type::Body, 266, 3);
    }
    float y = panel.y + (panel.h - lines.size() * lineHeight(type::Body)) / 2;
    for (size_t index = 0; index < lines.size(); ++index) {
        if (pathLine && index == 1) {
            drawMarquee(lines[index], 160, y, type::Body, color::Ink, 266,
                        C2D_AlignCenter);
        } else {
            drawText(lines[index], 160, y, type::Body, color::Ink,
                     C2D_AlignCenter);
        }
        y += lineHeight(type::Body);
    }
    if (view.cancellable && view.stopping) {
        drawButton(ProgressCancelButton, "Stopping...", Style::Disabled,
                   type::Button);
    } else if (view.cancellable) {
        drawButton(ProgressCancelButton, "Cancel");
        drawFocus(ProgressCancelButton, 9);
    }
    endFrame();
}

namespace {

constexpr Rect BusyBox = {10, 62, 300, 116};
constexpr Rect CancellableBusyBox = {10, 50, 300, 140};

} // namespace

const Rect BusyCancelButton = {CancellableBusyBox.x,
                               CancellableBusyBox.y + CancellableBusyBox.h -
                                   StripHeight,
                               CancellableBusyBox.w, StripHeight};

void drawBusy(const std::string &header, const std::string &title)
{
    BusyView view;
    view.header = header;
    view.title = title;
    drawBusy(view);
}

void drawBusy(const BusyView &view)
{
    if (!lastFrameWasDialog && !inDialog) {
        captureBackdrop();
    }
    const bool wasInDialog = inDialog;
    inDialog = true;
    beginFrame();
    if (!view.title.empty()) {
        drawTopChrome(titleCase(view.header), Icon::AppMark, color::Accent);
        drawPanel();
        drawPanelText(view.title, TopPanel);
    } else {
        drawBackdrop(false);
    }
    beginBottom();
    drawBackdrop(true);
    rect(0, 0, BottomWidth, ScreenHeight, color::rgb(0x141208, 0x7A));
    const Rect box = view.cancellable ? CancellableBusyBox : BusyBox;
    drawDialogBox(box);
    const float contentHeight = box.h - (view.cancellable ? StripHeight : 0);
    // With a count, the spinner row moves up to make room for a bar, or
    // for the count alone.
    const bool counting = !view.progress && !view.amount.empty();
    const float rowY = view.progress ? box.y + 30
                       : counting    ? box.y + contentHeight / 2 - 12
                                     : box.y + contentHeight / 2;
    const float width = 30 + 12 + textWidth("Please wait...", type::Body);
    drawSpinner(box.x + (box.w - width) / 2 + 15, rowY, 15);
    drawCentered("Please wait...",
                 {box.x + (box.w - width) / 2 + 42, rowY - 20, 200, 40},
                 type::Body, color::InkDialog, true);
    if (view.progress && !view.left.empty()) {
        // The count under the bar's start, the time left under its end.
        drawProgressBar({box.x + 30, box.y + 54, box.w - 60, 12}, view.fraction);
        const float half = (box.w - 60) / 2 - 4;
        drawFitted(view.amount, box.x + 30, box.y + 72, type::Tiny,
                   color::InkSoft, half, 0);
        drawFitted(view.left, box.x + box.w - 30, box.y + 72, type::Tiny,
                   color::InkDialog, half, C2D_AlignRight);
    } else if (view.progress) {
        drawProgressBar({box.x + 30, box.y + 54, box.w - 60, 12}, view.fraction);
        drawFitted(view.amount, box.x + box.w / 2, box.y + 72, type::Tiny,
                   color::InkSoft, box.w - 60, C2D_AlignCenter);
    } else if (counting) {
        drawFitted(view.amount, box.x + box.w / 2, rowY + 18, type::Small,
                   color::InkSoft, box.w - 60, C2D_AlignCenter);
    }
    if (view.cancellable) {
        drawStrip(box, {view.stopping ? "Stopping..." : "Cancel"}, -1,
                  view.pressed ? 0 : -1, false, StripHeight, -1);
    }
    endFrame();
    inDialog = wasInDialog;
    lastFrameWasDialog = true;
}

const Rect ScannerCancelButton = {0, 209, 120, 31};
const Rect ScannerTypeButton = {36, 96, 248, 40};

void drawScanner(const C2D_Image *preview, const std::string &statusText,
                 u32 statusColor)
{
    beginFrame();
    rect(0, 0, TopWidth, ScreenHeight, color::rgb(0x2A2E31));
    if (preview != nullptr) {
        plainImage(*preview, 0, 0);
    }
    // Corner brackets around the middle, where the code should go.
    const float left = 130;
    const float top = 46;
    const float size = 140;
    const float arm = 26;
    const float line = 4;
    for (int pass = 0; pass < 2; ++pass) {
        const float o = pass == 0 ? 1 : 0;
        const u32 fill = pass == 0 ? color::rgb(0x000000, 0x70)
                                   : color::rgb(0xFFFFFF);
        rect(left + o, top + o, arm, line, fill);
        rect(left + o, top + o, line, arm, fill);
        rect(left + size - arm + o, top + o, arm, line, fill);
        rect(left + size - line + o, top + o, line, arm, fill);
        rect(left + o, top + size - line + o, arm, line, fill);
        rect(left + o, top + size - arm + o, line, arm, fill);
        rect(left + size - arm + o, top + size - line + o, arm, line, fill);
        rect(left + size - line + o, top + size - arm + o, line, arm, fill);
    }
    C2D_DrawRectangle(0, 0, Depth, TopWidth, 19, color::rgb(0xF2EBB2, 0xEB),
                      color::rgb(0xF2EBB2, 0xEB), color::rgb(0xF2E9A3, 0xEB),
                      color::rgb(0xF2E9A3, 0xEB));
    drawStatusBar();
    rect(0, 206, TopWidth, 34, color::rgb(0x141618, 0xB8));
    drawCenteredMarquee(statusText, {10, 206, TopWidth - 20, 34}, type::Body,
                 statusColor == color::Warning ? color::rgb(0xFFD36B)
                                               : color::rgb(0xFFFFFF));

    beginBottom();
    const std::string title = "Scan QR Code";
    const float width = 26 + textWidth(title, 0.72f);
    drawIcon(Icon::Scan, 160 - width / 2, 9, 20, color::Accent);
    drawText(title, 160 - width / 2 + 26, 19 - lineHeight(0.72f) / 2 + 1, 0.72f,
             color::Accent);
    drawButton(ScannerTypeButton, "Type a URL Instead", Style::Normal,
               type::Body + 0.06f);
    drawTab(Tab::Left, "Back", "B");
    endFrame();
}

} // namespace ui
