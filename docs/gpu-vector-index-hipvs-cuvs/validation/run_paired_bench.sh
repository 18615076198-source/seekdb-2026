#!/usr/bin/env bash
#
# Paired build/query benchmark for the AMD CAGRA contest task.
#
# Runs the frozen V5 baseline observer and the candidate observer on the SAME
# dataset, on the same machine, and reports the pairing the contest scores:
#
#   build_speedup = baseline_build_ms / candidate_build_ms
#   query_speedup = geomean(QPS_top10 ratio, QPS_top100 ratio)
#   recall@k      = |approx_topk & exact_topk| / k, averaged over the probes
#
# The timed window follows the contest definition: from CREATE VECTOR INDEX
# until the first fixed APPROXIMATE query returns a legal result.
#
# Design notes
#   * No third-party python packages. Dataset generation is stdlib `random`,
#     ground truth is produced by seekdb itself with an exact (non-APPROXIMATE)
#     kNN scan, so the numbers match the server's own distance semantics.
#   * Ground truth is computed once, before any vector index exists, and cached
#     in the evidence directory so the second variant reuses it.
#   * Everything is seeded, so two hosts produce identical datasets.
#
# Usage: run_paired_bench.sh --help
#
set -Eeuo pipefail

usage()
{
  cat <<'EOF'
Usage: run_paired_bench.sh --baseline PATH --candidate PATH --candidate-bridge PATH
                           --base-dir PATH --port PORT --evidence PATH
                           [--baseline-bridge PATH] [--baseline-name NAME]
                           [--candidate-name NAME]
                           [--n 250000] [--dim 128] [--nq 4096] [--gt-queries 256]
                           [--k 10] [--k 100]
                           [--base-tsv PATH] [--query-tsv PATH]
                           [--skip-baseline] [--skip-candidate]
                           [--mysql PATH]

  --baseline      seekdb observer binary built from the frozen V5 commit
  --candidate     seekdb observer binary built from the candidate commit
  --candidate-bridge  libseekdb_cuvs_bridge.so built from the candidate tree
  --baseline-bridge   libseekdb_cuvs_bridge.so built from the frozen V5 commit.
                  Defaults to --candidate-bridge only when the branch does not
                  touch tools/hipvs_bridge; otherwise the two MUST differ or the
                  baseline runs with the candidate's accelerated bridge and the
                  comparison is meaningless.
  --base-dir      scratch directory for observer data dirs and generated data
  --evidence      directory for logs, traces and result tables
  --n / --dim     synthetic dataset size (default 250000 x 128)
  --nq            number of single-row queries used for the QPS measurement
  --gt-queries    how many of those queries get an exact ground truth (recall
                  is averaged over these only; 0 means "all", which is slow)
  --base-tsv      optional: id<TAB>[f0,f1,...] rows. Generated when omitted.
  --query-tsv     optional: one [f0,f1,...] vector literal per line

Outputs, under --evidence:
  paired_runs.tsv     raw per-variant measurements
  paired_result.tsv   the two-row comparison table for amd_contest_report.md
  paired_result.json  same content, machine readable, with the score estimate
  observer-<name>.log server log (contains the cuVS build_us breakdown)
  trace-<name>.log    OB_VSAG_TRACE output
EOF
}

BASELINE=""
CANDIDATE=""
BRIDGE=""
BASELINE_BRIDGE=""
BASE_DIR=""
PORT=""
EVIDENCE=""
BASELINE_NAME="baseline"
CANDIDATE_NAME="candidate"
N=250000
DIM=128
NQ=4096
GT_QUERIES=256
KS=()
BASE_TSV=""
QUERY_TSV=""
SKIP_BASELINE=0
SKIP_CANDIDATE=0
MYSQL_BIN=""

