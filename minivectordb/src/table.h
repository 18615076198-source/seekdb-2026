#pragma once
#include "codec.h"
#include "common.h"
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Row store with an optional cold segment file.
// Hot rows live in memory; when a table is spilled, committed rows are written
// once to an immutable segment and only (pk -> file offset) stays resident.
// The engine never writes a data page during a transaction, so recovery keeps
// the simple "WAL prefix ending in COMMIT" rule (see docs/DESIGN.md).
struct Table {
  std::string name;
  std::vector<ColumnDef> cols;
  int pk = -1;
  std::unordered_map<int64_t, std::vector<Value>> rows;
  std::vector<int64_t> order;

  bool spilled = false;
  std::string segPath;
  int segFd = -1;
  std::unordered_map<int64_t, uint64_t> offsets;
  std::unordered_set<int64_t> dead;

  Table() = default;
  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;
  Table(Table &&o) noexcept { moveFrom(o); }
  Table &operator=(Table &&o) noexcept {
    if (this != &o) {
      closeSeg();
      moveFrom(o);
    }
    return *this;
  }
  ~Table() { closeSeg(); }

  void moveFrom(Table &o) noexcept {
    name = std::move(o.name);
    cols = std::move(o.cols);
    pk = o.pk;
    rows = std::move(o.rows);
    order = std::move(o.order);
    spilled = o.spilled;
    segPath = std::move(o.segPath);
    segFd = o.segFd;
    offsets = std::move(o.offsets);
    dead = std::move(o.dead);
    o.segFd = -1;
    o.spilled = false;
  }

  void closeSeg() {
    if (segFd >= 0) {
      ::close(segFd);
      segFd = -1;
    }
  }

  int colIndex(const std::string &n) const {
    for (int i = 0; i < (int)cols.size(); ++i)
      if (ieq(cols[i].name, n)) return i;
    return -1;
  }

  bool contains(int64_t k) const {
    if (rows.find(k) != rows.end()) return true;
    return spilled && offsets.find(k) != offsets.end() && dead.find(k) == dead.end();
  }

  bool getRow(int64_t k, std::vector<Value> &out) const {
    auto it = rows.find(k);
    if (it != rows.end()) {
      out = it->second;
      return true;
    }
    if (std::getenv("MVDB_TRACE"))
      std::fprintf(stderr, "GET k=%lld spilled=%d dead=%d off=%d fd=%d\n", (long long)k, (int)spilled,
                   (int)(dead.find(k) != dead.end()), (int)(offsets.find(k) != offsets.end()), segFd);
    if (!spilled || dead.find(k) != dead.end()) return false;
    auto o = offsets.find(k);
    if (o == offsets.end()) return false;
    uint32_t len = 0;
    if (::pread(segFd, &len, 4, (off_t)o->second) != 4) return false;
    std::string buf(len, '\0');
    size_t got = 0;
    while (got < len) {
      ssize_t r = ::pread(segFd, &buf[got], len - got, (off_t)o->second + 4 + (off_t)got);
      if (r <= 0) return false;
      got += (size_t)r;
    }
    Reader r(buf.data(), buf.size());
    uint32_t n = r.u32();
    out.clear();
    out.reserve(n);
    for (uint32_t i = 0; i < n && r.ok; ++i) out.push_back(r.value());
    return r.ok;
  }

  void putRow(int64_t k, std::vector<Value> row) {
    auto it = rows.find(k);
    if (it != rows.end()) {
      it->second = std::move(row);
      return;
    }
    // A key may still sit in `order` after a delete (we do not compact on every
    // delete), so remember deleted keys to avoid adding duplicates on re-insert.
    bool known = dead.erase(k) > 0;
    if (!known && spilled && offsets.find(k) != offsets.end()) known = true;
    rows.emplace(k, std::move(row));
    if (!known) order.push_back(k);
  }

  void delRow(int64_t k) {
    bool visible = rows.find(k) != rows.end() || (spilled && offsets.find(k) != offsets.end());
    rows.erase(k);
    if (visible) dead.insert(k);
  }

  size_t hotBytes() const {
    size_t n = 0;
    for (const auto &kv : rows) n += rowBytes(kv.second) + 32;
    return n;
  }

  size_t memBytes() const { return hotBytes() + order.size() * 8 + offsets.size() * 24 + dead.size() * 16; }

