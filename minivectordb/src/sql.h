#pragma once
#include "common.h"

struct Expr {
  enum Kind { Col, IntLit, StrLit, L2 } kind = Col;
  std::string name;
  int64_t i = 0;
  std::string s;
};

struct Cond {
  std::string col;
  std::string op;
  Value val;
};

struct SelectItem {
  Expr e;
  std::string alias;
};

struct Statement {
  enum Type { Create, CreateIndex, Insert, Select, Update, Delete, Begin, Commit, Rollback, Unsupported } type = Unsupported;
  std::string table;
  std::vector<ColumnDef> cols;
  std::vector<std::vector<Expr>> values;
  std::vector<SelectItem> items;
  bool star = false;
  std::vector<Cond> where;
  Expr orderExpr;
  bool hasOrder = false;
  bool desc = false;
  int64_t limit = -1;
  std::vector<std::pair<std::string, Expr>> sets;
};

struct Token {
  enum K { Id, Num, Str, Sym, End } k = End;
  std::string text;
};

inline std::vector<Token> tokenize(const std::string &sql) {
  std::vector<Token> t;
  size_t i = 0;
  while (i < sql.size()) {
    unsigned char c = (unsigned char)sql[i];
    if (std::isspace(c)) { i++; continue; }
    if (std::isalpha(c) || c == '_') {
      size_t j = i;
      while (j < sql.size() && (std::isalnum((unsigned char)sql[j]) || sql[j] == '_')) j++;
      t.push_back({Token::Id, sql.substr(i, j - i)});
      i = j;
      continue;
    }
    if (std::isdigit(c)) {
      size_t j = i;
      while (j < sql.size() && std::isdigit((unsigned char)sql[j])) j++;
      t.push_back({Token::Num, sql.substr(i, j - i)});
      i = j;
      continue;
    }
    if (c == '\'') {
      size_t j = i + 1;
      std::string s;
      bool closed = false;
      while (j < sql.size()) {
        if (sql[j] == '\'') {
          if (j + 1 < sql.size() && sql[j + 1] == '\'') { s += '\''; j += 2; continue; }
          closed = true;
          break;
        }
        s += sql[j++];
      }
      if (!closed) throw DbError("syntax_error", "unterminated string literal");
      t.push_back({Token::Str, s});
      i = j + 1;
      continue;
    }
    if (c == '<' && i + 1 < sql.size() && sql[i + 1] == '=') { t.push_back({Token::Sym, "<="}); i += 2; continue; }
    if (c == '>' && i + 1 < sql.size() && sql[i + 1] == '=') { t.push_back({Token::Sym, ">="}); i += 2; continue; }
    t.push_back({Token::Sym, std::string(1, (char)c)});
    i++;
  }
  t.push_back({Token::End, ""});
  return t;
}

class Parser {
 public:
  explicit Parser(const std::string &sql) : ts_(tokenize(sql)) {}

  Statement parse() {
    Statement st;
    if (peek().k == Token::End) throw DbError("syntax_error", "empty statement");
    Token kw = next();
    if (kw.k != Token::Id) throw DbError("syntax_error", "expected keyword");
    std::string K = upper(kw.text);
    if (K == "CREATE") {
      if (peekIsKw("TABLE")) { next(); parseCreateTable(st); }
      else if (peekIsKw("VECTOR")) { skipAll(); st.type = Statement::CreateIndex; }
      else { skipAll(); st.type = Statement::Unsupported; }
      return st;
    }
    if (K == "INSERT") { parseInsert(st); return st; }
    if (K == "SELECT") { parseSelect(st); return st; }
    if (K == "UPDATE") { parseUpdate(st); return st; }
    if (K == "DELETE") { parseDelete(st); return st; }
    if (K == "BEGIN" || K == "START") { st.type = Statement::Begin; if (peekIsKw("TRANSACTION")) next(); endStmt(); return st; }
    if (K == "COMMIT") { st.type = Statement::Commit; endStmt(); return st; }
    if (K == "ROLLBACK") { st.type = Statement::Rollback; endStmt(); return st; }
    skipAll();
    st.type = Statement::Unsupported;
    return st;
  }

 private:
  std::vector<Token> ts_;
  size_t pos_ = 0;

  const Token &peek(size_t k = 0) const { return ts_[std::min(pos_ + k, ts_.size() - 1)]; }
  Token next() { Token t = ts_[pos_]; if (pos_ + 1 < ts_.size()) pos_++; return t; }
  void skipAll() { pos_ = ts_.size() - 1; }

  bool peekIsKw(const char *kw) const { return peek().k == Token::Id && ieq(peek().text, kw); }
  bool acceptKw(const char *kw) { if (peekIsKw(kw)) { next(); return true; } return false; }
  void expectKw(const char *kw) { if (!acceptKw(kw)) throw DbError("syntax_error", std::string("expected ") + kw); }
  bool peekSym(const char *s) const { return peek().k == Token::Sym && peek().text == s; }
  bool acceptSym(const char *s) { if (peekSym(s)) { next(); return true; } return false; }
  void expectSym(const char *s) { if (!acceptSym(s)) throw DbError("syntax_error", std::string("expected ") + s); }

