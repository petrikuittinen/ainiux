# Index-navigation prompt benchmark: Qwen and DeepSeek

Date: 2026-08-14

## Executive summary

Two directional, single-run experiments tested the same prompt-only code-navigation
change on the same six source-grounded tasks. The first used the local
`qwen3.6-35b-a3b-mtp` endpoint. The rerun used DeepSeek v4 Flash with high
reasoning: Ainiux connected directly as `deepseek-v4-flash`, while Hermes used
OpenRouter's `deepseek/deepseek-v4-flash-0731` route.

DeepSeek materially improved Ainiux current-prompt reliability and frozen literal
coverage: current indexed Ainiux rose from 5/6 completed and 45/59 checks with Qwen
to 6/6 and 58/59 with DeepSeek. It did not make the candidate navigation paragraph
acceptable. The candidate failed the release gate with both models.

With DeepSeek, current indexed Ainiux completed the six tasks in 942.19 seconds.
Hermes required 3,281.98 seconds, making Hermes 3.48 times as slow, or 248.3%
slower. Hermes nevertheless completed all six tasks in both experiments.

Cache reuse was high when exact counters were available. DeepSeek Ainiux conditions
reported aggregate cache-hit rates of 95.53% to 95.98%. DeepSeek Hermes reported
84.83%, and Qwen Hermes reported 86.63%. The local Qwen endpoint did not return
Ainiux cache counters, so no honest Ainiux/Qwen cache-hit rate can be calculated.

The production prompt therefore retains its original short navigation sentence.
The independently accepted Plan-mode, error-handling, optimization, typo, and
prompt-size-ceiling changes remain in production.

## Test design

Each standard-task condition ran once:

1. Current Ainiux prompt with the code index enabled.
2. Candidate navigation prompt with the code index enabled.
3. Candidate navigation prompt with indexing disabled.
4. Hermes with file and terminal toolsets only.

The six tasks asked the agent to explain source-grounded behavior for:

1. Command-history implementation.
2. Agent-state persistence across turns and restarts.
3. Error logging, rotation, and credential redaction.
4. `resolve_runtime_provider` custom-endpoint resolution.
5. System-prompt construction in `agent/prompt_builder.py`.
6. The path of `HERMES_INFERENCE_PROVIDER` from input to the model request.

Every run used a private project copy and private HOME/state directory. All source
copies came from Hermes commit
`c896c09c42910c584c4c7d2325b58c14713ea42c`; that checkout identifies itself as
v0.20.1 despite the originally planned v0.19.0 label. The Ainiux indexed conditions
started from the same prebuilt index template. The candidate was loaded through
`--trusted-prompt-dir`; it was not embedded in the production binary.

The candidate replaced the vague navigation sentence with a concrete rule: use
`index` once when the path or symbol is unknown, use `symbol` for named concepts,
use `outline` when the file is known but its relevant range is not, and follow with
targeted `read`. It also instructed the model to fall back to `grep`, `glob`, or
`ls` for references, generated/dynamic names, stale or ambiguous index results, and
non-source files, and to skip the index when exact ranges were already known.

Answers were scored against frozen required paths and source-derived literal facts.
There was no LLM judge. This detects missing evidence consistently, but it does not
prove that every surrounding explanation is semantically correct. One run per
condition makes the results directional rather than statistically conclusive.

## Token and cache accounting

The report uses this cache-hit definition:

```text
cache hit rate = cache-read tokens / (fresh-input tokens + cache-read tokens)
```

For direct DeepSeek Ainiux runs, `input_tokens` equals fresh input plus cache reads;
cache-write tokens were zero. For Hermes, the usage file labels only uncached input
as `input_tokens`, so the denominator must be constructed by adding its cache-read
field. These values should not be treated as identical billing records across
providers: direct DeepSeek and OpenRouter can differ in prefix boundaries,
rounding, and accounting.

The local Qwen endpoint omitted Ainiux usage/cache fields. Ainiux's Qwen input and
output totals are therefore internal estimates, useful for relative volume but not
for billed-token or cache-rate claims. Hermes did return Qwen cache counters.

