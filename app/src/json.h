// SPDX-License-Identifier: GPL-3.0-or-later
// json.h - the little JSON reader the BlazingMount app needs for blazingmountd's API (bm_api.c): objects, arrays, strings
// (with \uXXXX -> UTF-8, surrogate pairs), numbers, true/false/null. Header-only, no exceptions, no platform types.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace bm::json {

struct Value {
  enum Kind { kNull, kBool, kNumber, kString, kArray, kObject } kind = kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Value> items;                              // array
  std::vector<std::pair<std::string, Value>> fields;     // object, in order

  const Value& operator[](const char* key) const {
    static const Value none;
    for (const auto& f : fields) if (f.first == key) return f.second;
    return none;
  }
  const Value& operator[](size_t i) const { static const Value none; return i < items.size() ? items[i] : none; }
  size_t size() const { return kind == kArray ? items.size() : fields.size(); }
  std::string str(const char* def = "") const { return kind == kString ? s : kind == kNumber ? Num() : std::string(def); }
  double num(double def = 0) const { return kind == kNumber ? n : kind == kString ? std::strtod(s.c_str(), nullptr) : def; }
  long long ll(long long def = 0) const { return kind == kNumber ? (long long)n : kind == kString ? std::strtoll(s.c_str(), nullptr, 10) : def; }
  bool truthy() const { return kind == kBool ? b : kind == kNumber ? n != 0 : false; }
  bool ok() const { return kind != kNull; }

 private:
  std::string Num() const { char t[32]; std::snprintf(t, sizeof t, "%.15g", n); return t; }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : p_(text.c_str()), end_(text.c_str() + text.size()) {}
  bool Parse(Value& out) { Ws(); if (!Val(out, 0)) return false; Ws(); return p_ == end_; }

 private:
  const char* p_;
  const char* end_;
  void Ws() { while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_; }
  bool Lit(const char* w) { const char* q = p_; for (; *w; ++w, ++q) if (q >= end_ || *q != *w) return false; p_ = q; return true; }
  bool Val(Value& v, int depth) {
    if (depth > 32 || p_ >= end_) return false;
    char c = *p_;
    if (c == '{') {
      v.kind = Value::kObject; ++p_; Ws();
      if (p_ < end_ && *p_ == '}') { ++p_; return true; }
      for (;;) {
        Ws(); std::string k; if (!Str(k)) return false; Ws();
        if (p_ >= end_ || *p_ != ':') return false; ++p_; Ws();
        v.fields.emplace_back(std::move(k), Value{});
        if (!Val(v.fields.back().second, depth + 1)) return false;
        Ws(); if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        return false;
      }
    }
    if (c == '[') {
      v.kind = Value::kArray; ++p_; Ws();
      if (p_ < end_ && *p_ == ']') { ++p_; return true; }
      for (;;) {
        Ws(); v.items.emplace_back(); if (!Val(v.items.back(), depth + 1)) return false; Ws();
        if (p_ < end_ && *p_ == ',') { ++p_; continue; }
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        return false;
      }
    }
    if (c == '"') { v.kind = Value::kString; return Str(v.s); }
    if (Lit("true")) { v.kind = Value::kBool; v.b = true; return true; }
    if (Lit("false")) { v.kind = Value::kBool; v.b = false; return true; }
    if (Lit("null")) { v.kind = Value::kNull; return true; }
    char* q = nullptr;
    v.n = std::strtod(p_, &q);
    if (q == p_ || q > end_) return false;
    v.kind = Value::kNumber; p_ = q; return true;
  }
  static void Utf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) o += char(cp);
    else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
  }
  bool Hex4(uint32_t& v) {
    if (end_ - p_ < 4) return false;
    v = 0;
    for (int i = 0; i < 4; ++i) {
      char h = *p_++; v <<= 4;
      if (h >= '0' && h <= '9') v |= uint32_t(h - '0'); else if (h >= 'a' && h <= 'f') v |= uint32_t(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') v |= uint32_t(h - 'A' + 10); else return false;
    }
    return true;
  }
  bool Str(std::string& o) {
    if (p_ >= end_ || *p_ != '"') return false;
    ++p_;
    while (p_ < end_ && *p_ != '"') {
      char c = *p_++;
      if (c != '\\') { o += c; continue; }
      if (p_ >= end_) return false;
      char e = *p_++;
      switch (e) {
        case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
        case 'b': o += '\b'; break; case 'f': o += '\f'; break;
        case 'u': {
          uint32_t cp; if (!Hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00 && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2; uint32_t lo; if (!Hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          Utf8(o, cp); break;
        }
        default: o += e;
      }
    }
    if (p_ >= end_) return false;
    ++p_;
    return true;
  }
};

inline bool Parse(const std::string& text, Value& out) { out = Value{}; return Parser(text).Parse(out); }

}  // namespace bm::json
