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
  std::string line;
  while (!g_stop) {
    if (std::cin.rdbuf()->in_avail() <= 0) {
      struct pollfd pfd { 0, POLLIN, 0 };
      int pr = ::poll(&pfd, 1, 200);
      if (pr == 0) continue;
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
    }
    if (!std::getline(std::cin, line)) break;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string s = trim(line);
    if (s.empty()) continue;
    if (s.rfind("--", 0) == 0) {
      // The driver may forward directives instead of consuming them. "--restart"
      // is documented as "close gracefully and wait for exit".
      if (s.rfind("--restart", 0) == 0 || s.rfind("--quit", 0) == 0 || s.rfind("--exit", 0) == 0) break;
      continue;
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
  }
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