## Qwen run

Model: `qwen3.6-35b-a3b-mtp`
Endpoint: `http://localhost:30000/v1`

### Aggregate results

| Condition | Completed | Wall | Rounds/API | Tools | Input tokens | Cache read | Cache hit | Output tokens | Score |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | 5/6 | 1,011.57 s | 162/162 | 296 | 8,582,918 estimated | unavailable | unavailable | 39,303 estimated | 45/59 |
| Ainiux candidate, indexed | 4/6 | 1,042.34 s | 152/153 | 272 | 8,114,823 estimated | unavailable | unavailable | 34,256 estimated | 38/59 |
| Ainiux candidate, no index | 5/6 | 1,010.52 s | 157/157 | 293 | 7,018,781 estimated | unavailable | unavailable | 38,479 estimated | 46/59 |
| Hermes | 6/6 | 1,471.15 s | 79/79 | 189 | 530,567 fresh | 3,437,339 | 86.63% | 42,253 | 54/59 |

Hermes processed 3,967,906 reported prompt tokens when its fresh and cache-read
fields are combined. Its 530,567 fresh tokens were only 13.37% of that prompt
volume. Hermes was 45.4% slower than current indexed Ainiux but produced the best
Qwen completion count and frozen score.

### Per-task outcome

Each cell shows `wall seconds; score`. `cap` means Ainiux's normal 50-model-round
limit was reached before a final answer. `context` means the endpoint rejected an
oversized request.

| Task | Current indexed | Candidate indexed | Candidate no index | Hermes |
| --- | ---: | ---: | ---: | ---: |
| 1. Command history | 99.12; 9/10 | 174.47; 10/10 | 121.02; 10/10 | 186.57; 8/10 |
| 2. Agent persistence | 251.95; 9/10 | 116.35; context, 0/10 | 217.94; 9/10 | 269.76; 10/10 |
| 3. Logging/redaction | 133.10; 9/10 | 120.61; 10/10 | 105.10; 10/10 | 293.97; 9/10 |
| 4. Provider resolution | 154.66; 7/7 | 129.29; 7/7 | 114.62; 7/7 | 156.75; 7/7 |
| 5. Prompt construction | 171.78; 11/11 | 277.76; 11/11 | 224.59; 10/11 | 224.90; 10/11 |
| 6. Provider-variable trace | 200.96; cap, 0/11 | 223.86; cap, 0/11 | 227.25; cap, 0/11 | 339.20; 10/11 |

Qwen followed the candidate's broad navigation intent reasonably well: it used an
index-family tool first on four of the five index-suitable tasks and started task 6
with text search. The efficiency and correctness gates still failed. Candidate
indexed serialized 31.48 MiB of requests, 17.6% more than candidate no-index, and
only 3.9% less than current. Individual request volume grew 50.0% on task 1 and
62.9% on task 5. The task-2 candidate also exceeded the local server's 131,072-token
context window.

### Qwen cache detail

Only Hermes supplied exact cache counters. Its aggregate 86.63% hit rate varied
with tool-result size and trajectory length; task 6 alone reported 1,420,291 cache
reads against 160,779 fresh tokens. Ainiux's unavailable counters must not be
interpreted as a 0% hit rate. Serialized requests and estimated input tokens show
volume, not which portion the Qwen server reused internally.

## DeepSeek v4 Flash high-reasoning run

Ainiux route: provider `deepseek`, model `deepseek-v4-flash`
Hermes route: provider `openrouter`, model
`deepseek/deepseek-v4-flash-0731`
Reasoning setting: `high` for both clients

### Aggregate results

