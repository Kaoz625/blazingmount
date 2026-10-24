// SPDX-License-Identifier: GPL-3.0-or-later
// bm_app.h - the BlazingMount app: every screen, the controller, and what it asks blazingmountd (bm_api.c, 127.0.0.1:8094).
//
// Pure C++ and header-only: no sockets, no GPU, no pad here. A platform shell (ps5/blazingmount_app.cpp on the console,
// host/preview.cpp on the MacBook) does three things a frame:
//   1. app.Press(button) for each new press (it does the key repeat),
//   2. runs app.TakeRequests() (GET/POST to the API) on a worker and hands each answer to app.OnResponse(path, code, body)
//      (code 0 = nothing answered),
//   3. app.Draw(batch, now) into draw.h's quad batch, drawn by ui_vk on the PS5 and by raster.h on the MacBook.
// The look is DESIGN-V2 "ONE LIGHT" (roku channels/DESIGN-V2.md): one black, one red focus, one green, Barlow, 90 px gutters.
// Layout and controls: docs/UI-DESIGN.md.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "covers.h"
#include "json.h"
#include "ui/draw.h"

namespace bm {

using mf::ui::Align;
using mf::ui::Batch;
using mf::ui::Hex;
using mf::ui::Rgba;
using mf::ui::TextStyle;

// ------------------------------------------------------------------ ONE LIGHT tokens (DESIGN-V2 2.5), made opaque over bg
namespace c {
constexpr Rgba kBg = Hex(0x0A0A0B), kSurface = Hex(0x0E0A0C), kSurfaceHi = Hex(0x171012);
constexpr Rgba kLine = Hex(0x3B1417);                    // #FF3D47 @ 20% over bg
constexpr Rgba kTextHi = Hex(0xFFFFFF), kText = Hex(0xCDCDCD), kTextDim = Hex(0x9C9C9D);   // white @ 100 / 80 / 60 % over bg
constexpr Rgba kAccent = Hex(0xFF3D47), kFocusFill = Hex(0xC9323A);   // #FF3D47 @ 78% over bg
constexpr Rgba kGood = Hex(0x4CD48A), kWarn = Hex(0xFFB84D);
}  // namespace c

// type: Barlow (Condensed Bold, the atlas we bake); sizes are DESIGN-V2's HEAD / BODY / META, a step up for the condensed face
constexpr float kHead = 40, kBody = 28, kMeta = 24, kWhisper = 21;
constexpr float kGutter = 90, kEdge = 1830, kBottom = 1010;

enum Button { kUp, kDown, kLeft, kRight, kCross, kCircle, kSquare, kTriangle, kL1, kR1, kL2, kR2, kOptions, kTouch };

struct Request {
  std::string method, path;   // "GET" / "POST"; path with its query
};

struct Game {
  std::string tid, title, platform, key, version, fw, format, file, local, localPath, state, choice, build, reason, proof;
  long long size = 0;
  bool compressed = false, onDrives = false, pinned = false;
  int tries = 0;
};

struct Setting {
  std::string key, type, value, help, edit;   // edit: the value being changed with Left/Right (empty = none)
};

enum class Tab { kLibrary, kActivity, kSettings };
enum class Filter { kAll, kOnConsole, kNotInstalled, kProblems, kHidden, kPs5, kPs4, kCount };   // (kept for the preview's old shots)

// The whole library (Markus 24 Oct: everything he owns, every system): L2/R2 walk the systems, OPTIONS what to show, TOUCH PAD grid/list
enum class Group { kAll, kPs5, kPs4, kPs3, kPs2, kPsClassic, kSwitch, kNintendo, kXbox, kPc, kSega, kRetro, kMobile, kCount };
enum class Show { kEverything, kPlayable, kOnDrives, kOnConsole, kNotOnDrives, kAttention, kHidden, kCount };
inline const char* GroupName(Group g) {
  static const char* n[] = {"All", "PS5", "PS4", "PS3", "PS2", "PS1 / PSP / Vita", "Switch", "Nintendo", "Xbox", "PC", "Sega", "Retro", "Mobile"};
  return n[int(g)];
}
inline const char* ShowName(Show s) {
  static const char* n[] = {"Everything I own", "Playable now", "On my drives", "On this console", "Not on my drives", "Needs attention", "Hidden"};
  return n[int(s)];
}
inline Group GroupOf(const std::string& sys, const std::string& family) {
  if (sys == "ps5") return Group::kPs5;
  if (sys == "ps4") return Group::kPs4;
  if (sys == "ps3") return Group::kPs3;
  if (sys == "ps2") return Group::kPs2;
  if (sys == "ps1" || sys == "psp" || sys == "vita") return Group::kPsClassic;
  if (sys == "switch" || sys == "switch2") return Group::kSwitch;
  if (family == "Nintendo") return Group::kNintendo;
  if (family == "Xbox") return Group::kXbox;
  if (family == "PC") return Group::kPc;
  if (family == "Sega") return Group::kSega;
  if (family == "Mobile") return Group::kMobile;
  return Group::kRetro;
}

// One game of the library: an entry of the host's /library.json, joined (by title id) to this console's PS5 row when it has one
struct Item {
  std::string id, title, system, sysLabel, family, how, plays, ids, firstPath, firstType, drive;
  long long size = 0;
  int files = 0;
  bool compressed = false, verified = false, streamable = false;
  int game = -1;            // index into the console's /api/library rows (-1: not a PS5 game this console tracks)
  Group group = Group::kRetro;
  Rgba colour{23, 16, 18, 255};
  int page = -1, cell = -1; // where its cover is in the sprite sheets (-1: no cover)
  std::string coverKey;     // the atlas key: the catalog id, or the title id (icon0) without a catalog
};

inline std::string Gb(long long bytes) {
  char t[32];
  if (bytes <= 0) return "-";
  if (bytes >= 100LL * 1000 * 1000 * 1000) std::snprintf(t, sizeof t, "%.0f GB", bytes / 1e9);
  else if (bytes >= 1000LL * 1000 * 1000) std::snprintf(t, sizeof t, "%.1f GB", bytes / 1e9);
  else std::snprintf(t, sizeof t, "%.0f MB", bytes / 1e6);
  return t;
}
inline std::string FreeText(long long mb) {
  char t[32];
  if (mb < 0) return "";
  if (mb >= 1024 * 1024) std::snprintf(t, sizeof t, "%.1f TB free", mb / 1048576.0);
  else std::snprintf(t, sizeof t, "%.0f GB free", mb / 1024.0);
  return t;
}

// What a game's state is called on screen, and how loud its badge is
struct Badge {
  const char* text;
  int tone;   // 0 dim outline, 1 green outline (good), 2 red outline (needs you), 3 solid red (live: building/playing)
};
inline Badge BadgeOf(const Game& g) {
  const std::string& s = g.state;
  if (s == "playing") return {"PLAYING", 3};
  if (s == "building") return {"BUILDING", 3};
  if (s == "installed") return {g.local == "starter" ? "STREAMING" : g.local == "image" ? "IMAGE" : "FULL COPY", 1};
  if (s == "outdated") return {"OLD IMAGE", 2};
  if (s == "orphan") return {"NOT ON DRIVES", 2};
  if (s == "queued") return {"QUEUED", 0};
  if (s == "available") return {"AVAILABLE", 0};
  if (s == "hidden") return {"HIDDEN", 0};
  if (s == "needs_fw") return {"NEEDS NEWER FW", 2};
  if (s == "refused") return {"CAN'T STREAM", 2};
  if (s == "failed") return {"FAILED", 2};
  return {"?", 0};
}
inline Badge ItemBadge(const Item& it, const std::vector<Game>& games) {
  if (it.game >= 0 && games[size_t(it.game)].state != "available" && games[size_t(it.game)].state != "queued") return BadgeOf(games[size_t(it.game)]);
  if (it.streamable) return {"STREAMS", 1};
  if (it.files) return {"ON MY DRIVES", 1};
  if (it.how == "ps_plus") return {"PS PLUS", 0};
  if (it.how == "licence") return {"LICENCE", 0};
  return {"OWNED", 0};
}
inline int StateRank(const Game& g) {   // the Status sort: live first, then what needs you, then installed, then the rest
  static const char* order[] = {"playing", "building", "outdated", "orphan", "failed", "refused", "queued", "installed", "available", "needs_fw", "hidden"};
  for (int i = 0; i < 11; ++i) if (g.state == order[i]) return i;
  return 11;
}
inline bool IsProblem(const Game& g) {
  return g.state == "outdated" || g.state == "orphan" || g.state == "failed" || g.state == "refused" || g.state == "needs_fw";
}

class App {
 public:
  // ---------------------------------------------------------------- the API's answers
  void OnResponse(const std::string& path, int code, const std::string& body) {
    inflight_.erase(std::remove(inflight_.begin(), inflight_.end(), Base(path)), inflight_.end());
    json::Value v;
    const bool ok = code >= 200 && code < 300 && json::Parse(body, v);
    if (Base(path) == "/api/status") {
      if (!ok) { if (++misses_ >= 2) connected_ = false; return; }
      misses_ = 0;
      connected_ = true;
      status_ = v;
      gen_ = (unsigned long)v["gen"].ll();
      if (v["catalog_etag"].str() != statusCatalogEtag_) statusCatalogEtag_ = v["catalog_etag"].str();
      const json::Value& rec = v["recent"];
      if (rec.size()) {
        const std::string last = rec[rec.size() - 1].str();
        if (last != lastRecent_) { if (!lastRecent_.empty() || seenStatus_) Toast(last); lastRecent_ = last; }
      }
      seenStatus_ = true;
    } else if (Base(path) == "/api/library") {
      if (!ok) return;
      libGen_ = (unsigned long)v["gen"].ll();
      hostOk_ = v["host_ok"].truthy();
      auto_ = v["auto"].truthy();
      consoleFw_ = v["console_fw"].str();
      std::string keep = FocusedItem() ? FocusedItem()->id : "";
      games_.clear();
      for (const auto& j : v["games"].items) {
        Game g;
        g.tid = j["tid"].str(); g.title = j["title"].str(); g.platform = j["platform"].str(); g.key = j["key"].str();
        g.version = j["version"].str(); g.fw = j["fw"].str(); g.format = j["format"].str(); g.file = j["file"].str();
        g.local = j["local"].str(); g.localPath = j["local_path"].str(); g.state = j["state"].str(); g.choice = j["choice"].str();
        g.build = j["build"].str(); g.reason = j["reason"].str(); g.proof = j["proof"].str();
        g.size = j["size"].ll(); g.compressed = j["compressed"].truthy(); g.onDrives = j["on_drives"].truthy();
        g.pinned = j["pinned"].truthy(); g.tries = (int)j["tries"].ll();
        games_.push_back(std::move(g));
      }
      haveLibrary_ = true;
      RebuildItems(keep);
    } else if (Base(path) == "/api/settings") {
      if (!ok) return;
      auto_ = v["auto"].truthy();
      std::vector<Setting> s;
      for (const auto& j : v["keys"].items) s.push_back({j["key"].str(), j["type"].str(), j["value"].str(), j["help"].str(), ""});
      settings_ = s;
      settingsGen_ = gen_;
    } else if (Base(path) == "/api/log") {
      if (!ok) return;
      log_.clear();
      for (const auto& l : v["lines"].items) log_.push_back(l.str());
    } else if (Base(path) == "/api/catalog") {
      if (!ok) { catalogTried_ = true; return; }
      catalog_ = std::move(v);
      catalogEtag_ = statusCatalogEtag_;
      catalogTried_ = true;
      RebuildItems(FocusedItem() ? FocusedItem()->id : "");
    } else if (Base(path) == "/api/covers.json") {
      if (!ok) return;
      sheetCells_.clear();
      coverIx_ = std::move(v);
      RebuildItems(FocusedItem() ? FocusedItem()->id : "");
    } else if (path.rfind("/api/cover/sheet/", 0) == 0) {
      const int page = std::atoi(path.c_str() + 17);
      auto it = sheetCells_.find(page);
      if (code == 200 && it != sheetCells_.end()) covers_.PutSheet(it->second, body);
      else if (it != sheetCells_.end()) for (auto& c2 : it->second) covers_.Failed(c2.first);
    } else if (path.rfind("/api/cover/full/", 0) == 0) {
      const std::string id = path.substr(16, path.size() - 16 - 4);
      if (code == 200) covers_.PutFocus(id, body);
      if (fullTried_.size() > 256) fullTried_.clear();
      fullTried_.push_back(id);
    } else if (path.rfind("/api/icon/", 0) == 0) {
      const std::string tid = path.substr(10);
      if (code == 200 && !body.empty()) covers_.Put(tid, body); else covers_.Failed(tid);
    } else if (Base(path) == "/api/action") {
      if (!ok) Toast(code == 0 ? "BlazingMount did not answer" : "Refused: " + v["error"].str());
      nextStatus_ = 0;   // see the result soon
    }
  }

