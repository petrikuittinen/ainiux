# Prompt-cache hit rate test runs

Point-in-time measurements of **provider prompt-cache hit rate** for Ainiux agent mode (`ainiux run` / interactive agent). Metrics come from per-run JSONL under each project’s `.ainiux-pr/logs/agent/` (`agent_round` and `agent_turn_usage` events).

This is an empirical lab note, not a product guarantee. Provider caching, model choice, tool payload size, and turn length all change the numbers.

## How metrics are defined

Ainiux normalizes provider usage into:

| Field | Meaning |
| --- | --- |
| `input_tokens` | Full prompt size for that model round |
| `cache_read_tokens` | Prompt tokens served from provider cache (hits) |
| `fresh_input_tokens` | Uncached prompt tokens |
| `cache_write_tokens` | Cache write accounting when the provider reports it (`-1` / `0` if not) |

**Token-weighted hit rate** (preferred overall metric):

```text
hit_rate = cache_read_tokens / input_tokens
```

Summed over all `agent_round` events in a run (or use `agent_turn_usage` totals when present).

Reproduce from logs:

```bash
scripts/cache_hit_rate.py ~/path/to/project
scripts/cache_hit_rate.py ~/path/to/project -v
```

DeepSeek fields mapped by the provider adapter include `prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` (and OpenAI-style `prompt_tokens_details.cached_tokens` where applicable).

## Common test conditions

Unless noted otherwise:

| Item | Value |
| --- | --- |
| Provider | `deepseek` |
| Model | `deepseek-v4-flash` |
| API | Chat Completions (`https://api.deepseek.com/v1/chat/completions`) |
| Reasoning | `--reasoning high` where stated |
| Entry | headless `ainiux run --agent-log` |
| Workspace policy | Read-only goals (no intentional file mutations) |
| Tool cadence | Serial: one tool call per model round when the goal enforced it |
| Agent tool-turn cap | Hard-coded `max_scripted_turns = 50` |

**Important tool limit:** `read_file` defaults to `max_bytes=65536` (schema max `262144`). Small default slices limit how fast context (and thus peak `cache_read`) can grow. Large-cache runs passed `max_bytes=262144`.

## Cross-run summary

| Workspace | Scale | Best long-run rounds | Peak `input` | Peak `cache_read` | Overall hit (that run) | Late-round behavior |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `~/text-game` | Small Python game (~21 `.py`) | 31 | ~58.7k | ~58.2k | **94.3%** | Often **98–99.9%** on small deltas |
| `~/agent_analysis/hermes-agent` | Large monorepo (~3k+ `.py`, ~2M LOC claim) | 32 | ~161.5k | ~160.9k | **92.9%** | Often **99%+** when tool tails are small |
| `~/agent_analysis/opencode` | TS monorepo (~3k `.ts`/`.tsx`) | 48 | **~339.1k** | **~338.9k** | **96.3%** | **97–100%** after ~300k context |

Headline findings:

1. Longer multi-round agent turns raise **overall** hit rate into the **low–mid 90%s** on DeepSeek v4 flash.
2. **Late “steady” rounds** (small tool deltas on a warm prefix) often land at **≥98%**, sometimes **~99.9–100%**.
3. **Repo size alone does not lower hit rate.** Hit rate is driven by prefix stability vs. size of new tool results.
4. Pattern is a **sawtooth**: large tool payloads (god-file outlines, big reads) spike `fresh_*` for one round; the next rounds re-read most of the transcript from cache.
5. Peak **cache_read ≥ 300k** is achievable within the 50-turn cap if tool results are large enough (`read_file` with `max_bytes=262144` and multi-hundred-line slices).

---

## 1. `~/text-game` (small project)

### 1.1 Short baseline runs (2026-08-09)

Provider/model from project row and logs: **deepseek / deepseek-v4-flash**.

Three short completed runs (typical 3–5 model rounds) originally measured around **~86%** overall token-weighted hit rate. Round-1 hit was ~**63–64%** (often ~4096 cache-read tokens on a ~6.4k input); later rounds often **91–98%**.

Representative logs still on disk (partial set):

- `.../logs/agent/agent-20260809T191940.712Z-2188846-1.jsonl` — 4 rounds, overall **88.6%**
- `.../logs/agent/agent-20260809T192051.756Z-2188846-2.jsonl` — 3 rounds, overall **86.2%**

