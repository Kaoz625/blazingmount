// bake.h - the font atlas baker (tools/ui/bake_font.cpp writes the file; tests/ui_test.cpp bakes again and compares).
// See bake_font.cpp. Include in ONE file of a program: it carries stb_truetype's implementation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#include "../../src/ui/font.h"

namespace mf::ui::bake {


using mf::ui::Font;
using mf::ui::Glyph;

constexpr float kBaseSize = 48.0f;   // the font's pixel height (ascent - descent) at bake time
constexpr int kSpread = 6;           // pixels of distance kept on each side of an edge
constexpr int kAtlasWidth = 1024;
constexpr float kOnEdge = 128.0f, kPerPixel = 128.0f / kSpread;

struct Bitmap {
  Glyph g;
  std::vector<uint8_t> px;
};

// ------------------------------------------------------------------ exact distance functions (Inigo Quilez's)

struct V2 {
  float x, y;
};
V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
float Dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
float Len(V2 a) { return std::sqrt(Dot(a, a)); }

float Segment(V2 p, V2 a, V2 b) {
  const V2 pa = p - a, ba = b - a;
  const float h = std::clamp(Dot(pa, ba) / Dot(ba, ba), 0.0f, 1.0f);
  return Len(pa - ba * h);
}

float Box(V2 p, float half) {
  const V2 d{std::fabs(p.x) - half, std::fabs(p.y) - half};
  const V2 o{std::max(d.x, 0.0f), std::max(d.y, 0.0f)};
  return Len(o) + std::min(std::max(d.x, d.y), 0.0f);
}

float Triangle(V2 p, V2 p0, V2 p1, V2 p2) {
  const V2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
  const V2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
  const V2 pq0 = v0 - e0 * std::clamp(Dot(v0, e0) / Dot(e0, e0), 0.0f, 1.0f);
  const V2 pq1 = v1 - e1 * std::clamp(Dot(v1, e1) / Dot(e1, e1), 0.0f, 1.0f);
  const V2 pq2 = v2 - e2 * std::clamp(Dot(v2, e2) / Dot(e2, e2), 0.0f, 1.0f);
  const float s = (e0.x * e2.y - e0.y * e2.x) > 0 ? 1.0f : -1.0f;
  const float d0 = Dot(pq0, pq0), d1 = Dot(pq1, pq1), d2 = Dot(pq2, pq2);
  const float c0 = s * (v0.x * e0.y - v0.y * e0.x), c1 = s * (v1.x * e1.y - v1.y * e1.x), c2 = s * (v2.x * e2.y - v2.y * e2.x);
  const float d = std::min({d0, d1, d2});
  const float c = std::min({c0, c1, c2});
  return -std::sqrt(d) * (c > 0 ? 1.0f : -1.0f);   // negative inside
}

V2 Rotate(V2 p, float radians) {
  const float c = std::cos(radians), s = std::sin(radians);
  return {c * p.x - s * p.y, s * p.x + c * p.y};
}

// Our glyphs: a square of `size` pixels whose bottom sits `drop` below the baseline. Distances in pixels, y down.
float Shape(int32_t cp, V2 p, float h) {   // p from the centre, h = half the size
  const float t = 0.13f * h;               // line half-thickness of the outlined shapes
  switch (cp) {
    case mf::ui::kGlyphSolid:
      return -1e3f;
    case mf::ui::kGlyphCross: {
      const float a = 0.55f * h;
      return std::min(Segment(p, {-a, -a}, {a, a}), Segment(p, {-a, a}, {a, -a})) - t;
    }
    case mf::ui::kGlyphCircle:
      return std::fabs(Len(p) - 0.6f * h) - t;
    case mf::ui::kGlyphSquare:
      return std::fabs(Box(p, 0.55f * h)) - t;
    case mf::ui::kGlyphTriangle: {
      const float r = 0.68f * h;   // corners on a circle, the point up (y down: up is -y)
      const V2 a{0, -r}, b{r * 0.8660254f, r * 0.5f}, c{-r * 0.8660254f, r * 0.5f};
      const V2 q{p.x, p.y - 0.08f * h};
      return std::fabs(Triangle(q, a, b, c)) - t;
    }
    case mf::ui::kGlyphDisc:
      return Len(p) - 0.94f * h;
    case mf::ui::kGlyphRing:
      return std::fabs(Len(p) - 0.7f * h) - 0.16f * h;
    default:
      break;
  }
  if (cp >= mf::ui::kGlyphArrow1 && cp <= mf::ui::kGlyphArrow9) {
    const int numpad = cp - mf::ui::kGlyphArrow1 + 1;
    if (numpad == 5) return Len(p) - 0.28f * h;   // neutral: a dot
    const int dx = (numpad - 1) % 3 - 1, dy = (numpad - 1) / 3 - 1;   // numpad: 1-3 the bottom row; dy +1 = up
    const float angle = std::atan2(float(-dy), float(dx));            // the screen's y grows downwards
    const V2 q = Rotate(p, -angle);                                   // the arrow along +x
    const float shaft = Segment(q, {-0.78f * h, 0}, {0.2f * h, 0}) - 0.15f * h;
    const float head = Triangle(q, {0.82f * h, 0}, {0.05f * h, -0.5f * h}, {0.05f * h, 0.5f * h});
    return std::min(shaft, head);
  }
  return 1e3f;
}

Bitmap DrawShape(int32_t cp, int size, int drop, int advance) {
  Bitmap b;
  const int w = size + 2 * kSpread, h = size + 2 * kSpread;
  b.px.resize(size_t(w) * h);
  const float half = size * 0.5f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const V2 p{x + 0.5f - w * 0.5f, y + 0.5f - h * 0.5f};
      const float d = cp == mf::ui::kGlyphSolid ? -1e3f : Shape(cp, p, half);
      b.px[size_t(y) * w + x] = uint8_t(std::clamp(std::lround(kOnEdge - d * kPerPixel), 0L, 255L));
    }
  }
  b.g.codepoint = cp;
  b.g.w = uint16_t(w), b.g.h = uint16_t(h);
  b.g.xoff = float(-kSpread) + 2.0f;
  b.g.yoff = float(drop - size - kSpread);
  b.g.advance = float(advance);
  return b;
}


