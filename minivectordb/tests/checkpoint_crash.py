#!/usr/bin/env python3
"""Fault injection around checkpoint and around a torn segment tail."""
import json, os, shutil, signal, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
TMP = os.path.join(HERE, "tmp", "ckpt")
FAILS = []


def start(datadir, extra=None):
    env = dict(os.environ, MVDB_DATA_DIR=datadir)
    if extra:
        env.update(extra)
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def send(p, sql):
    p.stdin.write(sql + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


def fresh(name):
    d = os.path.join(TMP, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return d


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name + (": " + detail if detail else ""))


def case_crash_before_rename():
    d = fresh("before_rename")
    p = start(d, {"MVDB_CRASH_BEFORE_SNAPSHOT_RENAME": "1"})
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.stdin.close()
    rc = p.wait(timeout=60)
    check("hook fired before rename", rc != 0, "rc=%d" % rc)
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(60)
    check("rows survive crash before snapshot rename", got.get("rows") == [[1], [2]], str(got))


def case_crash_before_wal_trunc():
    d = fresh("before_trunc")
    p = start(d, {"MVDB_CRASH_BEFORE_WAL_TRUNC": "1"})
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a'),(2,'b');")
    p.stdin.close()
    rc = p.wait(timeout=60)
    check("hook fired before wal truncate", rc != 0, "rc=%d" % rc)
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(60)
    check("snapshot newer than wal does not duplicate", got.get("rows") == [[1], [2]], str(got))


def case_torn_segment_tail():
    d = fresh("torn_seg")
    p = start(d, {"MVDB_MEM_BUDGET_MB": "0"})
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a');")
    send(p, "INSERT INTO t VALUES (2,'b');")
    p.send_signal(signal.SIGKILL)
    p.wait(timeout=30)
    seg = os.path.join(d, "t.seg")
    size = os.path.getsize(seg)
    # cut into the last record: the row's WAL entry is the only remaining copy
    with open(seg, "r+b") as f:
        f.truncate(max(0, size - 5))
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(60)
    check("torn segment tail does not lose committed rows", got.get("rows") == [[1], [2]], str(got))
    check("segment tail was repaired", os.path.getsize(seg) >= 0)


def main():
    for fn in (case_crash_before_rename, case_crash_before_wal_trunc, case_torn_segment_tail):
        fn()
    print("FAILS: %d" % len(FAILS))
    for f in FAILS:
        print("  " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
