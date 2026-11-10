#pragma once
#include "codec.h"
#include "common.h"
#include "json.h"
#include "sql.h"
#include "storage.h"
#include "table.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

struct Result {
  enum Kind { Empty, Affected, Rows } kind = Empty;
  std::vector<std::string> columns;
  std::vector<std::vector<Value>> rows;
  int64_t affected = 0;

  std::string toJson() const {
    if (kind == Affected) return "{\"affected_rows\":" + std::to_string(affected) + "}";
    if (kind == Empty) return "{}";
    std::string o = "{\"columns\":[";
    for (size_t i = 0; i < columns.size(); ++i) {
      if (i) o += ',';
      jsonString(o, columns[i]);
    }
    o += "],\"rows\":[";
    for (size_t r = 0; r < rows.size(); ++r) {
      if (r) o += ',';
      o += '[';
      for (size_t c = 0; c < rows[r].size(); ++c) {
        if (c) o += ',';
        jsonValue(o, rows[r][c]);
      }
      o += ']';
    }
    o += "]}";
    return o;
  }
};

inline std::vector<float> parseVecLiteral(const std::string &s, int dim) {
  const char *p = s.c_str();
  while (*p == ' ') p++;
  if (*p != '[') throw DbError("invalid_vector", "vector literal must start with [");
  p++;
  std::vector<float> out;
  while (true) {
    while (*p == ' ') p++;
    if (*p == ']') { p++; break; }
    char *end = nullptr;
    double d = std::strtod(p, &end);
    if (end == p) throw DbError("invalid_vector", "bad vector element");
    p = end;
    if (!std::isfinite(d) || std::fabs(d) > 3.4028234663852886e38)
      throw DbError("value_out_of_range", "vector element out of float32 range");
    float f = (float)d;
    if (!std::isfinite(f)) throw DbError("value_out_of_range", "non-finite float32");
    out.push_back(f);
    while (*p == ' ') p++;
    if (*p == ',') { p++; continue; }
    if (*p == ']') { p++; break; }
    throw DbError("invalid_vector", "expected , or ]");
  }
  while (*p == ' ') p++;
  if (*p) throw DbError("invalid_vector", "trailing characters in vector");
  if ((int)out.size() != dim) throw DbError("dimension_mismatch", "vector dimension mismatch");
  return out;
}

#if defined(__x86_64__)
#include <immintrin.h>
__attribute__((target("avx2,fma"))) inline double l2Avx2(const float *a, const float *b, size_t n) {
  __m256d sum = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    __m256d d0 = _mm256_cvtps_pd(_mm256_castps256_ps128(d));
    __m256d d1 = _mm256_cvtps_pd(_mm256_extractf128_ps(d, 1));
    sum = _mm256_fmadd_pd(d0, d0, sum);
    sum = _mm256_fmadd_pd(d1, d1, sum);
  }
  double lane[4];
  _mm256_storeu_pd(lane, sum);
  double r = lane[0] + lane[1] + lane[2] + lane[3];
  for (; i < n; ++i) {
    double d = (double)a[i] - (double)b[i];
    r += d * d;
  }
  return std::sqrt(r);
}
#endif

inline double l2Distance(const std::vector<float> &a, const std::vector<float> &b) {
#if defined(__x86_64__)
  static const bool hasAvx2 = []() {
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  }();
  if (hasAvx2 && a.size() == b.size() && a.size() >= 8) return l2Avx2(a.data(), b.data(), a.size());
#endif
  double s = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    double d = (double)a[i] - (double)b[i];
    s += d * d;
  }
  return std::sqrt(s);
}

inline bool testCond(const Value &a, const std::string &op, const Value &b) {
  int c = compareValue(a, b);
  if (op == "=") return c == 0;
  if (op == "<") return c < 0;
  if (op == "<=") return c <= 0;
  if (op == ">") return c > 0;
  if (op == ">=") return c >= 0;
  throw DbError("syntax_error", "bad operator");
}

struct Undo {
  uint8_t kind = 0;  // 0 = restore row, 1 = drop table
  std::string table;
  int64_t key = 0;
  bool existed = false;
  std::vector<Value> row;
};

class Engine {
 public:
  explicit Engine(Storage *storage = nullptr) : storage_(storage) {
    const char *env = std::getenv("MVDB_MEM_BUDGET_MB");
    budget_ = (size_t)(env ? std::atoll(env) : 512) * 1024 * 1024;
  }

