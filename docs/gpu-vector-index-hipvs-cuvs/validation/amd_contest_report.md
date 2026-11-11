# AMD contest validation report

Reproducible validation procedure for the AMD CAGRA contest branch. Official
performance numbers must come from a paired evaluation on the contest AMD GPU;
local smoke results are functional evidence only.

## Source and build

- Branch: `amd-v5-base`
- V5 ancestor: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Backend: AMD hipVS/cuVS CAGRA
- Toolchain: ROCm 7.2.4, GCC 13.3, CMake 3.31, Rust 1.97
- hipVS root: `/opt/hipvs`, ROCm root: `/opt/rocm`

Build the bridge and seekdb with the fixed contest toolchain:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

## Functional validation

Run the self-contained smoke test and persistence test before submitting:

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
  --observer /path/to/trace-build/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-cuvs-smoke \
  --port 2981 \
  --render /dev/dri/renderD135 \
  --evidence /tmp/seekdb-cuvs-evidence
```

The smoke summary must report PASS and include GPU CAGRA build/serve, VSAG
isolation, result correctness, freshness handling, and persistence checks.

## Paired performance evaluation

`run_paired_bench.sh` runs the frozen V5 observer and the candidate observer on
the same machine, same dataset and same SQL, and reports exactly the pairing the
contest scores. It needs no third-party python packages: the dataset is
generated from a fixed seed, and the exact ground truth is produced by seekdb
itself with a non-APPROXIMATE kNN scan before any vector index exists.

```bash
LD_LIBRARY_PATH=/work/bridge:/opt/hipvs/lib:/opt/rocm/lib \
  docs/gpu-vector-index-hipvs-cuvs/validation/run_paired_bench.sh \
  --baseline   /work/build-v5/build_release/src/observer/seekdb \
  --candidate  /work/build-cand/build_release/src/observer/seekdb \
  --bridge     /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir   /tmp/seekdb-paired \
  --evidence   /work/paired-evidence \
  --port       2981 \
  --n 250000 --dim 128 --nq 4096 --gt-queries 256
```

Swap `--dim 128` for `--dim 256` to match the Hidden250K shape. Pass
`--base-tsv`/`--query-tsv` to use a real dataset instead of the synthetic one.

The timed window is the contest definition: from `CREATE VECTOR INDEX` until the
first fixed APPROXIMATE query returns a legal result. The script also lifts
`build_us` and `graph_bytes` out of the server log line

```
[OBVSAG][cuVS] serialized GPU CAGRA snapshot ... built=<bool> build_us=<us> graph_bytes=<bytes>
```

so the GPU build phase is separated from the surrounding CPU/snapshot work.
That split is what decides where further optimisation pays off.

`paired_result.tsv` is directly pasteable below; `paired_result.json` also
contains the estimated score.

| Dataset | V5 build (s) | Candidate build (s) | Build speedup | V5 Recall@10/@100 | Candidate Recall@10/@100 | V5 QPS top10/top100 | Candidate QPS top10/top100 |
|---|---:|---:|---:|---|---|---|---|
| SIFT250K (250K x 128) | pending | pending | pending | pending | pending | pending | pending |
| Hidden250K (250K x 256) | pending | pending | pending | pending | pending | pending | pending |

Build speedup is `V5 build time / candidate build time`. Query speedup is the
geometric mean of the Top-10 and Top-100 QPS ratios for each dataset. Combined
score follows the contest formula in section 9 of the problem statement.

Submit only after all hard gates pass: correct unique IDs, Recall thresholds,
real GPU build/search, query throughput at least 90% of the paired baseline,
and memory limits.
