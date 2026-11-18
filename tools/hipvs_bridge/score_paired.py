#!/usr/bin/env python3
"""Combine two run_paired_bench.sh JSON reports; never certify unmeasured gates."""
import argparse
import json
import math


def positive(value, label):
    value = float(value)
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f"{label} must be finite and positive")
    return value


def summarize(reports):
    failures = []
    datasets = {}
    build_log = query_log = 0.0
    for name, weight, floors in (
        ("sift", 0.4, {10: 0.95, 100: 0.95}),
        ("hidden_proxy", 0.6, {10: 0.90, 100: 0.85}),
    ):
        report = reports[name]
        raw = report["raw"]
        baseline = raw[report["baseline"]]
        candidate = raw[report["candidate"]]
        ratios = []
        times = []
        for k in (10, 100):
            b, c = baseline[str(k)], candidate[str(k)]
            ratios.append(positive(c["qps"], "candidate QPS") /
                          positive(b["qps"], "baseline QPS"))
            times.append((positive(b["build_ms"], "baseline build_ms"),
                          positive(c["build_ms"], "candidate build_ms")))
            br, cr = float(b["recall"]), float(c["recall"])
            if not (0 <= br <= 1 and 0 <= cr <= 1):
                raise ValueError("recall must be finite and in [0, 1]")
            if cr + 1e-12 < floors[k]:
                failures.append(f"{name}:recall@{k}_minimum")
            if br - cr > 0.005 + 1e-12:
                failures.append(f"{name}:recall@{k}_baseline_delta")
        if times[0] != times[1]:
            raise ValueError(f"{name}: Top-10 and Top-100 must share one build")
        build = times[0][0] / times[0][1]
        query = math.exp(sum(math.log(r) for r in ratios) / 2)
        if query + 1e-12 < 0.9:
            failures.append(f"{name}:query_regression")
        build_log += weight * math.log(build)
        query_log += weight * math.log(query)
        datasets[name] = {"build_speedup": build, "query_speedup": query}
    build_points = 14 * min(1, max(0, build_log / math.log(3)))
    query_points = 2 * min(1, max(0, query_log / math.log(1.5)))
    return {
        "datasets": datasets,
        "build_speedup": math.exp(build_log),
        "query_speedup": math.exp(query_log),
        "measured_gate_failures": failures,
        "score_if_all_other_gates_pass": 0 if failures else round(4 + build_points + query_points, 3),
        "official_score": None,
        "unverified": ["official Hidden250K", "GPU profiler/path", "VRAM and host memory",
                       "CPU backend and persistence", "all-query ID integrity",
                       "full 4096-query recall if ground truth was sampled"],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sift", required=True)
    parser.add_argument("--hidden-proxy", required=True)
    args = parser.parse_args()
    reports = {}
    for name, path in (("sift", args.sift), ("hidden_proxy", args.hidden_proxy)):
        with open(path, encoding="utf-8") as source:
            reports[name] = json.load(source)
    print(json.dumps(summarize(reports), indent=2))


if __name__ == "__main__":
    main()