  std::unordered_map<std::string, Table> &tables() { return tables_; }

  void applyRecovered(const std::vector<RedoOp> &ops) {
    for (const auto &op : ops) {
      std::string k = lower(op.table);
      if (op.type == OP_CREATE) {
        if (tables_.count(k)) continue;
        Table t;
        t.name = op.table;
        t.cols = op.cols;
        t.pk = op.pk;
        tables_[k] = std::move(t);
      } else if (op.type == OP_UPSERT) {
        auto it = tables_.find(k);
        if (it == tables_.end()) continue;
        it->second.putRow(op.key, op.row);
      } else if (op.type == OP_DELETE) {
        auto it = tables_.find(k);
        if (it == tables_.end()) continue;
        it->second.delRow(op.key);
      }
    }
  }

  void shutdown() {
    if (inTxn_) {
      applyUndo(txnUndo_);
      inTxn_ = false;
      txnOps_.clear();
      txnUndo_.clear();
    }
    if (storage_) storage_->checkpoint(tables_);
  }

  Result execute(const Statement &st) {
    switch (st.type) {
      case Statement::Begin:
        if (inTxn_) throw DbError("transaction_active", "nested transaction is not supported");
        inTxn_ = true;
        txnOps_.clear();
        txnUndo_.clear();
        return Result{};
      case Statement::Commit:
        if (!inTxn_) throw DbError("no_active_transaction", "COMMIT without BEGIN");
        if (storage_ && !txnOps_.empty()) storage_->appendTxn(txnOps_, true);
        inTxn_ = false;
        txnOps_.clear();
        txnUndo_.clear();
        maybeSpill();
        return Result{};
      case Statement::Rollback:
        if (!inTxn_) throw DbError("no_active_transaction", "ROLLBACK without BEGIN");
        applyUndo(txnUndo_);
        inTxn_ = false;
        txnOps_.clear();
        txnUndo_.clear();
        return Result{};
      case Statement::Unsupported:
        throw DbError("unsupported_sql", "unsupported statement");
      case Statement::Select:
        return doSelect(st);
      default:
        break;
    }

    stmtOps_.clear();
    stmtUndo_.clear();
    Result r;
    try {
      switch (st.type) {
        case Statement::Create: r = doCreate(st); break;
        case Statement::CreateIndex: r = Result{}; break;
        case Statement::Insert: r = doInsert(st); break;
        case Statement::Update: r = doUpdate(st); break;
        case Statement::Delete: r = doDelete(st); break;
        default: throw DbError("unsupported_sql", "unsupported statement");
      }
    } catch (...) {
      applyUndo(stmtUndo_);
      throw;
    }
    if (inTxn_) {
      txnOps_.insert(txnOps_.end(), stmtOps_.begin(), stmtOps_.end());
      txnUndo_.insert(txnUndo_.end(), stmtUndo_.begin(), stmtUndo_.end());
    } else {
      if (!stmtOps_.empty() && storage_) storage_->appendTxn(stmtOps_, true);
      maybeSpill();
    }
    return r;
  }

 private:
  Storage *storage_ = nullptr;
  std::unordered_map<std::string, Table> tables_;
  bool inTxn_ = false;
  std::vector<RedoOp> txnOps_;
  std::vector<Undo> txnUndo_;
  std::vector<RedoOp> stmtOps_;
  std::vector<Undo> stmtUndo_;
  size_t budget_ = 512ull * 1024 * 1024;

  void maybeSpill() {
    if (!storage_) return;
    // Budget covers hot row payload; the pk index stays resident by design.
    size_t totalHot = 0;
    for (const auto &kv : tables_) totalHot += kv.second.hotBytes();
    if (totalHot <= budget_ / 2) return;
    Table *big = nullptr;
    size_t best = 0;
    for (auto &kv : tables_) {
      size_t m = kv.second.hotBytes();
      if (m > best) { best = m; big = &kv.second; }
    }
    if (!big || big->rows.empty()) return;
    std::string path = storage_->dir() + "/" + lower(big->name) + ".seg";
    if (std::getenv("MVDB_TRACE")) std::fprintf(stderr, "SPILL %s hot=%zu totalHot=%zu\n", big->name.c_str(), best, totalHot);
    if (!big->spilled) {
      if (!big->openSegmentForAppend(path)) return;
    }
    big->spillHot();
  }

