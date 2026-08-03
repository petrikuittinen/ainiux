# Version history

This compact timeline moves release material out of the landing page. It describes the main result of each release family, not every implementation detail. Current behavior is documented in the [user guides](README.md); superseded interfaces are identified where important. Git history and [Decisions](decisions.md) preserve deeper rationale.

| Version | Main result |
| --- | --- |
| v0.0 | Repository, C++17/Makefile build, tests, sanitizer, and leak-check foundation |
| v0.1 | Script-friendly OpenAI-compatible CLI, streaming, and LM Studio profile; an early browser-mode note was planning only and is superseded by the postponed [web-mode snapshot](web-mode.md) |
| v0.2 | Line-oriented REPL and JSON chat persistence foundations |
| v0.3 | Cancellable runtime jobs and the first full-screen terminal chat |
| v0.4 | Data-driven provider registry, compatibility profiles, model listing, and text Responses API |
| v0.5 | Request context policies, text/image inputs, HTML conversion, and explicit safe URL fetching |
| v0.6 | Layered TOML-like system/user configuration and strict schema validation |
| v0.7 | Concurrent JSONL benchmark runner, reports, and cancellable cases |
| v0.8 | Standalone piece-table editor with optional streamed AI assist |
| v0.81–v0.82 | Editor, TUI, persistence, and provider reliability polish |
| v0.83 | Modular unit/fault test organization and expanded failure-path coverage |
| v0.84 | Large application, editor, TUI, and benchmark modules split into focused files |
| v0.85 | Per-thread generation settings, system prompt editing, cloning, and persisted settings |
| v0.86 | Styled terminal panels, compact activity chrome, and embedded editor help |
| v0.87 | Shared editing bindings, select-all, chat history editing, and improved navigation |
| v0.88 | Tavily, Firecrawl, Exa, Searxng, and keyless DuckDuckGo web search |
| v0.89 | Provider-specific reasoning request mapping and multiple editor buffers |
| v0.90 | Unified terminal bindings and roadmap cleanup; this version number did not ship the separately planned local server |
| v0.91–v0.93 | Cutoff benchmark, terminal interaction, and compatibility polish |
| v0.94 | Explicit chat↔editor switching with state retained |
| v0.95 | Shared multi-language syntax highlighting and Markdown terminal attributes |
| v0.96–v0.97 | Provider, editor, chat, conversion, and test hardening |
| v0.98 | Unified `--reasoning`, layered `models.conf`, model capability metadata, and purpose presets; this superseded earlier split reasoning controls |
| v0.99 | Headless read-only whole-project security review |
| v1.00 | Native agent tool and project-index foundation |
| v1.01 | One-shot local Act agent (`run`, `--run`, `-r`) |
| v1.02 | Ordinary workspace mutations with containment and approval policy |
| v1.03 | Project-local `.ainiux-pr/` sessions, history, logs, and separation from user chat data |
| v1.04 | Interactive agent TUI, multi-turn tools, live activity, and chat/agent transcript isolation |
| v1.05 | Permanent agent chrome and command Guard classification |
| v1.06 | Interactive Guard Ask approvals, clipboard bridge, and responsive widgets |
| v1.07 | Session-scoped Act/Plan modes and one-shot Plan with planning-document-only writes |
| v1.08 | Bounded provider reasoning previews and in-place live tool completion rows |
| v1.09 | Stable prompt caching/accounting, vetted Smart read-only commands, and context polish |
| v1.10 | Definitions-only static importance, optional index startup, mutation-aware refresh, and one-shot tool metrics; this superseded graph-index experimentation |
| v1.11 | Transcript-preserving `fast`, `smart`, and `summary` agent compaction with visible progress |
| v1.12 | Retained row-diff terminal rendering and punctuation-aware Markdown highlighting |
| v1.13 | First-run index offer, index chrome marker, timed index summary, and index progress lifetime fix |
| v1.14 | Offline editor text layout and line cleanup, width/align history display modes, and width-capped pretty tables |
| v1.15 | Documentation overhaul with a concise landing page, focused user guides, provider and credential references, corrected shortcuts, and explicit compatibility limits |
| v1.16 | Full-screen editor dired (directory browser): CLI `--dired`, F4 / Ctrl+X d, list↔read-only view, file ops, content-hash dirty markers, POSIX mode/owner/group |

## Current status after v1.16

The current product includes CLI chat, conversion, REPL, saved-thread TUI chat, standalone editor with dired, benchmarks, judge grading, security review, one-shot and interactive agents, session Act/Plan policy, Guard approvals, `/goal`, transcript-preserving compaction, and a lightweight definitions index.

The local OpenAI-compatible server, browser UI, image generation, PDF/DOCX conversion, `/loop`, sub-agents, and native Anthropic Messages adapter are not implemented. See [PLANS.md](../PLANS.md) and [TODO.md](../TODO.md) instead of inferring features from old version labels.

Related documentation: [documentation index](README.md), [project README](../README.md), [decisions](decisions.md).
