#!/usr/bin/env python3
"""Rank the bottlenecks of a design-agent run from its JSONL trace.

Usage:
    agent_bench_profile.py TRACE                      # one run: rank the costs
    agent_bench_profile.py BEFORE AFTER               # two runs: what changed
    agent_bench_profile.py RUN_DIR                     # dir with task subdirs

A trace is the `trace.jsonl` written by GLADIUS_AGENT_TRACE or by a benchmark run
(`<output>/<task>/trace.jsonl`). Records are `run_meta`, `turn`, `tool_call`, `phase`
and `note`; the aggregation mirrors `analyzeTrace()` in src/agent/trace so the script
and the benchmark report can never disagree about a number.

The output is ordered by what an optimization could actually win: time split first,
then the model-turn shape (prefill versus generation), then context growth, then the
waste the harness caused. Exit code is 0 unless a trace file cannot be read.
"""

import argparse
import json
import math
import os
import sys


def percentile(values, q):
    """Nearest-rank percentile of a sample list (unsorted input is fine)."""
    if not values:
        return 0
    ordered = sorted(values)
    idx = max(0, min(len(ordered) - 1, math.ceil(q * len(ordered)) - 1))
    return ordered[idx]


def rate(tokens, ms):
    if tokens <= 0 or ms <= 0:
        return 0.0
    return tokens / (ms / 1000.0)


