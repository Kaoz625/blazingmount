// SPDX-License-Identifier: GPL-3.0-or-later
// preview.cpp - pictures of the BlazingMount app's screens on the MacBook: the same App (src/bm_app.h) and the same quad batch
// the PS5 draws, put through the software raster (src/ui/raster.h = the console shader's maths).
//
//   app/host/preview.sh [outdir]                      build + every shot from app/fixtures (real API answers)
//   preview <font.sdf> <fixtures dir> <outdir> [shot...]
//   BM_HOSTDATA=http://127.0.0.1:4451 app/host/preview.sh   the same shots against the LIVE library + covers on imgserve
//
// Each shot walks the app with the presses a person would make, so a picture is also a check that the path to it works.
// It also checks the rules every frame: no text under 20 px, nothing past the right gutter, no unknown {TOKENS}.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/bm_app.h"
#include "../src/ui/raster.h"

namespace {

std::string Read(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}

// BM_HOSTDATA=http://127.0.0.1:4451 reads the host's LIVE files from imgserve (the routes blazingmountd passes through:
// /library.json, /covers.json, /covers/<thumb|full|sheet>/<id>.jpg); a folder path reads the same names from disk.
std::string HostData(const std::string& base, const std::string& rel) {
  if (base.rfind("http://", 0) != 0) return Read(base + "/" + rel);
  const bool top = rel.size() > 5 && rel.compare(rel.size() - 5, 5, ".json") == 0;      // library / covers / discover / wishlist .json
  std::string url = base + "/" + (top ? rel : "covers/" + rel);
  if (url.find_first_of("'\\ \n") != std::string::npos) return "";
  std::string body;
  if (FILE* p = popen(("curl -s -f --max-time 30 '" + url + "'").c_str(), "r")) {
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, p)) > 0;) body.append(buf, n);
    if (pclose(p) != 0) body.clear();
  }
  return body;
}

struct Rig {
  bm::App app;
  std::string dir;
  double t = 1000;
  bool offline = false;
  // answers every request from the fixtures, as the API would
  void Pump() {
    for (int round = 0; round < 6; ++round) {
      app.Tick(t);
      auto reqs = app.TakeRequests();
      if (reqs.empty()) break;
      for (auto& r : reqs) {
        std::string base = r.path.substr(0, r.path.find('?'));
        std::string file = base == "/api/status" ? "status.json" : base == "/api/library" ? "library.json" : base == "/api/settings" ? "settings.json"
                         : base == "/api/log" ? "log.json" : "";
        if (offline) { app.OnResponse(r.path, 0, ""); continue; }
        if (base == "/api/catalog" || base == "/api/covers.json" || r.path.rfind("/api/cover/", 0) == 0 || base == "/api/discover"
            || base == "/api/wishlist") {   // BM_HOSTDATA: the host's files
          const char* d = std::getenv("BM_HOSTDATA");
          std::string rel = base == "/api/catalog" ? "library.json" : base == "/api/covers.json" ? "covers.json" : base == "/api/discover" ? "discover.json"
                          : base == "/api/wishlist" ? "wishlist.json" : r.path.substr(11);
          std::string data = d ? HostData(d, rel) : "";
          app.OnResponse(r.path, data.empty() ? 404 : 200, data);
          continue;
        }
        if (r.path.rfind("/api/icon/", 0) == 0) {   // covers: BM_ICONS=<dir of TID.png> (never committed: game art)
          const char* d = std::getenv("BM_ICONS");
          std::string png = d ? Read(std::string(d) + "/" + r.path.substr(10) + ".png") : "";
          app.OnResponse(r.path, png.empty() ? 404 : 200, png);
          continue;
        }
        if (r.method == "POST") { app.OnResponse(r.path, 200, "{\"ok\":true}"); continue; }
        std::string body = file.empty() ? "" : Read(dir + "/" + file);
        app.OnResponse(r.path, body.empty() ? 404 : 200, body);
      }
    }
  }
  void Press(bm::Button b) { app.Press(b); t += 0.2; Pump(); }
  void Wait(double s) { t += s; Pump(); for (int i = 0; i < 30; ++i) { t += 0.05; Pump(); } }   // covers trickle in two at a time
};

