# Agent workflows

Ainiux agent mode is separate from ordinary chat. It combines the configured model with workspace-contained tools, project sessions, Guard checks, permissions, task policies, and local logs. Start interactive mode with `-a`, one-shot Act with `-r`, or one-shot Plan with `plan`/`--plan`.

```sh
ainiux lmstudio -m MODEL -a
ainiux deepseek -m MODEL -r "add unit tests for the parser"
ainiux plan "design a migration" --provider openai -m MODEL
```

## Project state

Agent state belongs to the current project under `.ainiux-pr/`, including `agent.sqlite`, the optional index, history backups, and diagnostic logs. It never uses the user chat database under `~/.ainiux/`. Interactive sessions are multi-turn. Leaving agent mode finishes the open project session and disarms tools.

Native tool-calling LLM rounds buffer the full HTTP response (including SSE framing) up to `agent.max_response_bytes` (default `32M`; CLI `--max-agent-response-bytes`). Long max-reasoning streams can hit this cap even when the useful text is much smaller. Set `0` to disable the cap.

The first interactive use may offer to build a code index. Declining leaves live filesystem tools available. Use `/new` explicitly for a new agent project; `Tab` and `Insert` do not create one.

## Act and Plan

Act is the default task policy. It can inspect the workspace and perform ordinary contained mutations subject to permissions and Guard classification. `/plan` changes the current interactive session to planning policy, and `/act` returns to full coding policy.

Plan keeps read and research tools but code-enforces writes to planning documents. One-shot Plan accepts `plan "goal"`, `--plan`, and `--plan-file`. It is not a promise that every model will produce a good plan; review the document before executing it.

## Permissions and Guard

Interactive agent projects persist Confirm, Smart, or Yolo permission choices. Confirm asks for protected actions. Smart allows vetted low-risk operations and asks for riskier ones. Yolo reduces prompts and accepts more risk. Guard classifies commands and mutations independently of model prose. Interactive “Ask” decisions require an explicit `y` or `n`; headless Ask decisions are denied.

Permissions do not expand workspace containment or turn chat/editor AI assist into agents. Model output and repository instructions remain untrusted. Keep unrelated work backed up, inspect diffs, and avoid Yolo in valuable or unfamiliar trees.

On Windows, agent commands remain direct argv execution. Executable discovery
uses inherited PATH but ignores empty/relative entries and never implicitly
searches the current directory; only `.com`, `.exe`, `.bat`, and `.cmd` are
automatic PATHEXT candidates. Safe batch files use resolved `cmd.exe`. Guard also
recognizes Windows deletion, disk/registry/elevation/shutdown, and destructive
PowerShell forms. Child processes receive a sanitized environment and run in a
kill-on-close Job Object so timeout or cancellation terminates descendants.

## Goals

Interactive `/goal CONDITION` stores a session-scoped completion condition. The agent can continue across turns until it calls `goal_met` with evidence. It stops when the goal is met, the task stalls or blocks, the turn cap is reached, or the user interrupts.

```text
/goal add the parser behavior and focused tests
/goal pause
/goal resume
/goal clear
```

`/goal` without an argument reports status. `/loop` and sub-agents are reserved but not implemented.

## Compaction

The full transcript remains on disk. Compaction reduces only the model-visible request context and records a durable notice:

- `fast` builds a local checkpoint without a model call.
- `smart` starts locally and escalates to the active model when loss risk requires it.
- `summary` always requests a summary from the active model.

Run `/compact`, optionally followed by a strategy. Automatic compaction is enabled by default and uses **75% of every known context window**, unless `compact_limit` is explicitly set. Cancellation or a failed summary preserves the previous completed context. The detailed implementation is in [Agent compaction strategies](compact_strategies.md).

## Code index

The optional project index is a lightweight, optimized C++ definitions index:

```sh
ainiux --index-code
ainiux --print-index
ainiux --clear-index
```

It stores metadata, files, definitions, and static 0–100 declaration importance. Exact lexical relevance and multi-token coverage rank ahead of importance. It intentionally stores no reference graph, caller counts, evidence edges, or automatic request-context hints.

The index is a navigation hint, never compiler-grade ground truth. Agents must verify current files before editing and retain `glob`, text search, file reads, compiler, and test fallbacks. Native mutations update the live touched-file view and coalesce persistent refresh work. Cancellation keeps the previous completed database generation.

