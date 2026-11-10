# AMD contest validation report

This document records the reproducible validation procedure for the AMD
contest branch. Official performance numbers must be filled from a paired
evaluation on the contest AMD GPU; local smoke results are functional evidence
only.

## Source and build

- Branch: `amd-v5-base`
- V5 ancestor: `634888d9b037153fa143d2c913ba4b25503e1d62`
- Backend: AMD hipVS/cuVS CAGRA
- Expected toolchain: ROCm 7.2.4, GCC 13.3, CMake 3.31, Rust 1.97
- hipVS root: `/opt/hipvs`
- ROCm root: `/opt/rocm`

Build the bridge and seekdb with the fixed contest toolchain:

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j64
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

The smoke summary must report PASS and include GPU CAGRA build/serve,
VSAG isolation, result correctness, freshness handling, and persistence
checks.

## Paired performance evaluation

Run the frozen V5 commit and the candidate on the same AMD Radeon PRO W7900D,
with caches and indexes cleared before each run. Use the fixed SIFT250K and
Hidden250K inputs and include the first fixed query in build timing.

Record the official result here:

| Dataset | V5 build (s) | Candidate build (s) | Build speedup | V5 Recall@10/@100 | Candidate Recall@10/@100 | V5 QPS top10/top100 | Candidate QPS top10/top100 |
|---|---:|---:|---:|---|---|---|---|
| SIFT250K (250K x 128) | pending | pending | pending | pending | pending | pending | pending |
| Hidden250K (250K x 256) | pending | pending | pending | pending | pending | pending | pending |

Build speedup is `V5 build time / candidate build time`. Query speedup is
the geometric mean of the Top-10 and Top-100 QPS ratios for each dataset.
Submit only after all hard gates pass: correct unique IDs, Recall thresholds,
real GPU build/search, query throughput at least 90% of the paired baseline,
and memory limits.
