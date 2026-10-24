// raster.h - the UI batch (draw.h) drawn in software: the same distance cut, outline and premultiplied blend as the
// consoles' shaders (src/platform/ps5/shaders/ui.frag, src/platform/xbox/shaders/ui.hlsl), into an sRGB image as the
// Xbox's UNORM back buffer holds it. For host pictures (tools/ui/preview.cpp) and tests (tests/ui_test.cpp); no
// console uses it.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "draw.h"

namespace mf::ui {

struct Image {
  int width = 0, height = 0;
  std::vector<float> rgb;   // sRGB 0..1, 3 per pixel

  void Fill(Rgba c) {
    rgb.assign(size_t(width) * height * 3, 0.0f);
    for (size_t i = 0; i < rgb.size(); i += 3) rgb[i] = c.r / 255.0f, rgb[i + 1] = c.g / 255.0f, rgb[i + 2] = c.b / 255.0f;
  }
  bool WritePpm(const char* path) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6 %d %d 255\n", width, height);
    std::vector<uint8_t> row(size_t(width) * 3);
    for (int y = 0; y < height; ++y) {
      for (int i = 0; i < width * 3; ++i) row[size_t(i)] = uint8_t(std::lround(std::clamp(rgb[size_t(y) * width * 3 + i], 0.0f, 1.0f) * 255.0f));
      std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
  }
};

inline float SampleAtlas(const Font& f, float u, float v) {
  const float x = u * f.width - 0.5f, y = v * f.height - 0.5f;
  const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
  const float fx = x - x0, fy = y - y0;
  auto at = [&](int xx, int yy) {
    xx = std::clamp(xx, 0, int(f.width) - 1);
    yy = std::clamp(yy, 0, int(f.height) - 1);
    return f.pixels[size_t(yy) * f.width + xx] / 255.0f;
  };
  return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

inline float SmoothStep(float a, float b, float x) {
  const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3 - 2 * t);
}

// The shaders' colour of one fragment, premultiplied: rgb * alpha and alpha.
inline void Shade(float d, float fw, const Vertex& v, float out[4]) {
  const float w = std::max(fw * 0.75f, 1e-4f);
  const float edge = 0.5f - v.bias;
  const float fill = SmoothStep(edge - w, edge + w, d);
  const float outer = v.outline > 0 ? SmoothStep(edge - v.outline - w, edge - v.outline + w, d) : fill;
  const float fa = v.fill.a / 255.0f * fill;
  const float la = v.line.a / 255.0f * std::max(outer - fill, 0.0f);
  out[0] = v.fill.r / 255.0f * fa + v.line.r / 255.0f * la;
  out[1] = v.fill.g / 255.0f * fa + v.line.g / 255.0f * la;
  out[2] = v.fill.b / 255.0f * fa + v.line.b / 255.0f * la;
  out[3] = fa + la;
}

// A parallelogram from Batch::Slant (vertices top-left, top-right, bottom-right, top-left, bottom-right, bottom-left):
// solid, so one sample of the atlas's solid cell colours it
inline void RasterSlant(const Font& font, const Vertex* q, Image& img, float sx, float sy) {
  const float ty = q[0].y * sy, by = q[2].y * sy;
  const float tl = q[0].x * sx, tr = q[1].x * sx, br = q[2].x * sx, bl = q[5].x * sx;
  const int py0 = std::max(0, int(std::floor(ty))), py1 = std::min(img.height, int(std::ceil(by)));
  const float d = SampleAtlas(font, q[0].u, q[0].v);
  float s[4];
  Shade(d, 0.0f, q[0], s);
  for (int y = py0; y < py1; ++y) {
    const float cy = y + 0.5f;
    if (cy < ty || cy >= by) continue;
    const float t = (cy - ty) / (by - ty);
    const float x0 = tl + (bl - tl) * t, x1 = tr + (br - tr) * t;
    for (int x = std::max(0, int(std::floor(x0))); x < std::min(img.width, int(std::ceil(x1))); ++x) {
      const float cx = x + 0.5f;
      if (cx < x0 || cx >= x1) continue;
      float* p = &img.rgb[(size_t(y) * img.width + x) * 3];
      for (int i = 0; i < 3; ++i) p[i] = s[i] + p[i] * (1 - s[3]);
    }
  }
}

// Draws the batch's quads (6 vertices each, axis-aligned, as draw.h makes them) over the image, scaled from the
// 1080p layout to the image's size.
inline void Raster(const Font& font, const std::vector<Vertex>& vs, Image& img) {
  const float sx = img.width / kScreenW, sy = img.height / kScreenH;
  for (size_t q = 0; q + 5 < vs.size(); q += 6) {
    const Vertex& a = vs[q];
    const Vertex& c = vs[q + 2];
    if (a.x != vs[q + 5].x) {   // a slanted solid quad (Batch::Slant): its top-left is not above its bottom-left
      RasterSlant(font, vs.data() + q, img, sx, sy);
      continue;
    }
    const float x0 = a.x * sx, y0 = a.y * sy, x1 = c.x * sx, y1 = c.y * sy;
    const int px0 = std::max(0, int(std::floor(x0))), px1 = std::min(img.width, int(std::ceil(x1)));
    const int py0 = std::max(0, int(std::floor(y0))), py1 = std::min(img.height, int(std::ceil(y1)));
    if (px1 <= px0 || py1 <= py0) continue;
    const float du = (c.u - a.u) / (x1 - x0), dv = (c.v - a.v) / (y1 - y0);
    for (int y = py0; y < py1; ++y) {
      const float cy = y + 0.5f;
      if (cy < y0 || cy >= y1) continue;
      for (int x = px0; x < px1; ++x) {
        const float cx = x + 0.5f;
        if (cx < x0 || cx >= x1) continue;
        const float u = a.u + (cx - x0) * du, v = a.v + (cy - y0) * dv;
        const float d = SampleAtlas(font, u, v);
        const float ddx = SampleAtlas(font, u + du, v) - d, ddy = SampleAtlas(font, u, v + dv) - d;
        float s[4];
        Shade(d, std::fabs(ddx) + std::fabs(ddy), a, s);
        float* p = &img.rgb[(size_t(y) * img.width + x) * 3];
        for (int i = 0; i < 3; ++i) p[i] = s[i] + p[i] * (1 - s[3]);
      }
    }
  }
}

}  // namespace mf::ui
