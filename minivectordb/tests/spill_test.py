#!/usr/bin/env python3
"""Spill path test: force a tiny memory budget, verify correctness across restart."""
import json, os, random, resource, shutil, signal, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
TMP = os.environ.get("SPILL_DIR", os.path.join(HERE, "tmp", "spill"))
N = int(os.environ.get("SPILL_N", "20000"))
DIM = int(os.environ.get("SPILL_DIM", "8"))
BUDGET = os.environ.get("SPILL_BUDGET_MB", "1")
random.seed(11)


def start(datadir, budget):
    env = dict(os.environ)
    env["MVDB_DATA_DIR"] = datadir
    env["MVDB_MEM_BUDGET_MB"] = str(budget)
    p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)
    ready = json.loads(p.stdout.readline())
    if ready != {"type": "ready"}:
        p.kill()
        raise RuntimeError("expected ready, got %r" % (ready,))
    send.counter = 0
    return p


def send(p, sql):
    send.counter += 1
    req = {"id": send.counter, "op": "execute", "sql": sql, "search_mode": "exact"}
    p.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
    p.stdin.flush()
    got = json.loads(p.stdout.readline())
    if got.get("id") != send.counter:
        raise RuntimeError("response id mismatch: %r" % (got,))
    return got
send.counter = 0


def vec(vals):
    return "[" + ",".join("%.3f" % v for v in vals) + "]"


def l2(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main():
    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP, exist_ok=True)
    rows = {}
    for i in range(N):
        rows[i] = ([round(random.uniform(-1, 1), 3) for _ in range(DIM)], i % 5)

    p = start(TMP, 1)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, cat VARCHAR(8), content VARCHAR(8), emb VECTOR(%d));" % DIM)
    buf = []
    for i in range(N):
        buf.append("(%d,'c%d','v%d','%s')" % (i, rows[i][1], i, vec(rows[i][0])))
        if len(buf) == 100:
            send(p, "INSERT INTO t VALUES " + ",".join(buf) + ";")
            buf = []
            if i % 5000 == 0:
                print("progress insert %d" % i, file=sys.stderr, flush=True)
    if buf:
        send(p, "INSERT INTO t VALUES " + ",".join(buf) + ";")

    seg = [f for f in os.listdir(TMP) if f.endswith(".seg")]
    print("segment_files=%r" % (seg,))

    got = send(p, "SELECT id FROM t WHERE id >= %d ORDER BY id LIMIT 10;" % (N - 5))
    ok_range = got.get("rows") == [[i] for i in range(N - 5, N)]

    q = [round(random.uniform(-1, 1), 3) for _ in range(DIM)]
    qs = vec(q)
    got = send(p, "SELECT id FROM t WHERE cat = 'c3' AND id < 500 ORDER BY L2_DISTANCE(emb, '%s') LIMIT 5;" % qs)
    cands = [(l2(rows[i][0], q), i) for i in range(500) if rows[i][1] == 3]
    cands.sort()
    exp = [[i] for _, i in cands[:5]]
    ok_topk = got.get("rows") == exp

    # update + delete on a spilled table
    send(p, "UPDATE t SET content = 'X' WHERE id = 0;")
    send(p, "DELETE FROM t WHERE id = 1;")
    got = send(p, "SELECT id, content FROM t WHERE id <= 1 ORDER BY id;")
    ok_mut = got.get("rows") == [[0, "X"]]

    # graceful restart, same data dir
    p.stdin.close()
    p.wait(timeout=120)
    p = start(TMP, 1)
    got = send(p, "SELECT id FROM t WHERE id >= %d ORDER BY id LIMIT 10;" % (N - 5))
    ok_restart = got.get("rows") == [[i] for i in range(N - 5, N)]
    got = send(p, "SELECT id, content FROM t WHERE id <= 1 ORDER BY id;")
    ok_restart_mut = got.get("rows") == [[0, "X"]]
    p.stdin.close()
    p.wait(timeout=120)

    peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    print("peak_child_rss_kb=%d" % peak)
    for name, ok in [("range", ok_range), ("topk_filtered", ok_topk), ("mutations", ok_mut),
                     ("restart", ok_restart), ("restart_mutations", ok_restart_mut)]:
        print(("PASS " if ok else "FAIL ") + name)
    return 0 if all([ok_range, ok_topk, ok_mut, ok_restart, ok_restart_mut]) else 1


if __name__ == "__main__":
    sys.exit(main())
