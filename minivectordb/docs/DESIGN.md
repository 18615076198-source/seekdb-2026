# MiniVectorDB 设计说明 v1（2026-11-09）

## 0. 协议现状（最高优先级风险）

官方说明原文：「SQL 的语句边界、结果集格式、错误码、就绪标识、精确／近似模式切换及正常退出方式，统一遵循 2026-competition-round2 分支配套测试驱动和题包约定，不再采用此前示例中的 JSON 请求／响应封装。」

结论：公开包里的 .result 是**断言格式**，不是线上协议。官方驱动/题包尚未发布（分支上只有 mysqltest/）。
因此实现按「引擎 + 可替换适配层」拆分：

    ProtocolAdapter:
      on_start()             -> ready 标识（待定）
      next_statement()       -> 一条完整 SQL（处理行/分号/空行/注释）
      emit(Result)           -> 结果/状态（待定格式）
      on_restart()/on_exit() -> 正常关闭路径
      set_mode(exact|ann)    -> 模式切换（待定载体）

本地自测先用「一行一条 SQL → 一行一个 JSON」，拿到题包后只改适配层。

## 1. 进程与目录

- 单进程、单线程、前台，stdin 读 SQL，stdout 写结果，stderr 写日志，不监听网络。
- 数据目录：环境变量 MVDB_DATA_DIR，默认 /data。
- 文件：
  - /data/wal.bin        预写日志（追加）
  - /data/snapshot.bin   全量快照
  - /data/snapshot.tmp   快照临时文件（原子替换用）

## 2. WAL 记录格式

每条记录：

    [u32 payload_len][u32 crc32(payload)][payload]

payload 首字节是类型：

    1 CREATE : table(str) colCount(u32) {name(str) ty(u8) width(i32) pk(u8)}* pk(i32)
    2 UPSERT : table(str) key(i64) valueCount(u32) value*
    3 DELETE : table(str) key(i64)
    4 COMMIT : （无负载）

value 编码：u8 type + 载荷（Int: i64；Double: f64；Str: u32 len + bytes；Vec: u32 dim + f32*）。

事务边界：一个事务的记录连续追加，最后写 COMMIT。恢复时只应用「以 COMMIT 结尾的前缀」。
尾部半条记录：长度越界或 crc 不匹配 → 截断到最后一条完整记录（ftruncate）。

## 3. 快照与检查点

- 写 snapshot.tmp → fdatasync → rename(snapshot.bin) → fsync(dir) → ftruncate(wal, 0)。
- 触发：正常关闭（EOF/关闭信号）；WAL 超过阈值（待实现）。
- 幂等性：CREATE 已存在则跳过；UPSERT 覆盖；DELETE 不存在则忽略。因此「快照已落盘但 WAL 未截断」时重复回放不会破坏数据。

## 4. 事务

- 语句执行前记录 undo（行前像 / 建表）；
- 语句失败：只回放本语句 undo（语句级 savepoint），事务继续；
- COMMIT：txnOps 一次性 append + fdatasync，之后才输出成功；
- ROLLBACK：逆序回放 txnUndo，不写日志；
- 自动提交：每条修改语句自成事务，append + fdatasync；
- 读己写：修改直接作用于内存表，读路径天然可见；
- 主键更新：题目不要求，直接返回 unsupported_sql。

## 5. 恢复流程

    open(data_dir):
      mkdir -p
      load snapshot.bin  -> 内存表
      open wal.bin
      replay wal: 逐条校验；遇到 COMMIT 才把暂存 op 应用；尾部坏记录截断
    apply(recovered ops) -> 幂等应用到内存表
    然后才输出 ready（索引重建也必须在 ready 之前完成）

恢复可重入：恢复过程只读 WAL（仅在尾部不完整时截断），重复被杀不会放大损坏。

## 6. 查询执行

- WHERE：解析为 (列, op, 常量)，比较用强类型；字符串按 UTF-8 字节序 = Unicode 码点序。
- 排序：ORDER BY 可引用输出别名或表列；DESC 只反转主键值，不反转并列主键升序规则。
- LIMIT 0 直接返回空。
- L2_DISTANCE：float32 存储、double 累加、开方；与键值同批输出。
- 浮点输出：整数值输出 x.0，其余用最短可回读表示；断言侧 1e-5 容差。

## 7. 向量索引与 ANN（未实现）

