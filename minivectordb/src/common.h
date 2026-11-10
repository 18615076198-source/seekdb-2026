#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

enum class Ty { Int, Double, Str, Vec };

struct Value {
  Ty ty = Ty::Int;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<float> vec;
  static Value ofInt(int64_t v) { Value x; x.ty = Ty::Int; x.i = v; return x; }
  static Value ofDouble(double v) { Value x; x.ty = Ty::Double; x.d = v; return x; }
  static Value ofStr(std::string v) { Value x; x.ty = Ty::Str; x.s = std::move(v); return x; }
  static Value ofVec(std::vector<float> v) { Value x; x.ty = Ty::Vec; x.vec = std::move(v); return x; }
};

struct ColumnDef {
  std::string name;
  Ty ty = Ty::Int;
  int width = 0;
  bool pk = false;
};

struct DbError : std::runtime_error {
  std::string code;
  explicit DbError(std::string c, const std::string &m = "")
      : std::runtime_error(m.empty() ? c : m), code(std::move(c)) {}
};

inline std::string lower(std::string s) {
  for (auto &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
inline std::string upper(std::string s) {
  for (auto &c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}
inline bool ieq(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}
inline std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return s.substr(a, b - a);
}

inline size_t utf8_count(const std::string &s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    size_t len = c < 0x80 ? 1 : ((c >> 5) == 0x6) ? 2 : ((c >> 4) == 0xE) ? 3 : ((c >> 3) == 0x1E) ? 4 : 0;
    if (!len || i + len > s.size()) throw DbError("invalid_utf8", "invalid utf8");
    n++;
    i += len;
  }
  return n;
}

inline int compareValue(const Value &a, const Value &b) {
  if (a.ty == Ty::Int && b.ty == Ty::Int) return a.i < b.i ? -1 : a.i > b.i ? 1 : 0;
  if (a.ty == Ty::Double && b.ty == Ty::Double) return a.d < b.d ? -1 : a.d > b.d ? 1 : 0;
  if (a.ty == Ty::Str && b.ty == Ty::Str) return a.s < b.s ? -1 : a.s > b.s ? 1 : 0;
  throw DbError("type_mismatch", "incomparable types");
}