| Condition | Completed | Wall | Rounds/API | Tools | Total/fresh input | Cache read | Cache hit | Output | Reasoning | Score |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Ainiux current, indexed | 6/6 | 942.19 s | 173/173 | 292 | 13,074,014 / 585,054 | 12,488,960 | 95.53% | 93,775 | unavailable | 58/59 |
| Ainiux candidate, indexed | 5/6 | 910.48 s | 183/183 | 294 | 14,918,021 / 599,813 | 14,318,208 | 95.98% | 86,378 | unavailable | 47/59 |
| Ainiux candidate, no index | 6/6 | 974.70 s | 169/169 | 281 | 12,048,722 / 529,746 | 11,518,976 | 95.60% | 93,916 | unavailable | 56/59 |
| Hermes | 6/6 | 3,281.98 s | 142/142 | 219 | 9,123,103 processed / 1,383,839 fresh | 7,739,264 | 84.83% | 61,467 | 17,600 | 56/59 |

DeepSeek current indexed Ainiux had the highest completion and score, missing only
one frozen fact across all tasks. It was also 2,339.79 seconds faster than Hermes.
Hermes was slower on every individual task, most dramatically on command history
(1,035.31 versus 182.82 seconds) and persistence (828.90 versus 199.93 seconds).

The high cache-hit percentages do not mean that indexing reduced total work. The
candidate indexed condition had the highest hit rate, 95.98%, because it repeatedly
reused an even larger trajectory. It still consumed 14.92 million total input
tokens—14.1% more than current and 23.8% more than candidate no-index. Cache rate
must therefore be read together with fresh tokens, total tokens, rounds, and wall
time.

### Current Ainiux versus Hermes by task

| Task | Ainiux fresh | Ainiux cache | Hit | Ainiux wall/score | Hermes fresh | Hermes cache | Hit | Hermes wall/score |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 65,873 | 1,063,808 | 94.17% | 182.82 s; 10/10 | 438,171 | 216,064 | 33.03% | 1,035.31 s; 10/10 |
| 2 | 158,717 | 4,402,688 | 96.52% | 199.93 s; 10/10 | 133,249 | 3,132,160 | 95.92% | 828.90 s; 9/10 |
| 3 | 88,226 | 1,098,752 | 92.57% | 106.51 s; 10/10 | 113,852 | 614,656 | 84.37% | 119.59 s; 10/10 |
| 4 | 84,836 | 1,449,728 | 94.47% | 128.54 s; 7/7 | 143,395 | 600,960 | 80.74% | 152.45 s; 7/7 |
| 5 | 61,054 | 937,984 | 93.89% | 113.57 s; 10/11 | 376,971 | 1,348,096 | 78.15% | 670.88 s; 10/11 |
| 6 | 126,348 | 3,536,000 | 96.55% | 210.82 s; 11/11 | 178,201 | 1,827,328 | 91.11% | 474.85 s; 10/11 |

Task 1 explains much of Hermes's aggregate cache-rate deficit. Its first DeepSeek
trajectory reported 438,171 fresh tokens and only 216,064 cache reads, a 33.03%
hit rate, while current Ainiux reused 94.17% on the same task. On tasks 2 and 6,
both clients reused most prompt tokens.

### Candidate outcome and failure

The DeepSeek candidate failed task 6 after 50 tool-using rounds. It began with
`index` instead of the expected text search, performed 72 tool calls, consumed
4,127,148 input tokens, and reached Ainiux's standard turn cap without emitting a
final answer. Exit status 130 represented `AINIUX_ERR_CANCELLED: agent turn cap of
50 exceeded`; it was not an API failure, external interrupt, or context-window
error.

The full DeepSeek gate result was:

- Correctness: failed, with 5/6 completion and 47/59 versus current's 6/6 and 58/59.
- Navigation: failed, with an index-family tool first on 3/5 suitable tasks and the
  wrong index-first choice on task 6.
- Indexed versus no-index tokens: failed; indexed used 23.8% more, not 15% less.
- Indexed versus current tokens: failed; candidate used 14.1% more, not 5% less.
- Aggregate wall limit: nominally passed at 3.4% faster, but only because task 6
  ended without a final answer.
- Per-task volume: failed; tasks 1, 2, and 5 exceeded current by 71.7%, 28.5%, and
  40.7%, respectively.

## Automatic-compaction surveys

The additional surveys forced a 16,384-token context policy and requested a bounded,
one-tool-per-round architecture survey across many subsystems.

