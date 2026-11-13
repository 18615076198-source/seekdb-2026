#include "protocol.h"
#include <csignal>
#include <cstdlib>
#include <iostream>
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

  JsonLineProtocol proto;
  // The judge waits for this before sending the first request.
  proto.ready();

  std::string line;
  while (!g_stop && std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    JsonRequest req;
    if (!parseJsonRequest(line, req)) {
      proto.protocolError("request must be a JSON object with id and op");
      continue;
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
      continue;
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
  }

  // EOF and signal shutdowns roll back an open transaction and checkpoint
  // committed state. SIGKILL, used by crash tests, bypasses this path and is
  // recovered from the WAL on the next start.
  eng.shutdown();
  storage.closeFd();
  return 0;
}
