#!/usr/bin/env python3
"""Spec edge cases beyond the public package (JSONL protocol)."""
import os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonl_client import Client

TMP = "/tmp/edge"
FAILS = []


def check(name, cond, detail=""):
    if not cond:
        FAILS.append(name + (": " + detail if detail else ""))


def is_err(r):
    return isinstance(r, dict) and "error" in r


def main():
    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP, exist_ok=True)
    db = Client(TMP)
    db.execute("CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(4), emb VECTOR(2));")

    check("nan rejected", is_err(db.execute("INSERT INTO t VALUES (1,'a','[nan,1]');")))
    check("inf rejected", is_err(db.execute("INSERT INTO t VALUES (1,'a','[inf,1]');")))
    check("overflow rejected", is_err(db.execute("INSERT INTO t VALUES (1,'a','[1e39,1]');")))
    check("dim rejected", is_err(db.execute("INSERT INTO t VALUES (1,'a','[1]');")))
    check("no partial after batch error", db.execute("SELECT id FROM t;").get("rows") == [])
    check("bad literal rejected", is_err(db.execute("INSERT INTO t VALUES (1,'a','1,2');")))

    check("varchar 4 cjk ok", "error" not in db.execute("INSERT INTO t VALUES (1,'中文中文','[0,0]');"))
    check("varchar 5 cjk rejected", is_err(db.execute("INSERT INTO t VALUES (2,'中文中文中','[0,0]');")))
    check("varchar ascii 4 ok", "error" not in db.execute("INSERT INTO t VALUES (2,'abcd','[1,1]');"))
    check("varchar ascii 5 rejected", is_err(db.execute("INSERT INTO t VALUES (9,'abcde','[1,1]');")))

    db.execute("CREATE TABLE b (id BIGINT PRIMARY KEY, v BIGINT);")
    check("bigint min ok", "error" not in db.execute("INSERT INTO b VALUES (-9223372036854775808, 1);"))
    check("bigint max ok", "error" not in db.execute("INSERT INTO b VALUES (9223372036854775807, -1);"))
    check("bigint overflow rejected", is_err(db.execute("INSERT INTO b VALUES (9223372036854775808, 1);")))
    check("bigint exact read", db.execute("SELECT id FROM b WHERE v = 1;").get("rows") == [[-9223372036854775808]])

    check("limit 0 empty", db.execute("SELECT id FROM t LIMIT 0;").get("rows") == [])
    check("empty where", db.execute("SELECT id FROM t WHERE id > 100;").get("rows") == [])

    db.execute("CREATE TABLE s (id BIGINT PRIMARY KEY, cat VARCHAR(4), emb VECTOR(2));")
    db.execute("INSERT INTO s VALUES (5,'x','[1,0]'),(2,'x','[1,0]'),(9,'y','[3,4]');")
    check("l2 tie by pk asc", db.execute("SELECT id FROM s ORDER BY L2_DISTANCE(emb,'[0,0]') LIMIT 2;").get("rows") == [[2], [5]])
    check("order by id desc", db.execute("SELECT id FROM s ORDER BY id DESC;").get("rows") == [[9], [5], [2]])
    d = db.execute("SELECT L2_DISTANCE(emb,'[0,0]') AS d FROM s WHERE id = 9;")["rows"][0][0]
    check("l2 value", abs(d - 5.0) < 1e-9)
    check("filtered topk stays in filter", db.execute("SELECT id FROM s WHERE cat = 'y' ORDER BY L2_DISTANCE(emb,'[0,0]');").get("rows") == [[9]])
    check("star column order", db.execute("SELECT * FROM s WHERE id = 2;").get("columns") == ["id", "cat", "emb"])

    db.execute("CREATE TABLE u (id BIGINT PRIMARY KEY, name VARCHAR(8));")
    db.execute("INSERT INTO u VALUES (1,'a'),(2,'b'),(3,'中'),(4,'阿');")
    check("utf8 order vs ascii", db.execute("SELECT id FROM u WHERE name < 'b' ORDER BY id;").get("rows") == [[1]])
    check("cjk order", db.execute("SELECT id FROM u WHERE name > '中' ORDER BY id;").get("rows") == [[4]])

    db.execute("CREATE TABLE a (id BIGINT PRIMARY KEY, v VARCHAR(4));")
    db.execute("BEGIN;")
    check("txn insert visible", db.execute("INSERT INTO a VALUES (1,'x');").get("affected_rows") == 1)
    check("read own write", db.execute("SELECT id FROM a;").get("rows") == [[1]])
    check("dup in txn errors", is_err(db.execute("INSERT INTO a VALUES (2,'y'),(1,'z');")))
    check("failed statement leaves nothing", db.execute("SELECT id FROM a;").get("rows") == [[1]])
    db.execute("INSERT INTO a VALUES (2,'y');")
    db.execute("COMMIT;")
    check("commit persisted", db.execute("SELECT id FROM a ORDER BY id;").get("rows") == [[1], [2]])

    db.execute("BEGIN;")
    db.execute("UPDATE a SET v = 'q' WHERE id = 1;")
    db.execute("DELETE FROM a WHERE id = 2;")
    db.execute("ROLLBACK;")
    check("rollback restores", db.execute("SELECT id, v FROM a ORDER BY id;").get("rows") == [[1, "x"], [2, "y"]])

    db.execute("BEGIN;")
    db.execute("INSERT INTO a VALUES (3,'z');")
    db.close()
    db = Client(TMP)
    check("uncommitted gone after graceful restart", db.execute("SELECT id FROM a ORDER BY id;").get("rows") == [[1], [2]])
    db.shutdown()

    print("FAILS: %d" % len(FAILS))
    for f in FAILS:
        print("  " + f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
