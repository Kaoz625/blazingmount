// SPDX-License-Identifier: GPL-3.0-or-later
// covers.h - the game covers: sprite sheets (64 covers of 128 px, /api/cover/sheet/<n>.jpg), the focused game's full cover
// (/api/cover/full/<id>.jpg, kept in one 512 x 512 corner) and icon0.png (/api/icon/<TID>), decoded (stb_image, PNG + JPEG), box-filtered to
// 128 x 128 and kept in ONE 2048 x 2048 RGBA atlas (256 slots, least recently drawn slot reused). The UI draws a cover as an
// image quad into this atlas (draw.h Batch::Image); the PS5 UI pass uploads the atlas when `version` moves, the host raster
// reads it directly. 16 MB, fixed: nothing grows (Markus: no console storage that grows - this is RAM, and capped).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef BM_STB_IMAGE_DONE
#define BM_STB_IMAGE_DONE
#ifndef BM_STB_EXTERNAL   // the PS5 title links the template's own stb_image (tinygltf): only the declarations here
#define STB_IMAGE_IMPLEMENTATION
#endif
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace bm {

class Covers {
 public:
  static constexpr int kSize = 2048, kCell = 128, kPerRow = kSize / kCell, kSlots = kPerRow * kPerRow;

  std::vector<uint8_t> rgba = std::vector<uint8_t>(size_t(kSize) * kSize * 4, 0);   // the atlas, row by row
  uint64_t version = 0;                                                             // bumps when a cover lands

  // Where tid's cover is in the atlas (u0, v0, u1, v1); false when it is not loaded (yet, or never: no icon).
  bool Uv(const std::string& tid, float uv[4]) {
    auto it = slot_.find(tid);
    if (it == slot_.end() || it->second < 0) return false;
    used_[size_t(it->second)] = ++clock_;
    const int s = it->second, x = (s % kPerRow) * kCell, y = (s / kPerRow) * kCell;
    const float half = 0.5f / kSize;   // half a texel in: the linear filter never reads the neighbour
    uv[0] = float(x) / kSize + half; uv[1] = float(y) / kSize + half;
    uv[2] = float(x + kCell) / kSize - half; uv[3] = float(y + kCell) / kSize - half;
    return true;
  }
  bool Known(const std::string& tid) const { return slot_.count(tid) != 0; }   // loaded, or tried and failed

  // The focused game's full cover: the bottom-right 512 x 512 of the atlas (never a thumb slot), aspect kept, centred.
  static constexpr int kFocus = 512, kFocusX = kSize - kFocus, kFocusY = kSize - kFocus;
  const std::string& focusId() const { return focusId_; }
  bool UvFocus(const std::string& id, float uv[4]) const {
    if (id != focusId_ || focusId_.empty()) return false;
    const float half = 0.5f / kSize;
    uv[0] = float(kFocusX) / kSize + half; uv[1] = float(kFocusY) / kSize + half;
    uv[2] = float(kSize) / kSize - half; uv[3] = float(kSize) / kSize - half;
    return true;
  }
  bool PutFocus(const std::string& id, const std::string& bytes) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return false; }
    const float sc = std::min(float(kFocus) / w, float(kFocus) / h);
    const int dw = std::max(1, int(w * sc)), dh = std::max(1, int(h * sc)), ox = kFocusX + (kFocus - dw) / 2, oy = kFocusY + (kFocus - dh) / 2;
    for (int y = kFocusY; y < kSize; ++y) std::memset(&rgba[(size_t(y) * kSize + kFocusX) * 4], 0, size_t(kFocus) * 4);
    Blit(px, w, h, 0, 0, w, h, ox, oy, dw, dh);
    stbi_image_free(px);
    focusId_ = id;
    ++version;
    return true;
  }

  // A sprite sheet (8 x 8 cells of `cell` px): every (id, cell) lands in its own slot.
  bool PutSheet(const std::vector<std::pair<std::string, int>>& cells, const std::string& bytes, int cell = 128) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px || w < cell || h < cell) { if (px) stbi_image_free(px); for (auto& c : cells) Failed(c.first); return false; }
    const int per = w / cell;
    for (auto& c : cells) {
      if (c.second < 0) { Failed(c.first); continue; }
      const int s = Place(c.first);
      Blit(px, w, h, (c.second % per) * cell, (c.second / per) * cell, cell, cell, (s % kPerRow) * kCell, (s / kPerRow) * kCell, kCell, kCell);
    }
    stbi_image_free(px);
    ++version;
    return true;
  }
  void Failed(const std::string& tid) { slot_[tid] = -1; }

  // A PNG for tid: decode, scale, place. false (remembered) when it is not a PNG this can read.
  bool Put(const std::string& tid, const std::string& png) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), int(png.size()), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); Failed(tid); return false; }
    const int s = Place(tid);
    Blit(px, w, h, 0, 0, w, h, (s % kPerRow) * kCell, (s / kPerRow) * kCell, kCell, kCell);
    stbi_image_free(px);
    ++version;
    return true;
  }

 private:
  std::string focusId_;
  // a slot for id (the least recently drawn one, its old owner forgotten)
  int Place(const std::string& id) {
    auto old = slot_.find(id);
    const int s = old != slot_.end() && old->second >= 0 ? old->second : FreeSlot();
    for (auto it = slot_.begin(); it != slot_.end();) it = it->second == s ? slot_.erase(it) : std::next(it);
    slot_[id] = s;
    used_[size_t(s)] = ++clock_;
    return s;
  }
  // box filter: source rect (sx, sy, sw, sh) of a w-wide RGBA image -> atlas rect (dx, dy, dw, dh)
  void Blit(const uint8_t* px, int w, int h, int sx, int sy, int sw, int sh, int dx, int dy, int dw, int dh) {
    (void)h;
    for (int y = 0; y < dh; ++y) {
      const int y0 = sy + y * sh / dh, y1 = std::max(y0 + 1, sy + (y + 1) * sh / dh);
      for (int x = 0; x < dw; ++x) {
        const int x0 = sx + x * sw / dw, x1 = std::max(x0 + 1, sx + (x + 1) * sw / dw);
        uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
        for (int yy = y0; yy < y1; ++yy)
          for (int xx = x0; xx < x1; ++xx) {
            const uint8_t* p = px + (size_t(yy) * w + xx) * 4;
            for (int k = 0; k < 4; ++k) acc[k] += p[k];
            ++cnt;
          }
        uint8_t* d = &rgba[(size_t(dy + y) * kSize + dx + x) * 4];
        for (int k = 0; k < 4; ++k) d[k] = uint8_t(acc[k] / cnt);
      }
    }
  }
  std::unordered_map<std::string, int> slot_;
  std::vector<uint64_t> used_ = std::vector<uint64_t>(kSlots, 0);
  uint64_t clock_ = 0;
  static bool Reserved(int i) { return (i % kPerRow) * kCell >= kFocusX && (i / kPerRow) * kCell >= kFocusY; }   // the focus corner
  int FreeSlot() const {
    int best = -1;
    for (int i = 0; i < kSlots; ++i) if (!Reserved(i) && (best < 0 || used_[size_t(i)] < used_[size_t(best)])) best = i;
    return best;
  }
};

}  // namespace bm
