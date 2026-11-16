---
name: ainiux-code
description: "Write, edit, check, run, test, and repair Ainiux (.ax) programs using a local ainiux-lang checkout and its Python semantic reference. Use for Ainiux programming requests and Python-to-Ainiux translations. Interpreter development and future C++ backends are separate tasks."
---

# Ainiux code

Generate runnable Ainiux using the local repository's contracts, then verify the
requested behavior with its actual runner. These instructions work with Codex,
Grok Build, and other agents that can read files and execute commands. Codex can
invoke this skill as `$ainiux-code`; other agents can load this `SKILL.md` directly.

## Locate the checkout and read the contracts

Resolve the repository root in this order:

1. Use an explicit user-provided checkout or `AINIUX_REPO` environment variable.
2. Look through the current working directory's ancestors for the checkout.
3. Resolve this skill directory's symlinks and look through its ancestors.

Validate the candidate by checking for `spec/ainiux_language_spec_v053.md`,
`spec/AGENT_SPEC.md`, `pyproject.toml`, and `src/python/ainiux/`. If an explicit
location is invalid, report it rather than silently selecting another checkout.
If none is found, ask for the existing checkout's location. Set `AINIUX_REPO` to
its absolute path for the commands below. Do not clone a replacement or assume a
fixed home-directory path. Resolve repository links below against the real skill
location; if the skill was copied elsewhere, use the paths under `AINIUX_REPO`.

Read that checkout's `AGENTS.md` and the
[generation card](../../spec/AGENT_SPEC.md) before writing code. Read relevant
sections of the [v0.53 specification](../../spec/ainiux_language_spec_v053.md)
when a rule or operation needs clarification. Load the following contracts when
they affect the task:

| Concern | Source and authority |
|---|---|
| Errors, arithmetic, docstrings | [REVISION_051](../../spec/amendments/REVISION_051.md) overrides legacy propagation/effect syntax and checked ordinary arithmetic. |
| Imported types/enums, directory creation | [GAP_FIXES](../../spec/amendments/GAP_FIXES.md) is the follow-up; registry 23 supersedes 22. |
| PR10 library APIs and signatures | [LAUNCH_STDLIB](../../spec/amendments/LAUNCH_STDLIB.md) and the [current API manifest](../../corpus/launch/v24/api.json) override older proposal cards. |
| Assertions, cases, fixtures, capture, stubs | [TESTING](../../docs/testing/TESTING.md) supersedes the historical `test` module. |
| Real TCP and interactive input | [TCP_PROFILE](../../docs/reference/TCP_PROFILE.md) overrides earlier live-TCP deferrals. |
| Examples and supplied host bindings | [Example index](../../examples/README.md); use the exact registered or supplied capability cards. |

Current identity is language **53**, registry **45**, AST **9**, diagnostics
**3**, and AXBC **10**. Legacy language-51/AXBC-7 and language-52/AXBC-8/9
artifacts retain their original semantics. Amendment headers supersede stale passages, including
examples inside otherwise current documents: `x.rol(64)` uses count zero, it
does not raise a count-range error. Use the current API manifest selected by the checkout; `old_spec` and v1
corpora are historical. Record a material unresolved contradiction rather than
changing expected behavior to match an implementation accident.

The default executable profile is PR10. Earlier stages 1–8 remain selectable;
stage 9, macros, unsafe/pointers, native loading, and numerical array
backends are unavailable in this reference. `math.BigInt`, `math.Rational`,
`math.Decimal` and `math.Complex` are available after `import @math`. A future feature in the full spec
does not establish availability. Use the CLI's `--profile` when needed, extracting
only relevant fields or signatures; its full inventory is large. Do not implement
an interpreter, transpiler, or host service to make generated code work.

## Write Ainiux

Use `.ax` files and preserve existing style and unrelated behavior when editing.
Prefer braces for new suites and annotate function parameters and returns,
except that `init` cannot declare a return type and lambdas have no annotations.
Use first-statement plain strings to document modules, functions, and classes.

- Spell functions `fn`, floats `fp`, constants `true false nil`, and lambdas
  `x => expression`. Use `elif`. Imports are `import module` or
  `from module import Name` for user source; built-ins require `import @math` or
  `from @math import sqrt`. Both forms support `as` aliases. Wildcards and dotted
  package imports remain unavailable. Source modules can import sibling `.ax` files
  with any ordinary module name, including `models`; `@models` selects the host
  facade. Use different local aliases when importing both namespaces together.