  std::string ident() {
    if (peek().k != Token::Id) throw DbError("syntax_error", "expected identifier");
    return next().text;
  }
  int64_t number() {
    if (peek().k != Token::Num) throw DbError("syntax_error", "expected number");
    std::string s = next().text;
    errno = 0;
    char *end = nullptr;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno == ERANGE) throw DbError("value_out_of_range", "integer out of range");
    return (int64_t)v;
  }
  void endStmt() {
    acceptSym(";");
    if (peek().k != Token::End) throw DbError("syntax_error", "trailing tokens");
  }

  void parseCreateTable(Statement &st) {
    st.type = Statement::Create;
    st.table = ident();
    expectSym("(");
    while (true) {
      ColumnDef c;
      c.name = ident();
      std::string ty = upper(ident());
      if (ty == "BIGINT" || ty == "INT" || ty == "INTEGER") c.ty = Ty::Int;
      else if (ty == "VARCHAR") { expectSym("("); c.width = (int)number(); expectSym(")"); c.ty = Ty::Str; }
      else if (ty == "VECTOR") { expectSym("("); c.width = (int)number(); expectSym(")"); c.ty = Ty::Vec; }
      else throw DbError("unsupported_type", ty);
      if (peekIsKw("PRIMARY")) { next(); expectKw("KEY"); c.pk = true; }
      st.cols.push_back(c);
      if (acceptSym(",")) continue;
      break;
    }
    expectSym(")");
    endStmt();
  }

  Expr parseLiteral() {
    Expr e;
    if (peek().k == Token::Num) { e.kind = Expr::IntLit; e.i = number(); return e; }
    if (peekSym("-")) { next(); e.kind = Expr::IntLit; e.i = -number(); return e; }
    if (peek().k == Token::Str) { e.kind = Expr::StrLit; e.s = next().text; return e; }
    throw DbError("syntax_error", "literal expected");
  }

  void parseInsert(Statement &st) {
    st.type = Statement::Insert;
    expectKw("INTO");
    st.table = ident();
    expectKw("VALUES");
    while (true) {
      expectSym("(");
      std::vector<Expr> row;
      while (true) {
        row.push_back(parseLiteral());
        if (acceptSym(",")) continue;
        break;
      }
      expectSym(")");
      st.values.push_back(std::move(row));
      if (acceptSym(",")) continue;
      break;
    }
    endStmt();
  }

  Expr parseExpr() {
    if (peek().k == Token::Id && ieq(peek().text, "L2_DISTANCE")) {
      next();
      expectSym("(");
      Expr e;
      e.kind = Expr::L2;
      e.name = ident();
      expectSym(",");
      if (peek().k != Token::Str) throw DbError("syntax_error", "vector literal expected");
      e.s = next().text;
      expectSym(")");
      return e;
    }
    if (peek().k == Token::Id) { Expr e; e.kind = Expr::Col; e.name = next().text; return e; }
    if (peek().k == Token::Num) { Expr e; e.kind = Expr::IntLit; e.i = number(); return e; }
    if (peekSym("-")) { next(); Expr e; e.kind = Expr::IntLit; e.i = -number(); return e; }
    if (peek().k == Token::Str) { Expr e; e.kind = Expr::StrLit; e.s = next().text; return e; }
    throw DbError("syntax_error", "unexpected token");
  }

  void parseWhere(Statement &st) {
    if (!acceptKw("WHERE")) return;
    while (true) {
      Cond c;
      c.col = ident();
      if (peek().k != Token::Sym) throw DbError("syntax_error", "operator expected");
      std::string op = peek().text;
      if (op != "=" && op != "<" && op != "<=" && op != ">" && op != ">=") throw DbError("syntax_error", "bad operator");
      next();
      c.op = op;
      Expr e = parseLiteral();
      c.val = (e.kind == Expr::IntLit) ? Value::ofInt(e.i) : Value::ofStr(e.s);
      st.where.push_back(c);
      if (acceptKw("AND")) continue;
      break;
    }
  }

  void parseSelect(Statement &st) {
    st.type = Statement::Select;
    if (acceptSym("*")) st.star = true;
    else {
      while (true) {
        SelectItem it;
        it.e = parseExpr();
        if (acceptKw("AS")) it.alias = ident();
        st.items.push_back(it);
        if (acceptSym(",")) continue;
        break;
      }
    }
    expectKw("FROM");
    st.table = ident();
    parseWhere(st);
    if (acceptKw("ORDER")) {
      expectKw("BY");
      st.orderExpr = parseExpr();
      st.hasOrder = true;
      if (acceptKw("DESC")) st.desc = true;
      else acceptKw("ASC");
    }
    if (acceptKw("LIMIT")) st.limit = number();
    endStmt();
  }

  void parseUpdate(Statement &st) {
    st.type = Statement::Update;
    st.table = ident();
    expectKw("SET");
    while (true) {
      std::string col = ident();
      expectSym("=");
      st.sets.push_back({col, parseLiteral()});
      if (acceptSym(",")) continue;
      break;
    }
    parseWhere(st);
    endStmt();
  }

  void parseDelete(Statement &st) {
    st.type = Statement::Delete;
    expectKw("FROM");
    st.table = ident();
    parseWhere(st);
    endStmt();
  }
};
