#include "protocol.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cstdlib>
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

  JsonLineAdapter jsonProto;
  TsvAdapter tsvProto;
  const char *protoEnv = std::getenv("MVDB_PROTOCOL");
  ProtocolAdapter &proto = (protoEnv && std::string(protoEnv) == "tsv")
                               ? static_cast<ProtocolAdapter &>(tsvProto)
                               : static_cast<ProtocolAdapter &>(jsonProto);
  // Newline-tolerant reader: the driver may or may not terminate the last
  // statement with '\n'. If bytes are pending but no newline arrives for a
  // short idle window, treat them as a complete statement instead of blocking
  // forever (a block here shows up as "JSON response timeout").
  std::string buf;
  auto lastData = std::chrono::steady_clock::now();
  auto handle = [&](std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string s = trim(line);
    if (s.empty()) return;
    if (s.rfind("--", 0) == 0) {
      // The driver may forward directives/comments instead of consuming them.
      // "--restart" is documented as "close gracefully and wait for exit"; every
      // other directive still needs an answer, otherwise the driver waits for a
      // JSON response that never comes (observed as a per-case timeout).
      if (s.rfind("--restart", 0) == 0 || s.rfind("--quit", 0) == 0 || s.rfind("--exit", 0) == 0) {
        g_stop = 1;
        return;
      }
      std::cout << "{}\n" << std::flush;
      return;
    }
    auto t0 = std::chrono::steady_clock::now();
    try {
      Parser p(s);
      Statement st = p.parse();
      Result r = eng.execute(st);
      proto.writeResult(r);
    } catch (const DbError &e) {
      proto.writeError(e.code);
    } catch (const std::exception &) {
      proto.writeError("internal_error");
    }
    if (std::getenv("MVDB_TRACE")) {
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
      if (ms >= 20) std::fprintf(stderr, "SLOW %lldms %s\n", (long long)ms, s.substr(0, 60).c_str());
    }
  };

  while (!g_stop) {
    size_t nl = buf.find('\n');
    if (nl != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      handle(line);
      continue;
    }
    struct pollfd pfd { 0, POLLIN, 0 };
    int pr = ::poll(&pfd, 1, 100);
    if (pr == 0) {
      if (!buf.empty() && std::chrono::steady_clock::now() - lastData > std::chrono::milliseconds(400)) {
        handle(buf);
        buf.clear();
      }
      continue;
    }
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
    if (n == 0) break;  // EOF
    buf.append(chunk, (size_t)n);
    lastData = std::chrono::steady_clock::now();
  }
  if (!buf.empty()) handle(buf);
  proto.onShutdown();
  eng.shutdown();
  storage.closeFd();
  if (std::getenv("MVDB_TRACE")) {
    FILE *f = std::fopen("/proc/self/status", "r");
    if (f) {
      char line[256];
      while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, "VmHWM", 5) == 0) std::fprintf(stderr, "PEAK %s", line);
      std::fclose(f);
    }
  }
  return 0;
}