- A binding's annotation or first assignment fixes its type. Give nil-initialized
  bindings an optional type, e.g. `name: str? = nil`; use `any` only when changing
  value kinds is intentional. Annotate fresh empty collections, e.g.
  `values: [int] = []`. Existing collections are invariant through every alias;
  fresh literals receive expected types recursively. Convert int/fp explicitly
  at assignment, parameter, and return boundaries. Bool is not numeric.
- Ordinary ints wrap as signed64. Two-int `//` truncates toward zero and `%`
  follows the dividend: `-7 // 3 == -2`, `-7 % 3 == -1`. Shift/rotation counts
  use their low six bits. Conversions, ranges/counts, time, and bounded library
  operations retain their checks; floating/mixed divmod retains its own rules.
- Call fallible operations normally. Never emit postfix propagation `?` or
  `! Error` effect declarations; optional type suffixes remain valid. Recover
  with typed `try { ... } catch IOError e { ... }`, `try expression` Results,
  `result.unwrap()`, or lazy `expression else fallback` for failure/nil. Put an
  ordinary catch-all last; it excludes sticky cancellation. Errors returned as
  data are successful values until raised. Use `with`/`defer` for cleanup.
- Assignments in functions create locals; writes to enclosing function cells
  remain closure writes. Update module state through a container or pass and
  return it. Ordinary suites do not create variable scopes; a `with` header alias
  and a catch binding do have scoped cells.
- `a..b` ascends with an exclusive end; `a..=b` includes it. Constant descending
  operators are compile errors. Use `range(hi, lo, -1)` or `reversed(...)` to
  descend. `.step(n)` replaces a stride; slices select positions. Use bracket
  slices with exclusive ends. Strings count code points, not UTF-8 bytes.
- Grow lists with `.push`, remove an item with `.remove`, and remove an index
  with `.remove_at`. Global `map(iterable, f)` and `filter(iterable, f)` take
  the iterable first and return lazy one-shot adapters; list methods are eager.
  Ranges are reusable. Mutable collections alias; copying must be explicit.
- Classes use parenthesized bases, fields before methods, implicit `this`, and
  plain protocol names such as `init`, `str`, `next`, `enter`, and `exit`.
  Do not add `self`, `cls`, or dunder receivers/hooks. `next()` uses `T | Done`;
  return `done` to end a user iterator. A manager's `exit(error)` returns nil and
  cannot suppress errors. Close generators/adapters after an early stop when
  they own cleanup; garbage collection does not run user cleanup.
- Match arms use `pattern => expression` or an arm suite. Bare names bind;
  use enum member patterns to compare enums. Add `_` unless coverage is proven.
  Imported user types and enum patterns may use `module.Type` and
  `module.Enum.member`. Only registered stdlib types accept generic arguments.
- `par`/`spawn` use the PR10 owner scheduler. Spawn must be lexical inside a
  `par` in the same function. Check `Received.ended` to distinguish EOF from a
  nil message. Explicit workers use imported module-level entries and copy supported graphs;
  handles/closures do not transfer. Parallel list callbacks have their own
  eligibility rules below, including main-script functions and noncapturing lambdas.
- Leading plain strings in suites are documentation metadata, not return values.
  Use `return "value"` or `fn name() -> str = "value"` when returning a string.
  There is no runtime `doc` or `__doc__` API.

Replace unsupported Python habits with documented Ainiux forms: comprehensions
with loops or lazy adapters, `except` with `catch`, `finally` with `with`/`defer`,
and `async`/`await` with structured tasks. Use f-strings for formatting. Set literals,
decorators, `global`/`nonlocal`, `yield from`, and match guards are unavailable.
Use `text.ord`/`text.chr` and the documented `fs` APIs instead of Python globals.
Verify library member names and signatures rather than guessing Python spellings.
For example, ensuring a directory exists is
`fs.mkdir(path, parents=true, exist_ok=true)` after `import @fs`.

A runnable starting example:

```ainiux
"""Demonstrate typed collections, masked shifts, and automatic error recovery."""
fn square(n: int) -> int = n * n
values: [int] = []
for n in 1..=3 { values.push(square(n)) }
print(values)
print(-7 // 3, -7 % 3, 1 << 64)
try {
    print(1 // 0)
} catch DivError e {
    print("caught")
}
```

Expected stdout is `[1, 4, 9]`, `-2 -1 1`, and `caught` on separate lines.

## Typed lists and parallel collections

Prefer explicit element annotations for homogeneous lists, especially empty
builders and function boundaries:

```ainiux
names: [str] = ["another", "example"]
counts: [int] = [1, 2]
measurements: [fp] = [3.13]  # fp is 64-bit floating point
rows: [[str]] = []
mixed: [any] = ["name", 1]
```

Without an expected type, `["abc", "example"]` already infers `[str]`, not
`[any]`. Empty `[]` and heterogeneous literals infer `[any]`. An empty list
built without a type does not narrow when integers are pushed into it. Existing
containers cannot be narrowed by assigning them to an annotated binding; create
a correctly typed list and explicitly copy/check elements instead. An `any`
binding can alias a typed list without removing its element restrictions.
Typed metadata can enable specialized execution, but an annotation on an already
inferred homogeneous list is not evidence of a speedup. Never change language
semantics or claim measured performance from annotations alone.

Use `values.par_map(f)` for independent computations on long lists and
`values.par_filter(predicate)` for parallel selection. Both are eager and use
isolated OS workers; map results and selected elements retain input order.
Filter returns original parent elements with their original collection metadata.
For cheap callbacks or short lists, worker startup and copying can outweigh the
benefit: prefer serial `.map`/`.filter` unless useful work justifies parallelism.
Native `ax -jN` controls the shared worker budget; one worker still isolates the
callback. This differs from cooperative `par`/`spawn`.

```ainiux
fn square(n: int) -> int = n * n
values: [int] = [1, 2, 3, 4]
squares = values.par_map(square)
evens = squares.par_filter(x => x % 2 == 0)
total = values.reduce((a, b) => a + b, 0)
```

Named main-script functions, imported unbound functions, noncapturing lambdas,
and supported unbound builtins work. Captured application data, bound methods,
and generator callbacks reject, even on empty input. Pass data in each input
value. Inputs are copied before dispatch; mutable parent state is not shared.
Current worker graph transport supports validated container and class snapshots,
including aliases/cycles within each element, but each input position is an
independent snapshot. Resources and live handles do not transfer. Failures
cancel and drain siblings; concurrent output order is unspecified.

Read [parallel collections](../../spec/amendments/PARALLEL_COLLECTIONS.md),
[worker graphs](../../spec/amendments/WORKER_STREAMS.md), and
[worker objects](../../spec/amendments/WORKER_OBJECTS.md) for version boundaries.
Registry 39 adds `values.for_each(f)` for sequential side effects in list order.
It accepts ordinary closures/bound methods, discards callback results without
testing truthiness, and returns `nil`; it does not build a result list. Use
`for_each` when order or shared state matters, and `par_for_each` for independent
isolated work. See [the serial contract](../../spec/amendments/LIST_FOR_EACH.md).

Registry 38 supports `.reduce(f)` and `.reduce(f, init)`. Without `init`,
reduction starts at the first element and combines left-to-right; an empty list
raises `ValueError`. With `init`, empty input returns it, including explicit
`nil`. Use a two-argument callback such as `(a, b) => a + b`. Older artifacts
still require `init`. Use `values.par_for_each(f)` when independent callbacks have side effects and
no result list is needed. It waits for all workers and returns nil, discarding
callback return values inside each worker. Writes may occur in any order and
are not rolled back on failure; give each item its own destination.

Use `values.par_reduce((a,b) => a+b, 0)` for an **associative** binary operation
closed over input/partial values. The initial value is optional and applied once;
empty input without it raises ValueError. Input order is preserved while grouping
changes. Integer wrapping addition/multiplication, string concatenation and
suitable min/max operations work. Floating-point rounding can differ from serial
reduction; do not use subtraction or division as associative reducers. The
callback must meet the same isolation rules as parallel map/filter.

Registry-38 parallel collection workers inherit an explicitly granted parent's
HTTP restrictions, with worker-local clients and request state. For example,
run the following with `--allow-http`, replacing the URLs and destination paths:

```ainiux
import @http
import @fs
struct Download(url: str, path: str)
fn fetch(job: Download) -> nil {
    response = http.get(job.url)
    defer response.close()
    fs.write(job.path, response.bytes())
}
jobs: [Download] = [Download("https://example.com/", "example.html")]
jobs.par_for_each(fetch)
```

Do not capture parent clients/streams or assume permission to write outside the
session's filesystem roots. Read the [registry-38 contract](../../spec/amendments/LIST_REDUCTION.md)
for empty-input, snapshot, error, and replay behavior.

## Check, run, and repair

Use the checkout's locked uv environment (Python **3.12.3**, Unicode **15.0.0**).
If setup is needed, run `uv sync --locked --project "$AINIUX_REPO"`. Keep the
lockfile intact and use its interpreter rather than system Python or an unrelated
environment. These examples use a POSIX shell; adapt quoting for another shell.