  void applyUndo(const std::vector<Undo> &undo) {
    for (auto it = undo.rbegin(); it != undo.rend(); ++it) {
      if (it->kind == 1) {
        tables_.erase(lower(it->table));
        continue;
      }
      auto tit = tables_.find(lower(it->table));
      if (tit == tables_.end()) continue;
      if (it->existed) tit->second.putRow(it->key, it->row);
      else tit->second.delRow(it->key);
    }
  }

  Table &getTable(const std::string &name) {
    auto it = tables_.find(lower(name));
    if (it == tables_.end()) throw DbError("no_such_table", name);
    return it->second;
  }

  Value coerce(const Value &lit, const ColumnDef &c) {
    if (c.ty == Ty::Int) {
      if (lit.ty != Ty::Int) throw DbError("type_mismatch", "expected integer");
      return lit;
    }
    if (c.ty == Ty::Str) {
      if (lit.ty != Ty::Str) throw DbError("type_mismatch", "expected string");
      if ((int)utf8_count(lit.s) > c.width) throw DbError("string_too_long", "string too long");
      return lit;
    }
    if (lit.ty != Ty::Str) throw DbError("type_mismatch", "expected vector literal");
    return Value::ofVec(parseVecLiteral(lit.s, c.width));
  }

  Result doCreate(const Statement &st) {
    Table t;
    t.name = st.table;
    t.cols = st.cols;
    for (int i = 0; i < (int)t.cols.size(); ++i)
      if (t.cols[i].pk) {
        if (t.pk >= 0) throw DbError("multiple_primary_keys", "multiple primary key columns");
        t.pk = i;
      }
    if (t.pk < 0) throw DbError("missing_primary_key", "primary key required");
    if (t.cols[t.pk].ty != Ty::Int) throw DbError("invalid_primary_key", "primary key must be BIGINT");
    std::string key = lower(st.table);
    if (tables_.count(key)) throw DbError("table_exists", st.table);
    std::string tname = t.name;
    std::vector<ColumnDef> tcols = t.cols;
    int tpk = t.pk;
    tables_.emplace(key, std::move(t));

    Undo u;
    u.kind = 1;
    u.table = tname;
    stmtUndo_.push_back(std::move(u));

    RedoOp op;
    op.type = OP_CREATE;
    op.table = tname;
    op.cols = tcols;
    op.pk = tpk;
    stmtOps_.push_back(std::move(op));
    return Result{};
  }

  Result doInsert(const Statement &st) {
    Table &t = getTable(st.table);
    std::vector<std::vector<Value>> newRows;
    for (const auto &exprs : st.values) {
      if (exprs.size() != t.cols.size()) throw DbError("column_count_mismatch", "column count mismatch");
      std::vector<Value> row;
      row.reserve(exprs.size());
      for (size_t i = 0; i < exprs.size(); ++i) {
        const Expr &e = exprs[i];
        if (e.kind == Expr::IntLit) row.push_back(coerce(Value::ofInt(e.i), t.cols[i]));
        else if (e.kind == Expr::StrLit) row.push_back(coerce(Value::ofStr(e.s), t.cols[i]));
        else throw DbError("syntax_error", "literal expected in VALUES");
      }
      newRows.push_back(std::move(row));
    }
    std::unordered_set<int64_t> batch;
    for (const auto &r : newRows) {
      int64_t k = r[t.pk].i;
      if (t.contains(k) || !batch.insert(k).second) throw DbError("duplicate_key", "duplicate primary key");
    }
    std::string tname = t.name;
    for (auto &r : newRows) {
      int64_t k = r[t.pk].i;
      Undo u;
      u.kind = 0;
      u.table = tname;
      u.key = k;
      u.existed = false;
      stmtUndo_.push_back(std::move(u));

      RedoOp op;
      op.type = OP_UPSERT;
      op.table = tname;
      op.key = k;
      op.row = r;
      stmtOps_.push_back(std::move(op));
      t.putRow(k, std::move(r));
    }
    Result res;
    res.kind = Result::Affected;
    res.affected = (int64_t)newRows.size();
    return res;
  }

