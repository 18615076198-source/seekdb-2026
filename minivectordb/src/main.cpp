#include "protocol.h"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void onTerm(int) { g_stop = 1; }

int main(int argc, char **argv) {
  std::string dataDir = "/data";
  if (const char *env = std::getenv("MVDB_DATA_DIR")) dataDir = env;
  if (argc > 1) dataDir = argv[1];

  Storage storage;
  std::unordered_map<std::string, Table> tables;
  std::string err;
  if (!storage.open(dataDir, tables, err)) {
    std::cerr << "mvdb: " << err << "\n";
    return 2;
  }

  Engine eng(&storage);
  eng.tables() = std::move(tables);
  std::signal(SIGTERM, onTerm);
  std::signal(SIGINT, onTerm);

  JsonLineProtocol proto;
  // Recovery is finished before the judge is told we are ready.
  proto.ready();

  // Raw read() + line splitting instead of poll()+getline(): iostream may read
  // ahead into its own buffer, so a later poll() on the fd blocks even though a
  // complete request is already buffered (deadlock on pipelined input).
  auto handle = [&](const std::string &line) -> int {
    if (line.empty()) return -1;
    JsonRequest req;
    if (!parseJsonRequest(line, req)) {
      proto.protocolError("request must be a JSON object with id and op");
      return -1;
    }
    if (req.op == "shutdown") {
      try {
        eng.shutdown();
        proto.result(req.id, Result{});
        storage.closeFd();
        return 0;
      } catch (const std::exception &e) {
        proto.error(req.id, "shutdown_failed", e.what());
        storage.closeFd();
        return 1;
      }
    }
    if (req.op != "execute" || req.sql.empty()) {
      proto.error(req.id, "unsupported_op", "op must be execute or shutdown");
      return -1;
    }
    try {
      Parser p(req.sql);
      Statement st = p.parse();
      Result r = eng.execute(st);
      proto.result(req.id, r);
    } catch (const DbError &e) {
      proto.error(req.id, e.code, e.what());
    } catch (const std::exception &e) {
      proto.error(req.id, "internal_error", e.what());
    }
    return -1;
  };

  std::string buf;
  bool eof = false;
  while (!g_stop && !eof) {
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      int rc = handle(line);
      if (rc >= 0) return rc;
    }
    struct pollfd pfd { 0, POLLIN, 0 };
    int pr = ::poll(&pfd, 1, 100);
    if (pr == 0) continue;
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    char chunk[65536];
    ssize_t n = ::read(0, chunk, sizeof chunk);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) { eof = true; break; }
    buf.append(chunk, (size_t)n);
  }
  if (!buf.empty()) {
    int rc = handle(buf);
    if (rc >= 0) return rc;
  }

  // EOF and signal shutdowns roll back an open transaction and checkpoint
  // committed state. SIGKILL, used by crash tests, bypasses this path and is
  // recovered from the WAL on the next start.
  eng.shutdown();
  storage.closeFd();
  return 0;
}