- CREATE VECTOR INDEX 目前是 no-op；ann 模式退化为 exact（结果仍正确，性能不达标）。
- 计划 IVF：k-means 聚类 → 倒排桶 → 查询探测 nprobe 桶 → float32 重排；ann 结果必须满足：ID 有效、无重复、满足 WHERE；Recall@10 < 0.95 时自动回退 exact。

## 8. 内存策略（已知缺口）

当前实现全量常驻内存。正式用例包含「数据量超过内存限额」，当前版本会失败。
计划：行存改为定长/变长段文件 + mmap + LRU 页缓存，扫描流式；向量列单独连续存储便于 SIMD。

## 9. 测试

- tests/run_cases.py：20 个公开用例，含 --restart 模拟（关 stdin → 重启进程，复用数据目录）。当前 20/20。
- tests/crash_matrix.py：kill -9 四个点（自动提交后、未提交事务、事务部分修改、恢复中再杀）。当前 4/4。
- 待补：大表超内存、SIGTERM 正常关闭、协议适配、ANN 召回。

## 10. 里程碑

M1 SQL/存储/向量精确检索（完成）
M2 WAL/快照/事务/崩溃恢复（完成，公开包 20/20）
M3 官方题包协议适配 + Dockerfile + 提测链路（阻塞：题包/驱动、推送权限）
M4 性能：SIMD、批量写、ANN（未开始）
M5 大表分页/流式（未开始）

## 11. 与 ARIES 的关系（为什么当前不需要 loser 回滚）

当前实现是 NO-STEAL + 逻辑 redo：运行期只写 WAL，数据只在 checkpoint 时以全量快照落盘；未提交事务的修改只存在于内存。
因此崩溃后不存在「已刷盘的脏页」，未提交事务天然不可见（presumed abort），无需 loser 列表、prevLSN 或 CLR。

一旦为大表引入页式缓冲池（必须能在事务中把脏页刷盘），就退化为 STEAL，届时必须补：
- 每条记录的 LSN、页头 pageLSN、WAL 与刷页顺序（write-ahead）
- 恢复三阶段（analysis/redo/undo）与 CLR
- 检查点恢复点（DPT）

结论：现在的简单恢复模型成立的前提是「绝不提前写数据页」。这个前提必须写进代码注释，避免后续优化时被无意破坏。

## 12. 大表：冷数据段（已实现，替代第 8 节的计划）

- 触发：`MVDB_MEM_BUDGET_MB`（默认 512MB），当所有表的 hot 行总字节超过 budget/2 时，把最大的表落段。
- 落段方式：**append-only**。把 hot 行按记录 `[u32 len][payload]` 追加到 `/data/<table>.seg`，更新内存中的 `pk -> 偏移` 索引，然后清空 hot 行。
- 顺序不变式：append 只发生在 WAL `fdatasync` 之后（自动提交/COMMIT 返回前），所以段文件里不会出现未提交数据；崩溃时撕裂的尾巴在重启扫描时截断，行本身由 WAL 回放补回。
- 删除：内存里记 tombstone（dead set），读取时过滤；checkpoint 时用 `compactTo()` 全量重写段文件，顺带丢弃旧版本与 tombstone。
- 恢复：快照里对已落段的表只记元数据（段文件名、行数），启动时 `scanSegment()` 重建 `pk->偏移` 与 order，并截断不完整尾部。
- 内存上界 ≈ hot 行 + pk 索引（每行约 24B：order 8B + offsets 16B）。**已知限制**：如果行数多到 pk 索引本身超过内存预算，需要把索引也放到磁盘（尚未实现）。

## 13. 测试与观测（当前）

- `tests/run_cases.py`：公开包 20/20；`MVDB_MEM_BUDGET_MB=0` 强制每步落段时同样 20/20。
- `tests/crash_matrix.py`：kill -9 四个场景 4/4。
- `tests/spill_test.py`：2 万行落段 + 重启 + 更新/删除 5/5；10 万行×128 维（52MB 数据、预算 16MB）6.2s，进程 VmHWM 17.7MB，运行期落段 16 次。
- 注意：WSL 的 `/mnt/d`（drvfs）上做落段测试会慢 40 倍以上，性能测试务必把数据目录放到 `/tmp`（或容器内 /data），否则测到的是文件系统而不是数据库。