(An earlier same-day 5-round run at ~85.3% was used in the first analysis; that JSONL may no longer be present.)

### 1.2 ~30-round serial audit (2026-08-10)

**Goal:** Forced serial read-only pass over all Python sources (~list_dir + 21× `read_file` + pytest/grep-style closeout).

| Item | Value |
| --- | --- |
| Log | `.ainiux-pr/logs/agent/agent-20260810T120145.257Z-3230892-1.jsonl` |
| Model rounds | **31** (30 tools + final) |
| Total input | 946,312 |
| Total cache_read | 892,160 |
| Total fresh | 54,152 |
| Total output | 5,520 |
| **Overall hit** | **94.3%** |
| Peak input | 58,657 |
| Final-round hit | 99.5% |
| Last-5 token-weighted hit | **97.3%** |
| Last-10 token-weighted hit | **96.9%** |

**Segment progression**

| Segment | Rounds | Token-weighted hit |
| --- | ---: | ---: |
| first 5 | 1–5 | 93.1% |
| mid (large file reads) | 6–15 | 85.9% |
| later | 16–25 | 95.9% |
| last 5 | 27–31 | 97.3% |

**Notable dips** (large tool tails): round 9 ~55.5% hit (fresh ~6.9k); round 28 ~88% (fresh ~6.9k). Intervening rounds with small deltas reached **99.4–99.9%**.

---

## 2. `~/agent_analysis/hermes-agent` (large Python monorepo)

### 2.1 Prior short run (2026-07-29)

| Item | Value |
| --- | --- |
| Log | `.ainiux-pr/logs/agent/agent-20260729T165304.709Z-1657989-1.jsonl` |
| Rounds | 11 |
| Overall hit | **86.7%** |
| Peak input | ~62.7k |
| Round 1 | 0% cache (cold) → late rounds up to **98.4%** |

### 2.2 ~30-round architecture survey (2026-08-10)

**Goal:** Serial map + index/outline/grep + bounded reads (avoid whole god-files). Same provider/model/reasoning as above.

| Item | Value |
| --- | --- |
| Log | `.ainiux-pr/logs/agent/agent-20260810T120647.766Z-3231220-1.jsonl` |
| Model rounds | **32** |
| Total input | 2,169,138 |
| Total cache_read | 2,015,104 |
| Total fresh | 154,034 |
| Total output | 8,529 |
| **Overall hit** | **92.9%** |
| Peak input | 161,512 |
| Peak cache_read | 160,896 |
| Final-round hit | 99.6% |
| Last-10 token-weighted hit | ~94.4–96.9% depending on window (large late dip affects last-5) |

**Segment progression**

| Segment | Rounds | Token-weighted hit |
| --- | ---: | ---: |
| first 5 | 1–5 | 79.0% |
| mid | 6–15 | 86.0% |
| later | 16–25 | **96.6%** |
| last 5 | 28–32 | 91.7% (pulled down by r29) |

**Largest fresh spikes**

| Round | Approx. cause | Fresh | Hit |
| ---: | --- | ---: | ---: |
| 15 | After `file_outline(run_agent.py)` god-file | ~34k | **49.7%** |
| 22 | Large `agent_init.py` slice | ~14k | 84.7% |
| 29 | After `file_outline(gateway/run.py)` (~1.1 MB file) | ~58k | **63.5%** |

Despite the monorepo size, overall hit was comparable to the small project’s long run; late steady rounds still hit **99%+**.

---

## 3. `~/agent_analysis/opencode` (TypeScript monorepo)

Index was missing at first; created with `ainiux --index-code` before runs (**5040** eligible files, **345 440** symbols).

### 3.1 Run matrix (2026-08-10)

All: deepseek / deepseek-v4-flash / `--reasoning high` / serial tool goals.

| Run | Intent | Rounds | Peak input | Peak cache_read | Overall hit | Log basename | Outcome |
| --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| v1 | Mixed list/outline/read ~50-step plan | 51 | ~112k | ~112k | **96.6%** | `agent-20260810T125454…` (may be rotated) | Hit 50-turn tool cap; aborted before clean final |
| v2 | Dense 350–500 line slices | 50 | ~185k | ~180k | **96.5%** | `agent-20260810T125803.402Z-3268377-1.jsonl` | Clean final; default 64 KiB reads limited growth |
| v3 | `max_bytes=262144` large slices | 46 | **~303k** | **~295k** | **96.0%** | `agent-20260810T130123.747Z-3268556-1.jsonl` | Nearly 300k cache; finished slightly early |
| **v4** | Forced large `max_bytes=262144` reads | **48** | **~339.1k** | **~338.9k** | **96.3%** | `agent-20260810T130507.110Z-3268691-1.jsonl` | **≥300k cache achieved**; aborted on 3 consecutive bad line ranges |