  // ---------------------------------------------------------------- the controller
  void Press(Button b) {
    if (confirm_.active) { PressConfirm(b); return; }
    if (b == kL1 || b == kR1) {
      int t = (int(tab_) + (b == kR1 ? 1 : 2)) % 3;
      tab_ = Tab(t);
      if (tab_ == Tab::kSettings) settingsGen_ = 0;
      return;
    }
    switch (tab_) {
      case Tab::kLibrary: if (b == kTouch) { grid_ = !grid_; top_ = 0; return; } PressLibrary(b); break;
      case Tab::kActivity: PressActivity(b); break;
      case Tab::kSettings: PressSettings(b); break;
    }
  }

  // ---------------------------------------------------------------- what to ask (the shell runs these, one at a time is fine)
  void Tick(double now) {
    now_ = now;
    if (now >= nextStatus_ && !Inflight("/api/status")) { Ask("GET", "/api/status"); nextStatus_ = now + (connected_ ? 1.0 : 2.0); }
    if (connected_ && (!haveLibrary_ || libGen_ != gen_) && !Inflight("/api/library") && now >= nextLibrary_) {
      Ask("GET", "/api/library"); nextLibrary_ = now + 0.5;
    }
    if (connected_ && tab_ == Tab::kSettings && settingsGen_ != gen_ && !Inflight("/api/settings")) Ask("GET", "/api/settings");
    // the whole library (host /library.json through the payload): at start, and again when its ETag moves (drives / owned list changed)
    if (connected_ && !Inflight("/api/catalog") && ((!catalogTried_) || (!statusCatalogEtag_.empty() && statusCatalogEtag_ != catalogEtag_))
        && now >= nextCatalog_) {
      Ask("GET", "/api/catalog"); Ask("GET", "/api/covers.json"); nextCatalog_ = now + 10.0;
    }
    if (connected_ && tab_ == Tab::kLibrary) RequestCovers(now);
    if (connected_ && tab_ == Tab::kActivity && now >= nextLog_ && !Inflight("/api/log")) { Ask("GET", "/api/log?lines=80"); nextLog_ = now + 2.0; }
  }
  std::vector<Request> TakeRequests() { std::vector<Request> r; r.swap(out_); return r; }