def load_records(path):
    records = []
    with open(path, "r", encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            records.append(json.loads(line))
    return records


def resolve_trace(arg):
    """Accept a trace file, a task directory, or a run directory."""
    if os.path.isfile(arg):
        return arg
    if os.path.isdir(arg):
        direct = os.path.join(arg, "trace.jsonl")
        if os.path.isfile(direct):
            return direct
        for entry in sorted(os.listdir(arg)):
            candidate = os.path.join(arg, entry, "trace.jsonl")
            if os.path.isfile(candidate):
                return candidate
    raise FileNotFoundError(arg)


def analyze(records, wall_ms=0):
    """Aggregates one trace; kept deliberately close to the C++ profiler."""
    out = {
        "turns": 0,
        "toolCalls": 0,
        "contextBuildMs": 0,
        "modelMs": 0,
        "modelPrefillMs": 0,
        "modelGenerateMs": 0,
        "toolMs": 0,
        "harnessMs": 0,
        "unaccountedMs": 0,
        "turnP50Ms": 0,
        "turnP95Ms": 0,
        "turnMaxMs": 0,
        "ttftP50Ms": 0,
        "ttftP95Ms": 0,
        "prefillTokPerSec": 0.0,
        "decodeTokPerSec": 0.0,
        "thinkingChars": 0,
        "textChars": 0,
        "toolJsonChars": 0,
        "firstPromptTokens": 0,
        "lastPromptTokens": 0,
        "peakPromptTokens": 0,
        "totalPromptTokens": 0,
        "cacheReadTokens": 0,
        "contextTax": 0.0,
        "cacheHitRate": 0.0,
        "cacheMissTurns": 0,
        "systemPromptMutations": 0,
        "imagesKept": 0,
        "imagesDropped": 0,
        "compactedToolResults": 0,
        "imagesInHistoryLastTurn": 0,
        "promptBytes": {},
        "largestToolResult": (0, 0),
        "tools": {},
        "truncatedTurns": 0,
        "failedToolCalls": 0,
        "deniedToolCalls": 0,
        "turnsAfterFailure": 0,
        "turnsAfterFailureMs": 0,
        "turnsAfterFailurePromptTokens": 0,
        "autonomousNudges": 0,
        "truncationNudges": 0,
        "turnsWithoutToolCalls": 0,
        "readOnlyTurnsMs": 0,
        "judgeMs": 0,
        "mutatingToolCalls": 0,
        "phases": {},
    }

    retry_turns = set()
    for rec in records:
        if rec.get("type") != "note":
            continue
        kind = rec.get("kind", "")
        if kind == "retry_after_failure":
            retry_turns.add((rec.get("runId", ""), rec.get("turnIndex", -1)))
            out["turnsAfterFailure"] += 1
        elif kind == "autonomous_nudge":
            out["autonomousNudges"] += 1
        elif kind == "truncation_nudge":
            out["truncationNudges"] += 1

    seen_hash_state: dict = {}
    turn_durations = []
    ttft_samples = []
    prefill_rates = []
    decode_rates = []

    for rec in records:
        rtype = rec.get("type")
        if rtype == "turn":
            out["turns"] += 1
            duration = rec.get("durationMs", 0)
            build = rec.get("buildMs", 0)
            completion = rec.get("completionMs", 0)
            # An SSE stream can open before the server has prefilled, so prefill ends at the first
            # visible output rather than at the first event.
            ttft = max(rec.get("ttftMs", 0), rec.get("firstContentMs", 0))
            turn_index = rec.get("turnIndex", 0)

            turn_durations.append(duration)
            out["contextBuildMs"] += build
            out["modelMs"] += completion if completion > 0 else duration - build
            out["modelPrefillMs"] += min(ttft, completion)
            out["modelGenerateMs"] += max(0, completion - ttft)
            if ttft > 0:
                ttft_samples.append(ttft)

            usage = rec.get("usage", {})
            if usage.get("cacheReadTokens", 0) <= 0:
                out["cacheMissTurns"] += 1
            # A changed system prompt invalidates the whole cached prefix: every such turn is a
            # full re-prefill, which is the most expensive mistake a harness can make.
            if "promptHashSeen" not in seen_hash_state:
                seen_hash_state["promptHashSeen"] = True
            elif rec.get("systemPromptHash", 0) != seen_hash_state["hash"]:
                out["systemPromptMutations"] += 1
            seen_hash_state["hash"] = rec.get("systemPromptHash", 0)
            ctx = rec.get("context") or {}
            out["imagesKept"] += ctx.get("imagesKept", 0)
            out["imagesDropped"] += ctx.get("imagesDropped", 0)
            out["compactedToolResults"] += ctx.get("toolResultsCompacted", 0)

            prompt = usage.get("totalPromptTokens", 0)
            output = usage.get("outputTokens", 0)
            out["totalPromptTokens"] += prompt
            out["cacheReadTokens"] += usage.get("cacheReadTokens", 0)
            if turn_index == 1:
                out["firstPromptTokens"] = prompt
            out["lastPromptTokens"] = prompt
            out["peakPromptTokens"] = max(out["peakPromptTokens"], prompt)

            prefill = rate(prompt, ttft)
            if prefill > 0:
                prefill_rates.append(prefill)
            decode = rate(output, max(0, completion - ttft))
            if decode > 0:
                decode_rates.append(decode)

            out["thinkingChars"] += rec.get("thinkingChars", 0)
            out["textChars"] += rec.get("textChars", 0)
            out["toolJsonChars"] += rec.get("toolJsonChars", 0)
            if rec.get("stopReason") in ("max_tokens", "length"):
                out["truncatedTurns"] += 1
            if rec.get("toolCallsIssued", 0) == 0:
                out["turnsWithoutToolCalls"] += 1
                out["readOnlyTurnsMs"] += duration
            if (rec.get("runId", ""), turn_index) in retry_turns:
                out["turnsAfterFailureMs"] += duration
                out["turnsAfterFailurePromptTokens"] += prompt

            prompt_bytes = rec.get("promptBytes") or {}
            out["imagesInHistoryLastTurn"] = prompt_bytes.get("images", 0)
            for key, value in prompt_bytes.items():
                if key.endswith("Bytes") and key != "totalBytes":
                    out["promptBytes"][key] = out["promptBytes"].get(key, 0) + value

        elif rtype == "tool_call":
            out["toolCalls"] += 1
            name = rec.get("name", "?")
            duration = rec.get("durationMs", 0)
            entry = out["tools"].setdefault(
                name, {"calls": 0, "failures": 0, "totalMs": 0, "maxMs": 0, "resultBytes": 0}
            )
            entry["calls"] += 1
            entry["totalMs"] += duration
            out["toolMs"] += duration
            entry["maxMs"] = max(entry["maxMs"], duration)
            entry["resultBytes"] += rec.get("resultBytes", 0)
            if not rec.get("success", True):
                entry["failures"] += 1
                out["failedToolCalls"] += 1
            if rec.get("denied", False):
                out["deniedToolCalls"] += 1
            if rec.get("policy") in ("design_graph_modification", "model_modification"):
                out["mutatingToolCalls"] += 1
            result_bytes = rec.get("resultBytes", 0)
            if result_bytes > out["largestToolResult"][0]:
                out["largestToolResult"] = (result_bytes, rec.get("turnIndex", 0))

        elif rtype == "phase":
            name = rec.get("name", "phase")
            duration = rec.get("durationMs", 0)
            out["harnessMs"] += duration
            phase = out["phases"].setdefault(name, {"calls": 0, "totalMs": 0})
            phase["calls"] += 1
            phase["totalMs"] += duration
            if name == "judge":
                out["judgeMs"] += duration

    out["turnP50Ms"] = percentile(turn_durations, 0.5)
    out["turnP95Ms"] = percentile(turn_durations, 0.95)
    out["turnMaxMs"] = max(turn_durations, default=0)
    out["ttftP50Ms"] = percentile(ttft_samples, 0.5)
    out["ttftP95Ms"] = percentile(ttft_samples, 0.95)
    out["prefillTokPerSec"] = (
        sum(prefill_rates) / len(prefill_rates) if prefill_rates else 0.0
    )
    out["decodeTokPerSec"] = sum(decode_rates) / len(decode_rates) if decode_rates else 0.0
    if out["totalPromptTokens"] > 0 and out["cacheReadTokens"] > 0:
        out["cacheHitRate"] = out["cacheReadTokens"] / out["totalPromptTokens"]

    # Each task's clock restarts at zero, so wall time and the context-tax denominator have to be
    # computed per run and then combined; merging raw timestamps would understate both.
    run_extents = {}
    run_final_prompt = {}
    for rec in records:
        run_id = rec.get("runId", "")
        if rec.get("type") == "turn":
            run_final_prompt[run_id] = rec.get("usage", {}).get("totalPromptTokens", 0)
        run_extents[run_id] = max(run_extents.get(run_id, 0), rec.get("tMs", 0))

    out["wallMs"] = wall_ms or sum(run_extents.values())
    if run_final_prompt:
        combined_final = sum(run_final_prompt.values())
        if out["totalPromptTokens"] > 0 and combined_final > 0:
            out["contextTax"] = out["totalPromptTokens"] / combined_final
    spent = (
        out["contextBuildMs"] + out["modelMs"] + out["toolMs"] + out["harnessMs"]
    )
    out["unaccountedMs"] = max(0, out["wallMs"] - spent)
    return out


def agent_wall_ms(run_dir, trace_path):
    """Agent-loop wall time from results.json, for cross-checking the trace extent."""
    if not run_dir:
        return 0
    results = os.path.join(run_dir, "results.json")
    if not os.path.isfile(results):
        return 0
    try:
        with open(results, "r", encoding="utf-8") as stream:
            document = json.load(stream)
    except (OSError, ValueError):
        return 0
    task = os.path.basename(os.path.dirname(trace_path))
    for run in document.get("runs", []):
        if run.get("taskId") == task:
            return run.get("metrics", {}).get("wallMs", 0)
    return 0


def time_percent(profile, ms):
    if profile["wallMs"] <= 0:
        return 0.0
    return 100.0 * ms / profile["wallMs"]


def print_profile(profile, label):
    print(f"== {label}: {profile['wallMs']}ms, {profile['turns']} turns, "
          f"{profile['toolCalls']} tool calls")

    print("\nWhere the wall time went (ranked):")
    rows = [
        ("model prefill", profile["modelPrefillMs"]),
        ("model generate", profile["modelGenerateMs"]),
        ("harness phases", profile["harnessMs"]),
        ("tool execution", profile["toolMs"]),
        ("context build", profile["contextBuildMs"]),
        ("unaccounted", profile["unaccountedMs"]),
    ]
    for name, ms in sorted(rows, key=lambda row: -row[1]):
        print(f"  {name:<18} {ms:>9}ms  {time_percent(profile, ms):5.1f}%")

    print("\nTurn shape: p50={}ms p95={}ms max={}ms | ttft p50={}ms p95={}ms".format(
        profile["turnP50Ms"], profile["turnP95Ms"], profile["turnMaxMs"],
        profile["ttftP50Ms"], profile["ttftP95Ms"]))
    print("  prefill ~{:.0f} tok/s, decode ~{:.1f} tok/s".format(
        profile["prefillTokPerSec"], profile["decodeTokPerSec"]))
    print("  output chars: thinking={} text={} toolJson={}".format(
        profile["thinkingChars"], profile["textChars"], profile["toolJsonChars"]))

    print("\nContext: prompt tokens first={} last={} peak={} total={} tax={:.2f}x "
          "cacheHit={:.1f}%".format(profile["firstPromptTokens"],
                                    profile["lastPromptTokens"],
                                    profile["peakPromptTokens"],
                                    profile["totalPromptTokens"],
                                    profile["contextTax"],
                                    profile["cacheHitRate"] * 100.0))
    print("  prompt cache: misses={}/{} systemPromptMutations={} | images kept={} dropped={} "
          "inLastTurn={} | toolResultsCompacted={}".format(
              profile["cacheMissTurns"], profile["turns"],
              profile["systemPromptMutations"], profile["imagesKept"],
              profile["imagesDropped"], profile["imagesInHistoryLastTurn"],
              profile["compactedToolResults"]))
    if profile["promptBytes"]:
        print("  prompt bytes by component:")
        for key, value in sorted(profile["promptBytes"].items(), key=lambda kv: -kv[1]):
            print(f"    {key:<24} {value:>9}")
    print("  largest tool result: {} bytes (turn {})".format(*profile["largestToolResult"]))

    print("\nWaste: failedToolCalls={} denied={} turnsAfterFailure={} ({}ms, {} prompt "
          "tokens) truncatedTurns={} nudges={}/{} readOnlyTurns={} ({}ms) "
          "mutatingToolCalls={}".format(
              profile["failedToolCalls"], profile["deniedToolCalls"],
              profile["turnsAfterFailure"], profile["turnsAfterFailureMs"],
              profile["turnsAfterFailurePromptTokens"], profile["truncatedTurns"],
              profile["autonomousNudges"], profile["truncationNudges"],
              profile["turnsWithoutToolCalls"], profile["readOnlyTurnsMs"],
              profile["mutatingToolCalls"]))

    if profile["tools"]:
        print("\nTools by total time:")
        ranked = sorted(profile["tools"].items(), key=lambda kv: -kv[1]["totalMs"])
        for name, entry in ranked[:10]:
            avg = entry["totalMs"] // entry["calls"] if entry["calls"] else 0
            print("  {:<28} {:>7}ms  {:>3} calls  avg {:>6}ms  max {:>7}ms  "
                  "fail {:>2}  out {:>8}B".format(name, entry["totalMs"], entry["calls"],
                                                  avg, entry["maxMs"], entry["failures"],
                                                  entry["resultBytes"]))

    if profile["phases"]:
        print("\nHarness phases by total time:")
        ranked = sorted(profile["phases"].items(), key=lambda kv: -kv[1]["totalMs"])
        for name, entry in ranked[:10]:
            print("  {:<28} {:>7}ms  {:>3} calls".format(name, entry["totalMs"],
                                                         entry["calls"]))


SCALAR_KEYS = (
    "wallMs", "turns", "toolCalls", "modelMs", "modelPrefillMs", "modelGenerateMs",
    "toolMs", "harnessMs", "contextBuildMs",
    "unaccountedMs", "turnP50Ms", "turnP95Ms", "turnMaxMs", "ttftP50Ms", "ttftP95Ms",
    "prefillTokPerSec", "decodeTokPerSec", "thinkingChars", "textChars", "toolJsonChars",
    "firstPromptTokens", "lastPromptTokens", "peakPromptTokens", "totalPromptTokens",
    "contextTax", "cacheHitRate", "cacheMissTurns", "systemPromptMutations",
    "imagesKept", "imagesDropped", "compactedToolResults", "imagesInHistoryLastTurn",
    "failedToolCalls", "turnsAfterFailure",
    "turnsAfterFailureMs", "turnsAfterFailurePromptTokens", "truncatedTurns",
    "turnsWithoutToolCalls", "readOnlyTurnsMs", "judgeMs", "mutatingToolCalls",
)


def print_diff(before, after, label_before, label_after):
    print(f"== {label_before} -> {label_after}")
    print("  {:<30} {:>14} {:>14} {:>12}".format("metric", label_before, label_after,
                                                 "delta"))
    for key in SCALAR_KEYS:
        left, right = before.get(key, 0), after.get(key, 0)
        if isinstance(left, float) or isinstance(right, float):
            print(f"  {key:<30} {left:>14.2f} {right:>14.2f} {right - left:>+12.2f}")
        else:
            print(f"  {key:<30} {left:>14} {right:>14} {right - left:>+12}")

    names = sorted(set(before["tools"]) | set(after["tools"]))
    print("\n  tool time deltas:")
    rows = []
    for name in names:
        left = before["tools"].get(name, {"totalMs": 0, "calls": 0})
        right = after["tools"].get(name, {"totalMs": 0, "calls": 0})
        rows.append((right["totalMs"] - left["totalMs"], name, left, right))
    for delta, name, left, right in sorted(rows, key=lambda row: -abs(row[0]))[:10]:
        print("  {:<28} {:>7}ms -> {:>7}ms  {:+}ms  ({} -> {} calls)".format(
            name, left["totalMs"], right["totalMs"], delta, left["calls"], right["calls"]))


def gather(directory):
    """Aggregates every task trace below a run directory."""
    merged = []
    for entry in sorted(os.listdir(directory)):
        candidate = os.path.join(directory, entry, "trace.jsonl")
        if os.path.isfile(candidate):
            merged.extend(load_records(candidate))
    return merged


def load_side(arg):
    """Loads the records for a path and returns (records, label). A run directory is merged."""
    if os.path.isdir(arg) and not os.path.isfile(os.path.join(arg, "trace.jsonl")):
        return gather(arg), os.path.basename(os.path.abspath(arg))
    path = resolve_trace(arg)
    return load_records(path), os.path.basename(path)


def main():
    parser = argparse.ArgumentParser(description="Rank design-agent trace bottlenecks")
    parser.add_argument("trace", help="trace.jsonl, a task dir, or a run dir")
    parser.add_argument("after", nargs="?", help="second trace for a before/after diff")
    parser.add_argument("--json", action="store_true", help="also dump the aggregate as JSON")
    args = parser.parse_args()

    try:
        records, label = load_side(args.trace)
    except FileNotFoundError as error:
        print(f"cannot read trace: {error}", file=sys.stderr)
        return 1
    if not records:
        print(f"no trace records found in {args.trace}", file=sys.stderr)
        return 1

    run_dir = args.trace if os.path.isdir(args.trace) else os.path.dirname(resolve_trace(args.trace))
    first = analyze(records)
    print_profile(first, label)
    measured = agent_wall_ms(run_dir, args.trace)
    if measured:
        print(f"\n  (agent loop alone measured {measured}ms in results.json; the trace also covers"
              f" seeding, export, render and judge)")

    if args.after:
        try:
            second_records, second_label = load_side(args.after)
        except FileNotFoundError as error:
            print(f"cannot read trace: {error}", file=sys.stderr)
            return 1
        if not second_records:
            print(f"no trace records found in {args.after}", file=sys.stderr)
            return 1
        print()
        print_diff(first, analyze(second_records), label, second_label)

    if args.json:
        print("\n" + json.dumps(first, indent=2, sort_keys=True, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
