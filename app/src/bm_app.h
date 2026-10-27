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
#include <cctype>
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
  // v2.8 (docs/DOWNLOAD-TO-CONSOLE.md): plays without the network? every copy; the app's request; mac1's download progress
  bool onConsole = false;
  long long localBytes = 0, requestAt = 0, dlDone = 0, dlTotal = 0, dlEta = 0, dlAt = 0;
  std::string copies, request, dlState, dlMsg;
};

struct Setting {
  std::string key, type, value, help, edit;   // edit: the value being changed with Left/Right (empty = none)
};

enum class Tab { kLibrary, kDiscover, kInstall, kActivity, kSettings };   // kInstall (0.6, 26 Oct): PKGs from the drives, 1x storage
enum class Filter { kAll, kOnConsole, kNotInstalled, kProblems, kHidden, kPs5, kPs4, kCount };   // (kept for the preview's old shots)

// The whole library (Markus 24 Oct: everything he owns, every system): L2/R2 walk the systems, OPTIONS what to show, TOUCH PAD grid/list
enum class Group { kAll, kPs5, kPs4, kPs3, kPs2, kPsClassic, kSwitch, kNintendo, kXbox, kPc, kSega, kRetro, kMobile, kCount };
enum class Show { kEverything, kPlayable, kOnDrives, kOnConsole, kStreams, kNotOnDrives, kAttention, kHidden, kCount };
inline const char* GroupName(Group g) {
  static const char* n[] = {"All", "PS5", "PS4", "PS3", "PS2", "PS1 / PSP / Vita", "Switch", "Nintendo", "Xbox", "PC", "Sega", "Retro", "Mobile"};
  return n[int(g)];
}
inline const char* ShowName(Show s) {
  static const char* n[] = {"Everything I own", "Playable now", "On my drives", "On this console", "Streams from mac1", "Not on my drives", "Needs attention",
                            "Hidden"};
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
  // DISCOVER (docs/UI-DESIGN.md 10.5): a game from the host's /discover.json (how = owned/ps_plus/... or "" = not owned, files = on my drives)
  std::string dateText, summary, sysLabels;
  std::vector<std::string> syss;
  long long date = 0;
  int hype = 0, rating = 0, votes = 0;
};

