#!/usr/bin/env python3
"""Smoke benchmark: 10k x 128, batch-100 inserts, 1k TopK queries (half filtered)."""
import json, os, random, subprocess, sys, time

N = int(os.environ.get("BENCH_N", "10000"))
DIM = int(os.environ.get("BENCH_DIM", "128"))
BATCH = int(os.environ.get("BENCH_BATCH", "100"))
Q = int(os.environ.get("BENCH_Q", "1000"))
SEED = int(os.environ.get("BENCH_SEED", "7"))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "build", "mvdb")))

random.seed(SEED)

def vec():
    return "[" + ",".join("%.4f" % random.uniform(-1, 1) for _ in range(DIM)) + "]"

def main():
    lines = ["CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), emb VECTOR(%d));" % DIM]
    for b in range(0, N, BATCH):
        vals = []
        for i in range(b, min(b + BATCH, N)):
            vals.append("(%d,'c%d','%s')" % (i, i % 10, vec()))
        lines.append("INSERT INTO docs VALUES " + ",".join(vals) + ";")
    for q in range(Q):
        if q % 2 == 0:
            lines.append("SELECT id, L2_DISTANCE(emb, '%s') AS d FROM docs ORDER BY d LIMIT 10;" % vec())
        else:
            lines.append("SELECT id, L2_DISTANCE(emb, '%s') AS d FROM docs WHERE category = 'c%d' ORDER BY d LIMIT 10;" % (vec(), q % 10))

    datadir = os.path.join(os.path.dirname(__file__), "tmp", "bench")
    subprocess.run(["rm", "-rf", datadir], check=False)
    os.makedirs(datadir, exist_ok=True)
    env = dict(os.environ, MVDB_DATA_DIR=datadir)
    t0 = time.time()
    p = subprocess.run([BIN], input="\n".join(lines) + "\n", capture_output=True, text=True, encoding="utf-8", env=env)
    t1 = time.time()
    outs = p.stdout.splitlines()
    print("lines_out=%d expected=%d wall=%.2fs rc=%d" % (len(outs), len(lines), t1 - t0, p.returncode))
    if p.returncode != 0:
        print(p.stderr[-500:])
        return 1
    # timings split: re-run only queries over reloaded data? keep simple: report total + per-query avg
    print("avg_per_statement_ms=%.3f" % ((t1 - t0) * 1000.0 / len(lines)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
