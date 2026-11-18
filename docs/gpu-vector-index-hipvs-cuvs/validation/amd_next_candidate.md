# Candidate after the 11.077-point submission

## Provenance and limits

The user reported **11.077**, `build=1.734x, query=1.014x, gates=pass`,
for branch `amd-cagra-d128`. Its retrieved head is
`beeefa159b77484e03ef05592c9bc70e8a05ecd7`; the evaluator's actual commit has
not been provided. This candidate descends from that head and V5
`634888d9b037153fa143d2c913ba4b25503e1d62`.

There are two independently reviewable changes:

1. **Worker reuse:** keep the scored 128/128/64 build parameters and reuse
   one 32 MiB search thread. Requests are synchronous and serialized, and the
   worker is joined at shutdown. Failure to start never runs GPU code on the
   small OB caller stack. GPU search remains the fixed hipVS CAGRA call.
2. **Build tuning:** use graph/intermediate degree 160/160 and 20 NN-descent
   iterations for all dimensions, retaining search width 4 and itopk >=512.
   This parameter set comes from the existing repository branch
   `codex/amd-juanzhi-local20`, commit
   `2932fc4ebb64b1619c90fed95fe5091a24bea0b5`. Its local report is evidence for
   a candidate, not proof of an official hidden-set result.

The original bridge's mutex, allocation checks, persistent-index shape checks
and OBCUVS02 snapshot format remain in place. This candidate does not include
the reference branch's snapshot-format change or parallel auxiliary scans.
Consequently, its published 3.05x/2.27x measurements do **not** describe this
candidate. Graph degree 160 may increase graph memory and must pass the
relative VRAM gate in an actual paired run.

## Validation performed on 2026-11-18

- V5 and the retrieved scored branch are ancestors.
- Standalone C++ worker test: 6003 jobs, eight concurrent submitters, exact
  result ownership, a stable worker thread, a touched 2 MiB stack buffer, and
  three successful create/join lifetimes. Built with MinGW GCC 8.1, C++11,
  `-O2 -Wall -Wextra -Werror -pthread` on Windows.
- Python score-combination tests cover caps, recall failures, per-dataset
  query regression, weighted geometric means and invalid inputs.
- No Linux/ROCm full build, GPU timing, VRAM measurement or recall measurement
  has been performed for this candidate. No usable SSH configuration was found
  on the current machine. Official score is **pending**.

## Reproduce on the contest GPU

Use the fixed bridge and CMake build commands in `amd_contest_report.md`.
Build the V5 observer with its own V5 bridge, and the candidate with its own
bridge. Run `run_gpu_smoke.sh` for CPU/GPU behavior and persistence, then use
`run_paired_bench.sh` for each dataset with `--nq 4096 --gt-queries 0` and
Top-10/Top-100 (the default). Supply real SIFT using `--base-tsv` and
`--query-tsv`; synthetic 256-dimensional data is a proxy, not Hidden250K.
Use separate evidence and base directories for different datasets.

The paired harness now rejects duplicate or unknown IDs across **all** queries,
including when recall is sampled. A resource/profiler check is still required.
Combine the two generated reports with:

```bash
python3 tools/hipvs_bridge/score_paired.py \
  --sift /work/evidence-sift/paired_result.json \
  --hidden-proxy /work/evidence-256/paired_result.json
```

The script computes weighted geometric means (40% SIFT, 60% 256-d), checks
the measured recall and query gates, and reports a conditional score. It
explicitly leaves the official score unset. A measured gate failure makes
the conditional score zero. The score caps require build >=3x and query >=1.5x.

If 160/160/20 fails recall, evaluate the worker-only commit: it preserves the
previously scored graph-building parameters. Do not compensate for a failed
gate by changing benchmark data, excluding queries or using CPU results.
