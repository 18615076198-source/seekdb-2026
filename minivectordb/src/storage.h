#pragma once
#include "codec.h"
#include "common.h"
#include "table.h"
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <unordered_map>
#include <vector>

enum : uint8_t { OP_CREATE = 1, OP_UPSERT = 2, OP_DELETE = 3, OP_COMMIT = 4 };

struct RedoOp {
  uint8_t type = 0;
  std::string table;
  std::vector<ColumnDef> cols;
  int32_t pk = -1;
  int64_t key = 0;
  std::vector<Value> row;
};

inline void putRedo(Buf &b, const RedoOp &op) {
  b.u8(op.type);
  b.str(op.table);
  if (op.type == OP_CREATE) {
    b.u32((uint32_t)op.cols.size());
    for (const auto &c : op.cols) {
      b.str(c.name);
      b.u8((uint8_t)c.ty);
      b.i32(c.width);
      b.u8(c.pk ? 1 : 0);
    }
    b.i32(op.pk);
  } else {
    b.i64(op.key);
    if (op.type == OP_UPSERT) {
      b.u32((uint32_t)op.row.size());
      for (const auto &v : op.row) b.value(v);
    }
  }
}

inline RedoOp readRedo(Reader &r) {
  RedoOp op;
  op.type = r.u8();
  if (op.type == OP_COMMIT) return op;
  op.table = r.str();
  if (!r.ok) return op;
  if (op.type == OP_CREATE) {
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; ++i) {
      ColumnDef c;
      c.name = r.str();
      c.ty = (Ty)r.u8();
      c.width = r.i32();
      c.pk = r.u8() != 0;
      op.cols.push_back(c);
    }
    op.pk = r.i32();
  } else {
    op.key = r.i64();
    if (op.type == OP_UPSERT) {
      uint32_t n = r.u32();
      for (uint32_t i = 0; i < n && r.ok; ++i) op.row.push_back(r.value());
    }
  }
  return op;
}

class Storage {
 public:
  ~Storage() { closeFd(); }

  const std::string &dir() const { return dir_; }

  bool open(const std::string &dir, std::unordered_map<std::string, Table> &tables,
            std::vector<RedoOp> &recovered, std::string &err) {
    dir_ = dir;
    if (::mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST) {
      err = std::string("cannot create data dir: ") + std::strerror(errno);
      return false;
    }
    loadSnapshot(tables);
    walFd_ = ::open((dir_ + "/wal.bin").c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (walFd_ < 0) {
      err = std::string("cannot open wal: ") + std::strerror(errno);
      return false;
    }
    replayWal(recovered);
    return true;
  }

  void appendTxn(const std::vector<RedoOp> &ops, bool sync) {
    if (walFd_ < 0) return;
    std::string out;
    for (const auto &op : ops) {
      Buf b;
      putRedo(b, op);
      uint32_t len = (uint32_t)b.d.size();
      uint32_t crc = crc32Of(b.d.data(), b.d.size());
      out.append((const char *)&len, 4);
      out.append((const char *)&crc, 4);
      out.append(b.d);
    }
    Buf c;
    c.u8(OP_COMMIT);
    uint32_t len = (uint32_t)c.d.size();
    uint32_t crc = crc32Of(c.d.data(), c.d.size());
    out.append((const char *)&len, 4);
    out.append((const char *)&crc, 4);
    out.append(c.d);
    writeAll(out);
    if (sync) ::fdatasync(walFd_);
  }

  void checkpoint(std::unordered_map<std::string, Table> &tables) {
    if (dir_.empty()) return;
    for (auto &kv : tables) {
      Table &t = kv.second;
      // Hot rows and tombstones both live only in memory, so any of them must be
      // merged into the segment before the snapshot drops the WAL.
      if (t.spilled && (!t.rows.empty() || !t.dead.empty())) t.compactTo(t.segPath);
    }
    std::string tmp = dir_ + "/snapshot.tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    Buf b;
    b.d = "MVDB02";
    b.u32((uint32_t)tables.size());
    for (const auto &kv : tables) {
      const Table &t = kv.second;
      b.str(t.name);
      b.u32((uint32_t)t.cols.size());
      for (const auto &c : t.cols) {
        b.str(c.name);
        b.u8((uint8_t)c.ty);
        b.i32(c.width);
        b.u8(c.pk ? 1 : 0);
      }
      b.i32(t.pk);
      b.u8(t.spilled ? 1 : 0);
      if (t.spilled) {
        std::string base = t.segPath;
        size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        b.str(base);
        b.u32((uint32_t)t.order.size());
      } else {
        Table &mt = const_cast<Table &>(t);
        std::vector<Value> row;
        uint32_t nlive = 0;
        for (int64_t k : t.order)
          if (mt.getRow(k, row)) nlive++;
        b.u32(nlive);
        for (int64_t k : t.order) {
          if (!mt.getRow(k, row)) continue;
          b.i64(k);
          b.u32((uint32_t)row.size());
          for (const auto &v : row) b.value(v);
        }
      }
    }
    writeAllFd(fd, b.d.data(), b.d.size());
    ::fdatasync(fd);
    ::close(fd);
    std::string cur = dir_ + "/snapshot.bin";
    if (std::getenv("MVDB_CRASH_BEFORE_SNAPSHOT_RENAME")) ::_exit(9);
    if (::rename(tmp.c_str(), cur.c_str()) != 0) return;
    int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
    if (std::getenv("MVDB_CRASH_BEFORE_WAL_TRUNC")) ::_exit(9);
    if (walFd_ >= 0) { ::ftruncate(walFd_, 0); ::fdatasync(walFd_); }
  }

  void closeFd() {
    if (walFd_ >= 0) { ::close(walFd_); walFd_ = -1; }
  }

 private:
  std::string dir_;
  int walFd_ = -1;

  void writeAll(const std::string &s) { writeAllFd(walFd_, s.data(), s.size()); }
  static bool writeAllFd(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
      ssize_t w = ::write(fd, p + off, n - off);
      if (w <= 0) return false;
      off += (size_t)w;
    }
    return true;
  }