### 3.2 Best run detail (v4)

| Item | Value |
| --- | --- |
| Log | `.ainiux-pr/logs/agent/agent-20260810T130507.110Z-3268691-1.jsonl` |
| Model rounds | 48 |
| Total input | 8,851,849 |
| Total cache_read | 8,524,800 |
| Total fresh | 327,049 |
| Total output | 9,107 |
| **Overall hit** | **96.31%** |
| Peak input | **339,104** |
| Peak cache_read | **338,944** |
| Rounds with cache ≥ 300k | **8** (from r42 onward) |
| Last-10 token-weighted hit | **98.5%** |
| Peak single-round hit | **~100.0%** (r48) |

**Growth milestones (v4)**

| Round | Input | Cache read | Hit |
| ---: | ---: | ---: | ---: |
| 1 | 6,624 | 6,144 | 92.8% |
| 10 | 76,014 | 64,128 | 84.4% |
| 20 | 160,889 | 152,320 | 94.7% |
| 30 | 225,948 | 224,256 | 99.3% |
| 40 | 301,566 | 292,480 | 97.0% |
| 42 | 313,696 | **304,128** | 96.9% |
| 48 | 339,104 | **338,944** | **100.0%** |

**v3 near-miss (for comparison):** max input 302,541; max cache_read 294,528; overall 95.98%; last rounds ~99% hit.

**v1 growth (default small tools):** ~2k tokens/round late growth → peak ~112k after 50 tools; overall already **96.6%** because the cached prefix dominated each request even when peak context was modest.

### 3.3 Why peak cache stalled near ~180k until v3/v4

Logged `read_file` tool results in the denser runs topped ~20k characters when line windows were modest; combined with the **64 KiB default `max_bytes`**, average growth was only a few thousand tokens per round. Passing **`max_bytes=262144`** and larger line windows roughly doubled growth rate and pushed peak cache past **300k** inside the 50-turn cap.

---

## 4. Index-navigation prompt benchmark (2026-08-14)

This is a directional, single-run prompt experiment, not a statistically conclusive
benchmark. It compared the shipped Ainiux prompt, a candidate index-navigation
paragraph, the candidate with indexing disabled, and Hermes using the same local
model and endpoint. The candidate was loaded only through `--trusted-prompt-dir`
during the benchmark.

| Item | Value |
| --- | --- |
| Source checkout | Hermes `c896c09c42910c584c4c7d2325b58c14713ea42c` |
| Ainiux | v1.18, prebuilt index copied into every isolated project |
| Hermes | checkout reports v0.20.1 (2026.8.13), despite the planned v0.19.0 label |
| Model / endpoint | `qwen3.6-35b-a3b-mtp` / `http://localhost:30000/v1` |
| Isolation | private project and HOME/state copy for every run |
| Hermes tools | file and terminal toolsets only |
| Scoring | frozen required paths/facts derived from the checkout; no LLM judge |
| Raw artifacts | `/tmp/ainiux-index-prompt-20260814-5rGicr/` (`metrics.json`, answers, stderr, state DBs, agent JSONL, proxy ledger) |

The six tasks covered command history, agent-state persistence, error
logging/rotation/redaction, `resolve_runtime_provider`, agent system-prompt
construction, and the `HERMES_INFERENCE_PROVIDER` reference path.

### 4.1 Aggregate standard-task results

`Rounds/API` counts model rounds and attempted Ainiux requests. Hermes reports 79
successful API calls; its proxy recorded 83 POSTs including retry/transport attempts.
Request and result sizes are UTF-8/on-wire bytes, not token estimates.