  // ---------------------------------------------------------------- drawing (1920 x 1080 layout pixels)
  void Draw(Batch& b, double now) {
    now_ = now;
    b.Rect(0, 0, 1920, 1080, c::kBg);
    DrawTop(b);
    if (!connected_) { DrawNotConnected(b); DrawToast(b); return; }
    switch (tab_) {
      case Tab::kLibrary: DrawLibrary(b); break;
      case Tab::kActivity: DrawActivity(b); break;
      case Tab::kSettings: DrawSettings(b); break;
    }
    if (confirm_.active) DrawConfirm(b);
    DrawToast(b);
  }

  // for tests and the preview
  const std::vector<Game>& games() const { return games_; }
  const std::vector<int>& visible() const { return view_; }
  Tab tab() const { return tab_; }
  bool connected() const { return connected_; }
  void SetFilter(Filter) {}
  void SetGroup(Group g) { group_ = g; sel_ = 0; RebuildView(""); }
  void SetShow(Show v) { show_ = v; sel_ = 0; RebuildView(""); }
  void SetGrid(bool g) { grid_ = g; }
  size_t itemCount() const { return items_.size(); }
  bool confirming() const { return confirm_.active; }
  Covers& covers() { return covers_; }

 private:
  // ---------------------------------------------------------------- state
  Covers covers_;
  std::vector<Game> games_;      // this console's PS5 rows (/api/library)
  std::vector<Item> items_;      // the whole library (catalog joined with games_), or games_ alone without a catalog
  std::vector<int> view_;        // items_ indices after group + show
  int groupCount_[int(Group::kCount)] = {};
  json::Value catalog_, coverIx_;
  std::map<int, std::vector<std::pair<std::string, int>>> sheetCells_;   // sheet page -> (cover id, cell)
  std::vector<std::string> fullTried_;
  std::string catalogEtag_, statusCatalogEtag_, focusId_;
  bool catalogTried_ = false, grid_ = true;
  double nextCatalog_ = 0, focusSince_ = 0;
  Group group_ = Group::kAll;
  Show show_ = Show::kEverything;
  std::vector<Setting> settings_;
  std::vector<std::string> log_;
  json::Value status_;
  std::vector<Request> out_;
  std::vector<std::string> inflight_;
  Tab tab_ = Tab::kLibrary;
  int sel_ = 0, top_ = 0, setSel_ = 0, logScroll_ = 0;
  unsigned long gen_ = 0, libGen_ = ~0ul, settingsGen_ = 0;
  bool connected_ = false, haveLibrary_ = false, hostOk_ = true, auto_ = true, seenStatus_ = false;
  int misses_ = 0;
  std::string consoleFw_, lastRecent_, toast_;
  double now_ = 0, nextStatus_ = 0, nextLibrary_ = 0, nextLog_ = 0, toastUntil_ = 0;
  struct Confirm {
    bool active = false;
    std::string action, tid, title, body, yes;
  } confirm_;

  static std::string Base(const std::string& path) { auto q = path.find('?'); return q == std::string::npos ? path : path.substr(0, q); }
  bool Inflight(const std::string& p) const { return std::find(inflight_.begin(), inflight_.end(), p) != inflight_.end(); }
  void Ask(const char* m, const std::string& path) { out_.push_back({m, path}); inflight_.push_back(Base(path)); }
  void Act(const std::string& what, const std::string& tid, const std::string& extra = "") {
    Ask("POST", "/api/action?do=" + what + (tid.empty() ? "" : "&tid=" + tid) + extra);
  }
  void Toast(const std::string& t) { toast_ = t; toastUntil_ = now_ + 5.0; }
  const Item* FocusedItem() const { return sel_ >= 0 && sel_ < int(view_.size()) ? &items_[size_t(view_[size_t(sel_)])] : nullptr; }
  const Game* Focused() const { const Item* it = FocusedItem(); return it && it->game >= 0 ? &games_[size_t(it->game)] : nullptr; }

