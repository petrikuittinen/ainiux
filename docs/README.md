# Ainiux documentation

This index separates current usage guides from design records, roadmaps, and point-in-time audits. Start with [Getting started](getting-started.md) or return to the [project README](../README.md).

## User guides

| Guide | Covers |
| --- | --- |
| [Getting started](getting-started.md) | Dependencies, installation, providers, first commands, and platform expectations |
| [Native Windows](windows.md) | UCRT64 build/package, Win32 terminal, PowerShell, clipboard, paths, and parity gate |
| [CLI and scripting](cli.md) | One-shot chat, REPL, conversion, image generation, attachments, fetch/search, output, and context |
| [CLI skill for other agents](skills/ainiux-cli/SKILL.md) | How a foreign agent should invoke `ainiux` from bash (one-shot only; no TUI) |
| [Chat TUI](chat.md) | Threads, persistence, commands, attachments, and mode switching |
| [Editor help](editor_help.md) | Complete editor operation and embedded help content |
| [Dired mode](dired-mode.md) | Full-screen directory browser: keys, listing, dirty markers, CLI `-d` / `--dired` |
| [Keyboard shortcuts](keyboard-shortcuts.md) | Current chat, editor, agent, and dired bindings |
| [Agent workflows](agent.md) | Act/Plan, permissions, Guard, goals, compaction, indexing, and security review |
| [MCP servers](mcp.md) | Install/list MCP servers; agent/run/plan tools (`mcp__server__tool`); mock and CLI |
| [Configuration](configuration.md) | Layering, credentials, themes, models, image catalog, benchmark prompts, and editor commands |
| [Benchmarks and grading](benchmarks.md) | Built-in corpus, JSONL runs, judge grading, and limitations |
| [API compatibility](api-compatibility.md) | Provider and protocol compatibility details |
| [Control API](api.md) | Start the loopback v1.3 server; use jobs, interactive sessions, and MCP; review authentication and limits |
| [Security](security.md) | Credential, persistence, fetch, attachment, image generation, and agent boundaries |
| [Testing](../TESTING.md) | Test targets and selection policy |
| [Version history](version-history.md) | Compact v0.0–v1.19 release timeline plus unreleased work |

## Architecture and implementation references

- [Decisions](decisions.md) records design rationale.
- [Code index and tool calls explained](code_index_and_tool_calls_explained.md) describes the current definitions-only index and agent navigation behavior.
- [Agent tool inventory](tool_calls.md) lists native tools advertised in agent mode, verbatim descriptions, and per-tool token estimates.
- [Agent compaction strategies](compact_strategies.md) documents the current `fast`, `smart`, and `summary` implementation in detail.
- [Unicode license](unicode-license.txt) covers generated Unicode data.
- [Project roadmap](../PLANS.md), [open work](../TODO.md), and [agent constraints](../AGENTS.md) govern future work.
- [Postponed web mode](web-mode.md) is a historical planning note, not a current usage guide.

## Point-in-time snapshots

These documents preserve audit context. Their dates and code references matter; they do not replace current usage or security guidance.

- [Security hardening audit](quick_security_todo.md)
- [Testing coverage analysis](testing_coverage.md)
- [Prompt-cache hit rate test runs](cache_hit_rate_test_runs.md)
- [Qwen and DeepSeek index-prompt benchmark report](index_prompt_benchmark_report.md)

## Other project material

- [Marketing copy](ainiux_marketing.txt) contains restrained factual descriptions for project announcements.
- [License](../LICENSE) contains the Modified MIT terms and no-warranty language.

Every Markdown document under `docs/` is linked from this index. Topical guides also link back here so readers can distinguish current instructions from historical or internal material.
