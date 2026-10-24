// draw.h - everything on screen that is not the 3D look: one list of textured quads in the fight's 1080p layout
// (1920 x 1080 pixels, y down), drawn by ONE pipeline on each console (src/platform/ps5/ui_vk.*, src/platform/xbox/
// ui_d3d11.*) and by the host's software raster (raster.h) for pictures and tests.
//
// A quad samples the font atlas (font.h): its distance value is cut at the edge with a smooth step a pixel wide, so
// text is sharp at every size, and an outline comes from a second cut further out. A solid rectangle samples the
// atlas's solid cell (always inside). Colours are sRGB bytes with alpha; the shaders blend premultiplied.
//
// Text: UTF-8, plus {TOKENS} for the pad's buttons and the stick, drawn per pad family (PadStyle): {CROSS} is the
// PlayStation cross on the PS5 and a green A on the Xbox. Tokens: {CROSS} {CIRCLE} {SQUARE} {TRIANGLE} {L1} {R1}
// {L2} {R2} {L3} {R3} {START} {SELECT}, and {1}..{9} for the stick in numpad notation (6 = right, as drawn).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "font.h"

namespace mf::ui {

constexpr float kScreenW = 1920, kScreenH = 1080;
constexpr float kMinTextSize = 28;   // the rule (docs/GAME-LAB-SF6.md): no text under 28 px at 1080p

// ------------------------------------------------------------------ colours

struct Rgba {
  uint8_t r = 255, g = 255, b = 255, a = 255;
};
constexpr Rgba Hex(uint32_t rgb, uint8_t a = 255) { return Rgba{uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a}; }
constexpr Rgba WithAlpha(Rgba c, float a) { return Rgba{c.r, c.g, c.b, uint8_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)}; }

namespace colour {
constexpr Rgba kText = Hex(0xF4F5F8), kDim = Hex(0xA9B1C2), kInk = Hex(0x0B0D14), kPanel = Hex(0x0E111B, 214);
constexpr Rgba kPanelEdge = Hex(0x2A3146), kFocus = Hex(0xFF8A1F), kFocus2 = Hex(0x33D1E0), kAccent = Hex(0xFFC83D);
constexpr Rgba kPlayer1 = Hex(0x3C8CFF), kPlayer2 = Hex(0xFF4A3C), kHealth = Hex(0xFFD23F), kHealthLost = Hex(0xFF5A36);
constexpr Rgba kHealthBack = Hex(0x1A1E2B, 230), kGood = Hex(0x35D46A), kBad = Hex(0xFF4D4D), kShade = Hex(0x000000, 120);
}  // namespace colour

// ------------------------------------------------------------------ the batch

struct Vertex {
  float x, y;          // 1080p layout pixels
  float u, v;          // atlas, 0..1
  Rgba fill;           // sRGB + alpha
  Rgba line;           // the outline's colour (alpha 0: none)
  float bias;          // added to the edge: > 0 bolder
  float outline;       // the outline's width in distance units (0.08 = about 1 px at the bake size)
};
static_assert(sizeof(Vertex) == 32, "the vertex layout both consoles declare");

enum class PadStyle { kPlayStation, kXbox };

struct TextStyle {
  float size = 38;            // pixels from the font's lowest to highest point (ascent - descent) at 1080p
  Rgba fill = colour::kText;
  Rgba line{0, 0, 0, 0};      // outline colour; alpha 0 = none
  float outline = 0.0f;       // outline width in distance units
  float bias = 0.0f;          // > 0 bolder (0.04 is a step)
};

enum class Align { kLeft, kCentre, kRight };

class Batch {
 public:
  std::vector<Vertex> vertices;
  float minTextSize = 1e9f;   // the smallest text this frame drew (tests hold it to kMinTextSize)
  int texts = 0;
  int unknownTokens = 0;      // {TOKENS} nobody knows (tests hold it to 0)
  float maxTextRight = 0;     // the rightmost pixel any text reached (tests: nothing runs off the screen)
  std::string* transcript = nullptr;   // tests: every text drawn, one per line (null on the consoles: no cost)

  void Reset(const Font* font, PadStyle style) {
    vertices.clear();
    maxTextRight = 0;
    if (transcript) transcript->clear();
    font_ = font;
    style_ = style;
    minTextSize = 1e9f;
    texts = 0;
    unknownTokens = 0;
    if (font_ && font_->ok()) {
      if (const Glyph* g = font_->Find(kGlyphSolid)) {
        solidU_ = (g->x + g->w * 0.5f) / font_->width;
        solidV_ = (g->y + g->h * 0.5f) / font_->height;
      }
    }
  }
  const Font* font() const { return font_; }
  PadStyle style() const { return style_; }

