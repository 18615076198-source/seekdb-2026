#!/usr/bin/env python3
"""Spec edge cases beyond the public package: rejection rules, atomicity, ordering ties."""
import json, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MVDB_BIN", os.path.abspath(os.path.join(HERE, "..", "build", "mvdb")))
TMP = os.path.join(HERE, "tmp", "edge")
FAILS = []


class DB:
    def __init__(self, datadir):
        self.dir = datadir
        shutil.rmtree(datadir, ignore_errors=True)
        os.makedirs(datadir, exist_ok=True)
        env = dict(os.environ, MVDB_DATA_DIR=datadir)
        self.p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)

    def q(self, sql):
        self.p.stdin.write(sql + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("no response for " + sql + " stderr=" + self.p.stderr.read()[-200:])
        return json.loads(line)

    def restart(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)
        env = dict(os.environ, MVDB_DATA_DIR=self.dir)
        self.p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=60)


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name + (": " + detail if detail else ""))


def is_err(r):
    return isinstance(r, dict) and "error" in r


def main():
    db = DB(TMP)
    db.q("CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(4), emb VECTOR(2));")

    # --- vector validation
    check("nan rejected", is_err(db.q("INSERT INTO t VALUES (1,'a','[nan,1]');")))
    check("inf rejected", is_err(db.q("INSERT INTO t VALUES (1,'a','[inf,1]');")))
    check("overflow rejected", is_err(db.q("INSERT INTO t VALUES (1,'a','[1e39,1]');")))
    check("dim rejected", is_err(db.q("INSERT INTO t VALUES (1,'a','[1]');")),
          str(db.q("SELECT id FROM t;")))
    check("no partial after batch error", db.q("SELECT id FROM t;").get("rows") == [])
    check("bad literal rejected", is_err(db.q("INSERT INTO t VALUES (1,'a','1,2');")))

    # --- varchar counted in unicode code points
    check("varchar 4 cjk ok", "error" not in db.q("INSERT INTO t VALUES (1,'中文中文','[0,0]');"))
    check("varchar 5 cjk rejected", is_err(db.q("INSERT INTO t VALUES (2,'中文中文中','[0,0]');")))
    check("varchar ascii 4 ok", "error" not in db.q("INSERT INTO t VALUES (2,'abcd','[1,1]');"))
    check("varchar ascii 5 rejected", is_err(db.q("INSERT INTO t VALUES (9,'abcde','[1,1]');")))

    # --- bigint boundaries
    db.q("CREATE TABLE b (id BIGINT PRIMARY KEY, v BIGINT);")
    check("bigint min ok", "error" not in db.q("INSERT INTO b VALUES (-9223372036854775808, 1);"))
    check("bigint max ok", "error" not in db.q("INSERT INTO b VALUES (9223372036854775807, -1);"))
    check("bigint overflow rejected", is_err(db.q("INSERT INTO b VALUES (9223372036854775808, 1);")))
    check("bigint exact read",
          db.q("SELECT id FROM b WHERE v = 1;").get("rows") == [[-9223372036854775808]])

    # --- limit 0 / empty results
    check("limit 0 empty", db.q("SELECT id FROM t LIMIT 0;").get("rows") == [])
    check("empty where", db.q("SELECT id FROM t WHERE id > 100;").get("rows") == [])
    check("delete empty affected 0", db.q("DELETE FROM t WHERE id > 100;").get("affected_rows") == 0)

    # --- ties by primary key, descending
    db.q("CREATE TABLE s (id BIGINT PRIMARY KEY, cat VARCHAR(4), emb VECTOR(2));")
    db.q("INSERT INTO s VALUES (5,'x','[1,0]'),(2,'x','[1,0]'),(9,'y','[3,4]');")
    check("l2 tie by pk asc",
          db.q("SELECT id FROM s ORDER BY L2_DISTANCE(emb,'[0,0]') LIMIT 2;").get("rows") == [[2], [5]])
    check("order by id desc", db.q("SELECT id FROM s ORDER BY id DESC;").get("rows") == [[9], [5], [2]])
    check("l2 value", abs(db.q("SELECT L2_DISTANCE(emb,'[0,0]') AS d FROM s WHERE id = 9;")["rows"][0][0] - 5.0) < 1e-9)
    check("filtered topk stays in filter",
          db.q("SELECT id FROM s WHERE cat = 'y' ORDER BY L2_DISTANCE(emb,'[0,0]');").get("rows") == [[9]])
    check("star column order", db.q("SELECT * FROM s WHERE id = 2;").get("columns") == ["id", "cat", "emb"])

    # --- string comparison by code point (UTF-8 byte order)
    db.q("CREATE TABLE u (id BIGINT PRIMARY KEY, name VARCHAR(8));")
    db.q("INSERT INTO u VALUES (1,'a'),(2,'b'),(3,'中'),(4,'阿');")
    rows = db.q("SELECT id FROM u WHERE name < 'b' ORDER BY id;").get("rows")
    check("utf8 order vs ascii", rows == [[1]], str(rows))
    check("cjk order", db.q("SELECT id FROM u WHERE name > '中' ORDER BY id;").get("rows") == [[4]])

    # --- statement atomicity inside a transaction
    db.q("CREATE TABLE a (id BIGINT PRIMARY KEY, v VARCHAR(4));")
    db.q("BEGIN;")
    check("txn insert visible", db.q("INSERT INTO a VALUES (1,'x');").get("affected_rows") == 1)
    check("read own write", db.q("SELECT id FROM a;").get("rows") == [[1]])
    check("dup in txn errors", is_err(db.q("INSERT INTO a VALUES (2,'y'),(1,'z');")))
    check("failed statement leaves nothing", db.q("SELECT id FROM a;").get("rows") == [[1]])
    check("txn continues", db.q("INSERT INTO a VALUES (2,'y');").get("affected_rows") == 1)
    db.q("COMMIT;")
    check("commit persisted", db.q("SELECT id FROM a ORDER BY id;").get("rows") == [[1], [2]])

    db.q("BEGIN;")
    db.q("UPDATE a SET v = 'q' WHERE id = 1;")
    db.q("DELETE FROM a WHERE id = 2;")
    db.q("ROLLBACK;")
    check("rollback restores", db.q("SELECT id, v FROM a ORDER BY id;").get("rows") == [[1, "x"], [2, "y"]])

    # --- restart keeps committed state only
    db.q("BEGIN;")
    db.q("INSERT INTO a VALUES (3,'z');")
    db.restart()
    check("uncommitted gone after graceful restart", db.q("SELECT id FROM a ORDER BY id;").get("rows") == [[1], [2]])
    check("schema survives", db.q("SELECT v FROM a WHERE id = 1;").get("rows") == [["x"]])
    db.close()

    print("FAILS: %d" % len(FAILS))
    for f in FAILS:
        print("  " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
