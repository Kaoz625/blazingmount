// font.h - the game's text: a signed-distance-field atlas of one font (Barlow Condensed Bold, OFL) plus our own
// drawn glyphs (the PlayStation shapes, discs, the eight stick directions), baked once on the MacBook by
// tools/ui/bake_font.cpp into assets/ui/mf-font.sdf, and read here on every platform (PS5, Xbox, host tests).
//
// One atlas serves every size: a pixel holds the distance to the glyph's edge (128 = on the edge, more = inside,
// 128/spread per pixel at the bake size), so a shader cuts the edge at any scale and can add an outline for free.
//
// File (little-endian):
//   "MFSDF1\0\0"  u32 width, height, glyphCount, kernCount   f32 baseSize, spread, ascent, descent, lineGap, capHeight
//   glyphCount x { i32 codepoint; u16 x, y, w, h; f32 xoff, yoff, advance }   (pixels at baseSize; yoff from the
//                                                                              baseline down to the bitmap's top)
//   kernCount  x { i32 first, second; f32 adjust }                            (sorted by first, second)
//   width x height bytes: the atlas, row by row
// Pure C++ (no platform types), header-only.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mf::ui {

// Our own glyphs, in Unicode's private-use area. Text names them with {TOKENS} (text.h).
enum : int32_t {
  kGlyphSolid = 0xE000,      // every pixel inside: a solid rectangle's texels
  kGlyphCross = 0xE001,      // PlayStation shapes, outlined
  kGlyphCircle = 0xE002,
  kGlyphSquare = 0xE003,
  kGlyphTriangle = 0xE004,
  kGlyphDisc = 0xE005,       // a filled circle (the Xbox's face buttons, round pips)
  kGlyphRing = 0xE006,       // an empty round pip
  kGlyphArrow1 = 0xE011,     // the stick: numpad 1..9 (0xE015 = neutral, a dot), as the screen shows them
  kGlyphArrow9 = 0xE019,
};
constexpr int32_t ArrowGlyph(int numpad) { return kGlyphArrow1 + (numpad - 1); }

struct Glyph {
  int32_t codepoint = 0;
  uint16_t x = 0, y = 0, w = 0, h = 0;   // in the atlas
  float xoff = 0, yoff = 0, advance = 0; // pixels at the bake size
};

struct Kern {
  int32_t first, second;
  float adjust;
};

struct Font {
  uint32_t width = 0, height = 0;
  float baseSize = 48, spread = 6, ascent = 0, descent = 0, lineGap = 0, capHeight = 0;
  std::vector<Glyph> glyphs;   // sorted by codepoint
  std::vector<Kern> kerns;     // sorted by (first, second)
  std::vector<uint8_t> pixels;

  bool ok() const { return width > 0 && height > 0 && !glyphs.empty() && pixels.size() == size_t(width) * height; }

  const Glyph* Find(int32_t cp) const {
    auto it = std::lower_bound(glyphs.begin(), glyphs.end(), cp, [](const Glyph& g, int32_t c) { return g.codepoint < c; });
    return it != glyphs.end() && it->codepoint == cp ? &*it : nullptr;
  }
  float KernOf(int32_t a, int32_t b) const {
    auto it = std::lower_bound(kerns.begin(), kerns.end(), Kern{a, b, 0}, [](const Kern& x, const Kern& y) {
      return x.first != y.first ? x.first < y.first : x.second < y.second;
    });
    return it != kerns.end() && it->first == a && it->second == b ? it->adjust : 0.0f;
  }
};

constexpr char kFontMagic[8] = {'M', 'F', 'S', 'D', 'F', '1', 0, 0};

namespace detail {
struct Reader {
  const uint8_t* p;
  size_t left;
  bool good = true;
  template <typename T>
  T Get() {
    T v{};
    if (left < sizeof(T)) { good = false; return v; }
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    left -= sizeof(T);
    return v;
  }
};
struct Writer {
  std::vector<uint8_t>& out;
  template <typename T>
  void Put(const T& v) {
    const auto* b = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), b, b + sizeof(T));
  }
};
}  // namespace detail

// The file's bytes into a Font; false (and an empty font) when they are not a whole atlas.
inline bool LoadFont(const uint8_t* data, size_t size, Font& f) {
  f = Font{};
  if (size < 8 || std::memcmp(data, kFontMagic, 8) != 0) return false;
  detail::Reader r{data + 8, size - 8};
  const uint32_t w = r.Get<uint32_t>(), h = r.Get<uint32_t>(), n = r.Get<uint32_t>(), k = r.Get<uint32_t>();
  f.baseSize = r.Get<float>();
  f.spread = r.Get<float>();
  f.ascent = r.Get<float>();
  f.descent = r.Get<float>();
  f.lineGap = r.Get<float>();
  f.capHeight = r.Get<float>();
  if (!r.good || w == 0 || h == 0 || w > 8192 || h > 8192 || n > 4096 || k > 65536) return false;
  f.glyphs.resize(n);
  for (Glyph& g : f.glyphs) {
    g.codepoint = r.Get<int32_t>();
    g.x = r.Get<uint16_t>(), g.y = r.Get<uint16_t>(), g.w = r.Get<uint16_t>(), g.h = r.Get<uint16_t>();
    g.xoff = r.Get<float>(), g.yoff = r.Get<float>(), g.advance = r.Get<float>();
    if (uint32_t(g.x) + g.w > w || uint32_t(g.y) + g.h > h) r.good = false;
  }
  f.kerns.resize(k);
  for (Kern& e : f.kerns) {
    e.first = r.Get<int32_t>(), e.second = r.Get<int32_t>(), e.adjust = r.Get<float>();
  }
  if (!r.good || r.left != size_t(w) * h) {
    f = Font{};
    return false;
  }
  f.width = w, f.height = h;
  f.pixels.assign(r.p, r.p + r.left);
  return f.ok();
}

inline std::vector<uint8_t> SaveFont(const Font& f) {
  std::vector<uint8_t> out(kFontMagic, kFontMagic + 8);
  detail::Writer w{out};
  w.Put(f.width), w.Put(f.height), w.Put(uint32_t(f.glyphs.size())), w.Put(uint32_t(f.kerns.size()));
  w.Put(f.baseSize), w.Put(f.spread), w.Put(f.ascent), w.Put(f.descent), w.Put(f.lineGap), w.Put(f.capHeight);
  for (const Glyph& g : f.glyphs) {
    w.Put(g.codepoint), w.Put(g.x), w.Put(g.y), w.Put(g.w), w.Put(g.h), w.Put(g.xoff), w.Put(g.yoff), w.Put(g.advance);
  }
  for (const Kern& e : f.kerns) w.Put(e.first), w.Put(e.second), w.Put(e.adjust);
  out.insert(out.end(), f.pixels.begin(), f.pixels.end());
  return out;
}

}  // namespace mf::ui
