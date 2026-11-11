#!/usr/bin/env python3
"""Measure graceful shutdown (checkpoint) cost after a large in-memory load."""
import json, os, random, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
N = int(os.environ.get("N", "200000"))
D = int(os.environ.get("DIM", "128"))
DIR = os.environ.get("GS_DIR", "/tmp/graceful")
random.seed(11)

shutil.rmtree(DIR, ignore_errors=True)
os.makedirs(DIR, exist_ok=True)
env = dict(os.environ, MVDB_DATA_DIR=DIR)
p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                     text=True, encoding="utf-8", env=env)
def send(sql):
    p.stdin.write(sql + "\n"); p.stdin.flush(); p.stdout.readline()
send("CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));" % D)
for b in range(0, N, 100):
    vals = []
    for i in range(b, min(b + 100, N)):
        v = "[" + ",".join("%.3f" % random.uniform(-1, 1) for _ in range(D)) + "]"
        vals.append("(%d,'c%d','%s')" % (i, i % 10, v))
    send("INSERT INTO t VALUES " + ",".join(vals) + ";")
t0 = time.time()
p.stdin.close()
p.wait(timeout=300)
print("rows=%d shutdown_seconds=%.2f snapshot_mb=%.1f" % (N, time.time() - t0,
      os.path.getsize(os.path.join(DIR, "snapshot.bin")) / 1e6))
