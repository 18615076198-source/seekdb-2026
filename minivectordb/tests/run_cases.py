#!/usr/bin/env python3
"""Local driver for the public mysqltest package.

Protocol here is PROVISIONAL (one JSON object per SQL line); the official driver
and wire format are still unpublished. This harness also simulates --restart by
closing stdin and relaunching the binary against the same data directory.
"""
import json, os, shutil, signal, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
CASES = os.path.join(HERE, "cases")
TMP = os.path.join(HERE, "tmp")
STMT_TIMEOUT = float(os.environ.get("MVDB_STMT_TIMEOUT", "120"))


class Timeout(Exception):
    pass


def _alarm(signum, frame):
    raise Timeout("statement timeout")


def close_num(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= 1e-5 or abs(a - b) <= 1e-5 * max(abs(a), abs(b))
    return None


def deep_eq(a, b):
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(deep_eq(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a.keys()) == set(b.keys()) and all(deep_eq(a[k], b[k]) for k in a)
    r = close_num(a, b)
    return (a == b) if r is None else r


def rows_match(exp, got, unordered):
    if unordered:
        if len(exp) != len(got):
            return False
        used = [False] * len(got)
        for e in exp:
            hit = False
            for i, g in enumerate(got):
                if not used[i] and deep_eq(e, g):
                    used[i] = True
                    hit = True
                    break
            if not hit:
                return False
        return True
    return deep_eq(exp, got)


def check(exp, got):
    if not isinstance(got, dict):
        return "response is not a JSON object"
    if "error" in exp:
        if "error" not in got:
            return "expected error %r, got %r" % (exp["error"], got)
        if got["error"] != exp["error"]:
            return "expected error %r, got %r" % (exp["error"], got["error"])
        return None
    if "error" in got:
        return "unexpected error %r" % (got["error"],)
    if exp == {}:
        return None
    if "affected_rows" in exp:
        if got.get("affected_rows") != exp["affected_rows"]:
            return "affected_rows: expected %r got %r" % (exp["affected_rows"], got.get("affected_rows"))
        return None
    if "columns" in exp:
        if not deep_eq(got.get("columns"), exp["columns"]):
            return "columns: expected %r got %r" % (exp["columns"], got.get("columns"))
        rows = got.get("rows")
        if not isinstance(rows, list):
            return "rows missing"
        if not rows_match(exp.get("rows", []), rows, bool(exp.get("unordered"))):
            return "rows: expected %r got %r" % (exp.get("rows", []), rows)
        return None
    return "unknown expectation %r" % (exp,)


def start(datadir):
    env = dict(os.environ)
    env["MVDB_DATA_DIR"] = datadir
    return subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)


def run_case(name):
    test = os.path.join(CASES, name + ".test")
    result = os.path.join(CASES, name + ".result")
    if not (os.path.exists(test) and os.path.exists(result)):
        return ("MISSING", [])
    raw = [l.rstrip("\n") for l in open(test, encoding="utf-8")]
    exp = json.load(open(result, encoding="utf-8"))
    datadir = os.path.join(TMP, name)
    shutil.rmtree(datadir, ignore_errors=True)
    os.makedirs(datadir, exist_ok=True)

    proc = start(datadir)
    diffs, idx = [], 0
    try:
        signal.signal(signal.SIGALRM, _alarm)
        for line in raw:
            s = line.strip()
            if not s:
                continue
            if s.startswith("--"):
                if s.startswith("--restart"):
                    proc.stdin.close()
                    proc.wait(timeout=STMT_TIMEOUT)
                    if proc.returncode != 0:
                        diffs.append("restart exit code %d stderr=%r" % (proc.returncode, proc.stderr.read()[-300:]))
                    proc = start(datadir)
                continue
            signal.setitimer(signal.ITIMER_REAL, STMT_TIMEOUT)
            proc.stdin.write(s + "\n")
            proc.stdin.flush()
            out = proc.stdout.readline()
            signal.setitimer(signal.ITIMER_REAL, 0)
            if not out.strip():
                diffs.append("[%d] no output for %s (stderr=%r)" % (idx, s[:80], proc.stderr.read()[-200:]))
                idx += 1
                continue
            try:
                got = json.loads(out)
            except Exception as e:
                diffs.append("[%d] bad JSON %r: %s" % (idx, out[:200], e))
                idx += 1
                continue
            if idx < len(exp):
                msg = check(exp[idx], got)
                if msg:
                    diffs.append("[%d] %s\n    sql=%s" % (idx, msg, s[:160]))
            idx += 1
        proc.stdin.close()
        proc.wait(timeout=STMT_TIMEOUT)
    except Timeout:
        diffs.append("timeout")
        proc.kill()
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
    if idx != len(exp):
        diffs.append("statement count: sql=%d expected=%d" % (idx, len(exp)))
    return ("PASS" if not diffs else "FAIL", diffs)


def main():
    names = sys.argv[1:]
    if not names:
        names = sorted(n[:-5] for n in os.listdir(CASES) if n.endswith(".test"))
    summary = {"PASS": 0, "FAIL": 0, "MISSING": 0}
    failed = []
    for n in names:
        status, diffs = run_case(n)
        summary[status] += 1
        print("%-6s %s" % (status, n))
        if status != "PASS":
            failed.append(n)
            for d in diffs[:6]:
                print("   " + d)
    print("\nSUMMARY " + " ".join("%s=%d" % (k, v) for k, v in summary.items()))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