| Model / prompt | Outcome | Wall | Input tokens | Serialized requests | Compactions |
| --- | --- | ---: | ---: | ---: | ---: |
| Qwen current | Endpoint context error | 97.89 s | estimated only | 2.49 MiB | 0 |
| Qwen candidate | 50-round cap | 114.29 s | estimated only | 3.48 MiB | 2 |
| DeepSeek current | 50-round cap | 172.33 s | 932,020 | 3.10 MiB | 3 |
| DeepSeek candidate | 50-round cap | 130.84 s | 1,112,029 | 3.92 MiB | 2 |

DeepSeek allowed both prompts to reach and persist automatic summaries. Neither
survey produced a final architecture report because both continued until the turn
cap. The DeepSeek candidate was faster but used 19.3% more input and 26.6% more
serialized request bytes than current. This again demonstrates why wall time or
cache-hit rate alone is not a sufficient efficiency measure.

## Cross-model interpretation

DeepSeek was a much stronger source-analysis model in the Ainiux current condition:
it completed the previously failed reference trace and increased literal coverage
from 76.3% to 98.3%. Its current-prompt wall time was 6.9% lower than Qwen despite
making 173 rather than 162 model rounds. Reported/estimated input volume was 52.3%
higher, but this comparison is approximate because Qwen Ainiux usage was estimated
while DeepSeek usage came from provider counters.

Hermes behaved differently. DeepSeek improved its score from 54/59 to 56/59 but
increased wall time from 1,471.15 to 3,281.98 seconds, a 123.1% increase. The
DeepSeek Hermes run used 142 API calls and 17,600 reasoning tokens versus 79 calls
and zero reported reasoning tokens for Qwen. Direct DeepSeek versus OpenRouter
latency, reasoning policy, cache behavior, and provider load are confounded, so the
wall-time increase should not be attributed to model intelligence alone.

The central result is stable across both models: the longer candidate prompt did
not reliably induce cheaper or more correct navigation. A smarter model improved
answers, but it did not convert the candidate into a release-worthy instruction.
Indexing itself was also not automatically beneficial: in both experiments the
candidate no-index condition used less aggregate input/request volume than the
candidate indexed condition.

## Limitations and artifact locations

- Each task ran once, sequentially, without randomization; timing is directional.
- Frozen literal scoring checks evidence presence, not full semantic correctness.
- The Qwen and DeepSeek runs occurred against different endpoint/provider paths.
- Ainiux and Hermes use different prompts, schemas, retry behavior, and tools;
  Hermes has no Ainiux index.
- DeepSeek Ainiux reasoning tokens were not broken out by the direct provider, so
  `unavailable` is not equivalent to zero.
- Hermes HTTPS request bodies were not intercepted in the DeepSeek rerun, so its
  serialized request-byte totals are unavailable there.
- The model identifiers were treated as routes to the same DeepSeek v4 Flash
  family, but provider-side deployment details cannot be independently proven.

Raw artifacts:

- Qwen: `/tmp/ainiux-index-prompt-20260814-5rGicr/`
- DeepSeek: `/tmp/ainiux-deepseek-prompt-20260814-riIjrJ/`

The compact run ledger and release-gate record remain in
`docs/cache_hit_rate_test_runs.md`.

## Tool-efficiency milestone follow-up

Date: 2026-08-15

This follow-up tested a compact provider-facing tool-result projection and a
separable managed-script feature. The projection and reduced `grep`/`symbol`
defaults plus `outline` pagination were implemented and tested, but the required
six-task DeepSeek release gate failed. Those projection/default changes were
therefore reverted. Managed scripts remain because their implementation and
acceptance path are independent of provider-result projection.

### Offline projection replay

The experimental projection was replayed through the implementation over all 23
preserved Ainiux JSONL files in each prior artifact directory. Diagnostic results
were already credential-redacted. Sizes below are UTF-8 bytes of the full tool
envelope versus the projected provider envelope; percentages are bytes removed.

