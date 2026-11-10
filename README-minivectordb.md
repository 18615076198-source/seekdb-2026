# MiniVectorDB（2026 competition round2 提交）

单机、单进程、单客户端的轻量向量数据库：SQL 子集、WAL + 快照、事务与崩溃恢复、精确 TopK / 过滤后 TopK，
大表按内存预算落成 append-only 冷段。

## 构建

    cmake -S minivectordb -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

或直接构建镜像：

    docker build -t minivectordb .
    docker run --rm -i -v /tmp/mvdb-data:/data minivectordb

程序从 stdin 读 SQL（一行一条，`;;` 可选），stdout 每条语句输出一个结果，stderr 输出日志。
默认数据目录 `/data`，可用 `MVDB_DATA_DIR` 覆盖。

## 运行参数（环境变量）

| 变量 | 默认 | 含义 |
| --- | --- | --- |
| `MVDB_DATA_DIR` | `/data` | 数据目录（wal.bin / snapshot.bin / *.seg） |
| `MVDB_MEM_BUDGET_MB` | `512` | hot 行内存预算；超过一半即把最大表落段 |
| `MVDB_PROTOCOL` | `json` | 输出协议：`json`（本地自测）或 `tsv`（占位） |

## SQL 支持

- `CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(n), emb VECTOR(d))`
- `INSERT INTO t VALUES (...), (...)`
- `SELECT [*|cols] FROM t [WHERE c op v [AND ...]] [ORDER BY col|alias|L2_DISTANCE(col,'[...]') [DESC]] [LIMIT n]`
- `UPDATE t SET col = v [, ...] [WHERE ...]`、`DELETE FROM t [WHERE ...]`
- `BEGIN` / `COMMIT` / `ROLLBACK`
- `CREATE VECTOR INDEX ...` 目前接受但不起作用（exact 结果始终正确）

错误以 `{"error":"<code>"}` 返回，例如 `duplicate_key`、`unsupported_sql`、`dimension_mismatch`。

## 数据与恢复

- `wal.bin`：长度 + CRC32 的记录流，事务以 COMMIT 记录结尾；恢复只应用以 COMMIT 结尾的前缀，尾部半条截断。
- `snapshot.bin`：全量快照（tmp → fsync → rename → fsync(dir)），随后截断 WAL。
- `<table>.seg`：冷数据段，append-only；重启扫描时截断不完整尾部，行由 WAL 回放补回。
- 提交语义：COMMIT 返回前完成 fdatasync；未提交事务在崩溃后不可见；进程被强杀后恢复可重入。

## 测试

    python3 minivectordb/tests/run_cases.py        # 公开包 20 例
    python3 minivectordb/tests/crash_matrix.py     # kill -9 场景
    python3 minivectordb/tests/spill_test.py       # 落段 + 重启

注意：数据目录放在 Linux 本机文件系统（容器内 /data）；WSL 的 /mnt/d 上做落段测试会慢几十倍。
