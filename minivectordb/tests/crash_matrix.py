#!/usr/bin/env python3
"""Crash matrix: kill -9 at controlled points, restart, check durability boundaries."""
import json, os, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
TMP = os.path.join(HERE, "tmp", "crash")


def start(datadir):
    env = dict(os.environ)
    env["MVDB_DATA_DIR"] = datadir
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def send(p, sql):
    p.stdin.write(sql + "\n")
    p.stdin.flush()
    line = p.stdout.readline()
    return json.loads(line)


def kill9(p):
    p.send_signal(signal.SIGKILL)
    p.wait(timeout=30)


def fresh(name):
    d = os.path.join(TMP, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d, exist_ok=True)
    return d


def case_autocommit_survives():
    d = fresh("autocommit")
    p = start(d)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a'),(2,'b');")
    kill9(p)
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(30)
    ok = got.get("rows") == [[1], [2]]
    return ok, got


def case_uncommitted_absent():
    d = fresh("uncommitted")
    p = start(d)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "BEGIN;")
    send(p, "INSERT INTO t VALUES (1,'a'),(2,'b');")
    kill9(p)
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(30)
    ok = got.get("rows") == []
    return ok, got


def case_partial_txn_not_visible():
    d = fresh("partial")
    p = start(d)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a');")
    send(p, "BEGIN;")
    send(p, "UPDATE t SET v='x' WHERE id=1;")
    send(p, "INSERT INTO t VALUES (2,'b');")
    kill9(p)
    p = start(d)
    got = send(p, "SELECT id, v FROM t ORDER BY id;")
    p.stdin.close(); p.wait(30)
    ok = got.get("rows") == [[1, "a"]]
    return ok, got


def case_recovery_reentrant():
    d = fresh("reentrant")
    p = start(d)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY);")
    for i in range(200):
        send(p, "INSERT INTO t VALUES (%d);" % i)
    kill9(p)
    # kill again immediately after restart (during recovery)
    p = start(d)
    time.sleep(0.001)
    p.send_signal(signal.SIGKILL)
    p.wait(timeout=30)
    p = start(d)
    got = send(p, "SELECT id FROM t WHERE id >= 199;")
    p.stdin.close(); p.wait(30)
    ok = got.get("rows") in ([[199]], [])
    return ok, got


def case_sigterm_rolls_back():
    d = fresh("sigterm")
    p = start(d)
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(8));")
    send(p, "INSERT INTO t VALUES (1,'a');")
    send(p, "BEGIN;")
    send(p, "INSERT INTO t VALUES (2,'b');")
    p.send_signal(signal.SIGTERM)
    rc = p.wait(timeout=30)
    p = start(d)
    got = send(p, "SELECT id FROM t ORDER BY id;")
    p.stdin.close(); p.wait(30)
    ok = rc == 0 and got.get("rows") == [[1]]
    return ok, got


def main():
    results = []
    for fn in (case_autocommit_survives, case_uncommitted_absent,
               case_partial_txn_not_visible, case_recovery_reentrant,
               case_sigterm_rolls_back):
        ok, got = fn()
        results.append((fn.__name__, ok, got))
        print(("PASS " if ok else "FAIL ") + fn.__name__ + " " + json.dumps(got, ensure_ascii=False)[:200])
    return 0 if all(r[1] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