| Tool | Qwen calls | Qwen full → projected | Saved | DeepSeek calls | DeepSeek full → projected | Saved |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `fetch` | — | — | — | 7 | 304,524 → 304,143 | 0.13% |
| `glob` | 37 | 24,507 → 22,378 | 8.69% | 21 | 3,255 → 2,016 | 38.06% |
| `grep` | 392 | 332,325 → 302,022 | 9.12% | 412 | 1,132,476 → 1,102,847 | 2.62% |
| `index` | 4 | 13,064 → 11,676 | 10.62% | 11 | 35,918 → 32,109 | 10.60% |
| `ls` | 3 | 22,566 → 22,435 | 0.58% | 16 | 90,964 → 90,254 | 0.78% |
| `outline` | 29 | 2,646,890 → 1,207,914 | 54.36% | 15 | 730,766 → 319,193 | 56.32% |
| `read` | 493 | 4,680,810 → 4,632,045 | 1.04% | 469 | 3,821,938 → 3,767,816 | 1.42% |
| `run` | 1 | 440 → 67 | 84.77% | 7 | 4,752 → 1,391 | 70.73% |
| `symbol` | 20 | 91,909 → 85,066 | 7.45% | 11 | 104,601 → 97,780 | 6.52% |
| **Total** | **979** | **7,812,511 → 6,283,603** | **19.57%** | **969** | **6,229,194 → 5,717,549** | **8.21%** |

`outline` documentation dominated the offline saving. Existing `read` content
dominated total bytes but had little removable envelope overhead. This replay
measures tool-result bytes, not provider tokenization or the behavioral effect of
changed history.

### Live DeepSeek gate

The rebuilt current indexed Ainiux ran the six established tasks once against
provider `deepseek`, model `deepseek-v4-flash`, reasoning `high`. It used fresh
private copies of the same frozen Hermes checkout and the existing prebuilt index.
No candidate, no-index, Hermes, local-Qwen, or Qwen conditions were rerun.

| Task | Exit | Wall | Rounds | Tools | Total / fresh / cache input | Output | Score | Input vs baseline |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1. Command history | 0 | 173.12 s | 31 | 50 | 1,639,715 / 86,563 / 1,553,152 | 16,383 | 10/10 | +45.15% |
| 2. Agent persistence | 130, 50-round cap | 139.13 s | 51 | 65 | 4,610,657 / 127,585 / 4,483,072 | 9,171 | 0/10 | +1.08% |
| 3. Logging/redaction | 0 | 93.31 s | 17 | 29 | 981,270 / 75,286 / 905,984 | 9,918 | 10/10 | -17.33% |
| 4. Provider resolution | 0 | 109.79 s | 25 | 41 | 1,310,211 / 67,459 / 1,242,752 | 11,888 | 7/7 | -14.62% |
| 5. Prompt construction | 0 | 115.34 s | 21 | 41 | 1,487,420 / 91,964 / 1,395,456 | 13,794 | 10/11 | +48.89% |
| 6. Provider-variable trace | 130, 50-round cap | 147.86 s | 51 | 70 | 4,326,550 / 108,182 / 4,218,368 | 9,317 | 0/11 | +18.14% |
| **Total** | **4/6** | **778.55 s** | **196** | **296** | **14,355,823 / 557,039 / 13,798,784** | **70,471** | **37/59** | **+9.80%** |

The aggregate cache-hit rate was 96.12%. Fresh input fell 4.79% while cache reads
rose 10.49%, so total input rose 9.80%. Live full tool results occupied 2,091,028
bytes and their projections occupied 1,892,762 bytes, a 9.48% reduction. That
byte reduction did not produce a cheaper trajectory in this single run.

| Release condition | Requirement | Result |
| --- | --- | --- |
| Completion | 6/6 | **Fail: 4/6** |
| Frozen score | at least 58/59 | **Fail: 37/59** |
| Total input | at least 5% below 13,074,014 (at most 12,420,313) | **Fail: 14,355,823, +9.80%** |
| Wall time | within 10% of 942.19 s | Numerically 778.55 s, but inadmissible because two tasks ended at the cap |
| Rounds | within 5% of 173 (at most 181 complete rounds) | **Fail: 196, +13.29%** |
| Per-task input | no task grows more than 15% | **Fail: tasks 1, 5, and 6 grew 45.15%, 48.89%, and 18.14%** |

