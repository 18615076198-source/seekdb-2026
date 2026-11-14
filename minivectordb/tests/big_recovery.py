#!/usr/bin/env python3
"""Large WAL recovery: kill -9 after N rows, then measure restart and verify (JSONL)."""
import json, os, random, shutil, signal, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonl_client import Client

N = int(os.environ.get("N", "200000"))
D = int(os.environ.get("DIM", "128"))
DIR = os.environ.get("BIG_DIR", "/tmp/bigrecover")
random.seed(7)


def main():
    shutil.rmtree(DIR, ignore_errors=True)
    os.makedirs(DIR, exist_ok=True)
    c = Client(DIR)
    c.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));" % D)
    for b in range(0, N, 100):
        vals = []
        for i in range(b, min(b + 100, N)):
            v = "[" + ",".join("%.3f" % random.uniform(-1, 1) for _ in range(D)) + "]"
            vals.append("(%d,'c%d','%s')" % (i, i % 10, v))
        c.execute("INSERT INTO t VALUES " + ",".join(vals) + ";")
    c.p.send_signal(signal.SIGKILL)
    c.p.wait(timeout=60)
    wal = os.path.getsize(os.path.join(DIR, "wal.bin"))
    t0 = time.time()
    c = Client(DIR)
    ready_s = time.time() - t0
    got = c.execute("SELECT id FROM t WHERE id >= %d ORDER BY id LIMIT 5;" % (N - 3))
    ok = got.get("rows") == [[N - 3], [N - 2], [N - 1]]
    print("rows=%d wal_mb=%.1f ready_seconds=%.2f ok=%s" % (N, wal / 1e6, ready_s, ok))
    c.p.stdin.close()
    c.p.wait(timeout=120)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
