#!/usr/bin/env python3
"""Compare design-agent benchmark JSON reports against a baseline.

The primary gate is the number of tasks that passed (visual judge AND deterministic
geometry checks): any drop fails. Secondary metrics (model turns, tool calls, tool
failures, tokens, wall time) are reported per task and fail when they regress beyond
both a relative tolerance and an absolute counter grace, because a worse harness should
show up there even when the task still passes.

Prompt cost is gated harder than the other effort metrics: prompt prefill dominates wall time, so
`usageWithJudge.totalPromptTokens` gets its own tighter tolerance and the prompt-cache hit rate
(`trace.profile.context.promptCacheHitRate`) may not drop. The cache hit rate is what catches a
harness change that quietly makes every turn re-prefill the whole prompt again.

The hit rate only carries that signal once the cached prefix is big enough for the provider to
cache at all: a provider refuses to cache prefixes below a minimum size, so a leaner harness (fewer
tools, shorter system prompt) can cut prompt tokens by half and still report 0% hits. The gate is
therefore skipped when the first turn's prompt is below --cache-min-prompt-tokens; total prompt
tokens are still gated there.

Usage:
    agent_bench_compare.py BASELINE.json CURRENT.json [--token-tolerance 0.5]
                                                    [--prompt-token-tolerance 0.25]
                                                    [--cache-hit-slack 0.05]
                                                    [--cache-min-prompt-tokens 2600]
                                                    [--counter-grace 5]
                                                    [--allow-same-pass-count]

Exit codes:
    0  no regression beyond the tolerances
    1  at least one gate regressed
    2  baseline and current do not describe the same task set
"""

import argparse
import json
import sys

SUPPORTED_SCHEMA_VERSIONS = {1}

# (metric path, higher_is_better) - a regression is a move in the wrong direction.
SECONDARY_METRICS = (
    ("metrics", "modelTurns", False),
    ("metrics", "toolCalls", False),
    ("metrics", "toolFailures", False),
    ("metrics", "providerFailures", False),
    ("metrics", "unknownToolCalls", False),
    ("metrics", "truncatedTurns", False),
    ("usageWithJudge", "outputTokens", False),
)

# Gated with the tighter --prompt-token-tolerance, because prefill is where the wall time is.
PROMPT_COST_METRICS = (
    ("usageWithJudge", "totalPromptTokens"),
)

# (path, higher_is_better) rates: a rate regression is a drop below baseline by more than slack.
RATE_METRICS = (
    (("trace", "profile", "context", "promptCacheHitRate"), True),
)

# Reported for diagnosis only, never gating.
DIAGNOSTIC_METRICS = (
    ("trace", "profile", "context", "contextTax"),
    ("trace", "profile", "context", "cacheMissTurns"),
    ("trace", "profile", "context", "cacheRewindTurns"),
    ("trace", "profile", "context", "systemPromptMutations"),
    ("trace", "profile", "context", "imagesInHistoryLastTurn"),
    ("trace", "profile", "context", "historyPrunes"),
    ("trace", "profile", "context", "prunedMessages"),
    ("trace", "profile", "timeSplit", "modelPrefillMs"),
)

# Reported but never gating: a local LLM server's throughput depends on whatever else
# the machine is doing, so wall time is not a harness-quality signal.
CONTEXT_METRICS = (
    ("metrics", "wallMs"),
    ("metrics", "libraryImports"),
    ("metrics", "libraryExports"),
    ("metrics", "viewportCaptures"),
    ("metrics", "librarySearches"),
)


def load_runs(path):
    with open(path, "r", encoding="utf-8") as stream:
        document = json.load(stream)
    version = document.get("schemaVersion")
    if version not in SUPPORTED_SCHEMA_VERSIONS:
        raise ValueError(f"{path}: unsupported schemaVersion {version!r}")
    runs = {}
    for run in document.get("runs", []):
        task_id = run["taskId"]
        if task_id in runs:
            raise ValueError(f"{path}: duplicate task {task_id}")
        runs[task_id] = run
    if not runs:
        raise ValueError(f"{path}: no runs found")
    return document, runs


def dig(run, path):
    value = run
    for key in path:
        if not isinstance(value, dict) or key not in value:
            return None
        value = value[key]
    return value


def passed(run):
    return bool(dig(run, ("verdict", "taskPassed")))