| Condition | Completed | Wall (s) | Rounds/API | Tools | Requests total | Peak request | Frozen score | Final answers |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | 5/6 | 1,011.57 | 162/162 | 296 | 32.74 MiB | 381.7 KiB | 45/59 (76.3%) | 58.5 KiB |
| Ainiux candidate, indexed | 4/6 | 1,042.34 | 152/153 | 272 | 31.48 MiB | 535.4 KiB | 38/59 (64.4%) | 49.7 KiB |
| Ainiux candidate, no index | 5/6 | 1,010.52 | 157/157 | 293 | 26.77 MiB | 384.1 KiB | 46/59 (78.0%) | 62.8 KiB |
| Hermes baseline | 6/6 | 1,471.15 | 79/79 | 189 | 13.69 MiB | 358.9 KiB | 54/59 (91.5%) | 81.9 KiB |

Hermes was 45.4% slower than current Ainiux in aggregate, but it completed the
reference-tracing task on which all three Ainiux conditions reached the 50-turn
cap. Hermes is comparative context, not a release gate.

| Condition | First navigation tools, tasks 1–6 | Index-family calls | Median time to first relevant read | Read / all result bytes | Largest result |
| --- | --- | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | symbol, symbol, glob, symbol, read, grep | 13 | 14.0 s | 1.27 / 1.65 MiB | 183.1 KiB |
| Ainiux candidate, indexed | grep, outline, symbol, symbol, outline, grep | 13 | 15.0 s | 1.39 / 2.23 MiB | 199.4 KiB |
| Ainiux candidate, no index | grep, glob, glob, grep, read, grep | 0 | 11.1 s | 1.48 / 1.60 MiB | 74.3 KiB |
| Hermes baseline | search, search, search, search, read, search | 0 | 28.6 s | 1.27 / 1.38 MiB | 42.5 KiB |

The local endpoint did not return Ainiux usage fields. Ainiux therefore logged
estimated input/output totals, while cache-read, cache-write, and reasoning-token
counts were unavailable; serialized request bytes were used for the release gate.
Hermes usage came from its usage file and is not directly comparable to the Ainiux
estimator.

| Condition | Input | Cache read | Cache write | Output | Reasoning |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | 8,582,918 estimated | unavailable | unavailable | 39,303 estimated | unavailable |
| Ainiux candidate, indexed | 8,114,823 estimated | unavailable | unavailable | 34,256 estimated | unavailable |
| Ainiux candidate, no index | 7,018,781 estimated | unavailable | unavailable | 38,479 estimated | unavailable |
| Hermes baseline | 530,567 fresh | 3,437,339 | 0 | 42,253 | 0 |

### 4.2 Candidate gate

The candidate used an index-family tool first on four of five index-suitable tasks
and correctly started the reference trace with `grep`. It nevertheless failed the
release gate, so the production navigation sentence was retained.

| Gate | Result | Evidence |
| --- | --- | --- |
| No correctness regression | **Fail** | Candidate completed 4/6 and scored 38/59 versus current at 5/6 and 45/59; task 2 exceeded the 131,072-token server window. |
| Index-first on at least 4/5; text search on task 6 | Pass | Index family first on tasks 2–5; `grep` first on task 6. The exact prescribed first tool matched only tasks 4–5. |
| At least 15% below candidate no-index request volume | **Fail** | 31.48 MiB versus 26.77 MiB: candidate indexed was 17.6% higher. |
| At least 5% below current indexed request volume | **Fail** | Candidate was only 3.9% lower than current. |
| Wall time no more than 10% above current | Pass | Candidate was 3.0% above current. |
| No task more than 25% above current request volume | **Fail** | Task 1 grew 50.0%; task 5 grew 62.9%. |

The accepted production prompt changes are therefore limited to the Plan-mode
long-term decision rule, default input/error and failure-path handling, the
algorithm/data-structure-first optimization rule, the two typo fixes, and the
700-word / 5,120-byte test ceilings. The candidate navigation paragraph remains a
benchmark-only artifact.

After rebuilding, an isolated indexed confirmation of task 4 with the embedded
production prompt completed in 149.13 s, started with `symbol`, and hit all 7/7
frozen path/fact checks.

### 4.3 Forced-compaction survey

Two additional indexed Ainiux runs used a 16,384-token config override and a
multi-subsystem survey requesting one bounded tool call per round. Current Ainiux
ignored the requested bound, emitted five large outlines, and failed with a context
error. The candidate kept results small, persisted two automatic compaction
checkpoints, and continued until the 50-turn cap.