while (($# > 0)); do
  case "$1" in
    --baseline)        BASELINE=$2; shift 2 ;;
    --candidate)       CANDIDATE=$2; shift 2 ;;
    --bridge)          BRIDGE=$2; shift 2 ;;
    --candidate-bridge) BRIDGE=$2; shift 2 ;;
    --baseline-bridge) BASELINE_BRIDGE=$2; shift 2 ;;
    --base-dir)        BASE_DIR=$2; shift 2 ;;
    --port)            PORT=$2; shift 2 ;;
    --evidence)        EVIDENCE=$2; shift 2 ;;
    --baseline-name)   BASELINE_NAME=$2; shift 2 ;;
    --candidate-name)  CANDIDATE_NAME=$2; shift 2 ;;
    --n)               N=$2; shift 2 ;;
    --dim)             DIM=$2; shift 2 ;;
    --nq)              NQ=$2; shift 2 ;;
    --gt-queries)      GT_QUERIES=$2; shift 2 ;;
    --k)               KS+=("$2"); shift 2 ;;
    --base-tsv)        BASE_TSV=$2; shift 2 ;;
    --query-tsv)       QUERY_TSV=$2; shift 2 ;;
    --skip-baseline)   SKIP_BASELINE=1; shift ;;
    --skip-candidate)  SKIP_CANDIDATE=1; shift ;;
    --mysql)           MYSQL_BIN=$2; shift 2 ;;
    -h|--help)         usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