Set `AX_FILE` to the absolute generated source path. Run from the program's
intended working directory so relative data paths behave as requested;
`--project` selects the interpreter checkout without changing that directory.
Source imports resolve relative to the entry script directory. Keep CLI options
before `--`; subsequent tokens are program arguments. Read them with `os.args()`
(a function returning a list, not a module property).

```sh
uv run --locked --project "$AINIUX_REPO" ainiux-py check "$AX_FILE"
uv run --locked --project "$AINIUX_REPO" ainiux-py run "$AX_FILE"
# When the program accepts arguments:
uv run --locked --project "$AINIUX_REPO" ainiux-py run "$AX_FILE" -- --count -3
```

Run `check` first, fix source diagnostics, then execute representative inputs and
verify results against the user's requirements or independently calculated
expectations. A successful check proves compilation, not runtime correctness.
Use subprocess execution with appropriate wall-time and output limits for loops,
interactive input, or concurrency; supply input fixtures instead of leaving a
noninteractive run waiting for input. CLI `--statement-limit` and `--output-limit`
bound language work/output but do not replace an external wall timeout.

Add `--stage N` to check/run/test only when a specific earlier profile is needed.
The default execution backend is bytecode for PR8/PR10; `run --backend reference`
is available for a meaningful parity check. Earlier profiles use the reference
backend. For investigation use `ast`, `--diagnostics-json` to a separate non-source
path, or `fix "$AX_FILE" --dry-run`. Diagnose suggestions against the intended
semantics before editing; do not blindly widen types or alter expected outputs.

Check/run exit statuses are 0 for success, 1 for a language diagnostic, 2 for an
internal/tooling error, 64 for invalid CLI usage, and 130 for interruption. A
program's explicit `os.exit(code)` supplies its status. Record the diagnostic
phase and message, not just a nonzero exit. Repair generated source and rerun the
relevant checks. Treat internal defects and missing dependencies/services as
distinct blockers, not evidence that a source test passed.

Host capabilities are separate from error handling. Use only the grants needed
by the requested operation and already authorized in the task; retain them on
subsequent runs. Never add every grant to bypass `CapabilityError`.

| Run option | Capability |
|---|---|
| `--allow-process` | External child commands and process-backend workers |
| `--allow-db` | SQLite |
| `--allow-http` | Outbound HTTPS |
| `--allow-tcp` | Real TCP and cancellable POSIX console input; requires PR10 and a source file, not `run -` |
| `--allow-secure-random` | OS secure entropy |
| `--temp-root PATH` | Managed temporary resources under the specified directory |

For injected host facades, logging, or transport bindings, consult actual
capability cards and availability. A module's presence does not imply a service
is installed or authorized. Do not invent endpoints, credentials, model names,
providers, or extra CLI grants. Report missing bindings precisely.

## Tests and delivery

Use always-on `assert condition, "message"` for small behavioral checks; do not
write `assert (condition, "message")`, which tests a truthy tuple. For application
unit tests use `import @testing` and public zero-parameter nongenerator
`fn test_name() -> nil` functions in `test_*.ax` modules. Bodies and fixtures must
return nil. Pass dependencies explicitly; use capture/stubs instead of monkeypatches.

For example, save this as `test_arithmetic.ax`:

```ainiux
"""Check current signed64 arithmetic against explicit expected values."""
import @testing
fn test_arithmetic() -> nil {
    testing.eq(9223372036854775807 + 1, -9223372036854775808)
    testing.eq(-7 // 3, -2)
    testing.eq(-7 % 3, -1)
    testing.eq(1 << 64, 1)
}
```

Set `AX_TEST_PATH` to an absolute test file or directory, then run:

```sh
uv run --locked --project "$AINIUX_REPO" ainiux-py test "$AX_TEST_PATH"
```

The CLI discovers `test_*.ax` recursively and executes modules in isolated
processes (PR6+). It supports `--match`, `--tag`, `--timeout`, and report output;
consult TESTING for suites/fixtures and exact exits. Empty selection exits 2;
skip-only success is not evidence that test bodies ran. The test CLI supports
`--allow-process`; it does not expose all the run grants.

Deliver the `.ax` files, exact commands, observed output/exit status, and any
remaining limitation. Say when a program was only checked or could not be run.
For edits, summarize behavior changed and relevant verification. Preserve
unrelated files and user scratch material. Full interpreter tests and benchmarks
are separate work unless the task changes the reference implementation itself.