  // A solid rectangle (layout pixels).
  void Rect(float x0, float y0, float x1, float y1, Rgba c) {
    if (x1 <= x0 || y1 <= y0 || c.a == 0) return;
    Quad(x0, y0, x1, y1, solidU_, solidV_, solidU_, solidV_, c, Rgba{0, 0, 0, 0}, 0, 0);
  }
  // An image from the cover atlas (BlazingMount: app/src/covers.h): outline < 0 marks the quad, so the shaders sample the
  // RGBA cover atlas at (u, v) instead of the font; `alpha` dims it over what is under it.
  void Image(float x0, float y0, float x1, float y1, const float uv[4], float alpha = 1.0f) {
    if (x1 <= x0 || y1 <= y0 || alpha <= 0) return;
    Quad(x0, y0, x1, y1, uv[0], uv[1], uv[2], uv[3], Rgba{255, 255, 255, uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f)},
         Rgba{0, 0, 0, 0}, 0, -1.0f);
  }

  // A frame `w` pixels thick inside the rectangle.
  void Frame(float x0, float y0, float x1, float y1, float w, Rgba c) {
    Rect(x0, y0, x1, y0 + w, c);
    Rect(x0, y1 - w, x1, y1, c);
    Rect(x0, y0 + w, x0 + w, y1 - w, c);
    Rect(x1 - w, y0 + w, x1, y1 - w, c);
  }

  // A solid parallelogram: the rectangle's top edge moved `shift` pixels right of its bottom edge (a comic panel's
  // slant: the super's title card, the tutorial's SUCCESS stamp).
  void Slant(float x0, float y0, float x1, float y1, float shift, Rgba c) {
    if (x1 <= x0 || y1 <= y0 || c.a == 0) return;
    const Vertex a{x0 + shift, y0, solidU_, solidV_, c, Rgba{0, 0, 0, 0}, 0, 0}, b{x1 + shift, y0, solidU_, solidV_, c, Rgba{0, 0, 0, 0}, 0, 0};
    const Vertex d{x1, y1, solidU_, solidV_, c, Rgba{0, 0, 0, 0}, 0, 0}, e{x0, y1, solidU_, solidV_, c, Rgba{0, 0, 0, 0}, 0, 0};
    vertices.insert(vertices.end(), {a, b, d, a, d, e});
  }

  // One of our glyphs (font.h) in a square of `size` pixels with its top-left at (x, y).
  void Shape(int32_t cp, float x, float y, float size, Rgba c, Rgba line = {0, 0, 0, 0}, float outline = 0) {
    const Glyph* g = font_ ? font_->Find(cp) : nullptr;
    if (!g || !g->w) return;
    // our glyphs are a square drawn with `spread` pixels of distance around it
    const float inner = float(g->w) - 2.0f * font_->spread;
    const float s = size / inner;
    const float pad = font_->spread * s;
    Quad(x - pad, y - pad, x + size + pad, y + size + pad, float(g->x) / font_->width, float(g->y) / font_->height,
         float(g->x + g->w) / font_->width, float(g->y + g->h) / font_->height, c, line, 0, outline);
  }

  // Text with its line box's top at y; returns the width drawn. Width only (nothing drawn) when `draw` is false.
  float Text(float x, float y, const char* text, const TextStyle& st, Align align = Align::kLeft, bool draw = true) {
    if (!font_ || !font_->ok() || !text) return 0;
    if (align != Align::kLeft) {
      const float w = Text(0, 0, text, st, Align::kLeft, false);
      x -= align == Align::kCentre ? w * 0.5f : w;
    }
    if (draw) {
      minTextSize = std::min(minTextSize, st.size);
      ++texts;
      if (transcript) *transcript += std::string(text) + "\n";
    }
    const float scale = st.size / font_->baseSize;
    const float baseline = y + font_->ascent * scale;
    float pen = x;
    int32_t prev = 0;
    const char* p = text;
    while (*p) {
      if (*p == '{') {
        const char* end = std::strchr(p, '}');
        if (end && end - p < 16) {
          char token[16] = {};
          std::memcpy(token, p + 1, size_t(end - p - 1));
          pen += Token(pen, y, token, st, draw);
          p = end + 1;
          prev = 0;
          continue;
        }
      }
      const int32_t cp = NextCodepoint(p);
      if (cp == '\n') continue;
      const Glyph* g = font_->Find(cp);
      if (!g) g = font_->Find('?');
      if (!g) continue;
      if (prev) pen += font_->KernOf(prev, cp) * scale;
      if (draw && g->w) {
        const float x0 = pen + g->xoff * scale, y0 = baseline + g->yoff * scale;
        Quad(x0, y0, x0 + g->w * scale, y0 + g->h * scale, float(g->x) / font_->width, float(g->y) / font_->height,
             float(g->x + g->w) / font_->width, float(g->y + g->h) / font_->height, st.fill, st.line, st.bias, st.outline);
      }
      pen += g->advance * scale;
      prev = cp;
    }
    if (draw) maxTextRight = std::max(maxTextRight, pen);
    return pen - x;
  }
  float Width(const char* text, const TextStyle& st) { return Text(0, 0, text, st, Align::kLeft, false); }
  float LineHeight(float size) const { return font_ ? (font_->ascent - font_->descent + font_->lineGap) * size / font_->baseSize : size; }
  float CapHeight(float size) const { return font_ ? font_->capHeight * size / font_->baseSize : size * 0.6f; }
  // The y offset that puts capitals of `size` centred on `centreY`.
  float TopForCentre(float centreY, float size) const {
    if (!font_) return centreY - size * 0.5f;
    const float scale = size / font_->baseSize;
    return centreY - (font_->ascent - font_->capHeight * 0.5f) * scale;
  }

 private:
  void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Rgba fill, Rgba line, float bias,
            float outline) {
    const Vertex a{x0, y0, u0, v0, fill, line, bias, outline}, b{x1, y0, u1, v0, fill, line, bias, outline};
    const Vertex c{x1, y1, u1, v1, fill, line, bias, outline}, d{x0, y1, u0, v1, fill, line, bias, outline};
    vertices.insert(vertices.end(), {a, b, c, a, c, d});
  }

  static int32_t NextCodepoint(const char*& p) {
    const uint8_t c = uint8_t(*p++);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    int32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && (uint8_t(*p) & 0xC0) == 0x80) cp = (cp << 6) | (uint8_t(*p++) & 0x3F);
    return cp;
  }

  // A button or stick glyph; returns its advance.
  float Token(float x, float y, const char* token, const TextStyle& st, bool draw) {
    const float cap = CapHeight(st.size);
    const float box = cap * 1.42f;                      // a face button's disc
    const float top = y + (font_->ascent * st.size / font_->baseSize) - cap * 1.21f;
    const float gap = st.size * 0.12f;
    if (token[0] >= '1' && token[0] <= '9' && token[1] == 0) {   // the stick
      if (draw) Shape(ArrowGlyph(token[0] - '0'), x + gap * 0.5f, top, box, st.fill, st.line, st.outline);
      return box + gap;
    }
    struct Face {
      const char* name;
      int32_t shape;
      uint32_t psColour;
      const char* xboxLetter;
      uint32_t xboxColour;
    };
    static constexpr Face faces[] = {{"CROSS", kGlyphCross, 0x8DB8FF, "A", 0x3DBE4C},
                                     {"CIRCLE", kGlyphCircle, 0xFF6B6B, "B", 0xE8423A},
                                     {"SQUARE", kGlyphSquare, 0xF59BD9, "X", 0x2F7BF0},
                                     {"TRIANGLE", kGlyphTriangle, 0x5FE0B4, "Y", 0xF2C12E}};
    for (const Face& f : faces) {
      if (std::strcmp(token, f.name) != 0) continue;
      if (draw) {
        if (style_ == PadStyle::kPlayStation) {
          Shape(kGlyphDisc, x + gap * 0.5f, top, box, Hex(0x10131C, 235));
          Shape(f.shape, x + gap * 0.5f, top, box, Hex(f.psColour));
        } else {
          Shape(kGlyphDisc, x + gap * 0.5f, top, box, Hex(f.xboxColour));
          TextStyle letter = st;
          letter.size = std::max(kMinTextSize, st.size * 0.82f);
          letter.fill = colour::kInk;
          letter.line = Rgba{0, 0, 0, 0};
          letter.outline = 0;
          const float w = Width(f.xboxLetter, letter);
          Text(x + gap * 0.5f + (box - w) * 0.5f, TopForCentre(top + box * 0.5f, letter.size), f.xboxLetter, letter);
        }
      }
      return box + gap;
    }
    // shoulder, stick-click and system buttons: their name in a box
    struct Label {
      const char* name;
      const char* ps;
      const char* xbox;
    };
    static constexpr Label labels[] = {{"L1", "L1", "LB"}, {"R1", "R1", "RB"}, {"L2", "L2", "LT"}, {"R2", "R2", "RT"},
                                       {"L3", "L3", "LS"}, {"R3", "R3", "RS"}, {"START", "OPTIONS", "MENU"},
                                       {"SELECT", "TOUCH PAD", "VIEW"}};
    for (const Label& l : labels) {
      if (std::strcmp(token, l.name) != 0) continue;
      TextStyle inner = st;
      inner.size = std::max(kMinTextSize, st.size * 0.78f);
      inner.fill = colour::kInk;
      inner.line = Rgba{0, 0, 0, 0};
      inner.outline = 0;
      const char* text = style_ == PadStyle::kPlayStation ? l.ps : l.xbox;
      const float w = Width(text, inner) + st.size * 0.36f;
      if (draw) {
        Rect(x + gap * 0.5f, top, x + gap * 0.5f + w, top + box, Hex(0xE6E9F0));
        Text(x + gap * 0.5f + st.size * 0.18f, TopForCentre(top + box * 0.5f, inner.size), text, inner);
      }
      return w + gap;
    }
    // unknown token: drawn as written in brackets, so a typo shows (never braces: they would be read again)
    char raw[20];
    std::snprintf(raw, sizeof(raw), "[%s]", token);
    if (draw) ++unknownTokens;
    return Text(x, y, raw, st, Align::kLeft, draw);
  }

  const Font* font_ = nullptr;
  PadStyle style_ = PadStyle::kPlayStation;
  float solidU_ = 0, solidV_ = 0;
};

}  // namespace mf::ui