int Shot(const mf::ui::Font& font, Rig& rig, const std::string& out) {
  mf::ui::Batch batch;
  std::string transcript;
  batch.transcript = &transcript;
  batch.Reset(&font, mf::ui::PadStyle::kPlayStation);
  rig.app.Draw(batch, rig.t);
  mf::ui::Image img;
  img.width = 1920;
  img.height = 1080;
  img.Fill(bm::c::kBg);
  mf::ui::ImageAtlas covers{rig.app.covers().rgba.data(), bm::Covers::kSize, bm::Covers::kSize};
  mf::ui::Raster(font, batch.vertices, img, &covers);
  int bad = 0;
  if (batch.minTextSize < 20) { std::printf("  FAIL %s: text at %.0f px (< 20)\n", out.c_str(), batch.minTextSize); ++bad; }
  if (batch.maxTextRight > 1830.5f) { std::printf("  FAIL %s: text reaches x=%.0f (> 1830)\n", out.c_str(), batch.maxTextRight); ++bad; }
  if (batch.unknownTokens) { std::printf("  FAIL %s: %d unknown {TOKENS}\n", out.c_str(), batch.unknownTokens); ++bad; }
  if (batch.vertices.size() > 16384 * 6) { std::printf("  FAIL %s: %zu vertices (> the PS5 UI pass's buffer)\n", out.c_str(), batch.vertices.size()); ++bad; }
  img.WritePpm((out + ".ppm").c_str());
  std::ofstream(out + ".txt") << transcript;
  std::printf("  %s: %zu quads, %d texts%s\n", out.c_str(), batch.vertices.size() / 6, batch.texts, bad ? "" : ", rules ok");
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: preview <font.sdf> <fixtures dir> <outdir> [shot...]\n"); return 2; }
  std::string fontBytes = Read(argv[1]);
  mf::ui::Font font;
  if (!mf::ui::LoadFont(reinterpret_cast<const uint8_t*>(fontBytes.data()), fontBytes.size(), font)) { std::fprintf(stderr, "bad font\n"); return 2; }
  const std::string fx = argv[2], out = argv[3];
  std::vector<std::string> want(argv + 4, argv + argc);
  auto on = [&](const char* n) { return want.empty() || std::find(want.begin(), want.end(), n) != want.end(); };
  int bad = 0;
  using namespace bm;
  if (on("01-library")) { Rig r; r.dir = fx; r.Wait(0); bad += Shot(font, r, out + "/01-library"); }
  if (on("02-library-scrolled")) { Rig r; r.dir = fx; r.Wait(0); for (int i = 0; i < 11; ++i) r.Press(kDown); bad += Shot(font, r, out + "/02-library-scrolled"); }
  if (on("03-not-installed")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR2); r.Press(kR2); bad += Shot(font, r, out + "/03-not-installed"); }
  if (on("04-needs-attention")) { Rig r; r.dir = fx; r.Wait(0); for (int i = 0; i < 3; ++i) r.Press(kR2); bad += Shot(font, r, out + "/04-needs-attention"); }
  if (on("05-sort-status")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kOptions); r.Press(kOptions); bad += Shot(font, r, out + "/05-sort-status"); }
  if (on("06-remove-confirm")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kDown); r.Press(kCross); bad += Shot(font, r, out + "/06-remove-confirm"); }
  if (on("07-install-toast")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR2); r.Press(kR2); r.Press(kCross); bad += Shot(font, r, out + "/07-install-toast"); }
  if (on("08-activity")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Press(kR1); r.Wait(2.5); bad += Shot(font, r, out + "/08-activity"); }
  if (on("09-settings")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kL1); r.Wait(0.5); for (int i = 0; i < 5; ++i) r.Press(kDown); r.Press(kRight); bad += Shot(font, r, out + "/09-settings"); }
  if (on("10-not-running")) { Rig r; r.dir = fx; r.offline = true; r.Wait(0); r.Wait(3); bad += Shot(font, r, out + "/10-not-running"); }
  if (on("11-all-grid")) { Rig r; r.dir = fx; r.Wait(0); r.Wait(1); for (int i = 0; i < 6; ++i) r.Press(kRight); r.Wait(1); bad += Shot(font, r, out + "/11-all-grid"); }
  if (on("12-ps2-grid")) { Rig r; r.dir = fx; r.Wait(0); for (int i = 0; i < 4; ++i) r.Press(kR2); r.Press(kDown); r.Press(kRight); r.Wait(1); bad += Shot(font, r, out + "/12-ps2-grid"); }
  if (on("13-nintendo-grid")) { Rig r; r.dir = fx; r.Wait(0); for (int i = 0; i < 7; ++i) r.Press(kR2); r.Press(kDown); r.Press(kDown); r.Wait(1); bad += Shot(font, r, out + "/13-nintendo-grid"); }
  if (on("14-on-drives-list")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kOptions); r.Press(kOptions); r.Press(kTouch); r.Press(kDown); r.Wait(1); bad += Shot(font, r, out + "/14-on-drives-list"); }
  if (on("15-retro-grid")) { Rig r; r.dir = fx; r.Wait(0); for (int i = 0; i < 11; ++i) r.Press(kR2); r.Press(kDown); r.Wait(1); bad += Shot(font, r, out + "/15-retro-grid"); }
  // DISCOVER (R1 from the Library): shelves on L2 / R2, systems on OPTIONS, TRIANGLE = wishlist
  if (on("16-discover-new")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Wait(1); r.Press(kRight); r.Wait(1); bad += Shot(font, r, out + "/16-discover-new"); }
  if (on("17-discover-soon")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Wait(0.5); r.Press(kR2); r.Press(kRight); r.Press(kRight); r.Wait(1);
                                bad += Shot(font, r, out + "/17-discover-soon"); }
  if (on("18-discover-wish-toast")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Wait(0.5); r.Press(kR2); r.Press(kDown); r.Wait(0.5); r.Press(kTriangle);
                                      bad += Shot(font, r, out + "/18-discover-wish-toast"); }
  if (on("19-discover-notable-system")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Wait(0.5); for (int i = 0; i < 3; ++i) r.Press(kR2);
                                          for (int i = 0; i < 4; ++i) r.Press(kOptions); r.Wait(1); bad += Shot(font, r, out + "/19-discover-notable-system"); }
  if (on("20-discover-wishlist")) { Rig r; r.dir = fx; r.Wait(0); r.Press(kR1); r.Wait(0.5); r.Press(kL2); r.Wait(1); bad += Shot(font, r, out + "/20-discover-wishlist"); }
  std::printf(bad ? "%d FAIL\n" : "ALL SHOTS OK\n", bad);
  return bad ? 1 : 0;
}
