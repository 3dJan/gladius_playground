#!/usr/bin/env python3
"""Compare WebGPU ray-march benchmark JSON files against a baseline.

Usage:
    bench_compare.py BASELINE.json CURRENT.json [--tolerance 0.10]

Both the full-frame schema (``medianWallMs``) and the progressive schema
(``timeToHqMs``) are detected automatically; the primary metric of the respective
schema decides the gate.

Exit codes:
    0  no regression beyond the tolerance
    1  at least one sample regressed beyond the tolerance
    2  baseline and current do not describe the same sample set
"""

import argparse
import json
import sys

SUPPORTED_SCHEMA_VERSIONS = {1, 2}
FULL_FRAME_METRIC = "medianWallMs"
PROGRESSIVE_METRIC = "timeToHqMs"
# Secondary progressive metrics worth reporting even though only the primary one gates.
PROGRESSIVE_CONTEXT = (
    "timeToFirstPixelMs",
    "chunks",
    "paceMs",
    "medianChunkGpuMs",
    "medianChunkSubmitCallMs",
    "medianChunkPaceMs",
    "medianChunkFixedOverheadMs",
)


def load_samples(path):
    with open(path, "r", encoding="utf-8") as stream:
        document = json.load(stream)
    version = document.get("schemaVersion")
    if version not in SUPPORTED_SCHEMA_VERSIONS:
        raise ValueError(f"{path}: unsupported schemaVersion {version!r}")
    samples = {}
    for sample in document.get("samples", []):
        key = (sample["model"], sample["resolution"])
        if key in samples:
            raise ValueError(f"{path}: duplicate sample {key}")
        samples[key] = sample
    return samples


def detect_metric(samples, path):
    """Pick the gating metric from whichever schema the file uses."""
    present = [
        metric
        for metric in (PROGRESSIVE_METRIC, FULL_FRAME_METRIC)
        if all(metric in sample for sample in samples.values())
    ]
    if not present:
        raise ValueError(f"{path}: no known timing metric found")
    return present[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument(
        "--tolerance",
        type=float,
        default=0.10,
        help="Relative wall-time regression fraction that fails the gate (default 0.10)",
    )
    args = parser.parse_args()

    baseline = load_samples(args.baseline)
    current = load_samples(args.current)

    missing = sorted(set(baseline) - set(current))
    extra = sorted(set(current) - set(baseline))
    if missing or extra:
        print(f"Sample sets differ: missing {missing}, extra {extra}")
        return 2

    metric = detect_metric(baseline, args.baseline)
    if detect_metric(current, args.current) != metric:
        print("Baseline and current use different benchmark schemas")
        return 2

    context_metrics = PROGRESSIVE_CONTEXT if metric == PROGRESSIVE_METRIC else ()

    regressions = []
    header = f"{'model':24} {'res':>5} {'baseline':>9} {'current':>9} {'delta':>7}"
    print(f"Metric: {metric}")
    print(header)
    for key in sorted(baseline):
        base = baseline[key]
        cur = current[key]
        base_ms = base[metric]
        cur_ms = cur[metric]
        delta = (cur_ms - base_ms) / base_ms if base_ms > 0.0 else 0.0
        status = "REGRESSION" if delta > args.tolerance else "ok"
        if delta > args.tolerance:
            regressions.append((key, delta))
        extras = ""
        if context_metrics:
            extras = "  " + " ".join(
                f"{name.removesuffix('Ms')}={base.get(name, '-')}/{cur.get(name, '-')}"
                for name in context_metrics
            )
        print(
            f"{key[0]:24} {key[1]:>5} {base_ms:>9.3f} {cur_ms:>9.3f} "
            f"{delta:>+6.1%}  {status}{extras}"
        )

    if regressions:
        print(f"\nFAIL: {len(regressions)} sample(s) regressed more than {args.tolerance:.0%}")
        for key, delta in regressions:
            print(f"  {key[0]} @ {key[1]}: {delta:+.1%}")
        return 1

    print("\nPASS: no wall-time regression beyond tolerance")
    return 0


if __name__ == "__main__":
    sys.exit(main())