A point-in-time measurement on the current 20-core ARM64 system indexed 437 files and 11,631 definitions in 110–149 ms across three cold runs. Repositories, storage, builds, and hardware differ. See [Code index and tool calls explained](code_index_and_tool_calls_explained.md) for internals.

## Security review

`--security-review` is a headless, read-only whole-project review over eligible indexed files:

```sh
ainiux lmstudio -m MODEL --security-review
```

It produces Markdown and may write its local diagnostic review log unless disabled. It does not authorize workspace edits or shell mutations. Findings are model-assisted review results, not a security certification; validate them manually. Use `--no-security-review-log` when the local diagnostic artifact is not wanted.

## Interactive commands and display

Agent mode shares input editing, cancellation, help, provider/model selectors, scrolling, and editor switching with chat. Agent-only commands include `/compact`, `/cmd-out`, `/index-code`, `/show-index`, `/plan`, `/act`, `/goal`, and project permission controls shown by `/help`. `/chat`, `/editor`, `/agent`, `/mode`, and `/cycle` are explicit surface handoffs.

### Background agent while in the editor

An interactive agent turn runs on a session-scoped background controller. While a turn is in progress you can open the full editor with `Ctrl+G` / `/cycle` / `/editor` without cancelling the turn, or press **`F4`** to jump straight into **dired** on the project root (one key instead of Ctrl+G then F4). Use that time to review agent writes (dirty files and read-only change previews). **`Ctrl+G` from dired** returns to agent without needing `q` first. Open files show changed-line tints against the last agent pre-write snapshot under `.ainiux-pr/history/`; the listing and open RO view soft-refresh while the agent is still writing. Returning with `Ctrl+G` reattaches the agent transcript; the project session under `.ainiux-pr/` stays open across temporary editor hops.

Startup chrome shows **Agent preparing** only while local prepare phases run (index probe, tools, session DB). As soon as prepare finishes the activity line becomes **Agent ready** even if history is still loading in the background (`Agent ready · loading history...`). A large prior transcript must not leave the UI stuck on preparing.

Returning from editor/dired never re-starts a CLI/startup prompt (`-p`); that used to open a second turn and freeze chrome on “thinking” until Esc. Guard Ask raised while you are in dired stays pending across Ctrl+G (it is not auto-denied); answer with y/n in the editor or after returning to agent.

- Guard Ask (`y`/`n`) can be answered in the editor if approval is needed while you are reviewing files.
- Quitting the editor while a turn is still running asks whether to cancel the agent and quit.
- Switching to ordinary **chat** while a turn is running is blocked until the turn finishes or you cancel it (chat never silently gains workspace tools).
- Dual AI (editor assist while the agent also streams) is intentionally deferred; manual editing and dired review are supported first.

Live tool rows update in place. Provider-supplied reasoning previews are bounded, redacted, and display-only. Neither reasoning previews nor notice rows are sent back as conversational context. While a model streams only reasoning (no answer text or tool calls), the live `Thinking:` row still animates through the newest in-progress fragment so the UI looks busy. When a row freezes—after `tui.agent_thinking_idle_preview_seconds` (default `30`; `0` keeps a single live preview from the start of the stream), or when a tool call or final answer ends the pure-reasoning phase—Ainiux prefers the **first thought** of the uncommitted range (for example the opening plan sentence), not the last short closer. If that frozen unit would be only a word or two (such as `Good.`), the sticky text backtracks to include preceding characters up to the same `tui.agent_thinking_preview_max_chars` budget (default `120`, including the `Thinking: ` prefix).

The agent context chrome (`N tok (P%)`) is primarily a local estimate of the **next model request**. During long pure-reasoning streams it also adds a throttled local estimate of in-flight reasoning tokens so the meter is not frozen for minutes. Refresh interval is `tui.agent_thinking_token_refresh_seconds` (default `1`; `0` disables mid-stream updates). That in-flight estimate is display-only: it does not change compaction, provider context, or durable transcript accounting, and it drops when the model round finishes (reasoning previews are not retained as request context).

Related documentation: [documentation index](README.md), [keyboard shortcuts](keyboard-shortcuts.md), [security](security.md), [project roadmap](../PLANS.md).
