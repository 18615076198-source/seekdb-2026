#!/usr/bin/env python3
"""Measure graceful shutdown (checkpoint) cost after a large in-memory load (JSONL)."""
import os, random, shutil, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonl_client import Client

N = int(os.environ.get("N", "200000"))
D = int(os.environ.get("DIM", "128"))
DIR = os.environ.get("GS_DIR", "/tmp/graceful")
random.seed(11)


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
    t0 = time.time()
    c.p.stdin.close()
    c.p.wait(timeout=300)
    print("rows=%d shutdown_seconds=%.2f snapshot_mb=%.1f" % (
        N, time.time() - t0, os.path.getsize(os.path.join(DIR, "snapshot.bin")) / 1e6))
    return 0


if __name__ == "__main__":
    sys.exit(main())
