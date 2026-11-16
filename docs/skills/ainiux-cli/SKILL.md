---
name: ainiux-cli
description: Invoke Ainiux as a headless subprocess for model prompts, independent agentic tasks, image/video generation, web retrieval, and document conversion. Use when a caller wants Ainiux CLI results or files without an interactive UI.
---

# Ainiux headless CLI

Use the installed `ainiux` executable. Run `ainiux --help` to discover options;
capabilities and model settings depend on the installed version and catalogs.
The calling agent owns task orchestration and consumes subprocess results.

## Choose the operation

| Need | Invocation |
| --- | --- |
| Model answer, no workspace tools | `ainiux --provider PROFILE -m MODEL -p "prompt" --no-stream` |
| Prompt from file or stdin | `ainiux --prompt-file prompt.txt` or `ainiux --prompt-file -` |
| Independent workspace task | `ainiux --provider PROFILE -m MODEL -r "task"` |
| Task from file or stdin | `ainiux --run-file task.txt` or `ainiux --run-file -` |
| One generated image | `ainiux image -p "description" --output image.png` |
| One generated video | `ainiux video -p "description" --duration 5 --resolution 768P --output clip.mp4` |
| Convert a local document | `ainiux --input source.docx --output-format md --output result.md` |
| Fetch a URL as Markdown | `ainiux --fetch-url https://example.com --output-format md` |
| Search the web | `ainiux --search "query" --output-format json` |
| Discover text models | `ainiux --provider PROFILE --list-models` |

`PROFILE` and `MODEL` are placeholders; omit them to use configured defaults.
A positional profile or API URL is also accepted. Do not start REPL, TUI,
editor, or server modes for a finite subprocess task.

## Independent agent tasks

Run in the intended workspace: the current directory determines project scope.
`-r`, `--run`, and `ainiux run -r "task"` perform one task, potentially using
multiple tool/model rounds. They never load or save interactive conversation
history or snapshots in `.ainiux-pr/agent.sqlite`. In-task context and compaction
use private memory. Configuration, project instructions, optional index, file
backups, and diagnostic logs retain their normal behavior. `--no-agent-log`
disables the diagnostic log.

Workspace tools may edit files and execute commands. Headless runs use Smart
permissions; decisions needing interactive approval are denied. Supply a complete
task. Lead and interactive questionnaires require interactive mode.

```sh
ainiux --provider lmstudio -m MODEL -r "Fix the parser and run its focused tests" --quiet > result.md
printf '%s' 'Review the parser and report findings' | ainiux --run-file - --quiet
```

Run emits final text only: redirect stdout instead of passing `--output`,
`--format`, or `--output-format`. Do not combine its task with `-p` or a system
prompt override. `--run-timeout` controls individual foreground tool commands;
`--timeout` controls provider requests, not the whole task.

## Prompts, web context, and media

```sh
ainiux -p "Summarize this" --attach report.pdf --no-stream --quiet
ainiux -p "Summarize the page" --fetch-url https://example.com --no-stream
ainiux -p "Describe this image" --input photo.png --no-stream
ainiux image -p "Make this scene snowy" --attach photo.jpg --output snowy.png
ainiux video -m minimax/h3-max-turbo/image-to-video -p "Slow camera push-in" --attach snowy.png --duration 5 --resolution 480P --output scene.mp4
```

Chat vision requires a compatible model. Image references are PNG/JPEG and need
an edit-capable catalog record. Image/video providers, accepted settings, and
reference types come from `images.conf` / `videos.conf`; use matching credentials.
Media commands print the saved path, or raw bytes with `--output stdout`.
Existing media output files require explicit `--force` to overwrite.

URL text in a prompt is not automatically fetched. Fetch/search without `-p`
needs no chat model. Private-address fetching requires `--allow-private-url-fetch`.
Text/document input and fetched bodies default to a 10 MiB cap; raise
`--max-input-bytes` or `--max-fetch-bytes` when needed.

## Conversions

No prompt is needed. Markdown is the canonical interchange for PDF, DOCX, XLSX,
and PPTX; conversion creates normalized documents rather than lossless edits.

```sh
ainiux --input report.pdf --output-format docx --output report.docx
ainiux --input notes.md --output-format pdf --output notes.pdf
ainiux --input sheet.xlsx --output-format md --output sheet.md
ainiux --input tables.md --output-format xlsx --output tables.xlsx
ainiux --input deck.pptx --output-format md --output deck.md
ainiux --input slides.md --output-format pptx --output slides.pptx
ainiux --input data.csv --output-format xlsx --output data.xlsx
printf '%s' '# Title' | ainiux --input stdin --output-format docx --output note.docx
```

Use `.xlsx` (not `.xslx`). XLSX output converts Markdown tables into worksheets;
DOCX omits images and advanced Word layout. PPTX slides use top-level `---`
delimiters; exact CLI PPTX-to-file Markdown conversion may create an image
sidecar directory. Other conversation paths use image placeholders. PDF extraction
and reflow do not preserve original layout or provide OCR. `--font PATH` supplies
a TrueType font for supported non-Latin PDF output.

Local CSV/JSON inputs default to native UTF-8 text; CSV converts to a table when
rendering a document such as XLSX. Fetched CSV becomes a Markdown table.
There is no headless `--output-format csv`; JSON output flags select response
serialization, not document conversion to JSON.

## Subprocess contract

- Read intentional results from stdout; status, progress, and errors use stderr.
- Use `--quiet` to suppress routine status and `--no-stream` for complete replies.
- Chat supports `--format text|json|ndjson`; `--output-format` selects document rendering. In image mode, `--format` selects the image codec instead.
- Binary PDF/Office/media stdout must be captured as bytes. Use explicit output paths for artifacts.
- Read stdin explicitly with `--prompt-file -`, `--run-file -`, or `--input stdin`; use only one stdin consumer per invocation.
- Use provider environment variables or `--key-env`, `--key-file`, or `--key-stdin`; keep keys out of argv. Examples: `OPENAI_API_KEY`, `GEMINI_API_KEY`, `XAI_API_KEY`, `REPLICATE_API_TOKEN`, `FAL_KEY`. Replicate/fal do not use `AINIUX_API_KEY`.
- Exit codes: `0` success; `2` arguments; `3` network/TLS/timeout; `4` HTTP/auth/provider/parse; `5` file/config; `6` unsupported/internal; `130` cancelled. Check the exit code before consuming the result.
