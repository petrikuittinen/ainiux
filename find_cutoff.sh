#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: ./find_cutoff.sh PROVIDER MODEL BENCHMARK_JSONL

Grade a cutoff benchmark JSONL file with a judge provider/model and write a
Markdown report next to the input file.

Example:
  ./find_cutoff.sh deepseek deepseek-v4-flash results/benchmark-1783537583.jsonl
  ./find_cutoff.sh openrouter "nvidia/nemotron-3-ultra-550b-a55b:free" results/benchmark-1783537583.jsonl
EOF
}

if [ "$#" -ne 3 ]; then
    usage >&2
    exit 1
fi

JUDGE_PROVIDER=$1
JUDGE_MODEL=$2
BENCHMARK_JSONL=$3

ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PKCHAT=$ROOT/pkchat

if [ ! -x "$PKCHAT" ]; then
    echo "find_cutoff.sh: pkchat binary not found or not executable: $PKCHAT" >&2
    echo "Build it first with: make" >&2
    exit 1
fi

if [ ! -f "$BENCHMARK_JSONL" ]; then
    echo "find_cutoff.sh: benchmark JSONL not found: $BENCHMARK_JSONL" >&2
    exit 1
fi

case "$BENCHMARK_JSONL" in
    */*)
        OUTPUT_DIR=$(dirname "$BENCHMARK_JSONL")
        BASE=$(basename "$BENCHMARK_JSONL" .jsonl)
        ;;
    *)
        OUTPUT_DIR=.
        BASE=$(basename "$BENCHMARK_JSONL" .jsonl)
        ;;
esac

OUTPUT_PATH=$OUTPUT_DIR/cutoff-judgement-${BASE#benchmark-}.md
if [ "$OUTPUT_PATH" = "$OUTPUT_DIR/cutoff-judgement-.md" ]; then
    OUTPUT_PATH=$OUTPUT_DIR/cutoff-judgement.md
fi

cat "$BENCHMARK_JSONL" | "$PKCHAT" --provider "$JUDGE_PROVIDER" \
    --model "$JUDGE_MODEL" \
    --no-stream \
    --temperature 0 \
    --attach stdin \
    -p 'You are grading a knowledge-cutoff benchmark.

For each JSONL record with category "cutoff":
1. Compare "response" to "reference_answer".
2. Classify as: correct, partially_correct, incorrect, refused_or_unknown, or hallucinated_beyond_cutoff.
3. Use the month tag (for example 2024-11) as the event month.
4. At the end, estimate the model knowledge cutoff window:
   - last_month_confidently_correct
   - first_month_clearly_wrong_or_hallucinated
   - brief reasoning

Output Markdown with a per-case table and a final cutoff summary.' \
    --output "$OUTPUT_PATH"

echo "Wrote cutoff judgement report: $OUTPUT_PATH" >&2