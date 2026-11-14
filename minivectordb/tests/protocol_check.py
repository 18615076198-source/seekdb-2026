#!/usr/bin/env python3
"""Independent conformance test for the documented JSONL protocol."""
import json, os, shutil, signal, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
DIR = "/tmp/protocheck"
FAILS = []


class Timeout(Exception):
    pass


def _alarm(signum, frame):
    raise Timeout()


signal.signal(signal.SIGALRM, _alarm)


def start():
    env = dict(os.environ, MVDB_DATA_DIR=DIR)
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def readmsg(p, timeout=5.0):
    signal.setitimer(signal.ITIMER_REAL, timeout)
    try:
        return p.stdout.readline()
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name + (": " + detail if detail else ""))


def main():
    shutil.rmtree(DIR, ignore_errors=True)
    os.makedirs(DIR, exist_ok=True)
    p = start()

    # 1. ready handshake before any request
    ready = readmsg(p, 10)
    check("ready line", ready.strip() == '{"type":"ready"}', repr(ready))

    def call(req, expect_ok=True, timeout=10.0):
        p.stdin.write(json.dumps(req) + "\n")
        p.stdin.flush()
        line = readmsg(p, timeout)
        return json.loads(line) if line.strip() else None

    r = call({"id": 1, "op": "execute", "sql": "CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));", "search_mode": "exact"})
    check("create", r == {"id": 1, "ok": True}, repr(r))

    r = call({"id": 2, "op": "execute", "sql": "INSERT INTO docs VALUES (1,'a'),(2,'b');", "search_mode": "exact"})
    check("insert rows", r.get("id") == 2 and r.get("ok") is True and r.get("affected_rows") == 2, repr(r))

    r = call({"id": 3, "op": "execute", "sql": "SELECT id, content FROM docs ORDER BY id;", "search_mode": "exact"})
    check("select", r.get("columns") == ["id", "content"] and r.get("rows") == [[1, "a"], [2, "b"]], repr(r))

    # id type must be echoed verbatim
    r = call({"id": "42", "op": "execute", "sql": "SELECT id FROM docs WHERE id = 1;", "search_mode": "exact"})
    check("string id echoed", r.get("id") == "42", repr(r))

    # error shape
    r = call({"id": 5, "op": "execute", "sql": "INSERT INTO docs VALUES (1,'dup');", "search_mode": "exact"})
    check("error ok false", r.get("ok") is False and isinstance(r.get("error", {}).get("code"), str), repr(r))
    check("error id echoed", r.get("id") == 5, repr(r))

    # unknown op
    r = call({"id": 6, "op": "bogus"})
    check("unknown op rejected", r.get("ok") is False and r.get("id") == 6, repr(r))

    # pipelined batch: three requests written at once
    batch = [
        {"id": 7, "op": "execute", "sql": "SELECT id FROM docs ORDER BY id;", "search_mode": "exact"},
        {"id": 8, "op": "execute", "sql": "DELETE FROM docs WHERE id = 2;", "search_mode": "exact"},
        {"id": 9, "op": "execute", "sql": "SELECT id FROM docs ORDER BY id;", "search_mode": "exact"},
    ]
    p.stdin.write("".join(json.dumps(x) + "\n" for x in batch))
    p.stdin.flush()
    outs = []
    try:
        for _ in batch:
            outs.append(json.loads(readmsg(p, 10)))
    except Timeout:
        check("pipelined batch answered", False, "timeout after %d responses" % len(outs))
        p.kill()
        p.wait()
        print("FAILS: %d" % len(FAILS))
        for f in FAILS:
            print("  " + f)
        return 1
    check("pipelined batch answered", [o.get("id") for o in outs] == [7, 8, 9], repr(outs))
    check("batch delete count", outs[1].get("affected_rows") == 1, repr(outs[1]))

    # shutdown: response then exit 0 with no extra stdout
    r = call({"id": 10, "op": "shutdown"}, timeout=30)
    check("shutdown ack", r == {"id": 10, "ok": True}, repr(r))
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        check("process exits after shutdown", False, "still running")
        p.kill()
    check("exit code 0", p.returncode == 0, str(p.returncode))
    leftover = p.stdout.read()
    check("no extra stdout", leftover.strip() == "", repr(leftover[:200]))

    print("FAILS: %d" % len(FAILS))
    for f in FAILS:
        print("  " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