// The atlas of a font file's bytes.
inline bool BakeFont(const std::vector<uint8_t>& ttf, Font& f, std::string* error = nullptr) {
  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0))) {
    if (error) *error = "not a font stb_truetype reads";
    return false;
  }
  const float scale = stbtt_ScaleForPixelHeight(&info, kBaseSize);
  int ascent, descent, lineGap;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
  int x0, y0, x1, y1;
  stbtt_GetCodepointBox(&info, 'H', &x0, &y0, &x1, &y1);
  const float capHeight = y1 * scale;

  // the font's glyphs: printable ASCII and a few marks the menus use
  std::vector<int32_t> codepoints;
  for (int32_t c = 32; c < 127; ++c) codepoints.push_back(c);
  // BlazingMount (game titles): Latin-1 + Latin Extended-A (Tokon's o-macron, Pokemon's e-acute, (R) (TM)) and the marks
  for (int32_t c = 0xA0; c < 0x180; ++c) if (stbtt_FindGlyphIndex(&info, c)) codepoints.push_back(c);
  for (int32_t c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2122, 0x2190, 0x2192, 0x221E}) {
    if (stbtt_FindGlyphIndex(&info, c)) codepoints.push_back(c);
  }
  std::vector<Bitmap> bitmaps;
  for (int32_t c : codepoints) {
    Bitmap b;
    int advance, lsb, w = 0, h = 0, xoff = 0, yoff = 0;
    stbtt_GetCodepointHMetrics(&info, c, &advance, &lsb);
    unsigned char* sdf = stbtt_GetCodepointSDF(&info, scale, c, kSpread, uint8_t(kOnEdge), kPerPixel, &w, &h, &xoff, &yoff);
    b.g.codepoint = c;
    b.g.advance = advance * scale;
    if (sdf) {
      b.g.w = uint16_t(w), b.g.h = uint16_t(h);
      b.g.xoff = float(xoff), b.g.yoff = float(yoff);
      b.px.assign(sdf, sdf + size_t(w) * h);
      stbtt_FreeSDF(sdf, nullptr);
    }
    bitmaps.push_back(std::move(b));
  }
  // our glyphs: a square a little taller than the capitals, standing on the baseline (dropping 3 px below it)
  const int shape = int(std::lround(capHeight * 1.18f)), drop = 3, adv = shape + 4;
  for (int32_t c : {mf::ui::kGlyphCross, mf::ui::kGlyphCircle, mf::ui::kGlyphSquare, mf::ui::kGlyphTriangle,
                    mf::ui::kGlyphDisc, mf::ui::kGlyphRing})
    bitmaps.push_back(DrawShape(c, shape, drop, adv));
  for (int k = 1; k <= 9; ++k) bitmaps.push_back(DrawShape(mf::ui::ArrowGlyph(k), shape, drop, adv));
  {
    Bitmap solid = DrawShape(mf::ui::kGlyphSolid, 4, 0, 0);
    bitmaps.push_back(std::move(solid));
  }

  // shelf packing, tallest first (ties by codepoint), one pixel apart
  std::vector<Bitmap*> order;
  for (Bitmap& b : bitmaps) order.push_back(&b);
  std::stable_sort(order.begin(), order.end(), [](const Bitmap* a, const Bitmap* b) {
    return a->g.h != b->g.h ? a->g.h > b->g.h : a->g.codepoint < b->g.codepoint;
  });
  int x = 1, y = 1, shelf = 0;
  for (Bitmap* b : order) {
    if (b->g.w == 0) continue;
    if (x + b->g.w + 1 > kAtlasWidth) {
      x = 1;
      y += shelf + 1;
      shelf = 0;
    }
    b->g.x = uint16_t(x), b->g.y = uint16_t(y);
    x += b->g.w + 1;
    shelf = std::max(shelf, int(b->g.h));
  }
  const int height = (y + shelf + 1 + 3) / 4 * 4;

  f = Font{};
  f.width = kAtlasWidth;
  f.height = uint32_t(height);
  f.baseSize = kBaseSize;
  f.spread = float(kSpread);
  f.ascent = ascent * scale;
  f.descent = descent * scale;
  f.lineGap = lineGap * scale;
  f.capHeight = capHeight;
  f.pixels.assign(size_t(f.width) * f.height, 0);
  for (const Bitmap& b : bitmaps) {
    for (int r = 0; r < b.g.h; ++r)
      std::copy_n(&b.px[size_t(r) * b.g.w], b.g.w, &f.pixels[size_t(b.g.y + r) * f.width + b.g.x]);
    f.glyphs.push_back(b.g);
  }
  std::sort(f.glyphs.begin(), f.glyphs.end(), [](const Glyph& a, const Glyph& b) { return a.codepoint < b.codepoint; });
  // kerning between the font's own glyphs (the GPOS / kern pairs stb_truetype reads), only the pairs that move
  for (int32_t a : codepoints)
    for (int32_t b : codepoints) {
      const int k = stbtt_GetCodepointKernAdvance(&info, a, b);
      if (k) f.kerns.push_back({a, b, k * scale});
    }

  return true;
}

}  // namespace mf::ui::bake
