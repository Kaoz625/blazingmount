// bake_font.cpp - makes assets/ui/mf-font.sdf, the game's one text atlas (src/ui/font.h says what is in it).
//
//   tools/ui/bake_font.sh                 build this and write assets/ui/mf-font.sdf
//   bake_font <font.ttf> <out.sdf>        the tool itself
//
// The font's glyphs come from stb_truetype's signed-distance-field rasteriser (third_party/stb, pinned); our own
// glyphs (the PlayStation shapes, discs, the stick's eight directions) are exact distance functions, so they are as
// sharp as the letters at every size. The same font file always gives the same bytes (tests/ui_test.cpp bakes again
// and compares with the committed atlas). The baking itself is tools/ui/bake.h.
#include "bake.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: bake_font <font.ttf> <out.sdf>\n");
    return 2;
  }
  FILE* in = std::fopen(argv[1], "rb");
  if (!in) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> ttf;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) ttf.insert(ttf.end(), buf, buf + n);
  std::fclose(in);
  mf::ui::Font f;
  std::string error;
  if (!mf::ui::bake::BakeFont(ttf, f, &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  const std::vector<uint8_t> bytes = mf::ui::SaveFont(f);
  FILE* out = std::fopen(argv[2], "wb");
  if (!out || std::fwrite(bytes.data(), 1, bytes.size(), out) != bytes.size()) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  std::fclose(out);
  std::printf("%s: %ux%u atlas, %zu glyphs (%zu of the font, %zu ours), %zu kerning pairs, cap height %.1f px at %.0f px, %zu bytes\n",
              argv[2], f.width, f.height, f.glyphs.size(), size_t(std::count_if(f.glyphs.begin(), f.glyphs.end(), [](const mf::ui::Glyph& g) { return g.codepoint < 0xE000; })), size_t(std::count_if(f.glyphs.begin(), f.glyphs.end(), [](const mf::ui::Glyph& g) { return g.codepoint >= 0xE000; })),
              f.kerns.size(), f.capHeight, f.baseSize, bytes.size());
  return 0;
}
