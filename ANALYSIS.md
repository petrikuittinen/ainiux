# ainiux Codebase Audit

Date: 2026-07-23

Scope: tracked production code, tests, build rules, prompts, configuration, and
project documentation after the v1.05 one-shot and interactive agent work.
Generated build output and untracked user sessions/results were not treated as
source and were not removed.

## Executive summary

The core ownership model remains sound: C++ code generally uses RAII, transport
and provider behavior remain outside the UIs, and agent state is project-local.
The largest current risk is not an abandoned production module. It is that the
interactive agent still enters the chat TUI controller and inherits chat-only
state and commands. The test suite also accumulated a large serial integration
script and retained assertions from the old `.ainiux/` project-state layout.

Confirmed generated baggage was removed in this audit. Reserved empty module
directories and future plan/refactor prompts were retained intentionally.

## Confirmed stale or inert items

- Timestamped files under `results/`, the tracked `kissa.txt~` editor backup,
  and tracked Python bytecode were generated artifacts, not fixtures.
- The root `keyboardshortcuts.md` described editor Tab as disabled and omitted
  agent mode. It has been moved to `docs/keyboard-shortcuts.md` and refreshed.
- The cutoff helper was useful but misplaced at repository root. It now lives
  under `tools/`.
- Integration tests still expected `.ainiux/index.sqlite` and
  `.ainiux/logs/security-review`; production has used `.ainiux-pr/` since the
  project-state separation.
- `resources/prompts/plan_prompt.md` and `refactor_prompt.md` are not embedded,
  installed, or loaded. They are retained as dormant inputs for the planned
  agent-mode work, not presented as implemented features.
- `/cmd-out` has user-facing state and a runtime conditional, but the
  `run_command` stdout branch currently performs no action. Do not advertise it
  as complete until the agent UI slice either implements or removes it.
- `InteractiveUiTarget::Agent` still had a “reserved for future” comment after
  the target became active.

## Interactive agent findings

`src/tui/run.cpp` is a shared terminal loop, but it is still primarily a chat
controller. In agent mode it currently:

- opens `~/.ainiux/ainiux.db` and starts chat media cleanup even though agent
  transcripts belong only in `.ainiux-pr/agent.sqlite`;
- exposes chat thread/history operations such as `/new`, `/list`, `/clone`,
  `/clear`, `/edit`, `/pop`, `/response`, `/system`, `/save`, and `/load`;
- mutates only the disposable chat display for several of those commands, not
  `AgentSessionRuntime` or its provider conversation;
- runs editor/chat-assist command dispatch before the generic command handler;
- duplicates agent runtime-option mapping also found in `src/app/agent_mode.cpp`.

The next agent slice should share terminal input, drawing, selectors, themes,
and runtime jobs, but give chat and agent separate controllers and command
tables. Agent entry must not open the user chat database or media store.

## Compaction finding

Automatic agent compaction exists, but it is not a complete context-compaction
implementation. It creates a local summary in the database and then reseeds the
provider conversation with the current user text; the generated summary and
retained recent transcript are not injected into that rebuilt request context.
There is no manual `/compact`. Correct compaction should atomically produce a
bounded summary, preserve recent tool/result pairing, rebuild the provider
conversation from summary plus recent messages, and expose a manual command.

## Test-suite findings and action

- The ordinary C++ runner is broad but fast; fault injection, subprocess
  servers, and PTYs were unnecessarily bundled into `make test-unit`.
- The main mock script mixed benchmark, grade, conversion, configuration,
  provider, security-review, editor, TUI, and persistence checks serially.
- It launched two delayed OpenAI mocks, an empty-model mock, and a separate
  SQLite mock in addition to its primary server.
- Several HTTP integration matrices repeat behavior already covered by pure
  provider, config, input, benchmark, and rendering unit tests.

The development gate is now `make test`: the in-process runner followed by a
small mock smoke covering Chat, Responses, and a real native-tool `--run`
round. `make test-full` retains unit, fault, and comprehensive integration
coverage. The comprehensive OpenAI integration reuses one server, uses
request-local delay/empty-model behavior, and reuses that server for SQLite
tests.

## Retained architecture/refactor debt

- `src/tui/run.cpp` remains the highest-priority split because it combines two
  product semantics with terminal, persistence, jobs, commands, and events.
- Provider, configuration, editor, and SQLite modules remain large but active;
  line count alone is not evidence of stale code.
- Security review is active and shares agent/provider tool infrastructure; it
  is not obsolete merely because general agent mode now exists.
- `src/web/`, `src/tools/`, and `src/unicode/` placeholders match the documented
  roadmap and remain intentionally reserved.

## Recommended next agent slice

1. Extract shared terminal-shell primitives from chat state and introduce an
   agent-specific controller, command table, help, and event handling.
2. Stop all chat SQLite/media initialization when entering agent mode and make
   `.ainiux-pr/agent.sqlite` the sole transcript source.
3. Implement correct manual and automatic compaction with summary/recent
   context reconstruction and focused cancellation/failure tests.
4. Add explicit read-only plan mode and scoped refactor mode using the dormant
   prompt layers, with visible mode chrome and tool-policy changes.
5. Add a focused agent PTY integration for mode switching, Guard Ask, durable
   history, and cancellation rather than copying the chat PTY matrix.