| Prompt | Outcome | Wall (s) | Rounds/API | Tools | Compactions | Request total / peak | Result total / peak |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Current | HTTP 400 context error | 97.89 | 7/8 | 7 | 0 | 2.49 MiB / 541.9 KiB | 474.5 / 199.4 KiB |
| Candidate | 50-turn cap | 114.29 | 51/51 | 51 | 2 | 3.48 MiB / 121.1 KiB | 168.3 / 10.2 KiB |

An initial pair of compaction pilots was excluded: both prompts batched several
outlines into one round and exceeded the server context before a usable checkpoint
could be produced (current 23.87 s, candidate 12.20 s). Those raw failures remain
in the benchmark directory.

### 4.4 Limitations

- One run per task makes timing and model-policy differences directional only;
  runs were sequential and not randomized.
- Frozen literal scoring verifies required path/fact presence, not explanatory
  correctness. In particular, a high presence score can still hide a mistaken
  interpretation.
- Ainiux and Hermes used the same model/endpoint and source commit, but their system
  prompts, schemas, retry behavior, and tool implementations differ. Hermes has no
  Ainiux code index.
- Hermes request bytes are a proxy-ledger lower bound. Several client disconnects
  raised proxy-side broken-pipe logging errors, although every reported successful
  API call and additional logged retry was captured.
- The exact frozen Hermes commit self-identifies as v0.20.1, not the v0.19.0 label
  anticipated before checkout verification.
- Codex was intentionally excluded: Hermes could use the identical local model and
  better isolates client/agent behavior.

### 4.5 DeepSeek v4 Flash high-reasoning rerun (2026-08-14)

This directional single-run rerun used the same frozen tasks, scoring facts, source
commit, isolated project copies, and prompt-only candidate. Ainiux used provider
`deepseek`, model `deepseek-v4-flash`, and `--reasoning high` against the DeepSeek
API. Hermes used OpenRouter model `deepseek/deepseek-v4-flash-0731` with high
reasoning and only its file/terminal toolsets. These routes were selected as the
closest available access to the same current DeepSeek v4 Flash family; provider
routing and accounting still differ.

| Condition | Completed | Wall (s) | Rounds/API | Tools | Input | Fresh / cache read | Frozen score |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | 6/6 | 942.19 | 173/173 | 292 | 13,074,014 | 585,054 / 12,488,960 | 58/59 (98.3%) |
| Ainiux candidate, indexed | 5/6 | 910.48 | 183/183 | 294 | 14,918,021 | 599,813 / 14,318,208 | 47/59 (79.7%) |
| Ainiux candidate, no index | 6/6 | 974.70 | 169/169 | 281 | 12,048,722 | 529,746 / 11,518,976 | 56/59 (94.9%) |
| Hermes baseline | 6/6 | 3,281.98 | 142/142 | 219 | 1,383,839 fresh | 1,383,839 / 7,739,264 | 56/59 (94.9%) |

Hermes took 3.48 times as long as current indexed Ainiux: 54m41.98s versus
15m42.19s, or 248.3% slower. It was slower on every individual task. Hermes's
fresh-input and cache-read fields are shown separately because its OpenRouter usage
file does not report the same aggregate input field as Ainiux's direct DeepSeek
route. HTTPS prevented collection of comparable Hermes serialized request bytes.

| Condition | First navigation tools, tasks 1–6 | Index-family calls | Median first relevant read | Read / all result bytes |
| --- | --- | ---: | ---: | ---: |
| Ainiux current, indexed | index, index, read, symbol, ls, grep | 16 | 7.3 s | 1.12 / 1.93 MiB |
| Ainiux candidate, indexed | grep, index, read, symbol, index, index | 14 | 5.9 s | 1.07 / 2.01 MiB |
| Ainiux candidate, no index | grep, ls, ls, grep, ls, grep | 0 | 6.3 s | 1.28 / 1.72 MiB |
| Hermes baseline | search, search, search, search, read, search | 0 | 23.0 s | 0.81 / 1.00 MiB |

The smarter model substantially improved answer completion and literal coverage
over the local Qwen run, but it did not rescue the candidate navigation paragraph.
The candidate's task 6 model loop made 50 tool-using rounds, began with `index`
instead of the required text search, and was aborted by Ainiux's normal 50-round
cap before producing a final answer. This was not an API, context-window, or
external cancellation failure.