Tasks 2 and 6 ended at Ainiux's normal 50-round cap, not from provider or harness
errors. The gate is conjunctive, so the compact projection, pagination, index-size
fields, and smaller result defaults were reverted rather than shipped.

### Managed-script live task

One additional interactive Yolo task used a real Hermes source tree. The model
created `.ainiux-pr/scripts/rank-python-declarations.sh`, corrected one parsing
bug, and ran the same final script bytes with `def` and `class` arguments. The
final script SHA-256 was
`9119b692bbd1da67934ac4ecf73bb8104aba8220b2c2f8c9ffbfd1855e950b60`.
Independent replay of those frozen bytes reproduced the final answer exactly:

| Argument | Total | Ranked files and counts |
| --- | ---: | --- |
| `def` | 68,802 | `tests/test_tui_gateway_server.py` 1,068; `hermes_cli/web_server.py` 558; `gateway/run.py` 553; `cli.py` 496; `tui_gateway/server.py` 457 |
| `class` | 11,299 | `tests/test_tui_gateway_server.py` 221; `tests/agent/test_auxiliary_client.py` 107; `hermes_cli/web_models.py` 86; `tests/gateway/test_matrix.py` 82; `tests/run_agent/test_run_agent.py` 68 |

The two-turn trajectory used 16 model rounds and 19 tool results: 14 `run`, two
`write`, two `mkdir`, and one `ls`. It reported 450,460 total input tokens, 50,076
fresh input, 400,384 cache reads (88.88% hit), and 13,755 output tokens. Yolo
correctly recorded no approval prompt. The final script file was mode `0600`, its
directory was mode `0700`, and no `.ainiux-run-*` temporary copy remained.

The first turn tried to create and inspect the protected parent with `mkdir`, then
stopped safely. A continuation clarified that direct-child `write` creates storage
on first use. The production agent prompt and `run` description now state that
workflow explicitly. This means the live managed-script result validates the
feature after clarification, not zero-shot discoverability of the earlier wording.

### Follow-up limitations and artifacts

- Each standard task and the managed-script task ran only once; model variance is
  not estimated.
- The standard run used the experimental projection build. The shipping tree was
  rebuilt and retested after projection/default rollback, but the six paid live
  tasks were not repeated, per the one-run constraint.
- The live projection changed both serialized history and tool defaults, so the
  run cannot attribute behavioral regression to one field or tool.
- Offline byte replay does not establish token savings, cache savings, correctness,
  or latency.
- The managed task required a user clarification and its ripgrep regex counts
  matching declaration-like lines; it is not a Python parser.
- Qwen3.8 testing remains deferred.

Additional artifacts:

- Offline projection analyzer output: `/tmp/ainiux-tool-projection-replay.json`
- DeepSeek gate and managed task: `/tmp/ainiux-tool-efficiency-20260814-eg8CTd/`

### Failed-task ablation rerun

Date: 2026-08-15

Tasks 2 and 6 were rerun with a temporary provider-envelope projection, while
restoring the original `grep` schema limits and the original unpaginated,
documentation-retaining `outline`. The temporary projector explicitly bypassed
projection for `outline`; it was removed after the run and is not part of the
shipping tree.

| Task | Exit | Wall | Rounds | Tools | Total / fresh / cache input | Output | Result |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 2. Agent persistence | 130, 50-round cap | 168.63 s | 51 | 76 | 5,510,087 / 139,335 / 5,370,752 | 11,724 | No final answer; live evidence contained all required paths/facts |
| 6. Provider-variable trace | 0 | 188.69 s | 46 | 68 | 3,223,977 / 100,649 / 3,123,328 | 17,738 | 11/11 |

This rerun reproduced task 2's cap failure despite full documented outlines and
the original `grep` limits. Task 6 completed successfully. The result makes
aggressive `grep` return limits unlikely to be the primary cause; trajectory
variance and the fixed round budget remain significant factors.