((${#KS[@]} > 0)) || KS=(10 100)

# ---------------------------------------------------------------- validation --
[[ -n "$BASE_DIR" && "$BASE_DIR" != "/" ]] || { echo "unsafe or missing --base-dir" >&2; exit 2; }
[[ -n "$EVIDENCE" && "$EVIDENCE" != "/" ]] || { echo "unsafe or missing --evidence" >&2; exit 2; }
[[ "$PORT" =~ ^[0-9]+$ ]] || { echo "missing or invalid --port" >&2; exit 2; }
[[ "$N" =~ ^[0-9]+$ && "$DIM" =~ ^[0-9]+$ && "$NQ" =~ ^[0-9]+$ && "$GT_QUERIES" =~ ^[0-9]+$ ]] \
  || { echo "invalid numeric argument" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "candidate bridge does not exist: $BRIDGE" >&2; exit 2; }
[[ -n "$BASELINE_BRIDGE" ]] || BASELINE_BRIDGE=$BRIDGE
[[ -f "$BASELINE_BRIDGE" ]] || { echo "baseline bridge does not exist: $BASELINE_BRIDGE" >&2; exit 2; }
if (( SKIP_BASELINE == 0 )) && [[ ! -x "$BASELINE" ]]; then
  echo "baseline observer is not executable: $BASELINE" >&2; exit 2
fi
if (( SKIP_CANDIDATE == 0 )) && [[ ! -x "$CANDIDATE" ]]; then
  echo "candidate observer is not executable: $CANDIDATE" >&2; exit 2
fi
if (( SKIP_BASELINE == 1 && SKIP_CANDIDATE == 1 )); then
  echo "both variants skipped, nothing to do" >&2; exit 2
fi
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }
if [[ -z "$MYSQL_BIN" ]]; then
  MYSQL_BIN=$(command -v mysql || true)
fi
[[ -n "$MYSQL_BIN" && -x "$MYSQL_BIN" ]] || { echo "mysql client not found; pass --mysql" >&2; exit 2; }

mkdir -p "$BASE_DIR" "$EVIDENCE"
DATA_DIR=$EVIDENCE/data
mkdir -p "$DATA_DIR"

RUN_TSV=$EVIDENCE/paired_runs.tsv
RESULT_TSV=$EVIDENCE/paired_result.tsv
RESULT_JSON=$EVIDENCE/paired_result.json
: > "$RUN_TSV"

DB=bench
OBS_PID=""
LOG=/dev/null

say() { printf '[paired-bench] %s\n' "$*"; }
die() { printf '[paired-bench] ERROR: %s\n' "$*" >&2; exit 1; }
now_ms() { date +%s%3N; }

# ------------------------------------------------------------------- dataset --
prepare_dataset()
{
  if [[ -n "$BASE_TSV" && -n "$QUERY_TSV" ]]; then
    [[ -f "$BASE_TSV" && -f "$QUERY_TSV" ]] || die "dataset file missing: $BASE_TSV / $QUERY_TSV"
    N=$(wc -l < "$BASE_TSV")
    NQ=$(wc -l < "$QUERY_TSV")
    DIM=$(head -n 1 "$BASE_TSV" | cut -f2 | tr -cd ',' | wc -c)
    DIM=$((DIM + 1))
    say "dataset: n=$N dim=$DIM nq=$NQ (user supplied)"
    return 0
  fi

  BASE_TSV=$DATA_DIR/base_n${N}_d${DIM}.tsv
  QUERY_TSV=$DATA_DIR/query_nq${NQ}_d${DIM}.tsv
  if [[ -s "$BASE_TSV" && -s "$QUERY_TSV" ]]; then
    say "dataset: reusing $BASE_TSV"
    return 0
  fi

  say "dataset: generating n=$N dim=$DIM nq=$NQ (seed=20261111)"
  python3 - "$N" "$DIM" "$NQ" "$BASE_TSV" "$QUERY_TSV" <<'PY'
import random
import sys

n, dim, nq = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
base_path, query_path = sys.argv[4], sys.argv[5]

rng = random.Random(20261111)


def emit(path, rows, with_id):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        for i in range(rows):
            vec = ",".join(f"{rng.random():.4f}" for _ in range(dim))
            if with_id:
                fh.write(f"{i}\t[{vec}]\n")
            else:
                fh.write(f"[{vec}]\n")


emit(base_path, n, True)
emit(query_path, nq, False)
PY
  say "dataset: wrote $BASE_TSV and $QUERY_TSV"
}

# ------------------------------------------------------------------ observer --
start_observer()
{
  local observer=$1
  local bridge=$2
  local run_dir=$3
  local log=$4
  local trace=$5

  rm -rf "$run_dir"
  mkdir -p "$run_dir"
  : > "$trace"

  ulimit -n 65535 2>/dev/null || true
  env -u OB_VSAG_USE_CUVS \
      "LD_LIBRARY_PATH=$(dirname "$bridge"):/opt/hipvs/lib:/opt/rocm/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
      "OB_VSAG_TRACE=1" \
      "OB_VSAG_TRACE_FILE=$trace" \
      "$observer" \
      --nodaemon \
      --port "$PORT" \
      --data-dir "$run_dir/store" \
      --base-dir "$run_dir" \
      --parameter __min_full_resource_pool_memory=2147483648 \
      --parameter memory_budget=24G \
      --parameter datafile_size=64G \
      --parameter cpu_count=24 \
      --parameter local_ip=127.0.0.1 \
      >"$log" 2>&1 &
  OBS_PID=$!
  wait_ready
}

stop_observer()
{
  [[ -n "$OBS_PID" ]] || return 0
  kill -TERM "$OBS_PID" 2>/dev/null || true
  for _ in $(seq 1 120); do
    kill -0 "$OBS_PID" 2>/dev/null || break
    sleep 1
  done
  kill -KILL "$OBS_PID" 2>/dev/null || true
  wait "$OBS_PID" 2>/dev/null || true
  OBS_PID=""
}

cleanup() { stop_observer || true; }
trap cleanup EXIT INT TERM

sql()
{
  "$MYSQL_BIN" --local-infile=1 --connect-timeout=10 --protocol=TCP \
    -h127.0.0.1 -P"$PORT" -uroot -A -N -B \
    -e "set ob_query_timeout=3000000000; $1"
}

wait_ready()
{
  local i
  for i in $(seq 1 240); do
    [[ -d "/proc/$OBS_PID" ]] || die "observer exited during startup (see $LOG)"
    if "$MYSQL_BIN" --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$PORT" \
        -uroot -e 'select 1' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  die "observer did not become ready on port $PORT"
}

# ------------------------------------------------------- ground truth via SQL --
gt_cache_path()
{
  printf '%s/gt_n%s_d%s_g%s_k%s.txt' "$DATA_DIR" "$N" "$DIM" "$GT_QUERIES" "$1"
}

collect_ground_truth()
{
  local k=$1
  local out
  out=$(gt_cache_path "$k")
  if [[ -s "$out" ]]; then
    say "ground truth: reusing $out"
    return 0
  fi
  local g=$GT_QUERIES
  (( g > 0 )) || g=$NQ
  say "ground truth: exact top-$k for $g probes (full scan, no vector index yet)"
  python3 - "$QUERY_TSV" "$DATA_DIR/gt_queries_k$k.sql" "$k" "$DB" "$g" <<'PY'
import sys

src, dst, k, db, g = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
with open(src, "r", encoding="utf-8") as fh, open(dst, "w", encoding="utf-8") as out:
    out.write(f"use {db};\n")
    for i, line in enumerate(fh):
        if i >= g:
            break
        q = line.strip()
        if q:
            out.write(f"select id from items order by l2_distance(v,{q}) limit {k};\n")
PY
  local t0 t1
  t0=$(now_ms)
  "$MYSQL_BIN" --local-infile=1 --connect-timeout=10 --protocol=TCP \
    -h127.0.0.1 -P"$PORT" -uroot -A -N -B < "$DATA_DIR/gt_queries_k$k.sql" > "$out"
  t1=$(now_ms)
  local got
  got=$(wc -l < "$out")
  [[ "$got" == "$(( g * k ))" ]] || die "ground truth top-$k returned $got rows, expected $(( g * k ))"
  say "ground truth: top-$k done in $(( (t1 - t0) / 1000 )) s -> $out"
}

# -------------------------------------------------------------------- running --
run_variant()
{
  local name=$1
  local observer=$2
  local bridge=$3
  local run_root=$BASE_DIR/$name
  LOG=$EVIDENCE/observer-$name.log
  local trace=$EVIDENCE/trace-$name.log

  say "=== variant: $name ==="
  say "$name: observer=$observer"
  if [[ "$bridge" == "$BASELINE_BRIDGE" ]]; then
    say "$name: bridge=$bridge (same library for both variants)"
  else
    say "$name: bridge=$bridge"
  fi
  start_observer "$observer" "$bridge" "$run_root" "$LOG" "$trace"
  say "$name: observer up on port $PORT (pid $OBS_PID)"

  sql "create database if not exists $DB;
       use $DB;
       drop table if exists items;
       create table items(id bigint primary key, v vector($DIM));"

  say "$name: loading $N rows"
  local l0 l1
  l0=$(now_ms)
  sql "use $DB;
       load data local infile '$BASE_TSV' into table items
       fields terminated by '\\t' lines terminated by '\\n' (id,v);"
  l1=$(now_ms)
  local rows
  rows=$(sql "select count(*) from $DB.items")
  [[ "$rows" == "$N" ]] || die "$name: expected $N rows after LOAD DATA, got $rows"
  say "$name: loaded $rows rows in $(( (l1 - l0) / 1000 )) ms"

  # Ground truth needs an index-free table; it is cached after the first run.
  local k
  for k in "${KS[@]}"; do
    collect_ground_truth "$k"
  done

  local probe
  probe=$(head -n 1 "$QUERY_TSV")

  # ---- timed window: CREATE VECTOR INDEX .. first fixed query -------------
  say "$name: CREATE VECTOR INDEX (timed)"
  local t0 t1 build_ms first_rows
  t0=$(now_ms)
  sql "use $DB;
       create vector index idx_items on items(v)
       with (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);"
  first_rows=$(sql "select id from $DB.items order by l2_distance(v,$probe) approximate limit 10" | wc -l)
  t1=$(now_ms)
  [[ "$first_rows" == "10" ]] || die "$name: first query returned $first_rows rows, expected 10"
  build_ms=$(( t1 - t0 ))
  say "$name: build window ${build_ms} ms"

  # ---- build phase breakdown reported by the server -----------------------
  local build_us graph_bytes
  build_us=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -n 1 \
             | sed -n 's/.*build_us=\([0-9]*\).*/\1/p' || true)
  graph_bytes=$(grep -a 'serialized GPU CAGRA snapshot' "$LOG" | tail -n 1 \
                | sed -n 's/.*graph_bytes=\([0-9]*\).*/\1/p' || true)
  [[ -n "${build_us:-}" ]] || build_us=-1
  [[ -n "${graph_bytes:-}" ]] || graph_bytes=-1
  say "$name: gpu_build_us=$build_us graph_bytes=$graph_bytes"

  # ---- query throughput: one SQL statement per query, one session --------
  for k in "${KS[@]}"; do
    python3 - "$QUERY_TSV" "$DATA_DIR/queries_k$k.sql" "$k" "$DB" <<'PY'
import sys

src, dst, k, db = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
with open(src, "r", encoding="utf-8") as fh, open(dst, "w", encoding="utf-8") as out:
    out.write(f"use {db};\n")
    for line in fh:
        q = line.strip()
        if q:
            out.write(f"select id from items order by l2_distance(v,{q}) approximate limit {k};\n")
PY
    say "$name: $NQ single-row queries, top-$k"
    local q0 q1 qms qps
    q0=$(now_ms)
    "$MYSQL_BIN" --local-infile=1 --connect-timeout=10 --protocol=TCP \
      -h127.0.0.1 -P"$PORT" -uroot -A -N -B < "$DATA_DIR/queries_k$k.sql" \
      > "$EVIDENCE/ids-$name-k$k.txt"
    q1=$(now_ms)
    qms=$(( q1 - q0 ))
    local got
    got=$(wc -l < "$EVIDENCE/ids-$name-k$k.txt")
    [[ "$got" == "$(( NQ * k ))" ]] || die "$name: top-$k returned $got ids, expected $(( NQ * k ))"
    qps=$(python3 -c "print(f'{$NQ * 1000.0 / $qms:.2f}')")
    say "$name: top-$k qps=$qps"

    local recall
    recall=$(python3 - "$EVIDENCE/ids-$name-k$k.txt" "$(gt_cache_path "$k")" "$k" \
                        "$NQ" "$GT_QUERIES" <<'PY'
import sys

ids_path, gt_path, k, nq, g = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5])
g = g or nq
g = min(g, nq)

with open(gt_path, "r", encoding="utf-8") as fh:
    gt = [int(x) for x in fh.read().split()]
with open(ids_path, "r", encoding="utf-8") as fh:
    got = [int(x) for x in fh.read().split()]

assert len(gt) >= g * k, f"ground truth has {len(gt)} ids, expected {g * k}"
assert len(got) >= g * k, f"results have {len(got)} ids, expected at least {g * k}"

hits = 0
unique_ok = True
for i in range(g):
    row = got[i * k:(i + 1) * k]
    if len(set(row)) != k:
        unique_ok = False
    hits += len(set(row) & set(gt[i * k:(i + 1) * k]))
print(f"{hits / (g * k):.6f}{'' if unique_ok else ' duplicate_ids=true'}")
PY
)
    say "$name: top-$k recall@$k=$recall"

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$k" "$build_ms" "$qps" "$recall" "$build_us" "$graph_bytes" >> "$RUN_TSV"
  done

  sql "use $DB; drop table if exists items;" || true
  stop_observer
  say "$name: done"
}