  static std::string Lower(std::string s) { for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32); return s; }
  static Rgba ParseColour(const std::string& h) {
    if (h.size() != 7 || h[0] != '#') return Rgba{23, 16, 18, 255};
    const uint32_t v = uint32_t(std::strtoul(h.c_str() + 1, nullptr, 16));
    return Rgba{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v), 255};
  }
  // catalog + this console's rows -> items_ (one per game); then the view
  void RebuildItems(const std::string& keepId) {
    items_.clear();
    std::unordered_map<std::string, int> byTid;
    for (size_t i = 0; i < games_.size(); ++i) byTid[games_[i].tid] = int(i);
    std::vector<char> used(games_.size(), 0);
    const json::Value& cov = coverIx_["covers"];
    std::unordered_map<std::string, const json::Value*> covBy;
    for (const auto& f : cov.fields) covBy[f.first] = &f.second;
    sheetCells_.clear();
    for (const auto& j : catalog_["games"].items) {
      Item it;
      it.id = j["id"].str(); it.title = j["title"].str(); it.system = j["system"].str(); it.family = j["family"].str();
      it.how = j["how"].str(); it.plays = j["plays"].str();
      for (const auto& x : j["ids"].items) { if (!it.ids.empty()) it.ids += ", "; it.ids += x.str(); }
      const json::Value& files = j["files"];
      it.files = int(files.size());
      if (it.files) {
        it.firstPath = files[size_t(0)]["path"].str(); it.firstType = files[size_t(0)]["type"].str(); it.drive = files[size_t(0)]["drive"].str();
        it.compressed = files[size_t(0)]["compressed"].truthy(); it.verified = files[size_t(0)]["verified"].truthy(); it.size = files[size_t(0)]["size"].ll();
      }
      const json::Value& st = j["stream"];
      it.streamable = st.ok() && !st["key"].str().empty();
      std::string tid = st.ok() ? st["tid"].str() : "";
      auto g = tid.empty() ? byTid.end() : byTid.find(tid);
      if (g == byTid.end()) for (const auto& x : j["ids"].items) if ((g = byTid.find(x.str())) != byTid.end()) break;
      if (g != byTid.end() && it.system == "ps5") { it.game = g->second; used[size_t(g->second)] = 1; }
      it.group = GroupOf(it.system, it.family);
      auto c2 = covBy.find(it.id);
      if (c2 != covBy.end()) {
        it.colour = ParseColour((*c2->second)["colour"].str());
        if ((*c2->second)["cover"].truthy()) {
          it.page = int((*c2->second)["page"].ll()); it.cell = int((*c2->second)["cell"].ll());
          sheetCells_[it.page].push_back({it.id, it.cell});
          it.coverKey = it.id;
        }
      }
      if (it.coverKey.empty() && it.game >= 0) it.coverKey = games_[size_t(it.game)].tid;   // icon0 from the payload
      items_.push_back(std::move(it));
    }
    for (size_t i = 0; i < games_.size(); ++i) {   // this console's games the catalog does not have (or no catalog at all)
      if (used[i]) continue;
      const Game& g = games_[i];
      Item it;
      it.id = g.tid; it.title = g.title; it.system = g.platform == "PS4" ? "ps4" : "ps5"; it.family = "PlayStation"; it.how = "on_drive";
      it.plays = "PS5: streams with BlazingMount"; it.ids = g.tid; it.files = g.onDrives ? 1 : 0; it.firstPath = g.file;
      it.firstType = g.format; it.compressed = g.compressed; it.size = g.size; it.streamable = !g.key.empty(); it.game = int(i);
      it.group = it.system == "ps4" ? Group::kPs4 : Group::kPs5; it.coverKey = g.tid;
      items_.push_back(std::move(it));
    }
    RebuildView(keepId);
  }
  bool Passes(const Item& it, Group grp) const {
    if (grp != Group::kAll && it.group != grp) return false;
    const Game* g = it.game >= 0 ? &games_[size_t(it.game)] : nullptr;
    switch (show_) {
      case Show::kEverything: return !(g && g->choice == "skip");
      case Show::kPlayable: return it.streamable || it.files > 0 || (g && g->local != "none");
      case Show::kOnDrives: return it.files > 0 || it.streamable;
      case Show::kOnConsole: return g && g->local != "none";
      case Show::kNotOnDrives: return it.files == 0 && !it.streamable;
      case Show::kAttention: return g && IsProblem(*g);
      case Show::kHidden: return g && g->choice == "skip";
      default: return true;
    }
  }
  void RebuildView(const std::string& keepId) {
    view_.clear();
    for (int k = 0; k < int(Group::kCount); ++k) groupCount_[k] = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (Passes(items_[i], Group::kAll)) { ++groupCount_[0]; ++groupCount_[int(items_[i].group)]; }
      if (Passes(items_[i], group_)) view_.push_back(int(i));
    }
    if (!keepId.empty())
      for (size_t i = 0; i < view_.size(); ++i) if (items_[size_t(view_[i])].id == keepId) sel_ = int(i);
    sel_ = std::clamp(sel_, 0, std::max(0, int(view_.size()) - 1));
  }
  // covers: the sheets of the rows on screen plus one row ahead (two requests at a time), the full cover after 0.35 s of focus
  void RequestCovers(double now) {
    int busy = 0;
    for (const auto& f : inflight_) busy += f.rfind("/api/cover/", 0) == 0 || f.rfind("/api/icon/", 0) == 0;
    const int cols = grid_ ? kCols : 1, rows = grid_ ? kGridRows : kRowsVisible;
    const int first = std::max(0, (top_ - 1) * cols), last = std::min(int(view_.size()), (top_ + rows + 1) * cols);
    for (int i = first; i < last && busy < 2; ++i) {
      const Item& it = items_[size_t(view_[size_t(i)])];
      if (it.coverKey.empty() || covers_.Known(it.coverKey)) continue;
      std::string p = it.page >= 0 ? "/api/cover/sheet/" + std::to_string(it.page) + ".jpg" : "/api/icon/" + it.coverKey;
      if (Inflight(p)) continue;
      Ask("GET", p);
      ++busy;
    }
    const Item* f = FocusedItem();
    const std::string fid = f ? f->id : "";
    if (fid != focusId_) { focusId_ = fid; focusSince_ = now; }
    if (f && f->page >= 0 && now - focusSince_ > 0.35 && covers_.focusId() != f->id &&
        std::find(fullTried_.begin(), fullTried_.end(), f->id) == fullTried_.end()) {
      std::string p = "/api/cover/full/" + f->id + ".jpg";
      if (!Inflight(p)) Ask("GET", p);
    }
  }

  // ---------------------------------------------------------------- library controls
  static constexpr float kListTop = 212, kRowH = 88, kRowGap = 8;
  static constexpr int kRowsVisible = 8;
  static constexpr int kCols = 5, kGridRows = 3;                  // the cover grid: 5 across, 3 rows on screen
  static constexpr float kCard = 184, kPitchX = 204, kPitchY = 262;
  void PressLibrary(Button b) {
    const int n = int(view_.size());
    const int step = grid_ ? kCols : 1, page = grid_ ? kCols * kGridRows : kRowsVisible;
    if (b == kUp) sel_ = sel_ - step >= 0 ? sel_ - step : sel_;
    if (b == kDown) sel_ = std::min(n - 1, sel_ + step);
    if (grid_ && b == kLeft && sel_ > 0) --sel_;
    if (grid_ && b == kRight && sel_ + 1 < n) ++sel_;
    if (!grid_ && b == kLeft) sel_ = std::max(0, sel_ - page);
    if (!grid_ && b == kRight) sel_ = std::min(std::max(0, n - 1), sel_ + page);
    if (b == kL2 || b == kR2) {
      group_ = Group((int(group_) + (b == kR2 ? 1 : int(Group::kCount) - 1)) % int(Group::kCount));
      sel_ = 0; top_ = 0; RebuildView("");
    }
    if (b == kOptions) {
      std::string keep = FocusedItem() ? FocusedItem()->id : "";
      show_ = Show((int(show_) + 1) % int(Show::kCount)); top_ = 0; RebuildView(keep);
    }
    sel_ = std::clamp(sel_, 0, std::max(0, n - 1));
    const int rowsVis = grid_ ? kGridRows : kRowsVisible, row = grid_ ? sel_ / kCols : sel_;
    top_ = std::clamp(top_, std::max(0, row - rowsVis + 1), row);
    const Item* it = FocusedItem();
    const Game* g = Focused();
    if (!it) return;
    if (!g) {
      if (b == kCross || b == kTriangle || b == kSquare)
        Toast(it->plays.empty() ? it->title + ": nothing plays it yet" : it->title + " plays on " + it->plays);
      return;
    }
    if (b == kCross) {
      if (g->state == "playing") Toast(g->title + " is running: close it first");
      else if (g->local == "starter") Ask_("remove", *g, "Remove " + g->title + "?",
                                           "Its starter is deleted from this console. The game stays on your drives: Install brings it back.", "Remove");
      else if (g->local == "full" || g->local == "image") Toast("A full copy or image: BlazingMount leaves it alone");
      else if (g->state == "needs_fw") Toast(g->title + " needs FW " + g->fw + " (this console: " + consoleFw_ + ")");
      else if (g->state == "building") Toast("Being built right now");
      else { Act("install", g->tid); Toast("Install " + g->title + ": queued"); }
    }
    if (b == kTriangle && g->local == "starter" && g->onDrives)
      Ask_("rebuild", *g, "Rebuild " + g->title + "?", "The starter is made again from the image on your drives (minutes, small download).", "Rebuild");
    if (b == kSquare) {
      if (g->choice == "skip") { Act("unhide", g->tid); Toast("Showing " + g->title + " again"); }
      else { Act("hide", g->tid); Toast("Hidden: " + g->title + " will not be built"); }
    }
  }
  void Ask_(const std::string& action, const Game& g, const std::string& title, const std::string& body, const std::string& yes) {
    confirm_ = {true, action, g.tid, title, body, yes};
  }
  void PressConfirm(Button b) {
    if (b == kCross) { Act(confirm_.action, confirm_.tid); Toast(confirm_.yes + ": queued"); confirm_.active = false; }
    if (b == kCircle) confirm_.active = false;
  }
  void PressActivity(Button b) {
    if (b == kUp) logScroll_ = std::min(logScroll_ + 1, std::max(0, int(log_.size()) - 12));
    if (b == kDown) logScroll_ = std::max(0, logScroll_ - 1);
    if (b == kCross) { Act("sync", ""); Toast("Checking the drives now"); }
  }
  void PressSettings(Button b) {
    const int n = 2 + int(settings_.size());   // row 0: new games, row 1: check now, then the keys
    if (b == kUp && setSel_ > 0) --setSel_;
    if (b == kDown && setSel_ + 1 < n) ++setSel_;
    if (setSel_ == 0 && (b == kCross || b == kLeft || b == kRight)) { Act("auto", "", std::string("&value=") + (auto_ ? "0" : "1")); auto_ = !auto_; return; }
    if (setSel_ == 1 && b == kCross) { Act("sync", ""); Toast("Checking the drives now"); return; }
    if (setSel_ < 2 || setSel_ - 2 >= int(settings_.size())) return;
    Setting& s = settings_[size_t(setSel_ - 2)];
    if (s.type == "b" && (b == kCross || b == kLeft || b == kRight)) {
      const std::string v = s.value == "0" ? "1" : "0";
      Act("set", "", "&key=" + s.key + "&value=" + v); s.value = v; return;
    }
    if (s.type == "n" && (b == kLeft || b == kRight)) {
      long long v = std::strtoll((s.edit.empty() ? s.value : s.edit).c_str(), nullptr, 10);
      long long step = v >= 100000 ? 10240 : v >= 10000 ? 1024 : v >= 1000 ? 256 : v >= 100 ? 16 : 1;
      v = std::max(0LL, v + (b == kRight ? step : -step));
      s.edit = std::to_string(v);
    }
    if (s.type == "n" && b == kCross && !s.edit.empty()) { Act("set", "", "&key=" + s.key + "&value=" + s.edit); s.value = s.edit; s.edit.clear(); }
    if (b == kCircle) s.edit.clear();
    if (s.type == "s" && b == kCross) Toast("Text settings: edit /data/blazing/mount.conf (on-screen keyboard: next version)");
  }

  // ---------------------------------------------------------------- drawing helpers
  static void Round(Batch& b, float x0, float y0, float x1, float y1, float r, Rgba col) {   // opaque colours only
    r = std::min({r, (x1 - x0) * 0.5f, (y1 - y0) * 0.5f});
    b.Rect(x0 + r, y0, x1 - r, y1, col);
    b.Rect(x0, y0 + r, x0 + r, y1 - r, col);
    b.Rect(x1 - r, y0 + r, x1, y1 - r, col);
    b.Shape(mf::ui::kGlyphDisc, x0, y0, 2 * r, col);
    b.Shape(mf::ui::kGlyphDisc, x1 - 2 * r, y0, 2 * r, col);
    b.Shape(mf::ui::kGlyphDisc, x0, y1 - 2 * r, 2 * r, col);
    b.Shape(mf::ui::kGlyphDisc, x1 - 2 * r, y1 - 2 * r, 2 * r, col);
  }
  static void Outline(Batch& b, float x0, float y0, float x1, float y1, float r, float w, Rgba line, Rgba fill) {
    Round(b, x0, y0, x1, y1, r, line);
    Round(b, x0 + w, y0 + w, x1 - w, y1 - w, std::max(0.0f, r - w), fill);
  }
  static TextStyle St(float size, Rgba col, float bias = 0) { TextStyle s; s.size = size; s.fill = col; s.bias = bias; return s; }
  // text cut to fit `w` with an ellipsis
  static std::string Fit(Batch& b, const std::string& t, const TextStyle& st, float w) {
    if (b.Width(t.c_str(), st) <= w) return t;
    std::string s = t;
    while (!s.empty()) {
      do { s.pop_back(); } while (!s.empty() && (uint8_t(s.back()) & 0xC0) == 0x80);   // whole UTF-8 characters
      if (!s.empty() && (uint8_t(s.back()) & 0xC0) == 0xC0) s.pop_back();
      std::string c2 = s + "\xE2\x80\xA6";
      if (b.Width(c2.c_str(), st) <= w) return c2;
    }
    return "";
  }
  // wraps into at most `lines` lines; returns the y after
  static float Wrap(Batch& b, float x, float y, float w, const std::string& t, const TextStyle& st, int lines, float pitch) {
    std::string line, word;
    std::vector<std::string> out;
    auto flush = [&]() { if (!line.empty()) out.push_back(line); line.clear(); };
    for (size_t i = 0; i <= t.size(); ++i) {
      char ch = i < t.size() ? t[i] : ' ';
      if (ch != ' ') { word += ch; continue; }
      if (word.empty()) continue;
      std::string tryl = line.empty() ? word : line + " " + word;
      if (b.Width(tryl.c_str(), st) <= w) line = tryl;
      else { flush(); line = word; }
      word.clear();
    }
    flush();
    for (size_t i = 0; i < out.size() && int(i) < lines; ++i) {
      std::string l = out[i];
      if (int(i) == lines - 1 && out.size() > size_t(lines)) l = Fit(b, l + " " + out[i + 1], st, w);
      b.Text(x, y, Fit(b, l, st, w).c_str(), st);
      y += pitch;
    }
    return y;
  }
  static float Pill(Batch& b, float xRight, float yMid, const char* text, int tone, Rgba under) {   // right-aligned; returns its left
    TextStyle st = St(kMeta - 2, tone == 3 ? c::kTextHi : tone == 1 ? c::kGood : tone == 2 ? c::kAccent : c::kTextDim);
    const float w = b.Width(text, st) + 28, h = 40;
    const float x0 = xRight - w;
    if (tone == 3) Round(b, x0, yMid - h / 2, xRight, yMid + h / 2, 16, c::kAccent);
    else Outline(b, x0, yMid - h / 2, xRight, yMid + h / 2, 16, 2, tone == 1 ? Hex(0x245E3F) : tone == 2 ? c::kLine : Hex(0x2A2A2C), under);
    b.Text(x0 + 14, b.TopForCentre(yMid, st.size), text, st);
    return x0;
  }
  static std::string Initials(const std::string& t) {
    std::string out;
    bool start = true;
    for (char ch : t) {
      if (ch == ' ' || ch == '-' || ch == ':') { start = true; continue; }
      if (start && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) {
        out += char(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        if (out.size() == 2) break;
      }
      start = false;
    }
    return out.empty() ? "?" : out;
  }

  // ---------------------------------------------------------------- top band: name, tabs, live chips
  void DrawTop(Batch& b) {
    b.Text(kGutter, 34, "BLAZINGMOUNT", St(kHead, c::kTextHi));
    const char* tabs[] = {"Library", "Activity", "Settings"};
    float x = kGutter + b.Width("BLAZINGMOUNT", St(kHead, c::kTextHi)) + 64;
    b.Text(x, 44, "{L1}", St(kMeta, c::kTextDim));
    x += 64;
    for (int i = 0; i < 3; ++i) {
      const bool on = int(tab_) == i;
      TextStyle st = St(kBody + 2, on ? c::kTextHi : c::kTextDim);
      b.Text(x, 42, tabs[i], st);
      const float w = b.Width(tabs[i], st);
      if (on) b.Rect(x, 86, x + w, 92, c::kAccent);   // the one accent bar
      x += w + 48;
    }
    b.Text(x - 16, 44, "{R1}", St(kMeta, c::kTextDim));
    // right: route, free space, building
    float r = kEdge;
    if (connected_) {
      const json::Value& fm = status_["free_mb"];
      const std::string hb = status_["homebrew_dir"].str();
      long long free = fm[hb.rfind("/mnt/ext1", 0) == 0 ? "ext1" : hb.rfind("/mnt/ext0", 0) == 0 ? "ext0" : "data"].ll(-1);
      std::string f = FreeText(free);
      if (!f.empty()) r = Pill(b, r, 64, f.c_str(), 0, c::kBg) - 16;
      const std::string route = status_["route"].str();
      r = Pill(b, r, 64, route == "TAILNET" ? "AWAY (TAILNET)" : "HOME (LAN)", route == "TAILNET" ? 2 : 1, c::kBg) - 16;
      const std::string busy = status_["busy"]["title"].str();
      if (!busy.empty()) {
        long long since = status_["busy"]["since"].ll();
        (void)since;
        std::string t = "BUILDING " + Fit(b, busy, St(kMeta - 2, c::kTextHi), 360);
        r = Pill(b, r, 64, t.c_str(), 3, c::kBg) - 16;
      }
      if (!hostOk_) Pill(b, r, 64, "DRIVES OFFLINE", 2, c::kBg);
    }
    b.Rect(kGutter, 112, kEdge, 114, c::kLine);
  }

  void DrawNotConnected(Batch& b) {
    Round(b, 360, 330, 1560, 750, 16, c::kSurface);
    b.Text(960, 380, "BlazingMount is not running", St(kHead, c::kTextHi), Align::kCentre);
    Wrap(b, 420, 470, 1080, "This app shows the payload's library. Load blazingmountd in the Payload Manager (it also starts by itself after a "
         "jailbreak when it is in the autoload list), and this screen fills in.", St(kBody, c::kText, -0.03f), 3, 46);
    char t[96];
    std::snprintf(t, sizeof t, "Trying 127.0.0.1:8094 every 2 seconds%s", (int(now_ * 2) % 2) ? "  \xE2\x80\xA2" : "");
    b.Text(960, 650, t, St(kMeta, c::kTextDim), Align::kCentre);
  }

  // ---------------------------------------------------------------- Library: everything he owns
  void Art(Batch& b, const Item& it, float x0, float y0, float x1, float y1, float alpha, bool big) {
    float uv[4];
    if (big && covers_.UvFocus(it.id, uv)) { b.Image(x0, y0, x1, y1, uv, alpha); return; }
    if (!it.coverKey.empty() && covers_.Uv(it.coverKey, uv)) { b.Image(x0, y0, x1, y1, uv, alpha); return; }
    Round(b, x0, y0, x1, y1, 12, Rgba{uint8_t(it.colour.r / 2), uint8_t(it.colour.g / 2), uint8_t(it.colour.b / 2), 255});   // the placeholder: its colour
    const float sz = std::min(kHead, (y1 - y0) * 0.36f);
    b.Text((x0 + x1) / 2, b.TopForCentre((y0 + y1) / 2, sz), Initials(it.title).c_str(), St(std::max(20.0f, sz), c::kTextDim), Align::kCentre);
  }

  void DrawLibrary(Batch& b) {
    // system chips (L2 / R2), scrolled so the chosen one is on screen; what to show (OPTIONS); grid/list (TOUCH PAD)
    char showT[80];
    std::snprintf(showT, sizeof showT, "{START} %s   {SELECT} %s", ShowName(show_), grid_ ? "List" : "Grid");
    const float showW = b.Width(showT, St(kMeta, c::kTextDim));
    b.Text(kEdge, 140, showT, St(kMeta, c::kTextDim), Align::kRight);
    const float chipsR = kEdge - showW - 72;
    std::vector<float> w; w.resize(size_t(Group::kCount));
    char t[64];
    for (int i = 0; i < int(Group::kCount); ++i) {
      std::snprintf(t, sizeof t, "%s  %d", GroupName(Group(i)), groupCount_[i]);
      w[size_t(i)] = b.Width(t, St(kMeta, c::kTextDim)) + 32;
    }
    float before = 0;
    for (int i = 0; i < int(group_); ++i) before += w[size_t(i)] + 12;
    const float room = chipsR - (kGutter + 56);
    const float shift = std::max(0.0f, before + w[size_t(group_)] - room * 0.7f);
    b.Text(kGutter, 140, "{L2}", St(kMeta, c::kTextDim));
    float x = kGutter + 56 - shift;
    for (int i = 0; i < int(Group::kCount); ++i) {
      std::snprintf(t, sizeof t, "%s  %d", GroupName(Group(i)), groupCount_[i]);
      const float x1 = x + w[size_t(i)];
      if (x >= kGutter + 56 - 1 && x1 <= chipsR) {
        const bool on = int(group_) == i;
        TextStyle st = St(kMeta, on ? c::kTextHi : groupCount_[i] ? c::kTextDim : Hex(0x55555A));
        if (on) Outline(b, x, 132, x1, 180, 16, 3, c::kAccent, c::kBg);
        else Outline(b, x, 132, x1, 180, 16, 2, c::kLine, c::kBg);
        b.Text(x + 16, b.TopForCentre(156, st.size), t, st);
      }
      x = x1 + 12;
    }
    b.Text(chipsR + 12, 140, "{R2}", St(kMeta, c::kTextDim));

    const float listR = 1110;
    if (!haveLibrary_ && items_.empty()) { b.Text(kGutter, kListTop + 20, "Reading the library...", St(kBody, c::kTextDim)); return; }
    if (view_.empty()) { b.Text(kGutter, kListTop + 20, "Nothing here", St(kBody, c::kTextDim)); return; }
    if (grid_) DrawGrid(b); else DrawList(b, listR);
    // scroll position
    const int rows = grid_ ? (int(view_.size()) + kCols - 1) / kCols : int(view_.size()), vis = grid_ ? kGridRows : kRowsVisible;
    if (rows > vis) {
      const float h = kBottom - kListTop;
      const float th = std::max(40.0f, h * vis / rows);
      const float ty = kListTop + (h - th) * (float(top_) / std::max(1, rows - vis));
      b.Rect(listR + 14, kListTop, listR + 18, kListTop + h, c::kLine);
      b.Rect(listR + 12, ty, listR + 20, ty + th, c::kAccent);
    }
    DrawDetail(b);
  }

  void DrawGrid(Batch& b) {
    const int rowSel = sel_ / kCols;
    top_ = std::clamp(top_, std::max(0, rowSel - kGridRows + 1), rowSel);
    for (int r = 0; r < kGridRows; ++r)
      for (int col = 0; col < kCols; ++col) {
        const int idx = (top_ + r) * kCols + col;
        if (idx >= int(view_.size())) return;
        const Item& it = items_[size_t(view_[size_t(idx)])];
        const bool f = idx == sel_, rowFocused = (top_ + r) == rowSel;
        const float x0 = kGutter + col * kPitchX + 10, y0 = kListTop + r * kPitchY, x1 = x0 + kCard, y1 = y0 + kCard;
        if (f) Round(b, x0 - 6, y0 - 6, x1 + 6, y1 + 6, 16, c::kAccent);   // DESIGN-V2 2.6: the 6 px accent halo is the slot
        Art(b, it, x0, y0, x1, y1, f ? 1.0f : rowFocused ? 0.65f : 0.45f, false);
        if (f) b.Rect(x0, y1 + 12, x1, y1 + 18, c::kAccent);              // ...and the one accent bar under the artwork
        const Badge bd = ItemBadge(it, games_);
        if (bd.tone != 0 || f) Pill(b, x1 - 6, y1 - 26, bd.text, bd.tone, c::kBg);
        b.Text(x0, y1 + 24, Fit(b, it.title, St(kMeta, f ? c::kTextHi : c::kText), kCard).c_str(), St(kMeta, f ? c::kTextHi : c::kText));
        std::string sub = it.game >= 0 ? games_[size_t(it.game)].tid : SystemLabel(it);
        b.Text(x0, y1 + 54, Fit(b, sub, St(kWhisper, c::kTextDim, -0.03f), kCard).c_str(), St(kWhisper, c::kTextDim, -0.03f));
      }
  }

  std::string SystemLabel(const Item& it) const {
    for (const auto& sy : catalog_["systems"].items) if (sy["key"].str() == it.system) return sy["label"].str();
    return it.system == "ps4" ? "PS4" : it.system == "ps5" ? "PS5" : it.system;
  }

  void DrawList(Batch& b, float listR) {
    top_ = std::clamp(top_, std::max(0, sel_ - kRowsVisible + 1), sel_);
    for (int i = 0; i < kRowsVisible && top_ + i < int(view_.size()); ++i) {
      const int idx = top_ + i;
      const Item& it = items_[size_t(view_[size_t(idx)])];
      const Game* g = it.game >= 0 ? &games_[size_t(it.game)] : nullptr;
      const bool f = idx == sel_;
      const float y0 = kListTop + i * (kRowH + kRowGap), y1 = y0 + kRowH;
      if (f) Round(b, kGutter, y0, listR, y1, 16, c::kFocusFill);
      Art(b, it, kGutter + 12, y0 + 10, kGutter + 80, y1 - 10, f ? 1.0f : 0.65f, false);
      const Rgba under = f ? c::kFocusFill : c::kBg;
      const Badge bd = ItemBadge(it, games_);
      float pl = Pill(b, listR - 16, (y0 + y1) / 2, bd.text, f && bd.tone != 3 ? 0 : bd.tone, under);
      if (g && !g->proof.empty()) pl = Pill(b, pl - 10, (y0 + y1) / 2, "PLAYED", f ? 0 : 1, under);
      const float tx = kGutter + 104, tw = pl - 20 - tx;
      b.Text(tx, y0 + 9, Fit(b, it.title, St(kBody, f ? c::kTextHi : c::kText), tw).c_str(), St(kBody, f ? c::kTextHi : c::kText));
      std::string meta = SystemLabel(it);
      if (!it.ids.empty()) meta += "  \xC2\xB7  " + it.ids.substr(0, it.ids.find(','));
      if (it.files) meta += "  \xC2\xB7  ." + it.firstType + (it.compressed ? " compressed" : "") + "  \xC2\xB7  " + Gb(it.size);
      else meta += "  \xC2\xB7  not on my drives";
      b.Text(tx, y0 + 48, Fit(b, meta, St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f), tw).c_str(), St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f));
    }
  }

  void Fact(Batch& b, float x, float& y, float w, const char* label, const std::string& value, Rgba col = c::kText) {
    if (value.empty() || y > kBottom - 150) return;
    b.Text(x, y, label, St(kWhisper, c::kTextDim));
    y = Wrap(b, x + 190, y - 3, w - 190, value, St(kMeta, col, -0.03f), 2, 34) + 6;
  }

  void DrawDetail(Batch& b) {
    const Item* it = FocusedItem();
    if (!it) return;
    const Game* g = Focused();
    const float x0 = 1158, y0 = kListTop, x1 = kEdge, y1 = kBottom;
    Round(b, x0, y0, x1, y1, 16, c::kSurface);
    const float px = x0 + 32, pw = x1 - x0 - 64;
    Art(b, *it, px, y0 + 32, px + 200, y0 + 232, 1.0f, true);   // the full cover once it has been focused a moment
    float ty = Wrap(b, px + 224, y0 + 36, pw - 224, it->title, St(kHead - 4, c::kTextHi), 3, 50);
    const Badge bd = ItemBadge(*it, games_);
    Pill(b, px + 224 + b.Width(bd.text, St(kMeta - 2, c::kText)) + 28, std::max(ty + 26, y0 + 200), bd.text, bd.tone, c::kSurface);
    float y = y0 + 264;
    b.Rect(px, y - 14, px + pw, y - 12, c::kLine);
    const char* how = it->how == "owned" ? "Owned" : it->how == "ps_plus" ? "PS Plus (yours while Plus is on)" : it->how == "licence" ? "Licence on DIGI5"
                    : it->how == "free" ? "Free with the console" : it->how == "package" ? "Installed from a package" : "On my drives";
    Fact(b, px, y, pw, "System", SystemLabel(*it) + (it->ids.empty() ? "" : "   " + it->ids.substr(0, 40)));
    Fact(b, px, y, pw, "Yours", how);
    if (it->files) {
      std::string f = it->firstPath + "   ." + it->firstType + (it->compressed ? " (compressed)" : " (not compressed)") + (it->verified ? ", verified" : "")
                    + (it->size ? "   " + Gb(it->size) : "");
      if (it->files > 1) f += "   +" + std::to_string(it->files - 1) + " more";
      Fact(b, px, y, pw, "On my drives", f, c::kGood);
    } else Fact(b, px, y, pw, "On my drives", it->streamable ? "streamed from the drives" : "not yet", it->streamable ? c::kGood : c::kTextDim);
    Fact(b, px, y, pw, "Plays on", it->plays);
    if (g) {
      Fact(b, px, y, pw, "Needs FW", g->fw.empty() ? "" : g->fw + "   (this console " + consoleFw_ + ")", g->state == "needs_fw" ? c::kAccent : c::kText);
      std::string here = g->local == "starter" ? "Streaming starter" : g->local == "full" ? "Full copy" : g->local == "image" ? "Image file" : "Not installed";
      if (!g->localPath.empty()) here += ": " + g->localPath;
      if (g->pinned) here += "  (pinned)";
      Fact(b, px, y, pw, "This console", here, g->local == "none" ? c::kTextDim : c::kGood);
      Fact(b, px, y, pw, "Played", g->proof.empty() ? "no proof yet" : g->proof, g->proof.empty() ? c::kTextDim : c::kGood);
      if (!g->reason.empty()) Fact(b, px, y, pw, g->build == "done" ? "Last build" : "Why", g->reason, g->build == "done" ? c::kText : c::kAccent);
    }
    std::string hint;
    if (g) {
      if (g->local == "starter" && g->state != "playing") hint = "{CROSS} Remove   ";
      else if (g->local == "none" && g->state != "needs_fw" && g->state != "building") hint = "{CROSS} Install   ";
      if (g->local == "starter" && g->onDrives && g->state != "playing") hint += "{TRIANGLE} Rebuild   ";
      hint += g->choice == "skip" ? "{SQUARE} Show" : "{SQUARE} Hide";
    } else hint = it->plays.empty() ? "" : "Plays on " + it->plays;
    b.Rect(px, y1 - 88, px + pw, y1 - 86, c::kLine);
    b.Text(px, y1 - 68, Fit(b, hint, St(kMeta, c::kText), pw).c_str(), St(kMeta, c::kText));
  }

  // ---------------------------------------------------------------- Activity
  void DrawActivity(Batch& b) {
    const float y0 = 140;
    Round(b, kGutter, y0, 900, 580, 16, c::kSurface);
    b.Text(kGutter + 32, y0 + 28, "NOW", St(kMeta, c::kTextDim));
    const std::string busy = status_["busy"]["title"].str();
    if (!busy.empty()) {
      b.Text(kGutter + 32, y0 + 70, Fit(b, "Building " + busy, St(kHead - 4, c::kTextHi), 740).c_str(), St(kHead - 4, c::kTextHi));
      long long since = status_["busy"]["since"].ll();
      char t[96]; std::snprintf(t, sizeof t, "started %lld s ago (only the small files are copied; the big ones stream)", since ? (long long)std::max(0.0, Epoch() - since) : 0LL);
      b.Text(kGutter + 32, y0 + 124, t, St(kMeta, c::kText, -0.03f));
    } else {
      b.Text(kGutter + 32, y0 + 70, "Idle: every game is up to date", St(kHead - 4, c::kTextHi));
    }
    float y = y0 + 190;
    char t[160];
    std::snprintf(t, sizeof t, "Payload %s   \xC2\xB7   fork %s", status_["version"].str().c_str(), status_["fork"].str().substr(0, 8).c_str());
    b.Text(kGutter + 32, y, t, St(kMeta, c::kText, -0.03f)); y += 40;
    std::snprintf(t, sizeof t, "Drives: %s:%lld   \xC2\xB7   %s", status_["host"].str().c_str(), status_["port"].ll(), hostOk_ ? "online" : "OFFLINE");
    b.Text(kGutter + 32, y, t, St(kMeta, hostOk_ ? c::kText : c::kAccent, -0.03f)); y += 40;
    const std::string smp = status_["smp"].str(), want = status_["smp_expected"].str();
    std::snprintf(t, sizeof t, "ShadowMount+ %s%s", smp.empty() ? "not found" : smp.c_str(), smp == want ? " (the proven build)" : (" - expected " + want).c_str());
    b.Text(kGutter + 32, y, t, St(kMeta, smp == want ? c::kGood : c::kAccent, -0.03f)); y += 40;
    std::snprintf(t, sizeof t, "Starters go to %s   \xC2\xB7   %s", status_["homebrew_dir"].str().c_str(), auto_ ? "new games: built by themselves" : "new games: only when you pick them");
    b.Text(kGutter + 32, y, Fit(b, t, St(kMeta, c::kText, -0.03f), 740).c_str(), St(kMeta, c::kText, -0.03f));
    b.Text(kGutter + 32, 520, "{CROSS} Check the drives now", St(kMeta, c::kText));
    // recent actions
    Round(b, kGutter, 608, 900, kBottom, 16, c::kSurface);
    b.Text(kGutter + 32, 636, "WHAT YOU ASKED", St(kMeta, c::kTextDim));
    const json::Value& rec = status_["recent"];
    float ry = 680;
    if (!rec.size()) b.Text(kGutter + 32, ry, "Nothing yet this session", St(kMeta, c::kTextDim, -0.03f));
    for (size_t i = rec.size(); i-- > 0 && ry < kBottom - 70;) ry = Wrap(b, kGutter + 32, ry, 740, rec[i].str(), St(kMeta, c::kText, -0.03f), 2, 34) + 12;
    // the log
    Round(b, 940, 140, kEdge, kBottom, 16, c::kSurface);
    b.Text(972, 168, "LOG  /data/blazing/blazingmountd.log", St(kMeta, c::kTextDim));
    b.Text(kEdge - 32, 168, "{8} {2} scroll", St(kMeta, c::kTextDim), Align::kRight);
    const int lines = 22;
    const int end = int(log_.size()) - logScroll_;
    float ly = 212;
    for (int i = std::max(0, end - lines); i < end; ++i) {
      std::string l = log_[size_t(i)];
      if (l.size() > 20 && l[4] == '-' && l[13] == ':') l = l.substr(11);   // drop the date, keep the time
      const bool bad = l.find("FAILED") != std::string::npos || l.find("WARNING") != std::string::npos ||
                       (l.find("refused") != std::string::npos && l.find("pass:") == std::string::npos);
      b.Text(972, ly, Fit(b, l, St(kWhisper, bad ? c::kAccent : c::kText, -0.03f), kEdge - 1004).c_str(), St(kWhisper, bad ? c::kAccent : c::kText, -0.03f));
      ly += 35;
    }
  }
  double Epoch() const { return status_["now"].ok() ? status_["now"].num() : (double)status_["last_pass"].ll(); }

  // ---------------------------------------------------------------- Settings
  void DrawSettings(Batch& b) {
    float y = 140;
    const float x0 = kGutter, x1 = kEdge;
    auto row = [&](int i, const std::string& label, const std::string& help, const std::string& value, Rgba vcol) {
      const bool f = setSel_ == i;
      const float h = 84;
      if (y + h > kBottom) return;
      if (f) Round(b, x0, y, x1, y + h, 16, c::kFocusFill);
      b.Text(x0 + 24, y + 10, label.c_str(), St(kBody, f ? c::kTextHi : c::kText));
      b.Text(x0 + 24, y + 46, Fit(b, help, St(kWhisper, f ? c::kTextHi : c::kTextDim, -0.03f), 1100).c_str(), St(kWhisper, f ? c::kTextHi : c::kTextDim, -0.03f));
      b.Text(x1 - 24, y + 24, value.c_str(), St(kBody, f ? c::kTextHi : vcol), Align::kRight);
      y += h + 8;
    };
    // keep the focused row on screen
    const int first = std::max(0, setSel_ - 8);
    int i = 0;
    auto vis = [&](int k) { return k >= first; };
    if (vis(i)) row(i, "New games on the drives", "Build every game automatically, or only the ones you pick in the Library ({CROSS} Install)",
                    auto_ ? "Build them all" : "Only the ones I pick", c::kGood);
    ++i;
    if (vis(i)) row(i, "Check the drives now", "Normally every few minutes, and at once when a game is added or rebuilt on the drives", "{CROSS}", c::kText);
    ++i;
    if (settings_.empty() && y < kBottom) b.Text(x0 + 24, y + 20, "Reading mount.conf...", St(kBody, c::kTextDim));
    for (const Setting& s : settings_) {
      if (vis(i)) {
        std::string v = s.edit.empty() ? s.value : s.edit;
        if (s.type == "b") v = s.value == "0" ? "Off" : "On";
        else if (s.type == "n") v = (s.edit.empty() ? "" : "{4} ") + v + (s.edit.empty() ? "" : " {6}   {CROSS} save");
        row(i, s.key, s.help, v, s.type == "s" ? c::kTextDim : c::kText);
      }
      ++i;
    }
  }

  // ---------------------------------------------------------------- overlays
  void DrawConfirm(Batch& b) {
    b.Rect(0, 0, 1920, 1080, Rgba{0, 0, 0, 170});
    Round(b, 460, 360, 1460, 720, 16, c::kSurfaceHi);
    Wrap(b, 508, 400, 904, confirm_.title, St(kHead, c::kTextHi), 2, 52);
    Wrap(b, 508, 520, 904, confirm_.body, St(kBody, c::kText, -0.03f), 3, 44);
    std::string h = "{CROSS} " + confirm_.yes + "      {CIRCLE} Cancel";
    b.Text(508, 650, h.c_str(), St(kBody, c::kTextHi));
  }
  void DrawToast(Batch& b) {
    if (toast_.empty() || now_ > toastUntil_) return;
    TextStyle st = St(kMeta, c::kTextHi, -0.03f);
    const float w = std::min(900.0f, b.Width(toast_.c_str(), st) + 64);
    const float x1 = kEdge, x0 = x1 - w, y1 = kBottom - 8, y0 = y1 - 64;
    Round(b, x0, y0, x1, y1, 16, c::kSurfaceHi);
    b.Rect(x0, y0 + 12, x0 + 6, y1 - 12, c::kWarn);   // toasts are the one place warn appears
    b.Text(x0 + 32, b.TopForCentre((y0 + y1) / 2, st.size), Fit(b, toast_, st, w - 56).c_str(), st);
  }
};

}  // namespace bm
