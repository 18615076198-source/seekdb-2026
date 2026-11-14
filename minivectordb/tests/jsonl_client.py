"""Minimal JSONL client shared by the local harnesses."""
import json, os, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BIN = os.path.abspath(os.path.join(HERE, "..", "build", "mvdb"))


class Client:
    def __init__(self, datadir, extra_env=None):
        self.bin = os.environ.get("MVDB_BIN", DEFAULT_BIN)
        env = dict(os.environ, MVDB_DATA_DIR=datadir)
        if extra_env:
            env.update(extra_env)
        self.p = subprocess.Popen([self.bin], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, encoding="utf-8", env=env)
        self._id = 0
        line = self.p.stdout.readline()
        self.ready = json.loads(line) if line.strip() else None

    def _next_id(self):
        self._id += 1
        return self._id

    def execute(self, sql, mode="exact"):
        self.p.stdin.write(json.dumps({"id": self._next_id(), "op": "execute", "sql": sql,
                                       "search_mode": mode}) + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        if not r.get("ok"):
            err = r.get("error") or {}
            return {"error": err.get("code", "error") if isinstance(err, dict) else err}
        return {k: r[k] for k in ("columns", "rows", "affected_rows") if k in r}

    def shutdown(self):
        self.p.stdin.write(json.dumps({"id": self._next_id(), "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        r = json.loads(self.p.stdout.readline())
        self.p.wait(timeout=120)
        return r

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        self.p.wait(timeout=120)
