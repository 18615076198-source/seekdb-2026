#!/usr/bin/env python3
"""Fault injection around checkpoint and around a torn segment tail (JSONL)."""
import os, shutil, signal, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonl_client import Client

TMP = "/tmp/ckpt"
FAILS = []


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name + (": " + detail if detail else ""))


def fresh(name):
    d = os.path.join(TMP, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return d


def case_crash_before_rename():
    d = fresh("before_rename")
    c = Client(d, {"MVDB_CRASH_BEFORE_SNAPSHOT_RENAME": "1"})
    c.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    c.execute("INSERT INTO t VALUES (1,'a'),(2,'b');")
    c.p.stdin.close()
    rc = c.p.wait(timeout=60)
    check("hook fired before rename", rc != 0, "rc=%d" % rc)
    c = Client(d)
    got = c.execute("SELECT id FROM t ORDER BY id;")
    c.shutdown()
    check("rows survive crash before snapshot rename", got.get("rows") == [[1], [2]], str(got))


def case_crash_before_wal_trunc():
    d = fresh("before_trunc")
    c = Client(d, {"MVDB_CRASH_BEFORE_WAL_TRUNC": "1"})
    c.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    c.execute("INSERT INTO t VALUES (1,'a'),(2,'b');")
    c.p.stdin.close()
    rc = c.p.wait(timeout=60)
    check("hook fired before wal truncate", rc != 0, "rc=%d" % rc)
    c = Client(d)
    got = c.execute("SELECT id FROM t ORDER BY id;")
    c.shutdown()
    check("snapshot newer than wal does not duplicate", got.get("rows") == [[1], [2]], str(got))


def case_torn_segment_tail():
    d = fresh("torn_seg")
    c = Client(d, {"MVDB_MEM_BUDGET_MB": "0"})
    c.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    c.execute("INSERT INTO t VALUES (1,'a');")
    c.execute("INSERT INTO t VALUES (2,'b');")
    c.p.send_signal(signal.SIGKILL)
    c.p.wait(timeout=30)
    seg = os.path.join(d, "t.seg")
    with open(seg, "r+b") as f:
        f.truncate(max(0, os.path.getsize(seg) - 5))
    c = Client(d)
    got = c.execute("SELECT id FROM t ORDER BY id;")
    c.shutdown()
    check("torn segment tail does not lose committed rows", got.get("rows") == [[1], [2]], str(got))


def main():
    for fn in (case_crash_before_rename, case_crash_before_wal_trunc, case_torn_segment_tail):
        fn()
    print("FAILS: %d" % len(FAILS))
    for f in FAILS:
        print("  " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
