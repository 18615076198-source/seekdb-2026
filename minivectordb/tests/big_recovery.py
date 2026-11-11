#!/usr/bin/env python3
"""Large WAL recovery: kill -9 after N rows, then measure restart and verify."""
import json, os, random, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
N = int(os.environ.get("N", "200000"))
D = int(os.environ.get("DIM", "128"))
D_DIR = os.environ.get("BIG_DIR", "/tmp/bigrecover")
random.seed(7)


def start():
    env = dict(os.environ, MVDB_DATA_DIR=D_DIR)
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def send(p, sql):
    p.stdin.write(sql + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


def main():
    shutil.rmtree(D_DIR, ignore_errors=True)
    os.makedirs(D_DIR, exist_ok=True)
    p = start()
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), emb VECTOR(%d));" % D)
    for b in range(0, N, 100):
        vals = []
        for i in range(b, min(b + 100, N)):
            v = "[" + ",".join("%.3f" % random.uniform(-1, 1) for _ in range(D)) + "]"
            vals.append("(%d,'c%d','%s')" % (i, i % 10, v))
        send(p, "INSERT INTO t VALUES " + ",".join(vals) + ";")
    p.send_signal(signal.SIGKILL)
    p.wait(timeout=60)
    wal = os.path.getsize(os.path.join(D_DIR, "wal.bin"))
    t0 = time.time()
    p = start()
    got = send(p, "SELECT id FROM t WHERE id >= %d ORDER BY id LIMIT 5;" % (N - 3))
    dt = time.time() - t0
    p.stdin.close(); p.wait(120)
    ok = got.get("rows") == [[N - 3], [N - 2], [N - 1]]
    print("rows=%d wal_mb=%.1f restart_seconds=%.2f ok=%s" % (N, wal / 1e6, dt, ok))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
