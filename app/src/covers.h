// SPDX-License-Identifier: GPL-3.0-or-later
// covers.h - the game covers: icon0.png from blazingmountd's /api/icon/<TID>, decoded (stb_image, PNG only), box-filtered to
// 128 x 128 and kept in ONE 2048 x 2048 RGBA atlas (256 slots, least recently drawn slot reused). The UI draws a cover as an
// image quad into this atlas (draw.h Batch::Image); the PS5 UI pass uploads the atlas when `version` moves, the host raster
// reads it directly. 16 MB, fixed: nothing grows (Markus: no console storage that grows - this is RAM, and capped).
#pragma once

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
  void Failed(const std::string& tid) { slot_[tid] = -1; }

  // A PNG for tid: decode, scale, place. false (remembered) when it is not a PNG this can read.
  bool Put(const std::string& tid, const std::string& png) {
    int w = 0, h = 0, n = 0;
    uint8_t* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), int(png.size()), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); Failed(tid); return false; }
    const int s = FreeSlot();
    const int ox = (s % kPerRow) * kCell, oy = (s / kPerRow) * kCell;
    for (int y = 0; y < kCell; ++y) {                 // area average (box filter) from w x h to kCell x kCell
      const int sy0 = y * h / kCell, sy1 = std::max(sy0 + 1, (y + 1) * h / kCell);
      for (int x = 0; x < kCell; ++x) {
        const int sx0 = x * w / kCell, sx1 = std::max(sx0 + 1, (x + 1) * w / kCell);
        uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
        for (int yy = sy0; yy < sy1; ++yy)
          for (int xx = sx0; xx < sx1; ++xx) {
            const uint8_t* p = px + (size_t(yy) * w + xx) * 4;
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
            ++cnt;
          }
        uint8_t* d = &rgba[(size_t(oy + y) * kSize + ox + x) * 4];
        for (int c = 0; c < 4; ++c) d[c] = uint8_t(acc[c] / cnt);
      }
    }
    stbi_image_free(px);
    for (auto it = slot_.begin(); it != slot_.end();) it = it->second == s ? slot_.erase(it) : std::next(it);   // the old owner
    slot_[tid] = s;
    used_[size_t(s)] = ++clock_;
    ++version;
    return true;
  }

 private:
  std::unordered_map<std::string, int> slot_;
  std::vector<uint64_t> used_ = std::vector<uint64_t>(kSlots, 0);
  uint64_t clock_ = 0;
  int FreeSlot() const {
    int best = 0;
    for (int i = 1; i < kSlots; ++i) if (used_[size_t(i)] < used_[size_t(best)]) best = i;
    return best;
  }
};

}  // namespace bm
