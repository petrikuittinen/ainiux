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

- Usage normalization: `src/provider/provider.cpp` / `provider.hpp` (`cache_read_tokens`, `fresh_prompt_tokens`, …)
- Agent logging: `src/agent/session_runtime.cpp` (`agent_round`, `agent_turn_usage`)
- Turn cap: `src/agent/agent_loop.hpp` (`max_scripted_turns`), set to 50 in `session_runtime.cpp`
- `read_file` byte caps: `src/agent/tools.cpp` (default 65536, max 262144)

## Document history

| Date | Content |
| --- | --- |
| 2026-08-10 | Initial write-up of text-game, hermes-agent, and opencode DeepSeek flash cache experiments |
