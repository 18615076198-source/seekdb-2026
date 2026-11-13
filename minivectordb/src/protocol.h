#pragma once
#include "engine.h"
#include <cctype>
#include <iostream>
#include <string>
#include <unordered_set>

struct JsonRequest {
  std::string id = "null";
  std::string op;
  std::string sql;
  std::string searchMode = "exact";
};

inline void jsonSkipWs(const std::string &s, size_t &p) {
  while (p < s.size() && std::isspace((unsigned char)s[p])) ++p;
}

inline bool jsonParseString(const std::string &s, size_t &p, std::string &out) {
  if (p >= s.size() || s[p] != '"') return false;
  ++p;
  out.clear();
  while (p < s.size()) {
    unsigned char c = (unsigned char)s[p++];
    if (c == '"') return true;
    if (c == '\\') {
      if (p >= s.size()) return false;
      char e = s[p++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (p + 4 > s.size()) return false;
          unsigned v = 0;
          for (int i = 0; i < 4; ++i) {
            char h = s[p++];
            unsigned d = (h >= '0' && h <= '9') ? (unsigned)(h - '0')
                       : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10)
                       : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10) : 16;
            if (d > 15) return false;
            v = (v << 4) | d;
          }
          if (v <= 0x7f) out.push_back((char)v);
          else if (v <= 0x7ff) {
            out.push_back((char)(0xc0 | (v >> 6)));
            out.push_back((char)(0x80 | (v & 0x3f)));
          } else {
            out.push_back((char)(0xe0 | (v >> 12)));
            out.push_back((char)(0x80 | ((v >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (v & 0x3f)));
          }
          break;
        }
        default: return false;
      }
    } else {
      if (c < 0x20) return false;
      out.push_back((char)c);
    }
  }
  return false;
}

inline bool jsonParseRaw(const std::string &s, size_t &p, std::string &raw) {
  jsonSkipWs(s, p);
  if (p >= s.size()) return false;
  size_t begin = p;
  if (s[p] == '"') {
    std::string ignored;
    if (!jsonParseString(s, p, ignored)) return false;
    raw = s.substr(begin, p - begin);
    return true;
  }
  while (p < s.size() && s[p] != ',' && s[p] != '}') ++p;
  size_t end = p;
  while (end > begin && std::isspace((unsigned char)s[end - 1])) --end;
  if (end == begin) return false;
  raw = s.substr(begin, end - begin);
  return true;
}

inline bool parseJsonRequest(const std::string &line, JsonRequest &req) {
  size_t p = 0;
  jsonSkipWs(line, p);
  if (p >= line.size() || line[p++] != '{') return false;
  bool haveId = false;
  std::unordered_set<std::string> keys;
  while (true) {
    jsonSkipWs(line, p);
    if (p < line.size() && line[p] == '}') { ++p; break; }
    std::string key, raw;
    if (!jsonParseString(line, p, key)) return false;
    if (!keys.insert(key).second) return false;
    jsonSkipWs(line, p);
    if (p >= line.size() || line[p++] != ':') return false;
    if (!jsonParseRaw(line, p, raw)) return false;
    if (key == "id") { req.id = raw; haveId = true; }
    else if (key == "op") {
      size_t q = 0; if (!jsonParseString(raw, q, req.op) || q != raw.size()) return false;
    } else if (key == "sql") {
      size_t q = 0; if (!jsonParseString(raw, q, req.sql) || q != raw.size()) return false;
    } else if (key == "search_mode") {
      size_t q = 0; if (!jsonParseString(raw, q, req.searchMode) || q != raw.size()) return false;
    }
    jsonSkipWs(line, p);
    if (p < line.size() && line[p] == ',') { ++p; continue; }
    if (p < line.size() && line[p] == '}') { ++p; break; }
    return false;
  }
  jsonSkipWs(line, p);
  return haveId && p == line.size() && !req.op.empty();
}

class JsonLineProtocol {
 public:
  void ready() {
    std::cout << "{\"type\":\"ready\"}\n" << std::flush;
  }

  void result(const std::string &id, const Result &r) {
    std::string o = "{\"id\":" + id + ",\"ok\":true";
    if (r.kind == Result::Affected) {
      o += ",\"affected_rows\":" + std::to_string(r.affected);
    } else if (r.kind == Result::Rows) {
      o += ",\"columns\":[";
      for (size_t i = 0; i < r.columns.size(); ++i) {
        if (i) o += ',';
        jsonString(o, r.columns[i]);
      }
      o += "],\"rows\":[";
      for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) o += ',';
        o += '[';
        for (size_t j = 0; j < r.rows[i].size(); ++j) {
          if (j) o += ',';
          jsonValue(o, r.rows[i][j]);
        }
        o += ']';
      }
      o += ']';
    }
    o += "}\n";
    std::cout << o << std::flush;
  }

  void error(const std::string &id, const std::string &code, const std::string &message) {
    std::string o = "{\"id\":" + id + ",\"ok\":false,\"error\":{\"code\":";
    jsonString(o, code);
    o += ",\"message\":";
    jsonString(o, message);
    o += "}}\n";
    std::cout << o << std::flush;
  }

  void protocolError(const std::string &message) {
    std::string o = "{\"id\":null,\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":";
    jsonString(o, message);
    o += "}}\n";
    std::cout << o << std::flush;
  }
};