# --------------------------------------------------------------------- output --
report()
{
  python3 - "$RUN_TSV" "$RESULT_TSV" "$RESULT_JSON" "$BASELINE_NAME" "$CANDIDATE_NAME" <<'PY'
import json
import math
import sys

runs_path, tsv_path, json_path, base_name, cand_name = sys.argv[1:6]

rows = []
with open(runs_path, "r", encoding="utf-8") as fh:
    for line in fh:
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 7:
            continue
        name, k, build_ms, qps, recall, build_us, graph_bytes = parts
        rows.append({
            "variant": name,
            "k": int(k),
            "build_ms": int(build_ms),
            "qps": float(qps),
            "recall": float(recall.split()[0]),
            "gpu_build_us": int(build_us),
            "graph_bytes": int(graph_bytes),
        })

by = {}
for row in rows:
    by.setdefault(row["variant"], {})[row["k"]] = row

base = by.get(base_name, {})
cand = by.get(cand_name, {})

b_build = base[max(base)]["build_ms"] if base else None
c_build = cand[max(cand)]["build_ms"] if cand else None
build_ratio = (b_build / c_build) if (b_build and c_build) else None

q_ratios = []
for k in sorted(set(base) & set(cand)):
    if base[k]["qps"] > 0:
        q_ratios.append(cand[k]["qps"] / base[k]["qps"])
query_ratio = None
if q_ratios and all(x > 0 for x in q_ratios):
    query_ratio = math.exp(sum(math.log(x) for x in q_ratios) / len(q_ratios))

build_points = 0.0
if build_ratio:
    build_points = 14.0 * min(max(math.log(build_ratio) / math.log(3.0), 0.0), 1.0)
query_points = 0.0
if query_ratio:
    query_points = 2.0 * min(max(math.log(query_ratio) / math.log(1.5), 0.0), 1.0)

lines = [
    "metric\tbaseline\tcandidate\tratio",
]
for k in sorted(set(base) | set(cand)):
    b = base.get(k, {})
    c = cand.get(k, {})
    r = (c.get("qps", 0) / b["qps"]) if (b.get("qps") and c.get("qps")) else ""
    lines.append(f"qps_top{k}\t{b.get('qps', '')}\t{c.get('qps', '')}\t{round(r, 4) if r != '' else ''}")
    lines.append(f"recall@{k}\t{b.get('recall', '')}\t{c.get('recall', '')}\t")
lines.append(f"build_ms\t{b_build if b_build is not None else ''}\t"
             f"{c_build if c_build is not None else ''}\t"
             f"{round(build_ratio, 4) if build_ratio else ''}")
lines.append(f"score_total(if gates pass)\t\t\t{round(4.0 + build_points + query_points, 3)}")
lines.append(f"score_build\t\t\t{round(build_points, 3)}")
lines.append(f"score_query\t\t\t{round(query_points, 3)}")

summary = {
    "baseline": base_name,
    "candidate": cand_name,
    "build_ms": {"baseline": b_build, "candidate": c_build},
    "build_speedup": None if build_ratio is None else round(build_ratio, 4),
    "query_speedup": None if query_ratio is None else round(query_ratio, 4),
    "score": {
        "build": round(build_points, 3),
        "query": round(query_points, 3),
        "total_if_gates_pass": round(4.0 + build_points + query_points, 3),
    },
    "raw": by,
    "table": lines,
}

with open(tsv_path, "w", encoding="utf-8", newline="\n") as fh:
    fh.write("\n".join(lines) + "\n")
with open(json_path, "w", encoding="utf-8", newline="\n") as fh:
    json.dump(summary, fh, indent=2, sort_keys=True)
    fh.write("\n")

print("\n".join(lines))
PY
}

# ------------------------------------------------------------------- main ------
prepare_dataset
kmax=${KS[0]}
for k in "${KS[@]}"; do
  if (( k > kmax )); then
    kmax=$k
  fi
done
if (( GT_QUERIES == 0 )); then
  GT_QUERIES=$NQ
fi

if (( SKIP_BASELINE == 0 )); then
  run_variant "$BASELINE_NAME" "$BASELINE" "$BASELINE_BRIDGE"
fi
if (( SKIP_CANDIDATE == 0 )); then
  run_variant "$CANDIDATE_NAME" "$CANDIDATE" "$BRIDGE"
fi

say "results:"
report
say "wrote $RESULT_TSV and $RESULT_JSON"
