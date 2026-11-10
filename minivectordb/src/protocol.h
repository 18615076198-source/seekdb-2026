#pragma once
#include "engine.h"
#include <iostream>
#include <string>

// Seam between the engine and the wire protocol. The official driver/format for
// 2026-competition-round2 is not published yet, so the JSON line adapter is the
// local-test default and can be replaced without touching the engine.
class ProtocolAdapter {
 public:
  virtual ~ProtocolAdapter() = default;
  virtual void writeResult(const Result &r) = 0;
  virtual void writeError(const std::string &code) = 0;
  virtual void onShutdown() {}
};

// Alternative line protocol, selected with MVDB_PROTOCOL=tsv.
// Format is a placeholder: header line + tab separated rows for SELECT,
// "OK<TAB>n" for DML, "ERROR<TAB>code" for failures. Swap once the official
// driver/format is published.
class TsvAdapter : public ProtocolAdapter {
 public:
  void writeResult(const Result &r) override {
    std::string o;
    if (r.kind == Result::Rows) {
      for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) o += '\t';
        o += r.columns[i];
      }
      o += '\n';
      for (const auto &row : r.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) o += '\t';
          o += cell(row[i]);
        }
        o += '\n';
      }
    } else if (r.kind == Result::Affected) {
      o = "OK\t" + std::to_string(r.affected) + "\n";
    } else {
      o = "OK\n";
    }
    std::cout << o << std::flush;
  }
  void writeError(const std::string &code) override {
    std::cout << "ERROR\t" << code << "\n" << std::flush;
  }

 private:
  static std::string cell(const Value &v) {
    switch (v.ty) {
      case Ty::Int: return std::to_string(v.i);
      case Ty::Double: {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.10g", v.d);
        return buf;
      }
      case Ty::Str: return v.s;
      case Ty::Vec: {
        std::string s = "[";
        for (size_t i = 0; i < v.vec.size(); ++i) {
          if (i) s += ',';
          char buf[32];
          std::snprintf(buf, sizeof buf, "%.9g", (double)v.vec[i]);
          s += buf;
        }
        return s + "]";
      }
    }
    return "";
  }
};

class JsonLineAdapter : public ProtocolAdapter {
 public:
  void writeResult(const Result &r) override {
    std::cout << r.toJson() << '\n' << std::flush;
  }
  void writeError(const std::string &code) override {
    std::cout << "{\"error\":\"" << code << "\"}\n" << std::flush;
  }
};
