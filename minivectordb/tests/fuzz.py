#!/usr/bin/env python3
"""Fuzz the server: it must never stop answering, whatever the statement is."""
import json, os, random, shutil, signal, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
DIR = "/tmp/fuzzdb"
random.seed(int(os.environ.get("SEED", "1")))
N = int(os.environ.get("N", "3000"))


def start():
    env = dict(os.environ, MVDB_DATA_DIR=DIR)
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


ATOMS = ["0", "1", "-1", "9223372036854775807", "-9223372036854775808", "99999999999999999999",
         "'a'", "'中文'", "'[1,2]'", "'[nan,1]'", "'[1e999,1]'", "''", "NULL", "1.5", "t", "v", "id"]
OPS = ["=", "<", "<=", ">", ">=", "!=", "AND", "OR"]
TOKENS = ["CREATE", "TABLE", "INSERT", "INTO", "VALUES", "SELECT", "FROM", "WHERE", "ORDER", "BY",
          "LIMIT", "UPDATE", "SET", "DELETE", "BEGIN", "COMMIT", "ROLLBACK", "L2_DISTANCE",
          "(", ")", ",", ";", "*", "[", "]", "--", "#", "\"", "'"]


def gen():
    kind = random.random()
    if kind < 0.35:
        parts = ["SELECT"]
        parts += [random.choice(["*", "id", "v", "L2_DISTANCE(v,'[0,0]')", "id, v"]) for _ in range(random.randint(1, 2))]
        parts += ["FROM", random.choice(["t", "nope", ""])]
        if random.random() < 0.6:
            parts += ["WHERE", random.choice(["id", "v"]), random.choice(OPS), random.choice(ATOMS)]
            if random.random() < 0.3:
                parts += ["AND", random.choice(["id", "v"]), random.choice(OPS), random.choice(ATOMS)]
        if random.random() < 0.5:
            parts += ["ORDER", "BY", random.choice(["id", "v", "L2_DISTANCE(v,'[0,0]')"])]
            if random.random() < 0.3:
                parts += [random.choice(["ASC", "DESC"])]
        if random.random() < 0.5:
            parts += ["LIMIT", random.choice(["0", "1", "10", "999999"])]
        return " ".join(parts) + ";"
    if kind < 0.55:
        return "INSERT INTO t VALUES " + ",".join(
            "(%s,%s,%s)" % (random.choice(ATOMS), random.choice(ATOMS), random.choice(ATOMS))
            for _ in range(random.randint(1, 4))) + ";"
    if kind < 0.7:
        return "CREATE TABLE %s (%s BIGINT PRIMARY KEY, v VECTOR(%s));" % (
            random.choice(["t2", "t3", ""]), random.choice(["id", "k"]), random.choice(["2", "0", "9999", "-1"]))
    if kind < 0.8:
        return " ".join(random.choice(TOKENS) for _ in range(random.randint(1, 8)))
    # random byte soup
    return "".join(random.choice("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 '(),;[]-*_")
                   for _ in range(random.randint(1, 60)))


def main():
    shutil.rmtree(DIR, ignore_errors=True); os.makedirs(DIR, exist_ok=True)
    p = start()
    create = "CREATE TABLE t (id BIGINT PRIMARY KEY, v VARCHAR(16), e VECTOR(2));"
    p.stdin.write(create + "\n"); p.stdin.flush(); p.stdout.readline()
    import signal as _sig
    def _alarm(signum, frame):
        raise TimeoutError()
    _sig.signal(_sig.SIGALRM, _alarm)
    for i in range(N):
        sql = gen()
        try:
            _sig.setitimer(_sig.ITIMER_REAL, 2.0)
            p.stdin.write(sql + "\n"); p.stdin.flush()
            line = p.stdout.readline()
            _sig.setitimer(_sig.ITIMER_REAL, 0)
        except TimeoutError:
            print("HANG at %d: %r" % (i, sql)); p.kill(); return 1
        except Exception as e:
            print("WRITE FAIL at %d: %r %s" % (i, sql, e)); return 1
        if not line:
            print("NO RESPONSE at %d: %r rc=%s stderr=%s" % (i, sql, p.poll(), p.stderr.read()[-300:]))
            return 1
        try:
            json.loads(line)
        except Exception:
            print("BAD JSON at %d: %r -> %r" % (i, sql, line[:120])); return 1
    p.stdin.close(); p.wait(timeout=60)
    print("fuzz ok: %d statements" % N)
    return 0


if __name__ == "__main__":
    sys.exit(main())
