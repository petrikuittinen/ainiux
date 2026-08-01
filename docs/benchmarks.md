# Benchmarks and grading

Ainiux runs UTF-8 JSONL benchmark datasets concurrently and can grade completed result files in a separate judge-model pass. Benchmark and grade modes cannot be combined in one invocation.

## Built-in corpus

The assembled `benchmarks/builtin.jsonl` currently contains **133 cases** split into category files under `benchmarks/builtin/`. Every case has a reference answer or assessment criteria. Safety cases use an expected classification/action rather than treating refusal as universally correct.

The corpus covers speed, long-context, quality, refusals, reasoning, multilingual and cutoff-oriented cases. It is useful for controlled comparisons, not a claim of comprehensive model quality.

Inspect without model calls:

```sh
ainiux benchmark --validate-dataset
ainiux benchmark --list-cases --category reasoning
```

## Run benchmarks

```sh
ainiux benchmark --provider lmstudio -m MODEL
ainiux benchmark --category reasoning --limit 2 --provider openai -m MODEL
ainiux --benchmark --dataset eval.jsonl --mode quality --output results/
```

Important selectors include `--mode`, `--category`, `--case`, `--limit`, `--runs`, and `--warmup`. `--concurrency N` allows 1–256 concurrent requests; the default is 1. Speed mode accepts `--duration` with `ms`, `s`, `m`, or `h` suffixes. `--summary-format table|csv` controls the human summary on `stderr`.

Runs remain cancellable and continue through individual case failures where possible. Results are JSONL so partial work and per-case errors remain inspectable. Do not compare two providers without controlling model, prompt, sampling, reasoning, concurrency, hardware, and endpoint load.

## Grade results

Grade mode reads a benchmark result JSONL file and asks a judge model for structured criterion findings, a verdict, rationale, and score:

```sh
ainiux --grade --grade-input results/benchmark-TIMESTAMP.jsonl \
  --provider openai -m JUDGE_MODEL
```

Without `--grade-input`, Ainiux selects the newest matching benchmark result in the relevant output directory. Grading prompts come only from layered `benchmarks.conf` at runtime; there is no compiled fallback grading prose. This makes grading policy inspectable and replaceable.

Judge outputs are fallible. They can be sensitive to the judge model, reasoning selection, prompt wording, criterion order, candidate style, and provider changes. A high score is not proof of factual correctness or safety. Review disagreements and sampled transcripts manually, and do not use one judge as the sole release gate.

## Knowledge cutoff work

Cutoff cases are especially fragile. Their references describe events by month and can become ambiguous, outdated, or contaminated by later knowledge. The active v0.9 polish track includes cutoff and grade calibration. When updating these cases:

- verify the event and reference independently;
- avoid vague “major event” wording where a stable factual question is available;
- record exact dates and acceptable variants;
- compare more than one judge or inspect manually;
- treat results as an estimated boundary, not an exact model property.

## Custom datasets

Custom inputs must be UTF-8 JSONL and follow the current schema enforced by dataset validation. Give every case a stable ID, category, prompt turns, and either a reference answer or explicit assessment criteria. Keep safety expected actions precise. Validate before spending provider credits.

Result and grade files may contain prompts, model outputs, endpoint/model metadata, and judge rationales. Protect them as potentially sensitive artifacts and check before sharing.

Related documentation: [documentation index](README.md), [configuration](configuration.md), [testing](../TESTING.md), [roadmap](../PLANS.md).
