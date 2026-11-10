#pragma once
#include "common.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

inline uint32_t crc32Of(const char *p, size_t n) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      tab[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = tab[(c ^ (uint8_t)p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

struct Buf {
  std::string d;
  void u8(uint8_t v) { d.push_back((char)v); }
  void u32(uint32_t v) { d.append((const char *)&v, 4); }
  void i32(int32_t v) { d.append((const char *)&v, 4); }
  void i64(int64_t v) { d.append((const char *)&v, 8); }
  void f32(float v) { d.append((const char *)&v, 4); }
  void str(const std::string &s) { u32((uint32_t)s.size()); d.append(s); }
  void value(const Value &v) {
    u8((uint8_t)v.ty);
    switch (v.ty) {
      case Ty::Int: i64(v.i); break;
      case Ty::Double: d.append((const char *)&v.d, 8); break;
      case Ty::Str: str(v.s); break;
      case Ty::Vec: u32((uint32_t)v.vec.size()); for (float f : v.vec) f32(f); break;
    }
  }
};

struct Reader {
  const char *p = nullptr;
  size_t n = 0, o = 0;
  bool ok = true;
  Reader(const char *d, size_t len) : p(d), n(len) {}
  bool need(size_t k) {
    if (!ok || o + k > n) { ok = false; return false; }
    return true;
  }
  uint8_t u8() { if (!need(1)) return 0; return (uint8_t)p[o++]; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v; std::memcpy(&v, p + o, 4); o += 4; return v; }
  int32_t i32() { if (!need(4)) return 0; int32_t v; std::memcpy(&v, p + o, 4); o += 4; return v; }
  int64_t i64() { if (!need(8)) return 0; int64_t v; std::memcpy(&v, p + o, 8); o += 8; return v; }
  float f32() { if (!need(4)) return 0; float v; std::memcpy(&v, p + o, 4); o += 4; return v; }
  std::string str() {
    uint32_t len = u32();
    if (!need(len)) return {};
    std::string s(p + o, len);
    o += len;
    return s;
  }
  Value value() {
    Value v;
    v.ty = (Ty)u8();
    switch (v.ty) {
      case Ty::Int: v.i = i64(); break;
      case Ty::Double: { if (!need(8)) { ok = false; break; } std::memcpy(&v.d, p + o, 8); o += 8; } break;
      case Ty::Str: v.s = str(); break;
      case Ty::Vec: {
        uint32_t dim = u32();
        for (uint32_t k = 0; k < dim && ok; ++k) v.vec.push_back(f32());
      } break;
    }
    return v;
  }
};

inline size_t rowBytes(const std::vector<Value> &row) {
  size_t n = 24;
  for (const auto &v : row) {
    n += 24;
    switch (v.ty) {
      case Ty::Int: n += 8; break;
      case Ty::Double: n += 8; break;
      case Ty::Str: n += 32 + v.s.size(); break;
      case Ty::Vec: n += v.vec.size() * 4 + 16; break;
    }
  }
  return n;
}