  void loadSnapshot(std::unordered_map<std::string, Table> &tables) {
    std::string path = dir_ + "/snapshot.bin";
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 6) { ::close(fd); return; }
    std::string data((size_t)st.st_size, '\0');
    size_t got = 0;
    while (got < data.size()) {
      ssize_t r = ::pread(fd, &data[got], data.size() - got, (off_t)got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    ::close(fd);
    data.resize(got);
    if (data.compare(0, 6, "MVDB02") != 0) return;
    Reader r(data.data() + 6, data.size() - 6);
    uint32_t nt = r.u32();
    for (uint32_t i = 0; i < nt && r.ok; ++i) {
      Table t;
      t.name = r.str();
      uint32_t nc = r.u32();
      for (uint32_t c = 0; c < nc && r.ok; ++c) {
        ColumnDef cd;
        cd.name = r.str();
        cd.ty = (Ty)r.u8();
        cd.width = r.i32();
        cd.pk = r.u8() != 0;
        t.cols.push_back(cd);
      }
      t.pk = r.i32();
      bool spilled = r.u8() != 0;
      if (spilled) {
        std::string base = r.str();
        r.u32();
        if (!base.empty()) t.openSegment(dir_ + "/" + base);
      } else {
        uint32_t nr = r.u32();
        for (uint32_t k = 0; k < nr && r.ok; ++k) {
          int64_t key = r.i64();
          uint32_t nv = r.u32();
          std::vector<Value> row;
          for (uint32_t v = 0; v < nv && r.ok; ++v) row.push_back(r.value());
          if (!r.ok) break;
          t.rows[key] = std::move(row);
          t.order.push_back(key);
        }
      }
      if (r.ok) tables[lower(t.name)] = std::move(t);
    }
  }

  void replayWal(std::vector<RedoOp> &recovered) {
    struct stat st {};
    if (::fstat(walFd_, &st) != 0 || st.st_size == 0) return;
    std::string data((size_t)st.st_size, '\0');
    size_t got = 0;
    while (got < data.size()) {
      ssize_t r = ::pread(walFd_, &data[got], data.size() - got, (off_t)got);
      if (r <= 0) break;
      got += (size_t)r;
    }
    data.resize(got);
    size_t off = 0;
    std::vector<RedoOp> pending;
    size_t good = 0;
    while (off + 8 <= data.size()) {
      uint32_t len, crc;
      std::memcpy(&len, data.data() + off, 4);
      std::memcpy(&crc, data.data() + off + 4, 4);
      if (len > (1u << 30) || off + 8 + len > data.size()) break;
      if (crc32Of(data.data() + off + 8, len) != crc) break;
      Reader r(data.data() + off + 8, len);
      RedoOp op = readRedo(r);
      if (!r.ok) break;
      off += 8 + len;
      good = off;
      if (op.type == OP_COMMIT) {
        for (auto &o : pending) recovered.push_back(std::move(o));
        pending.clear();
      } else {
        pending.push_back(std::move(op));
      }
    }
    if (good < data.size()) ::ftruncate(walFd_, (off_t)good);
  }
};