| Gate | Result | Evidence |
| --- | --- | --- |
| No correctness regression | **Fail** | Candidate completed 5/6 and scored 47/59, versus current at 6/6 and 58/59. |
| Index family first on at least 4/5; text search on task 6 | **Fail** | Candidate used an index-family tool first on 3/5 suitable tasks and incorrectly started task 6 with `index`. |
| At least 15% below candidate no-index input | **Fail** | Candidate indexed used 14.92M tokens, 23.8% more than no-index's 12.05M. |
| At least 5% below current indexed input | **Fail** | Candidate used 14.1% more input than current. |
| Wall time no more than 10% above current | Pass | Candidate was 3.4% faster, although it omitted a task-6 final answer. |
| No task more than 25% above current input | **Fail** | Tasks 1, 2, and 5 grew by 71.7%, 28.5%, and 40.7%, respectively. |

The production navigation sentence therefore remains unchanged. The Plan,
error-handling, optimization, typo, and prompt-ceiling changes accepted after the
Qwen benchmark remain in place.

The two 16,384-token forced-compaction surveys both persisted summaries and then
hit the normal 50-round cap without final answers. Current persisted three
summaries in 172.33s (932,020 input tokens; 3.10 MiB serialized requests); candidate
persisted two in 130.84s (1,112,029 input tokens; 3.92 MiB). Thus compaction worked
in both conditions, while the candidate consumed 19.3% more input and 26.6% more
request bytes. Raw outputs, state databases, usage files, and logs are under
`/tmp/ainiux-deepseek-prompt-20260814-riIjrJ/`. One initial set of three Ainiux
task-1 launches made zero model calls because the isolated prompt directories were
missing required companion prompt files; those setup-invalid attempts were moved
under `runs/invalid_missing_prompts/`, excluded, and rerun cleanly.

## Qualitative model of hit rate vs. turn length

```text
Round 1:     system + tools + user goal     → moderate hit if system/tools already cached
Rounds 2..N: same prefix + growing history  → high hit if only a small tail is new
After big tool result:                      → one low-hit round (large fresh)
Next rounds:                                → high hit again (previous tool text now cached)
```

So:

- **Overall** hit (token-weighted over the whole turn) trends toward **~93–97%** for long DeepSeek agent turns in these tests.
- **Marginal** late-round hit often sits at **≥98%** when new content is small.
- **Overall will not stay at 98%+** if the agent keeps injecting multi-10k-token tool results (god-file outlines, huge greps, large `read_file` slices)—those rounds dilute the average even while later rounds re-cache them.

## Operational notes

1. **Logs:** Completed runs use `*.jsonl`; prepare-only sessions leave `*.jsonl.partial` without usage metrics.
2. **Turn cap:** Headless agent aborts when tool-using rounds exceed **50**. Plan ~45–49 tool rounds then a tool-less final answer to avoid `agent turn cap of 50 exceeded`.
3. **Analysis helper:** `scripts/cache_hit_rate.py <project>` (absolute path, CWD-relative, or under `$HOME`).
4. **Cost:** These runs are useful for cache efficiency; dollar savings depend on DeepSeek’s current cache vs non-cache token pricing (not converted here).
5. **Not measured:** Cross-provider comparison (OpenAI, Anthropic, OpenRouter, local engines), Responses API, interactive multi-user-turn sessions over hours, or automatic compaction effects at 75% of a 1M window (none of these runs approached that threshold).

## Related code

- [Comparative Qwen/DeepSeek report](index_prompt_benchmark_report.md)
- Usage normalization: `src/provider/provider.cpp` / `provider.hpp` (`cache_read_tokens`, `fresh_prompt_tokens`, …)
- Agent logging: `src/agent/session_runtime.cpp` (`agent_round`, `agent_turn_usage`)
- Turn cap: `src/agent/agent_loop.hpp` (`max_scripted_turns`), set to 50 in `session_runtime.cpp`
- `read_file` byte caps: `src/agent/tools.cpp` (default 65536, max 262144)

## Document history

| Date | Content |
| --- | --- |
| 2026-08-14 | Added the standalone detailed Qwen/DeepSeek comparison report |
| 2026-08-14 | Added the DeepSeek v4 Flash high-reasoning rerun and retained the candidate rejection |
| 2026-08-14 | Added the directional Ainiux prompt/index/no-index/Hermes benchmark, release-gate rejection, and forced-compaction survey |
| 2026-08-10 | Initial write-up of text-game, hermes-agent, and opencode DeepSeek flash cache experiments |