def describe(run):
    verdict = run.get("verdict", {})
    parts = [
        f"agent={verdict.get('agentCompleted')}",
        f"geometry={verdict.get('geometryPassed')}",
        f"visual={verdict.get('visualPassed')}",
    ]
    checks = run.get("geometryChecks") or []
    failed = [c["name"] for c in checks if not c.get("passed")]
    if failed:
        parts.append("failedGates=" + ",".join(failed))
    judge_error = (run.get("judge") or {}).get("error")
    if judge_error:
        parts.append("judgeError=" + judge_error[:80])
    return " ".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline")
    parser.add_argument("current")
    parser.add_argument(
        "--token-tolerance",
        type=float,
        default=0.5,
        help="Relative cost/effort regression fraction that fails the gate (default 0.5). "
             "Local LLM runs vary a lot turn to turn, so this is deliberately loose.",
    )
    parser.add_argument(
        "--counter-grace",
        type=int,
        default=5,
        help="Absolute slack for small counters (turns, tool calls, failures) before a "
             "regression fails the gate (default 5)",
    )
    parser.add_argument(
        "--prompt-token-tolerance",
        type=float,
        default=0.25,
        help="Relative regression fraction for totalPromptTokens (default 0.25). Tighter than "
             "--token-tolerance because prompt bytes are what a harness optimisation targets.",
    )
    parser.add_argument(
        "--cache-hit-slack",
        type=float,
        default=0.05,
        help="How far the prompt-cache hit rate may drop below baseline before it fails "
             "(default 0.05, i.e. 5 percentage points)",
    )
    parser.add_argument(
        "--cache-min-prompt-tokens",
        type=int,
        default=2600,
        help="Skip the prompt-cache hit-rate gate for runs whose first turn is smaller than this. "
             "Providers only cache prefixes above a minimum size, so a leaner base prompt can cut "
             "tokens in half while legitimately reporting 0%% hits (default 2600).",
    )
    parser.add_argument(
        "--allow-same-pass-count",
        action="store_true",
        help="Do not fail when the pass count stays equal but an individual task regressed",
    )
    args = parser.parse_args()

    baseline_doc, baseline = load_runs(args.baseline)
    current_doc, current = load_runs(args.current)

    missing = sorted(set(baseline) - set(current))
    extra = sorted(set(current) - set(baseline))
    if missing or extra:
        print(f"Task set mismatch: missing={missing} extra={extra}")
        return 2

    regressions = []
    baseline_passed = sum(1 for run in baseline.values() if passed(run))
    current_passed = sum(1 for run in current.values() if passed(run))

    print(
        f"tasks passed: baseline={baseline_passed}/{len(baseline)} "
        f"current={current_passed}/{len(current)}"
    )
    print(
        "endpoint/model: "
        f"{current_doc.get('metadata', {}).get('endpoint')}/"
        f"{current_doc.get('metadata', {}).get('model')}"
    )

    base_harness = baseline_doc.get("metadata", {}).get("harness")
    curr_harness = current_doc.get("metadata", {}).get("harness")
    if base_harness is not None and curr_harness is not None and base_harness != curr_harness:
        print("harness configuration changed (cost metrics are not comparable across surfaces):")
        for key in sorted(set(base_harness) | set(curr_harness)):
            if base_harness.get(key) != curr_harness.get(key):
                print(f"  {key}: {base_harness.get(key)!r} -> {curr_harness.get(key)!r}")

    for task_id in sorted(baseline):
        base, now = baseline[task_id], current[task_id]
        print(f"\n[{task_id}]")
        print(f"  baseline: {describe(base)}")
        print(f"  current : {describe(now)}")

        if passed(base) and not passed(now):
            regressions.append(f"{task_id}: task went from passed to failed")

        for path in CONTEXT_METRICS:
            b, c = dig(base, path), dig(now, path)
            if b is not None and c is not None:
                print(f"  {path[1]}: {b} -> {c}")

        for path in DIAGNOSTIC_METRICS:
            b, c = dig(base, path), dig(now, path)
            if b is not None and c is not None:
                print(f"  {path[-1]}: {b} -> {c}")

        for group, metric in PROMPT_COST_METRICS:
            b, c = dig(base, (group, metric)), dig(now, (group, metric))
            if b is None or c is None:
                continue
            print(f"  {metric}: {b} -> {c} ({c - b:+d})")
            if b > 0 and c - b > b * args.prompt_token_tolerance:
                regressions.append(
                    f"{task_id}: {metric} regressed {b} -> {c} "
                    f"(> {args.prompt_token_tolerance:.0%} of baseline)"
                )

        for path, higher_is_better in RATE_METRICS:
            b, c = dig(base, path), dig(now, path)
            if b is None or c is None or not isinstance(b, float):
                continue
            if path[-1] == "promptCacheHitRate":
                currentFirstTurn = dig(now, ("trace", "profile", "context",
                                             "firstTurnPromptTokens")) or 0
                if currentFirstTurn < args.cache_min_prompt_tokens:
                    print(f"  {path[-1]}: {b:.3f} -> {c:.3f} (not gated: first turn is "
                          f"{currentFirstTurn} tokens, below the {args.cache_min_prompt_tokens} "
                          f"token provider cache minimum)")
                    continue
            print(f"  {path[-1]}: {b:.3f} -> {c:.3f}")
            if higher_is_better and c + args.cache_hit_slack < b:
                regressions.append(
                    f"{task_id}: {path[-1]} regressed {b:.3f} -> {c:.3f} "
                    f"(more than {args.cache_hit_slack:.2f} below baseline)"
                )

        for group, metric, higher_is_better in SECONDARY_METRICS:
            b, c = dig(base, (group, metric)), dig(now, (group, metric))
            if b is None or c is None:
                continue
            delta = c - b
            print(f"  {metric}: {b} -> {c} ({delta:+d})")
            # A relative tolerance alone is too brittle for the small counters (one extra
            # failed tool call would fail the gate), so a change has to clear both a
            # relative and an absolute floor.
            if not higher_is_better and delta > max(b * args.token_tolerance, args.counter_grace):
                regressions.append(
                    f"{task_id}: {metric} regressed {b} -> {c} "
                    f"(> max({args.token_tolerance:.0%} of baseline, {args.counter_grace}))"
                )

    if current_passed < baseline_passed:
        regressions.append(
            f"pass count dropped {baseline_passed} -> {current_passed}"
        )
    elif not args.allow_same_pass_count and current_passed == baseline_passed:
        # A task that flipped failed -> failed is still worth surfacing, but only a
        # pass-count change gates by default; individual flips are printed above.
        pass

    print()
    if regressions:
        print("REGRESSIONS:")
        for item in regressions:
            print(f"  - {item}")
        return 1

    print("OK: no gate regressed beyond tolerances")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}")
        sys.exit(2)