inline std::string Gb(long long bytes) {
  char t[32];
  if (bytes <= 0) return "-";
  if (bytes >= 100LL * 1000 * 1000 * 1000) std::snprintf(t, sizeof t, "%.0f GB", bytes / 1e9);
  else if (bytes >= 1000LL * 1000 * 1000) std::snprintf(t, sizeof t, "%.1f GB", bytes / 1e9);
  else if (bytes >= 1000LL * 1000) std::snprintf(t, sizeof t, "%.0f MB", bytes / 1e6);
  else std::snprintf(t, sizeof t, "%.0f KB", std::max(1.0, bytes / 1e3));   // a payload ELF is ~200 KB, not "0 MB"
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
inline bool DlActive(const std::string& st) {
  return st == "queued" || st == "waiting" || st == "sending" || st == "verifying" || st == "switching" || st == "removing";
}
inline const char* DlPercent(long long done, long long total) {   // "DOWNLOADING 37%": 101 fixed strings, so a Badge can point at one
  static std::string pct[101];
  const int p = total > 0 ? int(std::clamp(done * 100 / total, 0LL, 100LL)) : 0;
  if (pct[p].empty()) pct[p] = "DOWNLOADING " + std::to_string(p) + "%";
  return pct[p].c_str();
}
// The app's newest request that mac1's worker has not taken up yet: no job line, or one older than the request (the worker
// writes the line within ~20 s of taking it up). `request` alone cannot say: a remove run on mac1 leaves the old "download".
inline bool Pending(const Game& g) { return g.dlState.empty() || (!DlActive(g.dlState) && g.requestAt > g.dlAt); }
inline bool DlAsked(const Game& g) { return g.request == "download" && Pending(g); }
inline bool RmAsked(const Game& g) { return g.request == "remove_local" && Pending(g); }
// A copy mac1's helper can take back (0.5.1): an image (it checks that it is the drives' copy) or a folder game it filled itself
// (it has a job line). Never a pkg install, never a full copy someone else made (BlazingArch, his own dumps).
inline bool Removable(const Game& g) {
  return g.onConsole && g.local != "pkg" && (g.local == "image" || (!g.dlState.empty() && g.dlMsg.find("did not make") == std::string::npos));
}
inline Badge BadgeOf(const Game& g) {
  const std::string& s = g.state;
  if (s == "playing") return {"PLAYING", 3};
  if (s == "building") return {"BUILDING", 3};
  if (g.dlState == "sending") return {DlPercent(g.dlDone, g.dlTotal), 3};
  if (g.dlState == "verifying") return {"CHECKING COPY", 3};
  if (g.dlState == "switching") return {"SWITCHING", 3};
  if (g.dlState == "removing") return {"REMOVING", 3};
  if (g.dlState == "queued" || g.dlState == "waiting") return {"DOWNLOAD WAITS", 0};
  if (g.dlState == "failed" && g.request == "download") return {"DOWNLOAD FAILED", 2};
  if (g.dlState == "failed" && g.request == "remove_local") return {"REMOVE FAILED", 2};
  if (DlAsked(g)) return {"DOWNLOAD ASKED", 0};
  if (RmAsked(g)) return {"REMOVE ASKED", 0};
  if (g.onConsole) return {"ON CONSOLE", 1};
  if (s == "installed") return {g.local == "starter" ? "STREAMS" : g.local == "image" ? "IMAGE" : "FULL COPY", g.local == "starter" ? 0 : 1};
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
      statusDiscoverEtag_ = v["discover_etag"].str();
      wall_ = v["now"].ll();
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
        g.onConsole = j["on_console"].ok() ? j["on_console"].truthy() : (g.local == "full" || g.local == "image");
        g.localBytes = j["local_bytes"].ll();
        for (const auto& cp : j["copies"].items) {
          const std::string k = cp["kind"].str(), path = cp["path"].str();
          if (k == "starter") continue;                     // the starter is what "Streams" already says
          const char* drive = path.rfind("/mnt/ext1", 0) == 0 ? "M.2" : path.rfind("/mnt/ext0", 0) == 0 ? "extended" : path.rfind("/mnt/usb", 0) == 0 ? "USB" : "console storage";
          g.copies += (g.copies.empty() ? "" : ", ") + (k == "pkg" ? "installed game" : k == "parked" ? "parked image" : k) + " (" + drive + ")";
        }
        g.request = j["request"].str(); g.requestAt = j["request_at"].ll();
        g.dlState = j["dl_state"].str(); g.dlMsg = j["dl_msg"].str();
        g.dlDone = j["dl_done"].ll(); g.dlTotal = j["dl_total"].ll(); g.dlEta = j["dl_eta"].ll(); g.dlAt = j["dl_at"].ll();
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
    } else if (path.rfind("/api/cover/dsheet/", 0) == 0) {
      const int page = std::atoi(path.c_str() + 18);
      auto it = dsheetCells_.find(page);
      if (code == 200 && it != dsheetCells_.end()) covers_.PutSheet(it->second, body);
      else if (it != dsheetCells_.end()) for (auto& c2 : it->second) covers_.Failed(c2.first);
    } else if (Base(path) == "/api/discover") {
      discoverTried_ = true;
      if (!ok) { discoverMissing_ = code == 404; return; }
      discoverMissing_ = false;
      discover_ = std::move(v);
      discoverEtag_ = statusDiscoverEtag_;
      RebuildDiscover();
    } else if (Base(path) == "/api/wishlist") {
      if (!ok) return;
      wishOrder_.clear(); wished_.clear();
      for (const auto& w : v["items"].items) { wishOrder_.push_back(w["id"].str()); wished_[w["id"].str()] = true; }
      for (auto& kv : wishPending_) wished_[kv.first] = kv.second;      // a press still on its way wins over an older list
      wishTried_ = true;
      RebuildDView(FocusedD() ? FocusedD()->id : "");
    } else if (path.rfind("/api/action?do=wish", 0) == 0) {
      const std::string id = QueryArg(path, "id");
      const bool on = QueryArg(path, "value") == "1";
      wishPending_.erase(id);
      if (!ok || !v["ok"].truthy()) {                                   // mac1 did not take it: the card goes back
        wished_[id] = !on;
        Toast(code == 0 ? "BlazingMount did not answer: the wishlist did not change" : "Wishlist not changed: " + v["error"].str());
      }
      nextWish_ = 0;
      RebuildDView(FocusedD() ? FocusedD()->id : "");
    } else if (path.rfind("/api/cover/full/", 0) == 0) {
      const std::string id = path.substr(16, path.size() - 16 - 4);
      if (code == 200) covers_.PutFocus(id, body);
      if (fullTried_.size() > 256) fullTried_.clear();
      fullTried_.push_back(id);
    } else if (path.rfind("/api/icon/", 0) == 0) {
      const std::string tid = path.substr(10);
      if (code == 200 && !body.empty()) covers_.Put(tid, body); else covers_.Failed(tid);
    } else if (Base(path) == "/api/pm/drives") {
      pmTried_ = true;
      pmDown_ = code == 502 || code == 0;
      if (!ok) return;
      pmDrives_.clear();
      for (const auto& d : v.items) if (d["clickable"].truthy() && d["pkg_count"].ll() > 0) pmDrives_.push_back({d["id"].str(), d["label"].str(), d["pkg_count"].ll()});
      if (pmDrive_ >= int(pmDrives_.size())) pmDrive_ = 0;
      for (size_t i = 0; i < pmDrives_.size(); ++i) if (pmDrives_[i].id == "mac1_8tb" && !pmPicked_) pmDrive_ = int(i);   // the 8TB first
    } else if (Base(path) == "/api/pm/packages") {
      if (!ok) return;
      std::string keep = pmSel_ < int(pm_.size()) ? pm_[size_t(pmSel_)].path : "";
      pm_.clear();
      for (const auto& j : v.items) {
        PmPkg k;
        k.path = j["path"].str(); k.title = j["title_name"].str(); k.tid = j["title_id"].str(); k.version = j["app_version"].str();
        k.type = j["pkg_type"].str(); k.size = j["total_pkg_size"].ll(); k.parts = (int)j["total_parts"].ll();
        k.installed = j["is_installed"].truthy() || j["is_dlc_installed"].truthy(); k.installedVersion = j["installed_version"].str();
        k.canInstall = j["can_install"].truthy(); k.why = j["install_disabled_reason"].str();
        if (k.title.empty()) k.title = j["filename"].str();
        pm_.push_back(std::move(k));
      }
      pmSel_ = 0;
      for (size_t i = 0; i < pm_.size(); ++i) if (pm_[i].path == keep) pmSel_ = int(i);
      pmFor_ = QueryArg(path, "drive");
    } else if (Base(path) == "/api/store") {
      storeTried_ = true;
      if (!ok) { storeMissing_ = code == 404 || code == 0; return; }
      storeMissing_ = false;
      storeJob_ = v["job"];
      std::vector<std::string> folders, slots;
      for (const auto& f : v["have"]["folders"].items) folders.push_back(f.str());
      for (const auto& f : v["have"]["slots"].items) slots.push_back(f.str());
      std::string keep = stSel_ < int(store_.size()) ? store_[size_t(stSel_)].path : "";
      store_.clear();
      for (const auto& j : v["feed"].items) {
        StoreItem it;
        it.kind = j["kind"].str(); it.path = j["path"].str(); it.name = j["name"].str(); it.tid = j["title_id"].str();
        it.version = j["version"].str(); it.size = j["size"].ll(); it.files = (int)j["files"].ll();
        if (it.kind == "app") for (const auto& f : folders) if (f == it.path || (!it.tid.empty() && f.find(it.tid) != std::string::npos)) it.installed = true;
        if (it.kind == "payload") for (const auto& f : slots) if (f == Slug(it.name.empty() ? it.path : it.name)) it.installed = true;
        store_.push_back(std::move(it));
      }
      stSel_ = 0;
      for (size_t i = 0; i < store_.size(); ++i) if (store_[i].path == keep) stSel_ = int(i);
    } else if (Base(path) == "/api/store/install") {
      if (code == 0) Toast("BlazingMount did not answer");
      else if (!ok || !v["ok"].truthy()) Toast("Not installed: " + v["error"].str());
      else Toast("Installing from your drives: you can keep using the console");
      nextStore_ = 0;
    } else if (Base(path) == "/api/pm/status") {
      if (ok) pmStatus_ = std::move(v);
    } else if (Base(path) == "/api/pm/queue") {
      if (ok) pmQueue_ = std::move(v);
    } else if (Base(path) == "/api/pm/install" || Base(path) == "/api/pm/cancel" || Base(path) == "/api/pm/refresh") {
      const std::string what = Base(path).substr(8);
      if (code == 0) Toast("BlazingMount did not answer");
      else if (!ok || (v["success"].ok() && !v["success"].truthy()) || code >= 300)
        Toast((what == "install" ? "Not installed: " : what == "cancel" ? "Not cancelled: " : "Not rescanned: ") +
              (v["error"].str().empty() ? v["message"].str() : v["error"].str()));
      else Toast(what == "install" ? "Installing: it reads straight from your drives (no copy first)" : what == "cancel" ? "Install cancelled" : "Looking at your drives again");
      nextPm_ = 0; nextPmList_ = what == "refresh" ? now_ + 8.0 : 0;
    } else if (Base(path) == "/api/action") {
      if (!ok) Toast(code == 0 ? "BlazingMount did not answer" : "Refused: " + v["error"].str());
      nextStatus_ = 0;   // see the result soon
    }
  }

  // ---------------------------------------------------------------- the controller
  void Press(Button b) {
    if (confirm_.active) { PressConfirm(b); return; }
    if (b == kL1 || b == kR1) {
      int t = (int(tab_) + (b == kR1 ? 1 : 4)) % 5;
      tab_ = Tab(t);
      if (tab_ == Tab::kSettings) settingsGen_ = 0;
      return;
    }
    switch (tab_) {
      case Tab::kLibrary: if (b == kTouch) { grid_ = !grid_; top_ = 0; return; } PressLibrary(b); break;
      case Tab::kDiscover: PressDiscover(b); break;
      case Tab::kInstall: PressInstall(b); break;
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
    // DISCOVER: the shelves at start (for the OUT NOW count) and again when the host's ETag moves; the wishlist on the tab every 60 s
    if (connected_ && !Inflight("/api/discover") && now >= nextDiscover_ &&
        (!discoverTried_ || (!statusDiscoverEtag_.empty() && statusDiscoverEtag_ != discoverEtag_) || (tab_ == Tab::kDiscover && discoverMissing_))) {
      Ask("GET", "/api/discover"); nextDiscover_ = now + (discoverMissing_ ? 20.0 : 10.0);
    }
    if (connected_ && !Inflight("/api/wishlist") && now >= nextWish_ && (!wishTried_ || tab_ == Tab::kDiscover)) {
      Ask("GET", "/api/wishlist"); nextWish_ = now + 60.0;
    }
    if (connected_ && tab_ == Tab::kDiscover) RequestCoversOf(now, dItems_, dView_, dSel_, dTop_, true, "dsheet");
    // INSTALL: PKG Manager's drives + packages (slow), its status + queue every second while the tab is open or a job runs
    if (connected_ && (tab_ == Tab::kInstall || PmBusy())) {
      if (now >= nextPmList_ && !Inflight("/api/pm/drives")) { Ask("GET", "/api/pm/drives"); nextPmList_ = now + 15.0; nextPkgs_ = 0; }
      if (tab_ == Tab::kInstall && storeOn_ && now >= nextStore_ && !Inflight("/api/store")) {
        Ask("GET", "/api/store"); nextStore_ = now + (storeJob_["state"].str() == "running" ? 1.0 : 10.0);
      }
      if (!storeOn_ && !pmDrives_.empty() && (pmFor_ != pmDrives_[size_t(pmDrive_)].id || now >= nextPkgs_) && !Inflight("/api/pm/packages") && tab_ == Tab::kInstall) {
        Ask("GET", "/api/pm/packages?drive=" + pmDrives_[size_t(pmDrive_)].id); nextPkgs_ = now + 20.0;
      }
      if (now >= nextPm_ && !Inflight("/api/pm/status") && !Inflight("/api/pm/queue")) { Ask("GET", "/api/pm/status"); Ask("GET", "/api/pm/queue"); nextPm_ = now + 1.0; }
    }
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
      case Tab::kDiscover: DrawDiscover(b); break;
      case Tab::kInstall: DrawInstall(b); break;
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
  // ---- INSTALL (0.6): PKGs on the drives, installed with no copy first by PKG Manager on this console (payload /api/pm/*)
  struct PmDrive { std::string id, label; long long count; };
  struct PmPkg { std::string path, title, tid, version, type, installedVersion, why; long long size = 0; int parts = 0; bool installed = false, canInstall = true; };
  std::vector<PmDrive> pmDrives_;
  std::vector<PmPkg> pm_;
  json::Value pmStatus_, pmQueue_;
  std::string pmFor_;
  int pmDrive_ = 0, pmSel_ = 0, pmTop_ = 0;
  // ---- STORE (0.6.1): the homebrew in "Apps and Emulators/PS5" on the drives (payload /api/store); the first source of the tab
  struct StoreItem { std::string kind, path, name, tid, version; long long size = 0; int files = 0; bool installed = false; };
  std::vector<StoreItem> store_;
  json::Value storeJob_;
  bool storeOn_ = true, storeTried_ = false, storeMissing_ = false;
  int stSel_ = 0, stTop_ = 0;
  double nextStore_ = 0;
  bool pmTried_ = false, pmDown_ = false, pmPicked_ = false;
  double nextPm_ = 0, nextPmList_ = 0, nextPkgs_ = 0, pmRateAt_ = 0, pmRate_ = 0;
  long long pmRateDone_ = 0;
  unsigned long gen_ = 0, libGen_ = ~0ul, settingsGen_ = 0;
  bool connected_ = false, haveLibrary_ = false, hostOk_ = true, auto_ = true, seenStatus_ = false;
  int misses_ = 0;
  std::string consoleFw_, lastRecent_, toast_;
  double now_ = 0, nextStatus_ = 0, nextLibrary_ = 0, nextLog_ = 0, toastUntil_ = 0;
  struct Confirm {
    bool active = false;
    std::string action, tid, title, body, yes, altAction, alt;   // alt: a second choice on SQUARE (empty = none)
  } confirm_;
  // ---- DISCOVER (docs/UI-DESIGN.md 10.5): games he does not have yet, his wishlist, release alerts
  struct DShelf { std::string key, label; std::vector<int> items; };
  json::Value discover_;
  std::vector<Item> dItems_;                 // every Discover game once
  std::vector<DShelf> dShelves_;
  std::vector<int> dView_;                   // dItems_ indices: the shelf, then the system filter
  std::vector<std::string> dSysList_;        // the systems on this shelf (OPTIONS walks them)
  std::unordered_map<std::string, int> dById_;
  std::map<int, std::vector<std::pair<std::string, int>>> dsheetCells_;
  std::unordered_map<std::string, bool> wished_, wishPending_;
  std::unordered_map<std::string, std::string> alertOf_;   // id -> out / moved (from the host's alerts)
  std::vector<std::string> wishOrder_;
  std::string discoverEtag_, statusDiscoverEtag_, dSys_;
  bool discoverTried_ = false, discoverMissing_ = false, wishTried_ = false;
  double nextDiscover_ = 0, nextWish_ = 0;
  int dShelf_ = 0, dSel_ = 0, dTop_ = 0, outCount_ = 0;
  long long wall_ = 0;                       // the console's clock (the payload's /api/status "now"): "in N days"

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
      case Show::kOnConsole: return g && g->onConsole;
      case Show::kStreams: return g && g->local == "starter" && !g->onConsole;
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
  void RequestCovers(double now) { RequestCoversOf(now, items_, view_, sel_, top_, grid_, "sheet"); }
  void RequestCoversOf(double now, const std::vector<Item>& items, const std::vector<int>& view, int sel, int top, bool grid, const char* kind) {
    int busy = 0;
    for (const auto& f : inflight_) busy += f.rfind("/api/cover/", 0) == 0 || f.rfind("/api/icon/", 0) == 0;
    const int cols = grid ? kCols : 1, rows = grid ? kGridRows : kRowsVisible;
    const int first = std::max(0, (top - 1) * cols), last = std::min(int(view.size()), (top + rows + 1) * cols);
    for (int i = first; i < last && busy < 2; ++i) {
      const Item& it = items[size_t(view[size_t(i)])];
      if (it.coverKey.empty() || covers_.Known(it.coverKey)) continue;
      std::string p = it.page >= 0 ? "/api/cover/" + std::string(kind) + "/" + std::to_string(it.page) + ".jpg" : "/api/icon/" + it.coverKey;
      if (Inflight(p)) continue;
      Ask("GET", p);
      ++busy;
    }
    const Item* f = sel >= 0 && sel < int(view.size()) ? &items[size_t(view[size_t(sel)])] : nullptr;
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
      else if (g->dlState == "removing") Toast(g->title + ": removing the local copy, wait for it");   // a removal is never cancelled
      else if (RmAsked(*g)) Toast("Remove asked: mac1's helper starts it");
      else if (DlActive(g->dlState) || DlAsked(*g))
        Ask_("cancel_dl", *g, "Stop downloading " + g->title + "?",
             "What arrived so far is deleted from this console. The game keeps streaming from mac1.", "Stop download");
      else if (g->onConsole && g->local == "pkg") Toast(g->title + " is an installed game: remove it in the PS5's Storage settings");
      else if (Removable(*g)) AskRemoveLocal(*g);
      else if (g->onConsole) Toast(g->title + " plays from this console" + (g->copies.empty() ? "" : ": " + g->copies));
      else if (g->local == "starter") AskDownload(*g);
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
  void Ask_(const std::string& action, const Game& g, const std::string& title, const std::string& body, const std::string& yes,
            const std::string& altAction = "", const std::string& alt = "") {
    confirm_ = {true, action, g.tid, title, body, yes, altAction, alt};
  }
  // The drive downloads go to (the payload's homebrew drive): its name on screen and its free space now
  struct HomeDrive { const char* name; long long freeMb; };
  HomeDrive Home() {
    const std::string hb = status_["homebrew_dir"].str();
    const std::string drv = hb.rfind("/mnt/ext1", 0) == 0 ? "ext1" : hb.rfind("/mnt/ext0", 0) == 0 ? "ext0" : "data";
    return {drv == "ext1" ? "M.2" : drv == "ext0" ? "extended storage" : "console storage", status_["free_mb"][drv.c_str()].ll()};
  }
  // Remove the local copy (0.5.1): the space it frees; mac1 puts the streaming starter back
  void AskRemoveLocal(const Game& g) {
    const HomeDrive d = Home();
    const long long bytes = g.localBytes > 0 ? g.localBytes : g.size;
    std::string body = "Frees " + Gb(bytes) + " on the " + d.name + (d.freeMb >= 0 ? " (" + FreeText(d.freeMb + bytes / (1024 * 1024)) + " after)" : "") +
                       ". The game streams from mac1 again.";
    if (wall_ - status_["dl_worker_at"].ll() > 120) body += " mac1's download helper is not answering right now: it starts when it is back.";
    Ask_("remove_local", g, "Remove the local copy of " + g.title + "?", body, "Remove local copy");
  }
  // Download to this console: size, free space now and after, the time at mac1's measured speed, the data it uses when away
  void AskDownload(const Game& g) {
    const HomeDrive d = Home();
    const char* drvName = d.name;
    const long long freeMb = d.freeMb, keepMb = std::max(status_["min_free_mb"].ll(), 0LL);
    const long long need = g.size, afterMb = freeMb - need / (1024 * 1024);
    if (need <= 0) { Toast(g.title + ": its size is not known yet"); return; }
    if (afterMb < keepMb) {
      Toast("Not enough space on the " + std::string(drvName) + ": " + Gb(need) + " needed, " + FreeText(freeMb));
      return;
    }
    long long bps = status_["dl_rate"].ll();
    const bool measured = bps > 0 && wall_ - status_["dl_rate_at"].ll() < 3600;
    if (!measured) bps = 850000;                         // 24 Oct 2026, PHAT5 away on a phone hotspot: ~0.85 MB/s
    const long long s = need / std::max(bps, 1LL), h = s / 3600, m = s % 3600 / 60;
    char t[96];
    std::snprintf(t, sizeof t, "%s%lld h %02lld min", measured ? "About " : "Roughly ", h, m);
    std::string body = Gb(need) + " onto the " + drvName + " (" + FreeText(freeMb) + " now, " + FreeText(afterMb) + " after). " + t + " at " +
                       std::to_string(bps / 1000) + " KB/s. ";
    if (status_["route"].str() == "TAILNET") body += "You are away: it uses " + Gb(need) + " of this connection's data. ";
    body += "It keeps streaming meanwhile and switches to the console copy by itself once every piece is checked.";
    if (wall_ - status_["dl_worker_at"].ll() > 120) body += " mac1's download helper is not answering right now: it starts when it is back.";
    Ask_("download", g, "Download " + g.title + " to this console?", body, "Download", "remove", "Remove starter");
  }
  void PressConfirm(Button b) {
    if (b == kCross && confirm_.action == "store_install") { Ask("POST", "/api/store/install?" + confirm_.tid); confirm_.active = false; return; }
    if (b == kCross && confirm_.action == "pm_install") { Ask("POST", "/api/pm/install?path=" + PathEnc(confirm_.tid)); confirm_.active = false; return; }
    if (b == kCross) { Act(confirm_.action, confirm_.tid); Toast(confirm_.yes + ": queued"); confirm_.active = false; }
    if (b == kSquare && !confirm_.altAction.empty()) { Act(confirm_.altAction, confirm_.tid); Toast(confirm_.alt + ": queued"); confirm_.active = false; }
    if (b == kCircle) confirm_.active = false;
  }
  bool PmBusy() const { return pmStatus_["is_installing"].truthy() || pmQueue_["active_id"].ll() > 0; }
  static std::string PathEnc(const std::string& s) {           // a query value: everything but unreserved characters %-encoded
    static const char* hx = "0123456789ABCDEF"; std::string o;
    for (unsigned char ch : s) {
      if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == '/') o += char(ch);
      else { o += '%'; o += hx[ch >> 4]; o += hx[ch & 15]; }
    }
    return o;
  }
  static std::string Slug(const std::string& s) {               // = bm_store.c store_slug: a payload slot name
    std::string o;
    for (unsigned char ch : s) { char c = char(std::tolower(ch)); o += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '-'; }
    return o;
  }
  void PressInstall(Button b) {
    // L2 / R2 (or left / right) walk the sources: the STORE first, then every drive PKG Manager has packages on
    if (b == kLeft || b == kRight || b == kL2 || b == kR2) {
      const int k = int(pmDrives_.size()) + 1, pos = storeOn_ ? 0 : pmDrive_ + 1;
      const int next = (pos + ((b == kRight || b == kR2) ? 1 : k - 1)) % k;
      storeOn_ = next == 0;
      if (!storeOn_) { pmDrive_ = next - 1; pmPicked_ = true; pmSel_ = 0; pm_.clear(); } else nextStore_ = 0;
      return;
    }
    if (storeOn_) { PressStore(b); return; }
    const int n = int(pm_.size());
    if (b == kUp && n) pmSel_ = (pmSel_ + n - 1) % n;
    if (b == kDown && n) pmSel_ = (pmSel_ + 1) % n;
    if (b == kTriangle) Ask("POST", "/api/pm/refresh");
    if (b == kSquare && PmBusy()) Ask("POST", "/api/pm/cancel?id=" + std::to_string(pmQueue_["active_id"].ll()));
    if (b == kCross && pmSel_ < n) {
      const PmPkg& k = pm_[size_t(pmSel_)];
      if (!k.canInstall) { Toast("Can't install: " + (k.why.empty() ? std::string("PKG Manager says no") : k.why)); return; }
      if (PmBusy()) { Toast("One install at a time: wait for this one, or {SQUARE} to cancel it"); return; }
      const json::Value& fm = status_["free_mb"];
      std::string body = (k.type.empty() ? std::string("Game") : k.type == "base" ? std::string("Game") : k.type == "update" ? std::string("Update") : k.type == "dlc" ? std::string("Add-on") : k.type) +
                         " " + k.version + ", " + Gb(k.size) + (k.parts > 1 ? " in " + std::to_string(k.parts) + " parts" : "") +
                         ". It installs straight from your drives: nothing is copied to the console first, so it needs " + Gb(k.size) +
                         " on the console, not twice that. Free now: " + FreeText(fm["ext1"].ll(-1) > 0 ? fm["ext1"].ll() : fm["data"].ll(-1)) + ".";
      if (k.installed) body += " It is already installed" + (k.installedVersion.empty() ? std::string("") : " (" + k.installedVersion + ")") + ".";
      confirm_ = {true, "pm_install", k.path, "Install " + k.title + "?", body, "Install", "", ""};
    }
  }
  void PressStore(Button b) {
    const int n = int(store_.size());
    if (b == kUp && n) stSel_ = (stSel_ + n - 1) % n;
    if (b == kDown && n) stSel_ = (stSel_ + 1) % n;
    if (b == kTriangle) nextStore_ = 0;
    if (b == kCross && stSel_ < n) {
      const StoreItem& it = store_[size_t(stSel_)];
      if (storeJob_["state"].str() == "running") { Toast("One install at a time: this one is still running"); return; }
      if (it.installed) { Toast(it.name + " is already on this console"); return; }
      const char* what = it.kind == "app" ? "App" : it.kind == "payload" ? "Payload" : "PKG";
      std::string body = std::string(what) + (it.version.empty() ? "" : " " + it.version) + ", " + Gb(it.size) +
                         (it.kind == "app" ? ". It is copied from your drives into this console's homebrew, then it shows up as a tile."
                          : it.kind == "payload" ? ". It goes into the payload manager as its own slot; it is NOT loaded and NOT added to the boot list: load it there when you want it."
                          : ". PKG Manager on this console installs it straight from your drives.");
      std::string q = "kind=" + PathEnc(it.kind) + "&path=" + PathEnc(it.path) + "&name=" + PathEnc(it.name) + "&size=" + std::to_string(it.size);
      confirm_ = {true, "store_install", q, "Install " + it.name + "?", body, "Install", "", ""};
    }
  }
  void DrawStore(Batch& b, float listR) {
    std::string head = storeMissing_ ? std::string("The store needs BlazingMount 2.9 on this console and imgserve on the drives")
                     : !storeTried_ ? std::string("Looking at your drives...")
                     : "Store  \xC2\xB7  Apps and Emulators on your drives  \xC2\xB7  " + std::to_string(store_.size()) + " items";
    b.Text(kGutter, 140, Fit(b, head, St(kBody, c::kTextHi), listR - kGutter - 200).c_str(), St(kBody, c::kTextHi));
    b.Text(listR, 144, "{L2} {R2} source", St(kMeta, c::kTextDim), Align::kRight);
    const float top = 200, rowH = 88, gap = 8; const int vis = 8;
    if (store_.empty() && storeTried_) b.Text(kGutter, top + 20, "Nothing in Apps and Emulators/PS5 yet.", St(kMeta, c::kTextDim));
    stTop_ = std::clamp(stTop_, std::max(0, stSel_ - vis + 1), stSel_);
    for (int i = 0; i < vis && stTop_ + i < int(store_.size()); ++i) {
      const int idx = stTop_ + i; const StoreItem& it = store_[size_t(idx)]; const bool f = idx == stSel_;
      const float y0 = top + i * (rowH + gap), y1 = y0 + rowH;
      if (f) Round(b, kGutter, y0, listR, y1, 16, c::kFocusFill);
      const Rgba under = f ? c::kFocusFill : c::kBg;
      const char* badge = it.installed ? "INSTALLED" : it.kind == "app" ? "APP" : it.kind == "payload" ? "PAYLOAD" : "PKG";
      const float pl = Pill(b, listR - 16, (y0 + y1) / 2, badge, f ? 0 : (it.installed ? 1 : 0), under);
      const float tx = kGutter + 24, tw = pl - 20 - tx;
      b.Text(tx, y0 + 9, Fit(b, it.name, St(kBody, f ? c::kTextHi : c::kText), tw).c_str(), St(kBody, f ? c::kTextHi : c::kText));
      std::string meta = (it.tid.empty() ? std::string("") : it.tid + "  \xC2\xB7  ") + (it.version.empty() ? std::string("") : it.version + "  \xC2\xB7  ") + Gb(it.size) +
                         (it.kind == "app" ? "  \xC2\xB7  " + std::to_string(it.files) + " files" : "");
      b.Text(tx, y0 + 48, Fit(b, meta, St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f), tw).c_str(), St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f));
    }
    const float x0 = listR + 40, x1 = kEdge;
    Round(b, x0, 140, x1, kBottom, 16, c::kSurface);
    b.Text(x0 + 32, 168, "NOW", St(kMeta, c::kTextDim));
    const json::Value& j = storeJob_;
    const std::string st = j["state"].str();
    float y = 210;
    if (st == "running") {
      y = Wrap(b, x0 + 32, y, x1 - x0 - 64, j["name"].str(), St(kHead - 4, c::kTextHi), 2, 48) + 12;
      const long long done = j["bytes_done"].ll(), total = std::max(1LL, j["bytes"].ll());
      const float frac = float(std::clamp(double(done) / double(total), 0.0, 1.0));
      Round(b, x0 + 32, y, x1 - 32, y + 20, 10, c::kSurfaceHi);
      if (frac > 0) Round(b, x0 + 32, y, x0 + 32 + (x1 - x0 - 64) * frac, y + 20, 10, c::kAccent);
      y += 44;
      char t[160]; std::snprintf(t, sizeof t, "%.0f%%   %s of %s   \xC2\xB7   file %lld of %lld", frac * 100.0, Gb(done).c_str(), Gb(total).c_str(),
                                 j["files_done"].ll() + 1 > j["files"].ll() ? j["files"].ll() : j["files_done"].ll() + 1, j["files"].ll());
      b.Text(x0 + 32, y, t, St(kMeta, c::kTextHi, -0.03f)); y += 40;
      y = Wrap(b, x0 + 32, y, x1 - x0 - 64, j["step"].str(), St(kMeta - 2, c::kText, -0.03f), 2, 32) + 10;
    } else {
      b.Text(x0 + 32, y, "Nothing installing", St(kHead - 4, c::kTextHi)); y += 70;
      if (st == "done" || st == "failed") {
        b.Text(x0 + 32, y, "LAST", St(kMeta, c::kTextDim)); y += 42;
        y = Wrap(b, x0 + 32, y, x1 - x0 - 64, j["name"].str() + ": " + (st == "done" ? j["step"].str() : "failed - " + j["err"].str()),
                 St(kMeta - 2, st == "failed" ? c::kAccent : c::kText, -0.03f), 4, 32) + 10;
      }
    }
    b.Text(x0 + 32, kBottom - 52, "{CROSS} Install   {TRIANGLE} Look again", St(kMeta, c::kTextDim));
  }
  void DrawInstall(Batch& b) {
    const float listR = 1150;
    if (storeOn_) { DrawStore(b, listR); return; }
    // the drive: L2 / R2 (or left / right) walk the drives PKG Manager has packages on
    std::string dl = pmDrives_.empty() ? std::string(pmDown_ ? "PKG Manager is not running on this console" : pmTried_ ? "No drive with packages" : "Looking at your drives...")
                                       : pmDrives_[size_t(pmDrive_)].label + "  \xC2\xB7  " + std::to_string(pmDrives_[size_t(pmDrive_)].count) + " packages";
    b.Text(kGutter, 140, Fit(b, dl, St(kBody, c::kTextHi), listR - kGutter - 200).c_str(), St(kBody, c::kTextHi));
    b.Text(listR, 144, "{L2} {R2} source", St(kMeta, c::kTextDim), Align::kRight);
    const float top = 200, rowH = 88, gap = 8; const int vis = 8;
    if (pm_.empty()) b.Text(kGutter, top + 20, pmDown_ ? "Start PKG Manager (its tile on the home screen), then come back." : "Nothing to install here.", St(kMeta, c::kTextDim));
    pmTop_ = std::clamp(pmTop_, std::max(0, pmSel_ - vis + 1), pmSel_);
    for (int i = 0; i < vis && pmTop_ + i < int(pm_.size()); ++i) {
      const int idx = pmTop_ + i; const PmPkg& k = pm_[size_t(idx)]; const bool f = idx == pmSel_;
      const float y0 = top + i * (rowH + gap), y1 = y0 + rowH;
      if (f) Round(b, kGutter, y0, listR, y1, 16, c::kFocusFill);
      const Rgba under = f ? c::kFocusFill : c::kBg;
      // PKG Manager's own verdict, in words: "same or newer" = it is installed; "base package" = an update for a game not on the console
      const bool same = k.why.find("same or newer") != std::string::npos, needsBase = k.why.find("Base package") != std::string::npos;
      const char* badge = (k.installed && !k.canInstall) || same ? "INSTALLED" : needsBase ? "NEEDS THE GAME" : !k.canInstall ? "CAN'T INSTALL"
                        : k.installed ? "INSTALLED" : k.type == "update" ? "UPDATE" : k.type == "dlc" ? "ADD-ON" : "INSTALL";
      const int tone = (k.installed || same) ? 1 : (!k.canInstall && !needsBase) ? 2 : 0;
      const float pl = Pill(b, listR - 16, (y0 + y1) / 2, badge, f ? 0 : tone, under);
      const float tx = kGutter + 24, tw = pl - 20 - tx;
      b.Text(tx, y0 + 9, Fit(b, k.title, St(kBody, f ? c::kTextHi : c::kText), tw).c_str(), St(kBody, f ? c::kTextHi : c::kText));
      std::string meta = k.tid + "  \xC2\xB7  " + (k.type == "update" ? "update " : k.type == "dlc" ? "add-on " : "") + k.version + "  \xC2\xB7  " + Gb(k.size) +
                         (k.parts > 1 ? "  \xC2\xB7  " + std::to_string(k.parts) + " parts" : "") +
                         (k.installed && !k.installedVersion.empty() ? "  \xC2\xB7  on console " + k.installedVersion : "") +
                         (!k.canInstall && !same && !k.why.empty() ? "  \xC2\xB7  " + k.why : "");
      b.Text(tx, y0 + 48, Fit(b, meta, St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f), tw).c_str(), St(kMeta - 2, f ? c::kTextHi : c::kTextDim, -0.03f));
    }
    // right: what is installing now, live from PKG Manager
    const float x0 = listR + 40, x1 = kEdge;
    Round(b, x0, 140, x1, kBottom, 16, c::kSurface);
    b.Text(x0 + 32, 168, "NOW", St(kMeta, c::kTextDim));
    const json::Value& s = pmStatus_;
    float y = 210;
    if (PmBusy()) {
      y = Wrap(b, x0 + 32, y, x1 - x0 - 64, s["title_name"].str().empty() ? s["title_id"].str() : s["title_name"].str(), St(kHead - 4, c::kTextHi), 2, 48) + 12;
      const long long done = s["downloaded_bytes"].ll(), total = s["total_bytes"].ll();
      const double pct = std::clamp(s["progress"].num(), 0.0, 100.0);
      Round(b, x0 + 32, y, x1 - 32, y + 20, 10, c::kSurfaceHi);
      if (pct > 0) Round(b, x0 + 32, y, x0 + 32 + (x1 - x0 - 64) * float(pct / 100.0), y + 20, 10, c::kAccent);
      y += 44;
      char t[160];
      std::snprintf(t, sizeof t, "%.0f%%   %s of %s", pct, Gb(done).c_str(), Gb(total).c_str());
      b.Text(x0 + 32, y, t, St(kBody, c::kTextHi)); y += 44;
      if (pmRateAt_ > 0 && now_ - pmRateAt_ >= 3 && done > pmRateDone_) { pmRate_ = (done - pmRateDone_) / (now_ - pmRateAt_); pmRateAt_ = now_; pmRateDone_ = done; }
      if (pmRateAt_ <= 0 || done < pmRateDone_) { pmRateAt_ = now_; pmRateDone_ = done; }
      if (pmRate_ > 0 && total > done) {
        const long long left = (long long)((total - done) / pmRate_);
        std::snprintf(t, sizeof t, "%.1f MB/s   \xC2\xB7   about %lld min left", pmRate_ / 1e6, std::max(1LL, left / 60));
        b.Text(x0 + 32, y, t, St(kMeta, c::kText, -0.03f)); y += 40;
      }
      if (s["is_multipart"].truthy()) { std::snprintf(t, sizeof t, "part %lld of %lld", s["current_part"].ll(), s["total_parts"].ll()); b.Text(x0 + 32, y, t, St(kMeta, c::kText, -0.03f)); y += 40; }
      b.Text(x0 + 32, y + 10, "{SQUARE} Cancel this install", St(kMeta, c::kTextHi)); y += 70;
    } else {
      b.Text(x0 + 32, y, "Nothing installing", St(kHead - 4, c::kTextHi)); y += 70;
      pmRateAt_ = 0; pmRate_ = 0;
    }
    b.Text(x0 + 32, y, "LATELY", St(kMeta, c::kTextDim)); y += 42;
    const json::Value& jobs = pmQueue_["jobs"];
    for (size_t i = jobs.size(); i-- > 0 && y < kBottom - 120;) {
      const json::Value& j = jobs[i];
      const std::string st = j["state"].str(), err = j["error"].str();
      std::string line = j["title_name"].str() + ": " + (st == "canceled" && !err.empty() ? err : st);
      y = Wrap(b, x0 + 32, y, x1 - x0 - 64, line, St(kMeta - 2, st == "failed" ? c::kAccent : c::kText, -0.03f), 2, 32) + 10;
    }
    b.Text(x0 + 32, kBottom - 52, "{CROSS} Install   {TRIANGLE} Look again", St(kMeta, c::kTextDim));
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
    const char* tabs[] = {"Library", "Discover", "Install", "Activity", "Settings"};
    float x = kGutter + b.Width("BLAZINGMOUNT", St(kHead, c::kTextHi)) + 64;
    b.Text(x, 44, "{L1}", St(kMeta, c::kTextDim));
    x += 64;
    for (int i = 0; i < 5; ++i) {
      const bool on = int(tab_) == i;
      TextStyle st = St(kBody + 2, on ? c::kTextHi : c::kTextDim);
      b.Text(x, 42, tabs[i], st);
      const float w = b.Width(tabs[i], st);
      if (on) b.Rect(x, 86, x + w, 92, c::kAccent);   // the one accent bar
      x += w + 48;
      if (i == int(Tab::kDiscover) && outCount_ > 0) {   // wishlist games out now: a solid red count on the tab
        const std::string n = std::to_string(outCount_);
        const float cw = std::max(30.0f, b.Width(n.c_str(), St(kWhisper, c::kTextHi)) + 16);
        Round(b, x - 44, 34, x - 44 + cw, 64, 15, c::kAccent);
        b.Text(x - 44 + cw / 2, b.TopForCentre(49, kWhisper), n.c_str(), St(kWhisper, c::kTextHi), Align::kCentre);
        x += cw - 12;
      }
    }
    b.Text(x - 16, 44, "{R1}", St(kMeta, c::kTextDim));
    const float tabsEnd = x + 48;
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
        const float room = std::min(360.0f, r - tabsEnd - 28 - b.Width("BUILDING ", St(kMeta - 2, c::kTextHi)));
        if (room > 60) {
          std::string t = "BUILDING " + Fit(b, busy, St(kMeta - 2, c::kTextHi), room);
          r = Pill(b, r, 64, t.c_str(), 3, c::kBg) - 16;
        }
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

  void DrawGrid(Batch& b) { DrawGridOf(b, items_, view_, sel_, top_, false); }
  void DrawGridOf(Batch& b, const std::vector<Item>& items, const std::vector<int>& view, int sel, int& top, bool discover) {
    const int rowSel = sel / kCols;
    top = std::clamp(top, std::max(0, rowSel - kGridRows + 1), rowSel);
    for (int r = 0; r < kGridRows; ++r)
      for (int col = 0; col < kCols; ++col) {
        const int idx = (top + r) * kCols + col;
        if (idx >= int(view.size())) return;
        const Item& it = items[size_t(view[size_t(idx)])];
        const bool f = idx == sel, rowFocused = (top + r) == rowSel;
        const float x0 = kGutter + col * kPitchX + 10, y0 = kListTop + r * kPitchY, x1 = x0 + kCard, y1 = y0 + kCard;
        if (f) Round(b, x0 - 6, y0 - 6, x1 + 6, y1 + 6, 16, c::kAccent);   // DESIGN-V2 2.6: the 6 px accent halo is the slot
        Art(b, it, x0, y0, x1, y1, f ? 1.0f : rowFocused ? 0.65f : 0.45f, false);
        if (f) b.Rect(x0, y1 + 12, x1, y1 + 18, c::kAccent);              // ...and the one accent bar under the artwork
        const Badge bd = discover ? DBadge(it) : ItemBadge(it, games_);
        if (bd.tone != 0 || f) Pill(b, x1 - 6, y1 - 26, bd.text, bd.tone, c::kBg);
        b.Text(x0, y1 + 24, Fit(b, it.title, St(kMeta, f ? c::kTextHi : c::kText), kCard).c_str(), St(kMeta, f ? c::kTextHi : c::kText));
        std::string sub = discover ? (it.dateText.empty() ? it.sysLabels : it.dateText + "  \xC2\xB7  " + it.sysLabels)
                                   : it.game >= 0 ? games_[size_t(it.game)].tid : SystemLabel(it);
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
      if (it.files) meta += "  \xC2\xB7  " + std::string(it.firstType == "folder" ? "" : ".") + it.firstType + (it.compressed ? " compressed" : "") + "  \xC2\xB7  " + Gb(it.size);
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
    std::string ids = it->ids;   // whole ids only: cut at the last ", " that fits, then "+N more" (never a dangling comma)
    if (ids.size() > 40) {
      size_t cut = ids.rfind(", ", 40);
      int rest = 0;
      for (size_t p = cut; p != std::string::npos; p = ids.find(", ", p + 2)) ++rest;
      ids = cut == std::string::npos ? ids.substr(0, ids.find(", ")) : ids.substr(0, cut) + "  +" + std::to_string(rest) + " more";
    }
    Fact(b, px, y, pw, "System", SystemLabel(*it) + (ids.empty() ? "" : "   " + ids));
    Fact(b, px, y, pw, "Yours", how);
    if (it->files) {
      std::string f = it->firstPath + "   " + std::string(it->firstType == "folder" ? "" : ".") + it->firstType + (it->compressed ? " (compressed)" : " (not compressed)") + (it->verified ? ", verified" : "")
                    + (it->size ? "   " + Gb(it->size) : "");
      if (it->files > 1) f += "   +" + std::to_string(it->files - 1) + " more";
      Fact(b, px, y, pw, "On my drives", f, c::kGood);
    } else Fact(b, px, y, pw, "On my drives", it->streamable ? "streamed from the drives" : "not yet", it->streamable ? c::kGood : c::kTextDim);
    Fact(b, px, y, pw, "Plays on", g && g->onConsole ? "This PS5, from its own drive" : it->plays, g && g->onConsole ? c::kGood : c::kText);
    if (g) {
      Fact(b, px, y, pw, "Needs FW", g->fw.empty() ? "" : g->fw + "   (this console " + consoleFw_ + ")", g->state == "needs_fw" ? c::kAccent : c::kText);
      std::string here = g->onConsole ? "ON THIS CONSOLE: plays without the network" : g->local == "starter" ? "Streams from mac1 (a small starter here)"
                       : g->local == "full" ? "Full copy" : g->local == "image" ? "Image file" : "Not installed";
      if (!g->copies.empty()) here += g->onConsole ? ". Copy: " + g->copies : ". Also here: " + g->copies;
      else if (!g->localPath.empty()) here += ": " + g->localPath;
      if (g->pinned) here += "  (pinned)";
      Fact(b, px, y, pw, "This console", here, g->onConsole ? c::kGood : g->local == "none" ? c::kTextDim : c::kText);
      const bool rm = g->request == "remove_local" || g->dlState == "removing" || (g->dlState == "removed" && !DlAsked(*g));
      if (rm) {                                          // a removal: no "X of Y GB"; the job line of the old download is not this
        std::string d = RmAsked(*g) ? "asked, mac1 starts it" : g->dlState;
        if (!RmAsked(*g) && !g->dlMsg.empty()) d += ": " + g->dlMsg;
        Fact(b, px, y, pw, "Local copy", d, g->dlState == "failed" ? c::kAccent : c::kText);
      } else if (!g->dlState.empty() || g->request == "download") {
        std::string d = DlAsked(*g) ? "asked, mac1 starts it" : g->dlState;
        if (g->dlTotal > 0 && !DlAsked(*g)) d += "   " + Gb(g->dlDone) + " of " + Gb(g->dlTotal);
        if (g->dlState == "sending" && g->dlEta > 0) d += "   about " + std::to_string(g->dlEta / 3600) + " h " + std::to_string(g->dlEta % 3600 / 60) + " min left";
        if (!g->dlMsg.empty() && !DlAsked(*g)) d += "   " + g->dlMsg;
        Fact(b, px, y, pw, "Download", d, g->dlState == "failed" ? c::kAccent : c::kText);
      }
      Fact(b, px, y, pw, "Played", g->proof.empty() ? "no proof yet" : g->proof, g->proof.empty() ? c::kTextDim : c::kGood);
      if (!g->reason.empty()) Fact(b, px, y, pw, g->build == "done" ? "Last build" : "Why", g->reason, g->build == "done" ? c::kText : c::kAccent);
    }
    std::string hint;
    if (g) {
      if (DlActive(g->dlState)) hint = g->dlState == "removing" ? "" : "{CROSS} Stop download   ";
      else if (Removable(*g) && !RmAsked(*g) && g->state != "playing") hint = "{CROSS} Remove local copy   ";
      else if (g->local == "starter" && !g->onConsole && g->state != "playing") hint = "{CROSS} Download to console   ";
      else if (g->local == "none" && g->state != "needs_fw" && g->state != "building") hint = "{CROSS} Install   ";
      if (g->local == "starter" && g->onDrives && g->state != "playing") hint += "{TRIANGLE} Rebuild   ";
      hint += g->choice == "skip" ? "{SQUARE} Show" : "{SQUARE} Hide";
    } else hint = it->plays.empty() ? "" : "Plays on " + it->plays;
    b.Rect(px, y1 - 88, px + pw, y1 - 86, c::kLine);
    b.Text(px, y1 - 68, Fit(b, hint, St(kMeta, c::kText), pw).c_str(), St(kMeta, c::kText));
  }

  // ---------------------------------------------------------------- Discover: games he does not have yet (docs/UI-DESIGN.md 10.5)
  static std::string QueryArg(const std::string& path, const std::string& name) {
    const auto q = path.find('?');
    if (q == std::string::npos) return "";
    const std::string s = "&" + path.substr(q + 1) + "&";
    auto p = s.find("&" + name + "=");
    if (p == std::string::npos) return "";
    p += name.size() + 2;
    return s.substr(p, s.find('&', p) - p);
  }
  static std::string Thousands(long long n) {
    std::string d = std::to_string(n), out;
    for (size_t i = 0; i < d.size(); ++i) { if (i && (d.size() - i) % 3 == 0) out += ','; out += d[i]; }
    return out;
  }
  const Item* FocusedD() const { return dSel_ >= 0 && dSel_ < int(dView_.size()) ? &dItems_[size_t(dView_[size_t(dSel_)])] : nullptr; }
  bool Wished(const Item& it) const { auto w = wished_.find(it.id); return w != wished_.end() && w->second; }
  std::string SysName(const std::string& key) const {
    for (const auto& sy : catalog_["systems"].items) if (sy["key"].str() == key) return sy["label"].str();
    static const std::map<std::string, std::string> k = {{"ps5", "PS5"}, {"ps4", "PS4"}, {"switch", "Switch"}, {"switch2", "Switch 2"},
                                                         {"pc", "PC"}, {"xsx", "Xbox Series X/S"}, {"xone", "Xbox One"}};
    auto f = k.find(key);
    return f != k.end() ? f->second : key;
  }
  Badge DBadge(const Item& it) const {
    auto a = alertOf_.find(it.id);
    const bool w = Wished(it);
    if (w && a != alertOf_.end() && a->second == "out") return {"OUT NOW", 3};
    if (w) return {"WISHLIST", 2};
    if (it.files) return {"ON MY DRIVES", 1};
    if (!it.how.empty()) return {it.how == "ps_plus" ? "PS PLUS" : "OWNED", 1};
    return {"NOT OWNED", 0};
  }
  void RebuildDiscover() {
    const std::string keep = FocusedD() ? FocusedD()->id : "";
    dItems_.clear(); dsheetCells_.clear(); alertOf_.clear(); dById_.clear(); dShelves_.clear();
    for (const auto& j : discover_["games"].items) {
      Item it;
      it.id = j["id"].str(); it.title = j["title"].str(); it.how = j["owned"].str(); it.files = j["on_drives"].truthy() ? 1 : 0;
      it.summary = j["summary"].str(); it.dateText = j["date_text"].str(); it.sysLabels = j["sys_labels"].str(); it.date = j["date"].ll();
      it.hype = int(j["hype"].ll()); it.rating = int(j["rating"].ll()); it.votes = int(j["votes"].ll());
      for (const auto& x : j["systems"].items) it.syss.push_back(x.str());
      it.system = it.syss.empty() ? "" : it.syss[0];
      it.family = "Discover";
      it.colour = ParseColour(j["colour"].str());
      if (j["cover"].truthy()) {
        it.page = int(j["page"].ll()); it.cell = int(j["cell"].ll());
        dsheetCells_[it.page].push_back({it.id, it.cell});
        it.coverKey = it.id;
      }
      dById_[it.id] = int(dItems_.size());
      dItems_.push_back(std::move(it));
    }
    for (const auto& sh : discover_["shelves"].items) {
      DShelf d{sh["key"].str(), sh["label"].str(), {}};
      for (const auto& x : sh["ids"].items) { auto f = dById_.find(x.str()); if (f != dById_.end()) d.items.push_back(f->second); }
      dShelves_.push_back(std::move(d));
    }
    for (const auto& a : discover_["alerts"].items) alertOf_[a["id"].str()] = a["kind"].str();
    dShelf_ = std::clamp(dShelf_, 0, std::max(0, int(dShelves_.size()) - 1));
    RebuildDView(keep);
  }
  // a shelf's games; the Wishlist shelf is the live wishlist (mac1's /wishlist.json + presses on their way), not the daily file's copy
  std::vector<int> ShelfItems(int s) const {
    if (s < 0 || s >= int(dShelves_.size())) return {};
    const DShelf& d = dShelves_[size_t(s)];
    if (d.key != "wishlist" || !wishTried_) return d.items;
    std::vector<int> out;
    std::unordered_map<std::string, bool> seen;
    for (const auto& id : wishOrder_) {
      auto f = dById_.find(id);
      if (f != dById_.end() && Wished(dItems_[size_t(f->second)]) && !seen[id]) { out.push_back(f->second); seen[id] = true; }
    }
    return out;
  }
  void RebuildDView(const std::string& keepId) {
    dView_.clear(); dSysList_.clear();
    const std::vector<int> all = ShelfItems(dShelf_);
    static const char* pref[] = {"ps5", "ps4", "switch", "switch2", "pc", "xsx", "xone"};
    auto rank = [&](const std::string& k) { for (int i = 0; i < 7; ++i) if (k == pref[i]) return i; return 7; };
    for (int i : all)
      for (const auto& k : dItems_[size_t(i)].syss)
        if (std::find(dSysList_.begin(), dSysList_.end(), k) == dSysList_.end()) dSysList_.push_back(k);
    std::sort(dSysList_.begin(), dSysList_.end(), [&](const std::string& a, const std::string& b2) {
      return rank(a) != rank(b2) ? rank(a) < rank(b2) : SysName(a) < SysName(b2);
    });
    if (!dSys_.empty() && std::find(dSysList_.begin(), dSysList_.end(), dSys_) == dSysList_.end()) dSys_.clear();
    for (int i : all) {
      const auto& sy = dItems_[size_t(i)].syss;
      if (dSys_.empty() || std::find(sy.begin(), sy.end(), dSys_) != sy.end()) dView_.push_back(i);
    }
    if (!keepId.empty())
      for (size_t k = 0; k < dView_.size(); ++k) if (dItems_[size_t(dView_[k])].id == keepId) dSel_ = int(k);
    dSel_ = std::clamp(dSel_, 0, std::max(0, int(dView_.size()) - 1));
    outCount_ = 0;
    for (const auto& kv : alertOf_) {
      auto w = wished_.find(kv.first);
      if (kv.second == "out" && w != wished_.end() && w->second) ++outCount_;
    }
  }
  void PressDiscover(Button b) {
    if (b == kL2 || b == kR2) {
      if (!dShelves_.empty()) dShelf_ = (dShelf_ + (b == kR2 ? 1 : int(dShelves_.size()) - 1)) % int(dShelves_.size());
      dSel_ = 0; dTop_ = 0; dSys_.clear(); RebuildDView("");
      return;
    }
    if (b == kOptions) {                                     // the next system on this shelf, then All again
      if (dSysList_.empty()) return;
      auto f = std::find(dSysList_.begin(), dSysList_.end(), dSys_);
      dSys_ = dSys_.empty() ? dSysList_[0] : (f == dSysList_.end() || f + 1 == dSysList_.end()) ? std::string() : *(f + 1);
      dSel_ = 0; dTop_ = 0; RebuildDView("");
      return;
    }
    const int n = int(dView_.size());
    if (b == kUp && dSel_ - kCols >= 0) dSel_ -= kCols;
    if (b == kDown) dSel_ = std::min(n - 1, dSel_ + kCols);
    if (b == kLeft && dSel_ > 0) --dSel_;
    if (b == kRight && dSel_ + 1 < n) ++dSel_;
    dSel_ = std::clamp(dSel_, 0, std::max(0, n - 1));
    const int row = dSel_ / kCols;
    dTop_ = std::clamp(dTop_, std::max(0, row - kGridRows + 1), row);
    const Item* it = FocusedD();
    if (!it) return;
    if (b == kTriangle) {                                    // add / remove: shown at once, mac1 confirms (or the card goes back)
      const bool on = !Wished(*it);
      const std::string id = it->id, title = it->title;
      wished_[id] = on; wishPending_[id] = on;
      if (on && std::find(wishOrder_.begin(), wishOrder_.end(), id) == wishOrder_.end()) wishOrder_.push_back(id);
      Ask("POST", "/api/action?do=wish&id=" + id + "&value=" + (on ? "1" : "0"));
      Toast(on ? "Added to your wishlist: " + title : "Removed from your wishlist: " + title);
      RebuildDView(id);
    }
    if (b == kCross) Toast(Wished(*it) ? it->title + " is on your wishlist: {TRIANGLE} removes it" : "{TRIANGLE} adds " + it->title + " to your wishlist");
  }

  void DrawDiscover(Batch& b) {
    if (!discoverTried_) { b.Text(kGutter, kListTop + 20, "Reading Discover...", St(kBody, c::kTextDim)); return; }
    if (dShelves_.empty()) {
      Round(b, kGutter, kListTop, kEdge, kListTop + 260, 16, c::kSurface);
      b.Text(kGutter + 40, kListTop + 40, discoverMissing_ ? "Discover is being made on mac1" : "Discover did not answer", St(kHead, c::kTextHi));
      Wrap(b, kGutter + 40, kListTop + 110, kEdge - kGutter - 80, discoverMissing_
           ? "New releases, coming soon and the games you don't own yet, with your wishlist. The first time takes a few minutes; this page fills in by itself."
           : "mac1 did not answer through BlazingMount. This page tries again by itself.", St(kBody, c::kText, -0.03f), 3, 44);
      return;
    }
    const std::string sys = dSys_.empty() ? "All my systems" : SysName(dSys_);
    const std::string showT = "{START} " + sys;
    const float showW = b.Width(showT.c_str(), St(kMeta, c::kTextDim));
    b.Text(kEdge, 140, showT.c_str(), St(kMeta, c::kTextDim), Align::kRight);
    const float chipsR = kEdge - showW - 72;
    b.Text(kGutter, 140, "{L2}", St(kMeta, c::kTextDim));
    float x = kGutter + 56;
    char t[96];
    for (int i = 0; i < int(dShelves_.size()); ++i) {
      std::snprintf(t, sizeof t, "%s  %d", dShelves_[size_t(i)].label.c_str(), int(ShelfItems(i).size()));
      const float x1 = x + b.Width(t, St(kMeta, c::kTextDim)) + 32;
      if (x1 > chipsR) break;
      const bool on = dShelf_ == i;
      TextStyle st = St(kMeta, on ? c::kTextHi : c::kTextDim);
      if (on) Outline(b, x, 132, x1, 180, 16, 3, c::kAccent, c::kBg);
      else Outline(b, x, 132, x1, 180, 16, 2, c::kLine, c::kBg);
      b.Text(x + 16, b.TopForCentre(156, st.size), t, st);
      x = x1 + 12;
    }
    b.Text(x + 4, 140, "{R2}", St(kMeta, c::kTextDim));
    if (dView_.empty()) {
      const bool wl = dShelf_ < int(dShelves_.size()) && dShelves_[size_t(dShelf_)].key == "wishlist";
      b.Text(kGutter, kListTop + 20, wl ? "Your wishlist is empty: {TRIANGLE} on any game adds it" : "Nothing here", St(kBody, c::kTextDim));
      return;
    }
    DrawGridOf(b, dItems_, dView_, dSel_, dTop_, true);
    const float listR = 1110;
    const int rows = (int(dView_.size()) + kCols - 1) / kCols;
    if (rows > kGridRows) {
      const float h = kBottom - kListTop, th = std::max(40.0f, h * kGridRows / rows);
      const float ty = kListTop + (h - th) * (float(dTop_) / std::max(1, rows - kGridRows));
      b.Rect(listR + 14, kListTop, listR + 18, kListTop + h, c::kLine);
      b.Rect(listR + 12, ty, listR + 20, ty + th, c::kAccent);
    }
    DrawDDetail(b);
  }

  void DrawDDetail(Batch& b) {
    const Item* it = FocusedD();
    if (!it) return;
    const float x0 = 1158, y0 = kListTop, x1 = kEdge, y1 = kBottom;
    Round(b, x0, y0, x1, y1, 16, c::kSurface);
    const float px = x0 + 32, pw = x1 - x0 - 64;
    Art(b, *it, px, y0 + 32, px + 200, y0 + 232, 1.0f, true);
    const float ty = Wrap(b, px + 224, y0 + 36, pw - 224, it->title, St(kHead - 4, c::kTextHi), 3, 50);
    const Badge bd = DBadge(*it);
    Pill(b, px + 224 + b.Width(bd.text, St(kMeta - 2, c::kText)) + 28, std::max(ty + 26, y0 + 200), bd.text, bd.tone, c::kSurface);
    float y = y0 + 264;
    b.Rect(px, y - 14, px + pw, y - 12, c::kLine);
    const bool wished = Wished(*it);
    auto a = alertOf_.find(it->id);
    const bool alert = wished && a != alertOf_.end();
    std::string rel;
    if (!it->date) rel = "not announced yet";
    else if (wall_ && it->date <= wall_) rel = "out since " + it->dateText;
    else {
      rel = it->dateText;
      if (wall_) { const long long d = (it->date - wall_ + 86399) / 86400; rel += "   (in " + std::to_string(d) + (d == 1 ? " day)" : " days)"); }
    }
    Fact(b, px, y, pw, "Release", rel, alert ? c::kAccent : c::kText);
    if (alert && a->second == "moved") Fact(b, px, y, pw, "Date moved", "the date changed since you added it", c::kAccent);
    Fact(b, px, y, pw, "Systems", it->sysLabels);
    std::string yours = it->how.empty() ? "Not owned" : it->how == "ps_plus" ? "PS Plus" : "Owned";
    if (it->files) yours += ", on my drives";
    Fact(b, px, y, pw, "Yours", yours, it->how.empty() ? c::kTextDim : c::kGood);
    if (it->votes > 0) Fact(b, px, y, pw, "Rating", std::to_string(it->rating) + " / 100   (" + Thousands(it->votes) + " ratings)");
    else if (it->hype > 0) Fact(b, px, y, pw, "Waiting", Thousands(it->hype) + (it->hype == 1 ? " person follows it" : " people follow it"));
    const int lines = std::clamp(int((y1 - 110 - y) / 34), 0, 4);
    if (!it->summary.empty() && lines > 0) {
      b.Text(px, y, "About", St(kWhisper, c::kTextDim));
      Wrap(b, px + 190, y - 3, pw - 190, it->summary, St(kMeta, c::kText, -0.03f), lines, 34);
    }
    b.Rect(px, y1 - 88, px + pw, y1 - 86, c::kLine);
    const std::string hint = wished ? "{TRIANGLE} Remove from wishlist" : "{TRIANGLE} Add to wishlist";
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
    Round(b, 460, 300, 1460, 800, 16, c::kSurfaceHi);
    Wrap(b, 508, 340, 904, confirm_.title, St(kHead, c::kTextHi), 2, 52);
    Wrap(b, 508, 460, 904, confirm_.body, St(kBody, c::kText, -0.03f), 6, 40);
    std::string h = "{CROSS} " + confirm_.yes + (confirm_.alt.empty() ? "" : "      {SQUARE} " + confirm_.alt) + "      {CIRCLE} Cancel";
    b.Text(508, 730, h.c_str(), St(kBody, c::kTextHi));
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
