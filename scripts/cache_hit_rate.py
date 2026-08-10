#!/usr/bin/env python3
"""Compute Ainiux agent prompt-cache hit rate from project review logs.

Reads completed agent JSONL under <project>/.ainiux-pr/logs/agent/ and
summarizes provider prompt-cache accounting (cache_read_tokens vs input_tokens).

Usage:
  scripts/cache_hit_rate.py ~/text-game
  scripts/cache_hit_rate.py /path/to/project --verbose
  scripts/cache_hit_rate.py text-game   # relative to $HOME when path is not absolute
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


def resolve_project_root(arg: str) -> Path:
    """Resolve project path: absolute as-is; else try CWD then $HOME."""
    path = Path(arg).expanduser()
    if path.is_absolute():
        return path.resolve()
    cwd_candidate = (Path.cwd() / path).resolve()
    if cwd_candidate.exists():
        return cwd_candidate
    home_candidate = (Path.home() / path).resolve()
    if home_candidate.exists():
        return home_candidate
    # Prefer CWD even if missing so the error message is predictable.
    return cwd_candidate


def agent_log_dir(project: Path) -> Path:
    return project / ".ainiux-pr" / "logs" / "agent"


def load_events(log_dir: Path) -> List[Tuple[Path, Dict[str, Any]]]:
    events: List[Tuple[Path, Dict[str, Any]]] = []
    if not log_dir.is_dir():
        return events
    paths = sorted(log_dir.glob("*.jsonl")) + sorted(log_dir.glob("*.jsonl.partial"))
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"warning: {path}:{line_no}: {exc}", file=sys.stderr)
                continue
            if isinstance(obj, dict):
                events.append((path, obj))
    return events


def nonneg(value: Any) -> Optional[int]:
    if value is None:
        return None
    try:
        n = int(value)
    except (TypeError, ValueError):
        return None
    if n < 0:
        return None
    return n


def hit_rate(cache_read: int, input_tokens: int) -> Optional[float]:
    if input_tokens <= 0:
        return None
    return 100.0 * cache_read / input_tokens


def fmt_rate(rate: Optional[float]) -> str:
    if rate is None:
        return "n/a"
    return f"{rate:.1f}%"


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compute Ainiux agent prompt-cache hit rate from .ainiux-pr logs."
    )
    parser.add_argument(
        "project",
        help="Ainiux project directory (absolute, relative to CWD, or under $HOME)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="Print per-round and per-run detail",
    )
    parser.add_argument(
        "--include-partial",
        action="store_true",
        help="Also scan *.jsonl.partial (usually prepare-only; no cache metrics)",
    )
    args = parser.parse_args(list(argv) if argv is not None else None)

    project = resolve_project_root(args.project)
    log_dir = agent_log_dir(project)

    if not project.is_dir():
        print(f"error: project directory not found: {project}", file=sys.stderr)
        return 2
    if not log_dir.is_dir():
        print(
            f"error: no agent logs at {log_dir}\n"
            f"(run ainiux agent/run in this project first)",
            file=sys.stderr,
        )
        return 2

    # Load completed JSONL always; partial only on request for event inventory.
    paths = sorted(log_dir.glob("*.jsonl"))
    if args.include_partial:
        paths += sorted(log_dir.glob("*.jsonl.partial"))
    if not paths:
        print(f"error: no agent JSONL files in {log_dir}", file=sys.stderr)
        return 2

    rounds: List[Dict[str, Any]] = []
    turns: List[Dict[str, Any]] = []
    endpoints = set()

    for path in paths:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
            continue
        for line_no, line in enumerate(text.splitlines(), 1):
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                print(f"warning: {path}:{line_no}: {exc}", file=sys.stderr)
                continue
            if not isinstance(obj, dict):
                continue
            et = obj.get("event_type")
            if et == "llm_request" and obj.get("endpoint"):
                endpoints.add(str(obj["endpoint"]))
            if et == "agent_round":
                rounds.append(obj)
            elif et == "agent_turn_usage":
                turns.append(obj)

    if not rounds and not turns:
        print(
            f"No agent_round / agent_turn_usage events with usage in {log_dir}\n"
            f"({len(paths)} log file(s) scanned).",
            file=sys.stderr,
        )
        return 1

    # Prefer agent_round for detail; turn totals for cross-check.
    total_input = 0
    total_fresh = 0
    total_cache = 0
    total_output = 0
    by_run: Dict[str, List[Dict[str, Any]]] = defaultdict(list)

    for r in rounds:
        rid = str(r.get("run_id") or "unknown")
        by_run[rid].append(r)
        inp = nonneg(r.get("input_tokens")) or 0
        cr = nonneg(r.get("cache_read_tokens"))
        fr = nonneg(r.get("fresh_input_tokens"))
        out = nonneg(r.get("output_tokens")) or 0
        total_input += inp
        total_output += out
        if cr is not None:
            total_cache += cr
            if fr is None and inp >= cr:
                fr = inp - cr
        if fr is not None:
            total_fresh += fr

    print(f"project:     {project}")
    print(f"logs:        {log_dir}")
    if endpoints:
        print(f"endpoint(s): {', '.join(sorted(endpoints))}")
    print(f"runs:        {len(by_run)}")
    print(f"rounds:      {len(rounds)}")
    print(f"input:       {total_input}")
    print(f"fresh:       {total_fresh}")
    print(f"cache_read:  {total_cache}")
    print(f"output:      {total_output}")
    overall = hit_rate(total_cache, total_input)
    print(f"hit_rate:    {fmt_rate(overall)}  (token-weighted cache_read/input)")

    # Round-1 vs later warm-up summary
    first_in = first_cr = later_in = later_cr = 0
    first_n = later_n = 0
    for items in by_run.values():
        for r in items:
            inp = nonneg(r.get("input_tokens")) or 0
            cr = nonneg(r.get("cache_read_tokens")) or 0
            if r.get("round") == 1:
                first_in += inp
                first_cr += cr
                first_n += 1
            else:
                later_in += inp
                later_cr += cr
                later_n += 1
    if first_n or later_n:
        print(
            f"warm-up:     round1 n={first_n} hit={fmt_rate(hit_rate(first_cr, first_in))}; "
            f"later n={later_n} hit={fmt_rate(hit_rate(later_cr, later_in))}"
        )

    if turns:
        tin = tcr = 0
        for t in turns:
            tin += nonneg(t.get("input_tokens")) or 0
            tcr += nonneg(t.get("cache_read_tokens")) or 0
        print(
            f"turn_check:  {len(turns)} agent_turn_usage event(s), "
            f"hit={fmt_rate(hit_rate(tcr, tin))} (input={tin}, cache_read={tcr})"
        )

    if args.verbose:
        print()
        for rid in sorted(by_run.keys()):
            items = sorted(by_run[rid], key=lambda x: (x.get("round") or 0, x.get("sequence") or 0))
            rin = rfr = rcr = rout = 0
            print(f"=== {rid} ===")
            for r in items:
                inp = nonneg(r.get("input_tokens")) or 0
                cr = nonneg(r.get("cache_read_tokens"))
                fr = nonneg(r.get("fresh_input_tokens"))
                out = nonneg(r.get("output_tokens")) or 0
                if cr is None:
                    cr_disp = -1
                    rate = None
                    cr_sum = 0
                else:
                    cr_disp = cr
                    rate = hit_rate(cr, inp)
                    cr_sum = cr
                if fr is None and cr is not None and inp >= cr:
                    fr = inp - cr
                fr_sum = fr if fr is not None else 0
                rin += inp
                rfr += fr_sum
                rcr += cr_sum
                rout += out
                print(
                    f"  r{r.get('round', '?'):>2}  input={inp:>7}  fresh={fr if fr is not None else -1:>7}  "
                    f"cache_read={cr_disp:>8}  hit={fmt_rate(rate):>6}  "
                    f"out={out:>5}  outcome={r.get('outcome', '')}"
                )
            print(
                f"  total input={rin} fresh={rfr} cache_read={rcr} output={rout} "
                f"hit={fmt_rate(hit_rate(rcr, rin))}"
            )
            print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
