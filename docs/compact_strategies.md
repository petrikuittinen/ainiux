# Agent compaction strategies (`fast` / `smart` / `summary`)

This is the detailed implementation reference. For task-oriented usage, start with [Agent workflows](agent.md#compaction).

Implementation-grounded reference for rethinking high-performance context
compaction. Numbers and formulas match the code as of this document; if
behavior diverges, trust the sources below.

**Primary sources**

| Area | Location |
| --- | --- |
| Thresholds, partition, fast/smart/summary pure logic | `src/agent/compact.cpp`, `src/agent/compact.hpp` |
| Orchestration, auto-compact, model calls, commit | `src/agent/session_runtime.cpp` (`compact_impl`) |
| Durable append of summary row | `src/agent/session_store.cpp` (`compact_with_summary`) |
| Strategy enum | `include/ainiux/compaction_strategy.hpp` |
| Config defaults | `config/ainiux.conf` (`[agent]`), `src/cli/args.hpp` |
| Unit coverage of numbers | `tests/unit/agent/test_project_root.cpp` |

**Out of scope here:** ordinary chat CLI `--max-context-bytes` /
`truncate-*` / `summarize-*` policies in `src/context/` (separate product
surface). This document is **agent project sessions** only
(`.ainiux-pr/agent.sqlite`).

---

## 1. Design invariants

1. **Transcript on disk is never deleted.** Compaction appends a
   `summary` message and rebuilds the **model-visible** conversation.
   Original rows and full tool-event payloads remain in SQLite.
2. **`keep_recent` is unused.** `AgentSessionStore::compact_with_summary`
   takes `keep_recent` for API history but currently does
   `(void)keep_recent` — no row pruning.
3. **Token estimates are not provider tokenizers.** Everywhere below,
   “tokens” means the local heuristic:
   ```
   estimate_tokens_from_text(s) = (utf8_byte_length(s) + 3) / 4
   ```
   Empty string → 0. Logical items also add a small fixed overhead (+4)
   for bookkeeping when summing role/content/tool fields.
4. **Display-only roles never enter compaction timelines:** `system`,
   `notice`, `thinking`, `index` are excluded (`model_projection_role`).

---

## 2. Configuration

| Setting | Config key | CLI / options field | Default |
| --- | --- | --- | --- |
| Enable auto-compact | `agent.auto_compact` | `agent_auto_compact` | `true` |
| Default strategy | `agent.compact_strategy` | `agent_compact_strategy` | `smart` |
| Auto threshold % | `agent.compact_limit` | `agent_compact_limit` | `0` → effective **75** |

```toml
# config/ainiux.conf [agent]
auto_compact = on
compact_strategy = smart
# compact_limit = 75
```

### Effective limit percent

```cpp
// effective_compact_limit_percent
if (configured_limit >= 1 && configured_limit <= 100)
    return configured_limit;
return 75;   // always, for any context window size
```

Unit tests assert 75 for windows 8k, 64k, and 128k when the configured limit is 0.

### Strategy names

| Name | Enum | Parsing |
| --- | --- | --- |
| `fast` | `CompactionStrategy::Fast` | case-insensitive trim |
| `smart` | `CompactionStrategy::Smart` | |
| `summary` | `CompactionStrategy::Summary` | |

Bare `/compact` and automatic compaction use `compact_strategy`.
`/compact fast|smart|summary` overrides **one** invocation only.

---

## 3. When compaction runs

### 3.1 Manual

Interactive agent: `/compact` or `/compact fast|smart|summary`.

### 3.2 Automatic (proactive)

Before a user turn proceeds (`AgentSessionRuntime::run_user_turn` path),
if `auto_compact` and the project DB is open:

```
should_auto_compact(
  auto_compact_enabled,
  compact_limit_percent,
  context_window_tokens,      // context.options.context_tokens
  estimated_request_tokens)   // cached sum over conversation_.messages + continuation JSON
```

```
threshold = (context_window_tokens * limit_percent + 99) / 100   // ceil-like integer
fires when estimated_request_tokens >= threshold
```

Requires `context_window_tokens > 0` and `estimated_request_tokens > 0`.
If the window is unknown (`0`), auto-compact **does not fire** on the
proactive path (threshold path is gated).

**Failure cooldown:** after a failed automatic attempt, further automatic
attempts are no-ops for **60_000 ms** while `newest_seq` has not advanced
(no new stored messages).

### 3.3 Automatic (context-length recovery)

If a model round fails with a context-length style error **and**
`auto_compact` is on **and** configured strategy is **not** `Fast`, the
runtime runs one recovery compact with **`forced_summary = true`**
(always uses the model summarizer), then retries the round once.

Context-error detection (message heuristics): HTTP 413, substrings
`context length`, `context_length`, `maximum context`, `too many tokens`,
`request is too large` (case-insensitive).

---

## 4. Shared pipeline (`compact_impl`)

```
load messages + tool_events from SQLite
    ↓
build_compaction_timeline          (tiered tool reduction for all tools)
    ↓
partition_compaction_timeline(window)
    ↓  (no-op if middle.empty())
pre_shrink_compaction_middle       (merge reads/explore, read-then-edit, hash dedupe)
harvest_compaction_keep_list
    ↓
build_fast_compaction_candidate(... + keep_list)
    ↓
choose applied strategy (fast / smart escalate / summary)
    ↓
optional: move or bound oversized protected head/tail
    ↓
if model path: chunked summarize (≤1 consolidate) → checkpoint
              on timeout/error/empty/fit fail → fast checkpoint
else: use fast.checkpoint
    ↓
dry-run rebuild_compacted_conversation → must reduce tokens
    ↓  (summary: must also fit min(60% window, trigger); else try fast)
session_store.compact_with_summary(checkpoint + optional first_head_carry)
    ↓
rebuild live conversation permanently
```

### 4.1 Timeline construction (tiered tool reduction)

For each model-projected message and each tool event (sorted by `seq`):

- **Duplicate tool rows:** if a full `tool_events` row exists for a tool
  name, matching compact display rows of role `tool` with the same name
  are skipped (newest-first pairing).
- **Every tool event is reduced** via `tool_compaction_tier` /
  `reduce_tool_item_content` before it enters the timeline (including
  items that later land in the protected tail). This is the main
  latency win for tool-heavy sessions: the summarizer never sees raw
  reloadable bodies.

| Tier | Tools | Reduced form |
| --- | --- | --- |
| **Prune** | `list_dir` (+ legacy `list_directory`), `glob`, `index_overview` (+ legacy `project_overview`); also legacy removed `index_*` names | One line: `tool(args) -> ok\|fail` |
| **Stub** | `read_file`, `read_many`, `read_symbol`, `file_outline` (+ legacy `get_skeleton`), `search_symbol`, `grep` (+ legacy `search_text`/`find`), `fetch_url`, `web_search` (+ legacy `search_web`); also legacy removed macro tools | Args/target + status; bodies omitted (“reloadable”); searches keep hit paths + line numbers when parseable; failures keep ≤ **400** error bytes |
| **Digest** | `edit_file`, `write_file`, `str_replace`, `apply_patch`, `rename_path`, `remove`, `create_directory` | Path(s) + op + status; drop content/diff bodies |
| **Semantic** | `git_status`, `git_diff`, `run_command` | Exit status + failure-oriented lines (or a short pass head); git-like irreversible actions annotated |
| **Full / size** | other tools | Keep full text if result ≤ **1024** bytes; else args + 200 head/tail excerpt + “re-run to reload” |

Orphan display tool rows with content **> 512** bytes are also reduced
with the same tier table.

### 4.2 Partition: head / middle / tail

Constants:

| Constant | Value | Symbol |
| --- | --- | --- |
| Tail budget fraction | **8%** of context window | `kTailBudgetPercent` |
| Minimum tail items | **3** | `kMinimumTailItems` |
| Maximum tail items | **20** | `kMaximumTailItems` |
| First-compact protected head | **3** non-summary items | hard-coded loop |
| Fallback window (if window ≤ 0) | `max(source_tokens, 8000)` | for tail budget only |
| Substantive item threshold | **2000** estimated tokens | `kSubstantiveItemTokens` |

**Tail budget tokens:**

```
base = (context_window_tokens > 0) ? context_window_tokens
                                   : max(source_tokens, 8000)
tail_budget_tokens = max(1, base * 8 / 100)
```

Examples (from unit tests): window `200` → tail budget `16`; window
`100_000` → tail budget `8_000`.

**Tail selection (from newest, walking older):**

- Stop at a prior `summary` role item.
- Always take whole items until **3** items (even if over budget).
- After 3 items, stop before adding an item that would exceed
  `tail_budget_tokens`, or at **20** items.
- Remaining items between head and tail → **middle** (the compressible
  region).

**Head / prior summary:**

- If any timeline item has `role == "summary"`, use the **newest** such
  item’s content as `prior_summary`, and start the compactable range
  **after** it. Head is empty on repeated compaction.
- Else (first compact): head = first **3** non-summary items; middle
  starts after them.

If **middle is empty**, compaction is a **no-op** (notice explains that
nothing can be removed without dropping protected head/tail).

### 4.3 Fast checkpoint construction (local)

Always built, even when summary is chosen (used for smart escalation
decisions and as the non-model result).

**Max checkpoint size (tokens → bytes):**

```
// session_runtime when calling build_fast_compaction_candidate:
max_checkpoint_tokens = (window > 0) ? max(512, window * 10 / 100)
                                     : 1000

// compact.cpp:
max_bytes = max(256, max_checkpoint_tokens > 0 ? max_checkpoint_tokens * 4
                                               : 4096)
```

So for a known window, the checkpoint text is bounded by roughly
**10% of the context window** in estimated tokens (at least 512 tokens →
2048 bytes floor before the 256-byte floor on the byte path).

**Content assembled (deterministic, no model):**

| Section | Rule |
| --- | --- |
| Prior summary | If present, extract up to `min(max_bytes/3, 6000)` bytes |
| Goal | Up to **4** middle `user` lines, each single-line extract **480** bytes |
| Active State | Counts of middle items, tools, failed tools |
| Relevant Files/Evidence | Up to **24** paths (primary_path + path-like substrings), each up to **160** chars |
| Decisions / Completed / Blockers / Remaining | Up to **24** keep-list lines (mutations, failures, user decisions, git actions); else a single verify boilerplate line |
| Whole checkpoint | Truncated to `max_bytes` if needed |

**Middle pre-shrink** (after partition, before fast/summary):

1. Consecutive `read_file` stubs → one synthetic `read_many` (≤100 paths).
2. Consecutive explore tools (`grep` / `list_dir` / `glob` /
   `index_overview`, plus legacy names) → one `explored: …` line.
3. `read_file(path)` immediately followed by a Digest mutation on the same
   path → drop the read (edit digest already names the file).
4. Hash dedupe of identical stub re-reads (keep newest, annotate `×N`).
5. Empty/short assistant tool-only turns collapse to one line.

**Loss accounting (for smart):**

- Truncated user extracts: add truncated-away estimated tokens to
  `omitted_substantive_tokens`.
- Each middle **tool**: add `max(0, estimated_tokens - 12)` to omitted.
- Other middle roles: add full `estimated_tokens` to omitted.
- If any middle item has `estimated_tokens >= 2000`, set
  `omitted_item_at_least_2k = true`.
- Any byte truncation sets `protected_content_truncated = true`.

**Candidate size for fit checks:**

```
estimated_tokens = tokens(checkpoint)
                 + sum(head.estimated_tokens)
                 + sum(tail.estimated_tokens)
```

### 4.4 Oversized protected items (head/tail)

An item is “oversized protected” if
`estimated_tokens > tail_budget_tokens`.

| Path | Behavior |
| --- | --- |
| **Fast** (and smart that stays local) | In-place UTF-8 bound to `tail_budget_tokens * 4` bytes (min 64), append `" ..."`; flags protected truncation |
| **Summary** (and smart escalate / forced summary) | Move oversized head/tail items into **middle** so the model can summarize them; re-sort middle by `seq` |

### 4.5 Strategy decision

```
use_model = (requested == Summary) || forced_summary
if requested == Smart && !use_model:
    use_model = smart_compaction_should_escalate(...)
    if use_model: applied = Summary
if use_model: applied = Summary; run model path
else: applied = Fast (or Smart that did not escalate); use local checkpoint
```

#### Smart escalation rules (`smart_compaction_should_escalate`)

Any true → escalate to summary:

| # | Condition | Reason string (approx.) |
| --- | --- | --- |
| 1 | `candidate.estimated_tokens > size_limit` | “fast candidate exceeds the smart size ceiling” |
| 2 | `protected_content_truncated` | “fast candidate truncates protected content” |
| 3 | `omitted_item_at_least_2k` | “fast candidate omits a substantive item” |
| 4 | `omitted_substantive_tokens >= tail_budget_tokens` (when tail budget > 0) | “fast candidate omits at least the tail budget” |

**Size limit:**

```
if context_window > 0:
  size_limit = min(window * 60 / 100,
                   compact_trigger > 0 ? compact_trigger : INT_MAX)
else:
  size_limit = compact_trigger   // may be 0
```

Where `compact_trigger` is the same auto-compact threshold used for
proactive compaction:

```
trigger = (window > 0)
        ? (window * effective_limit_percent + 99) / 100
        : 0
```

So with default 75% and a known window, the smart ceiling is
**min(60% of window, 75% of window) = 60% of window**.

### 4.6 Summary (model) path

1. **Source text** = `render_compaction_source(partition)` over the
   **already pre-shrunk** middle (tiered stubs + merges), then redacted.
2. **System prompt** = `compaction_summary_schema_prompt(first_user_preamble)`:
   required headings; documents that history is already reduced; forbids
   pasting reloadable tool bodies (reads/searches/network); requires
   retention of mutations, failures, fail→pass transitions, git actions,
   and user constraints.
3. **User guidance** = `compaction_summary_user_guidance(keep_list)`:
   heading skeleton (`## Active Task` …) plus
   `Verified facts — must appear in the checkpoint` bullets from the
   deterministic keep-list.
4. **Input budget:**
   ```
   input_budget = window > 0 ? window * 60 / 100 : 8000
   chunks at byte_budget = max(256, input_budget * 4)
   ```
5. **Output budget per call:**
   ```
   upper = window > 0 ? min(2000, window / 20) : 1000
   output = max(512, min(upper, max(1, source_tokens / 8)))
   ```
   Unit-test anchors: `output_budget(8000, 10000) == 512`;
   `output_budget(80000, 128000) == 2000`.
6. Each chunk → one summary; if multiple summaries, **exactly one**
   consolidation pass. If still more than one summary, the path falls
   back to the local fast checkpoint (does not loop).
7. **Wall-clock budget:** `compaction_summary_model_timeout_ms()` =
   **30000**. Exceeding it falls back to fast.
8. **Fallback-to-fast:** empty model output, model call error (except
   cancel), timeout, failed consolidation, or post-projection reduce/fit
   failure → commit the deterministic fast checkpoint when it dry-runs
   as a reduction. Applied strategy becomes `Fast` with a reason note.
9. **Reasoning selection** for the summarizer call: prefer catalog
   options that **disable** reasoning; else named `min` / `minimal` /
   `low`; else Auto.

### 4.7 Commit gates

Before SQLite write:

1. Dry-run rebuild; if `tokens_before > 0` and
   `tokens_after >= tokens_before` → **fail** (no commit).
2. If applied strategy is Summary and window > 0:
   ```
   fit_limit = min(window * 60 / 100, trigger > 0 ? trigger : window)
   if tokens_after > fit_limit → fail
   ```

On success:

- Append `summary` message with checkpoint (+ optional first-compact
  “Protected Initial Context” carry of head items when there was no
  prior summary).
- Update `project.summary_text`.
- Rebuild live `conversation_`:
  - reseeds prompts for **active** Act/Plan mode only;
  - one user message: `compaction_checkpoint_wrapper(checkpoint)`
    (explicit “reference only / re-read files” preamble);
  - then head and tail as plain user/assistant, or tools as
    `user` + `[Retained agent tool activity]\n…`;
  - **clears** `continuation_items_json` (opaque Responses/Chat tool
    continuation discarded at the boundary).

Internal checkpoint text is **not** re-shown as ordinary chat chrome
beyond the success notice; TUI shows strategy progress + result line.

---

## 5. Strategy summary (behavioral)

| | **fast** | **smart** (default) | **summary** |
| --- | --- | --- | --- |
| Model call | Never | Only if escalate | Always (if middle ≠ ∅) |
| Checkpoint | Deterministic local | Local or model | Model (+ consolidate) |
| Latency cost | Lowest | Low unless escalate | Highest (I/O + 1..N rounds) |
| Risk | Lossy middle (stubs + short extracts) | Escalates when lossy/large | Still lossy if model omits detail; hard size gates |
| Context-error recovery | Not used for recovery | Forced summary recovery | Forced summary recovery |

---

## 6. Worked numeric examples

Assume known context window **W = 100_000** tokens (estimate units),
default `compact_limit = 75`, `compact_strategy = smart`.

| Quantity | Formula | Value |
| --- | --- | --- |
| Auto-compact trigger | `(100000 * 75 + 99) / 100` | **75_000** |
| Tail budget | `100000 * 8 / 100` | **8_000** |
| Fast max checkpoint tokens | `max(512, 100000 * 10 / 100)` | **10_000** |
| Fast max checkpoint bytes | `max(256, 10000 * 4)` | **40_000** |
| Smart size ceiling | `min(100000*60/100, 75000)` | **60_000** |
| Summary input budget | `100000 * 60 / 100` | **60_000** |
| Summary output upper | `min(2000, 100000/20)` | **2_000** |
| Post-summary fit limit | `min(60000, 75000)` | **60_000** |
| Model path timeout | fixed | **30_000 ms** |

Assume **W = 8_000**, same defaults:

| Quantity | Value |
| --- | --- |
| Trigger | **6_000** |
| Tail budget | **640** |
| Fast max checkpoint tokens | `max(512, 800)` = **800** |
| Smart ceiling | `min(4800, 6000)` = **4_800** |
| Summary input | **4_800** |
| Summary output upper | `upper = min(2000, 8000/20) = 400`, then `max(512, min(400, source/8)) = 512`. Output budget is still at least **512** even when `window/20 < 512`. |

---

## 7. Token estimate surfaces

| Surface | What is counted |
| --- | --- |
| Live chrome / post-seed metrics | Live `conversation_.messages` (+ continuation JSON items) + native tool schemas via `publish_request_token_estimate` |
| Idle chrome after `prepare` (not yet seeded) | Next-request seed only: system prompt (`agent_prompt.md` + protocol appendix), optional `AGENTS.md`, Act/Plan mode control, interactive `build_prior_session_context` block, native tool schemas. **Does not** sum the full durable SQLite transcript. |
| Compaction `tokens_before` | Live estimate when conversation is seeded; otherwise seed overhead + full durable model-projection transcript (`estimate_compact_tokens_before`). Intentionally **not** the smaller idle reopen seed, so reduction checks stay meaningful. |
| Timeline / partition | Logical items: role + content + tool_name (+4) |
| Transcript helper | `estimate_transcript_tokens` over stored messages with model-projection roles (compaction math / unseeded compact baseline; not idle chrome) |

Idle chrome and live chrome can still disagree slightly because the first
user turn also appends the new goal text, and mid-session compact rebuilds
`conversation_` from the checkpoint + retained head/tail rather than the
prior-session reopen block.

---

## 8. Related but not `/compact`

`build_prior_session_context` (session reopen seed + idle chrome prior term):

| Cap | Value |
| --- | --- |
| Messages scanned from end | last **80** |
| Per-message content | **1500** chars (truncated with `...`) |
| Total body | **24_000** chars default `max_chars` |
| Roles included | model-projection only (`user` / `assistant` / `tool` / `summary`); skips `notice`, `thinking`, `index` |

Interactive agent reopen injects this block as one user message. Headless
`run` / `plan` do not. Not part of the three-strategy compact path.

---

## 9. User-visible outcomes

| Result | Meaning |
| --- | --- |
| Success notice | Elapsed wall time; `~saved` tokens; remaining estimate |
| No-op notice | Middle empty; cannot remove without dropping head/tail |
| Failure notice | Error detail truncated to **400** bytes, single-line |
| Progress | `"Compacting context using {fast\|smart\|summary}"` + 1–3 dots by second |

Progress names the **requested** strategy in the formatter; applied
strategy may be Summary after smart escalation.

---

## 10. Levers for a high-performance rethink

These are the main knobs encoded today; changing them is the natural
design surface:

1. **Trigger 75%** — aggressive vs wasteful; window-unknown → no auto.
2. **Token model bytes/4** — simple and portable; can over/under-shoot real
   BPE counts and mis-fire thresholds.
3. **Tail 8% + 3..20 items** — recent fidelity vs room for checkpoint;
   earlier code used 15% (comment in `compact.cpp`).
4. **Smart ceiling 60%** and **substantive 2k** — how often smart pays
   for a model round.
5. **Fast checkpoint 10% of window** and extract caps (480 / 6000 / 24
   paths) — quality of local-only compact.
6. **Tiered tool reduction** — largest win for tool-heavy sessions
   (prune/stub/digest/semantic + size fallback); applied at timeline build.
7. **Middle pre-shrink + keep-list** — merge/dedupe + deterministic facts
   for fast checkpoints and the summarizer prompt.
8. **No SQLite pruning** — disk grows forever per project thread;
   `keep_recent` is a dead parameter.
9. **Summary consolidation bound + fallback-to-fast** — one consolidate
   pass max; model timeout 30s; empty/error/fit → fast commit when
   reducing.
10. **Context-error forced summary** — safety net; disabled when strategy
    is Fast; now also benefits from pre-shrink and fallback-to-fast.
11. **Clearing continuation JSON** — forces plain re-hydration of tools;
    may bloat or lose protocol state intentionally.
12. **Rolling background summaries** — deferred (not implemented).

---

## 11. Quick constant index (copy surface)

```
DEFAULT_COMPACT_LIMIT_PERCENT     = 75
TAIL_BUDGET_PERCENT               = 8
TAIL_ITEMS_MIN                    = 3
TAIL_ITEMS_MAX                    = 20
FIRST_HEAD_ITEMS                  = 3
SUBSTANTIVE_ITEM_TOKENS           = 2000
FAILED_TOOL_ERROR_BYTES           = 400
STUB_SIZE_THRESHOLD_BYTES         = 1024
STUB_EXCERPT_BYTES                = 200 head + 200 tail
SEMANTIC_OUTPUT_BYTES             = 1200
READ_MERGE_MAX_ITEMS              = 100
DISPLAY_TOOL_REDUCE_MIN_BYTES     = 512
FAST_CHECKPOINT_WINDOW_PERCENT    = 10   // max(512, window*10/100)
FAST_CHECKPOINT_NO_WINDOW_TOKENS  = 1000
FAST_USER_EXTRACTS                = 4 × 480 bytes
FAST_PRIOR_SUMMARY_BYTES          = min(max_bytes/3, 6000)
FAST_PATH_HARVEST_MAX             = 24 × 160 chars
KEEP_LIST_MAX_LINES               = 48
SMART_SIZE_CEILING_PERCENT        = 60  // of window, min'd with trigger
SUMMARY_INPUT_PERCENT             = 60  // or 8000 if no window
SUMMARY_OUTPUT_MAX                = 2000
SUMMARY_OUTPUT_MIN                = 512
SUMMARY_OUTPUT_WINDOW_DIVISOR     = 20  // upper = min(2000, window/20)
SUMMARY_OUTPUT_SOURCE_DIVISOR     = 8   // also min'd with upper
SUMMARY_MODEL_TIMEOUT_MS          = 30000
SUMMARY_CONSOLIDATE_PASSES_MAX    = 1
AUTO_FAIL_COOLDOWN_MS             = 60000
BYTES_PER_ESTIMATED_TOKEN         = 4
```

---

*End of reference. Prefer this file over older narrative snippets when
tuning compaction; re-verify against `compact.cpp` / `session_runtime.cpp`
after any edit.*
