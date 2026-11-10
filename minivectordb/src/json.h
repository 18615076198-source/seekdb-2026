#pragma once
#include "common.h"
#include <cstdio>
#include <string>

inline void jsonString(std::string &o, const std::string &s) {
  o += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          o += buf;
        } else {
          o += (char)c;
        }
    }
  }
  o += '"';
}

inline void jsonDouble(std::string &o, double v) {
  if (!std::isfinite(v)) { o += "null"; return; }
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f", v);
    o += buf;
    return;
  }
  char buf[64];
  for (int p = 15; p <= 17; ++p) {
    std::snprintf(buf, sizeof buf, "%.*g", p, v);
    if (std::strtod(buf, nullptr) == v) break;
  }
  o += buf;
}

inline void jsonValue(std::string &o, const Value &v) {
  switch (v.ty) {
    case Ty::Int: o += std::to_string(v.i); break;
    case Ty::Double: jsonDouble(o, v.d); break;
    case Ty::Str: jsonString(o, v.s); break;
    case Ty::Vec: {
      o += '[';
      for (size_t i = 0; i < v.vec.size(); ++i) {
        if (i) o += ',';
        jsonDouble(o, (double)v.vec[i]);
      }
      o += ']';
      break;
    }
  }
}