  Result doUpdate(const Statement &st) {
    Table &t = getTable(st.table);
    struct SetOp { int col; Value val; };
    std::vector<SetOp> sets;
    for (const auto &s : st.sets) {
      int ci = t.colIndex(s.first);
      if (ci < 0) throw DbError("no_such_column", s.first);
      if (ci == t.pk) throw DbError("unsupported_sql", "primary key update is not supported");
      const Expr &e = s.second;
      if (e.kind == Expr::IntLit) sets.push_back({ci, coerce(Value::ofInt(e.i), t.cols[ci])});
      else if (e.kind == Expr::StrLit) sets.push_back({ci, coerce(Value::ofStr(e.s), t.cols[ci])});
      else throw DbError("syntax_error", "literal expected in SET");
    }

    struct WCond { int col; std::string op; Value val; };
    std::vector<WCond> where;
    for (const auto &c : st.where) {
      int ci = t.colIndex(c.col);
      if (ci < 0) throw DbError("no_such_column", c.col);
      where.push_back({ci, c.op, coerce(c.val, t.cols[ci])});
    }

    std::vector<int64_t> matched;
    std::vector<Value> row;
    for (int64_t pk : t.order) {
      if (!t.getRow(pk, row)) continue;
      bool ok = true;
      for (const auto &w : where)
        if (!testCond(row[w.col], w.op, w.val)) { ok = false; break; }
      if (ok) matched.push_back(pk);
    }

    std::string tname = t.name;
    for (int64_t pk : matched) {
      if (!t.getRow(pk, row)) continue;
      Undo u;
      u.kind = 0;
      u.table = tname;
      u.key = pk;
      u.existed = true;
      u.row = row;
      stmtUndo_.push_back(std::move(u));

      for (const auto &s : sets) row[s.col] = s.val;
      RedoOp op;
      op.type = OP_UPSERT;
      op.table = tname;
      op.key = pk;
      op.row = row;
      stmtOps_.push_back(std::move(op));
      t.putRow(pk, row);
    }
    Result res;
    res.kind = Result::Affected;
    res.affected = (int64_t)matched.size();
    return res;
  }

  Result doDelete(const Statement &st) {
    Table &t = getTable(st.table);
    struct WCond { int col; std::string op; Value val; };
    std::vector<WCond> where;
    for (const auto &c : st.where) {
      int ci = t.colIndex(c.col);
      if (ci < 0) throw DbError("no_such_column", c.col);
      where.push_back({ci, c.op, coerce(c.val, t.cols[ci])});
    }
    std::vector<std::pair<int64_t, std::vector<Value>>> matched;
    std::vector<Value> row;
    for (int64_t pk : t.order) {
      if (!t.getRow(pk, row)) continue;
      bool ok = true;
      for (const auto &w : where)
        if (!testCond(row[w.col], w.op, w.val)) { ok = false; break; }
      if (ok) matched.push_back({pk, row});
    }
    std::string tname = t.name;
    for (auto &m : matched) {
      Undo u;
      u.kind = 0;
      u.table = tname;
      u.key = m.first;
      u.existed = true;
      u.row = m.second;
      stmtUndo_.push_back(std::move(u));

      RedoOp op;
      op.type = OP_DELETE;
      op.table = tname;
      op.key = m.first;
      stmtOps_.push_back(std::move(op));
      t.delRow(m.first);
    }
    Result res;
    res.kind = Result::Affected;
    res.affected = (int64_t)matched.size();
    return res;
  }