  // Merge old segment + hot rows into a new immutable segment (atomic rename).
  bool compactTo(const std::string &path) {
    std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    std::unordered_map<int64_t, uint64_t> noff;
    std::vector<int64_t> norder;
    Buf b;
    uint64_t off = 0;
    std::vector<Value> row;
    bool ok = true;
    for (int64_t k : order) {
      if (!getRow(k, row)) continue;
      b.d.clear();
      b.u32((uint32_t)row.size());
      for (const auto &v : row) b.value(v);
      uint32_t len = (uint32_t)b.d.size();
      if (!writeAll(fd, &len, 4) || !writeAll(fd, b.d.data(), len)) { ok = false; break; }
      noff[k] = off;
      norder.push_back(k);
      off += 4 + len;
    }
    if (ok) ::fdatasync(fd);
    ::close(fd);
    if (!ok) { ::unlink(tmp.c_str()); return false; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) return false;
    closeSeg();
    segFd = ::open(path.c_str(), O_RDONLY);
    if (segFd < 0) return false;
    offsets = std::move(noff);
    order = std::move(norder);
    rows.clear();
    dead.clear();
    spilled = true;
    segPath = path;
    return true;
  }

  // Append hot rows to the tail of the segment instead of rewriting it.
  // Safe because every hot row is already durable in the WAL (commit fsync
  // happens before this call); a torn tail is truncated on recovery.
  void spillHot() {
    if (segFd < 0) return;
    if (std::getenv("MVDB_TRACE"))
      std::fprintf(stderr, "SPILLHOT rows=%zu order=%zu fd=%d\n", rows.size(), order.size(), segFd);
    for (auto &kv : rows) appendRecord(kv.first, kv.second);
    rows.clear();
    rows.rehash(0);
#ifdef __GLIBC__
    malloc_trim(0);
#endif
  }

  bool openSegmentForAppend(const std::string &path) {
    closeSeg();
    segFd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (segFd < 0) return false;
    segPath = path;
    spilled = true;
    // Keep the in-memory order: this is a fresh spill of a table that already
    // tracks its keys. scanSegment only merges keys already present on disk.
    scanSegment();
    return true;
  }

  bool openSegment(const std::string &path) {
    closeSeg();
    segFd = ::open(path.c_str(), O_RDWR);
    if (segFd < 0) return false;
    spilled = true;
    segPath = path;
    offsets.clear();
    order.clear();
    rows.clear();
    dead.clear();
    scanSegment();
    return true;
  }

 private:
  void appendRecord(int64_t k, const std::vector<Value> &row) {
    Buf b;
    b.u32((uint32_t)row.size());
    for (const auto &v : row) b.value(v);
    uint32_t len = (uint32_t)b.d.size();
    off_t end = ::lseek(segFd, 0, SEEK_END);
    if (end < 0) return;
    if (!writeAll(segFd, &len, 4) || !writeAll(segFd, b.d.data(), len)) return;
    offsets[k] = (uint64_t)end;
  }

  void scanSegment() {
    uint64_t off = 0;
    uint64_t good = 0;
    while (true) {
      uint32_t len = 0;
      if (::pread(segFd, &len, 4, (off_t)off) != 4) break;
      if (len > (1u << 28)) break;
      std::string buf(len, '\0');
      size_t got = 0;
      bool ok = true;
      while (got < len) {
        ssize_t r = ::pread(segFd, &buf[got], len - got, (off_t)off + 4 + (off_t)got);
        if (r <= 0) { ok = false; break; }
        got += (size_t)r;
      }
      if (!ok) break;
      Reader r(buf.data(), buf.size());
      uint32_t n = r.u32();
      std::vector<Value> row;
      for (uint32_t i = 0; i < n && r.ok; ++i) row.push_back(r.value());
      if (!r.ok || pk < 0 || pk >= (int)row.size()) break;
      if (!offsets.count(row[pk].i)) order.push_back(row[pk].i);
      offsets[row[pk].i] = off;
      off += 4 + (uint64_t)len;
      good = off;
    }
    if (good < off) {
      // Torn tail (crash during append): drop it; WAL replay restores the row.
      struct stat st {};
      if (::fstat(segFd, &st) == 0 && (uint64_t)st.st_size > good) ::ftruncate(segFd, (off_t)good);
    }
  }

  static bool writeAll(int fd, const void *p, size_t n) {
    const char *c = (const char *)p;
    size_t off = 0;
    while (off < n) {
      ssize_t w = ::write(fd, c + off, n - off);
      if (w <= 0) return false;
      off += (size_t)w;
    }
    return true;
  }
};