  Result doSelect(const Statement &st) {
    Table &t = getTable(st.table);
    struct Proj { int col = -1; bool isL2 = false; std::vector<float> vec; };
    std::vector<Proj> projs;
    std::vector<std::string> outNames;

    if (st.star) {
      for (int i = 0; i < (int)t.cols.size(); ++i) {
        outNames.push_back(t.cols[i].name);
        Proj p; p.col = i;
        projs.push_back(p);
      }
    } else {
      for (const auto &it : st.items) {
        if (it.e.kind == Expr::L2) {
          int ci = t.colIndex(it.e.name);
          if (ci < 0) throw DbError("no_such_column", it.e.name);
          if (t.cols[ci].ty != Ty::Vec) throw DbError("type_mismatch", "L2_DISTANCE requires VECTOR column");
          Proj p; p.col = ci; p.isL2 = true;
          p.vec = parseVecLiteral(it.e.s, t.cols[ci].width);
          projs.push_back(std::move(p));
          outNames.push_back(it.alias.empty() ? "L2_DISTANCE" : it.alias);
        } else if (it.e.kind == Expr::Col) {
          int ci = t.colIndex(it.e.name);
          if (ci < 0) throw DbError("no_such_column", it.e.name);
          Proj p; p.col = ci;
          projs.push_back(p);
          outNames.push_back(it.alias.empty() ? t.cols[ci].name : it.alias);
        } else {
          throw DbError("syntax_error", "unsupported select item");
        }
      }
    }

    struct WCond { int col; std::string op; Value val; };
    std::vector<WCond> where;
    for (const auto &c : st.where) {
      int ci = t.colIndex(c.col);
      if (ci < 0) throw DbError("no_such_column", c.col);
      where.push_back({ci, c.op, coerce(c.val, t.cols[ci])});
    }

    bool hasOrder = st.hasOrder;
    int orderOut = -1, orderCol = -1;
    bool orderIsL2 = false;
    std::vector<float> orderVec;
    if (hasOrder) {
      if (st.orderExpr.kind == Expr::Col) {
        for (int i = 0; i < (int)outNames.size(); ++i)
          if (ieq(outNames[i], st.orderExpr.name)) { orderOut = i; break; }
        if (orderOut < 0) {
          orderCol = t.colIndex(st.orderExpr.name);
          if (orderCol < 0) throw DbError("no_such_column", st.orderExpr.name);
        }
      } else if (st.orderExpr.kind == Expr::L2) {
        orderCol = t.colIndex(st.orderExpr.name);
        if (orderCol < 0) throw DbError("no_such_column", st.orderExpr.name);
        if (t.cols[orderCol].ty != Ty::Vec) throw DbError("type_mismatch", "L2_DISTANCE requires VECTOR column");
        orderIsL2 = true;
        orderVec = parseVecLiteral(st.orderExpr.s, t.cols[orderCol].width);
      } else {
        throw DbError("syntax_error", "ORDER BY expects a column, alias or L2_DISTANCE");
      }
    }

    struct Cand { Value key; int64_t pk; };
    std::vector<Cand> cands;
    cands.reserve(t.order.size());
    auto consider = [&](int64_t pk, const std::vector<Value> &row) {
      for (const auto &w : where)
        if (!testCond(row[w.col], w.op, w.val)) return;
      Cand c;
      c.pk = pk;
      if (hasOrder) {
        if (orderOut >= 0) c.key = projs[orderOut].isL2
            ? Value::ofDouble(l2Distance(row[projs[orderOut].col].vec, projs[orderOut].vec))
            : row[projs[orderOut].col];
        else if (orderIsL2) c.key = Value::ofDouble(l2Distance(row[orderCol].vec, orderVec));
        else c.key = row[orderCol];
      }
      cands.push_back(std::move(c));
    };

    if (!t.spilled) {
      for (int64_t pk : t.order) {
        auto it = t.rows.find(pk);
        if (it == t.rows.end()) continue;
        consider(pk, it->second);
      }
    } else {
      std::vector<Value> buf;
      for (int64_t pk : t.order)
        if (t.getRow(pk, buf)) consider(pk, buf);
    }

    if (hasOrder) {
      auto cmp = [&](const Cand &a, const Cand &b) {
        int c = compareValue(a.key, b.key);
        if (st.desc) c = -c;
        if (c == 0 && a.pk != b.pk) c = (a.pk < b.pk) ? -1 : 1;
        return c < 0;
      };
      if (st.limit >= 0 && (int64_t)cands.size() > st.limit) {
        std::nth_element(cands.begin(), cands.begin() + (size_t)st.limit, cands.end(), cmp);
        cands.resize((size_t)st.limit);
      }
      std::sort(cands.begin(), cands.end(), cmp);
    } else if (st.limit >= 0 && (int64_t)cands.size() > st.limit) {
      cands.resize((size_t)st.limit);
    }

    Result res;
    res.kind = Result::Rows;
    res.columns = outNames;
    res.rows.reserve(cands.size());
    std::vector<Value> tmp;
    for (auto &c : cands) {
      const std::vector<Value> *rp = nullptr;
      if (!t.spilled) {
        auto it = t.rows.find(c.pk);
        if (it == t.rows.end()) continue;
        rp = &it->second;
      } else {
        if (!t.getRow(c.pk, tmp)) continue;
        rp = &tmp;
      }
      std::vector<Value> out;
      out.reserve(projs.size());
      for (const auto &p : projs)
        out.push_back(p.isL2 ? Value::ofDouble(l2Distance((*rp)[p.col].vec, p.vec)) : (*rp)[p.col]);
      res.rows.push_back(std::move(out));
    }
    return res;
  }
};
