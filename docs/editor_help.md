# pkchat Editor Help v0.97

Standalone editor mode (`pkchat --editor [PATH]`) is a multiline text editor with Unicode-aware navigation, search/replace, and optional AI assist when a provider and model are configured.

## Layout

- **Main area** — file content with soft wrap
- **Status line** (reverse video) — path, dirty flag, cursor position, quick hints
- **Minibuffer** (bottom line) — commands, prompts, AI status, and messages

## Getting help

| Action | Effect |
|--------|--------|
| `Esc` then `/help` | Open this help in read-only view |
| `Esc` then `/help` again | Close help and return to your file |
| `Ctrl+Q` while help is open | Close help and return to your file |
| `Ctrl+Q` while editing | Quit (with save prompts when needed) |

Arrow keys, `Page Up`/`Page Down`, `Home`, and `End` scroll the help document.

## File commands

| Key | Action |
|-----|--------|
| `Ctrl+S` | Save (prompts for path on scratch buffers) |
| `Ctrl+Shift+S` | Save as (prompts for path; asks before overwriting an existing file) |
| `Ctrl+O` | Open another file buffer |
| `Ctrl+N` | Open a new empty buffer |
| `Ctrl+L` | List open buffers; Enter chooses and Esc cancels |
| `Ctrl+W` | Close the active buffer; prompts before discarding modifications |
| `Ctrl+F` | Search (exact substring) |
| `Ctrl+H` | Replace (search, then replace each match) |
| `Ctrl+Q` | Quit |

Replace mode after `Ctrl+H`: `Space` replaces match, `s` skips, `a` replaces all remaining, `Esc` ends replace.

## File locking and external changes

Writable file buffers hold an advisory `FILE.LOCK` directory for their complete lifetime, including buffer and chat-mode switches. Opening an actively or unverifiably locked existing file produces a `[RO]` status. Search, selection, copy, settings, and AI output to a new buffer remain available; content-changing operations are blocked. The first attempted edit retries the lock. If the on-disk file changed while the buffer was read-only, accept the reload prompt to use the current disk contents and repeat the edit, or decline to remain read-only.

Before saving the current path, pkchat compares the file with its load/save fingerprint and asks before overwriting an external change, replacement, or deletion. It rechecks after confirmation. Save As acquires the destination lock first; it is also the way to turn a read-only buffer into a writable buffer at a new path. Auto-save is disabled for read-only buffers.

Dead same-host lock owners are recovered automatically. Remote, live, malformed, missing-metadata, token-mismatched, and unexpectedly nonempty locks are left in place. Only after verifying that no pkchat process owns the file, manually remove the known `FILE.LOCK/owner` file and then its empty `FILE.LOCK` directory.

## Editing

| Key | Action |
|-----|--------|
| `Ctrl+C` | Copy selection |
| `Ctrl+X` | Cut selection |
| `Ctrl+V` | Paste (internal clipboard, then terminal paste) |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo |
| `Ctrl+Y` | Redo |
| `Ctrl+A` | Select all (entire buffer) |
| `Home` | Beginning of line |
| `End` | End of line |
| `Ctrl+Home` | Beginning of buffer |
| `Ctrl+End` | End of buffer |
| `Backspace` | Delete before cursor |
| `Delete` (`Esc [3~`) | Delete at cursor |
| `Enter` | New line |
| `Tab` | Complete a word/symbol from open buffers; if none matches, insert indentation; with a selection, indent every touched line |
| `Shift+Tab` | Outdent the current line, or every touched line in a selection |

## Selection

Hold `Shift` while using arrow keys, `Page Up`/`Page Down`, `Home`/`End`, or `Ctrl+Home`/`Ctrl+End` to extend the selection. Selected text is highlighted in reverse video.

`Ctrl+E` is not used in standalone editor mode. In chat TUI mode, `Ctrl+E` copies the last user or assistant message into the input for editing; `Enter` saves and a bare `Esc` cancels.

## Provider and model

Start the editor with a provider shortcut or profile, then choose a model inside the editor — the same flow as `--chat`:

```text
pkchat openrouter --editor notes.txt
pkchat lmstudio --editor draft.md
```

When a provider is set but no model is chosen yet, the minibuffer shows **Choose a model with /model**. File editing still works; AI commands stay disabled until a model is selected.

Use `Esc` then `/provider` or `/model` to change provider or model. `/provider` with no argument opens a provider picker; `/model` with no argument loads `/v1/models` and opens a model picker.

`pkchat --provider none --editor` (or plain `pkchat --editor`) runs as a local editor with no network access. Use `/provider` and `/model` later to enable AI assist.

## AI continue (`Ctrl+Space`)

Requires a configured provider **and** model. If either is missing, `Ctrl+Space` and other AI commands report what to configure next.

`Ctrl+Space` runs **`/continue`** in **continue** mode:

1. In `text` and `markdown` modes, sends bounded context before and after the cursor. It requests a natural, developed bridge into an existing postfix. At the buffer end (including a whitespace-only remainder), it requests substantial continuation: concrete examples and supported numbers for factual text, and brave, vivid, specific development for creative writing. It asks for the document itself, not suggestions or an outline, and prohibits recap/restart behavior.
2. In every other `/mode`, sends bounded UTF-8-character context before and after the cursor and requests only insertion code for the canonical active language. This mode split applies even when `/highlight off` is set.
3. Streams the result at the original cursor without changing the existing postfix or normalizing generated whitespace. Prose accepts raw insertion text or an optional `<content>` wrapper. Code may remove a matching or unlabeled Markdown fence; a mismatched leading fence is rejected.
4. Omits postfix data when the postfix limit is `0` or the complete remainder is empty/whitespace-only (spaces, tabs, CR/LF, form feed, or vertical tab).
5. Keeps thinking traces out of the buffer and shows `thinking...` / `writing.` status.
6. `Esc` cancels generation but keeps partial output; the stream remains one undoable edit.

Context settings:

| Side | Default | Config | CLI | Environment |
|------|---------|--------|-----|-------------|
| Prose prefix | 16384 | `continue_prose_prefix_max_chars` | `--editor-continue-prose-prefix-max-chars N` | `MAX_CONTINUE_PROSE_PREFIX` |
| Prose postfix | 4096 | `continue_prose_postfix_max_chars` | `--editor-continue-prose-postfix-max-chars N` | `MAX_CONTINUE_PROSE_POSTFIX` |
| Code prefix | 4000 | `continue_prefix_max_chars` | `--editor-continue-prefix-max-chars N` | `MAX_CONTINUE_PREFIX` |
| Code postfix | 2000 | `continue_postfix_max_chars` | `--editor-continue-postfix-max-chars N` | `MAX_CONTINUE_POSTFIX` |
| Output tokens | 32768 | `continue_max_tokens` | `--editor-continue-max-tokens N` | `MAX_AI_CONTINUE_TOKENS` |

For any context side, `0` disables that side; it does not mean unlimited. Setting precedence is built-in, system config, user config, CLI, then environment. The output-token setting is shared by prose and code.

## Editor commands (`Esc` then type command)

Press **`Esc`** to open the command minibuffer (`Command:`). Type a command with or without a leading slash and press `Enter` (`rewrite all` and `/rewrite all` are equivalent). **`Tab`** completes commands and mode variants while preserving whether the current prefix is slashless. This minibuffer completion is independent from document word completion. Chat commands remain slash-only.

### Built-in commands

| Command | Purpose |
|---------|---------|
| `/spell` | Fix spelling |
| `/grammar` | Fix grammar |
| `/continue` | Continue prose or complete a code gap, based on active mode |
| `/fact` | Check factual accuracy |
| `/comment` | Suggest improvements |
| `/rewrite` | Rewrite for spelling, grammar, facts, and style |
| `/English` | Translate to English |
| `/Chinese` | Translate to Chinese |
| `/Finnish` | Translate to Finnish |
| `/prompt TEXT` | Custom one-shot AI task |
| `/regenerate` | Repeat the previous AI command with the same command options |
| `/save` | Save (same as `Ctrl+S`) |
| `/saveas [PATH]` | Save as (same as `Ctrl+Shift+S`; `Tab` completes paths after the command) |
| `/find` | Search (same as `Ctrl+F`) |
| `/replace` | Replace (same as `Ctrl+H`) |
| `/open [PATH]` | Open file (same as `Ctrl+O`; `Tab` completes paths after the command) |
| `/new` | Open a new empty editor buffer (same as `Ctrl+N`) |
| `/list` | List open editor buffers (same as `Ctrl+L`; Enter chooses, N new, DEL closes selected with y/n prompt, Esc cancels) |
| `/close` | Close the active editor buffer (same as `Ctrl+W`; prompts if modified) |
| `/highlight [on|off]` | Show or toggle syntax highlighting for editor and chat |
| `/mode [MODE|auto]` | Show or set this buffer's syntax mode |
| `/reformat` | Reformat leading indentation in the selected lines |
| `/reformat-all` | Reformat leading indentation in the entire buffer |
| `/tab-width [1..32]` | Show or set this buffer's tab width |
| `/tab-style [spaces|tab]` | Show or set this buffer's indentation style |
| `/linebreak [lf|cr|crlf]` | Show or set this buffer's save line endings |
| `/insert FILE_OR_URL` | Insert bounded UTF-8 file text or fetched HTML at the cursor |
| `/auto-convert-html-to-md [yes|no]` | Show or set URL HTML-to-Markdown conversion for this process |
| `/provider [NAME]` | Change provider (picker when omitted) |
| `/model [MODEL]` | Change model (picker when omitted) |
| `/help` | Toggle this help view |
| `/quit` | Quit the editor |

Most commands accept a **mode** (prompted if omitted):

`/prompt TEXT` uses the same four choices.

| Mode key | Name | Input to model | Output |
|----------|------|----------------|--------|
| `s` | selection | Selected text | Replace selection in place |
| `a` | all | Whole buffer | Replace entire buffer in place |
| `n` | new buffer | Selected text | Stream into a new editor buffer |
| `i` | insert | Selected text | Stream after cursor |

**Continue** mode (`c`) is only available on **`/continue`**, including `Ctrl+Space`. Other built-in commands do not offer continue.

Examples:

```text
/spell selection
/rewrite all
/Chinese newbuffer
/prompt Summarize the buffer in three bullets
```

`Esc` or `Ctrl+G` cancels the command minibuffer without running a command.

## Local editing without AI

`pkchat --provider none --editor` and plain `pkchat --editor` work offline. File editing, search, replace, undo, and clipboard still work. AI commands and `Ctrl+Space` report that a provider is required until `/provider` and `/model` are configured.

With a provider but no model, editing still works; AI commands report **No model chosen. Use /model to choose one**.

## Syntax highlighting

Highlighting is enabled by default. The editor detects Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML/SVG, JSON/JSONL, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI from common filename endings. `.html` and `.htm` select `html`; `.xhtml` selects `htmlonly`. Scratch buffers, `.txt`, and unknown endings use `text`. Markdown fenced blocks use the named language highlighter when the fence tag is recognized.

The status line shows the current language and line-ending mode compactly in parentheses, such as `(html LF)`, `(python CRLF)`, or `(text CR)`. `/mode text` disables syntax styling for the current buffer. `/mode markdown|python|c|cpp|csharp|java|javascript|typescript|html|htmlonly|css|xml|json|bash|php|perl|ruby|rust|go|powershell|assembly|sql|toml|yaml|ini` selects a manual mode. Short aliases include `md`, `py`, `c++`, `c#`, `js`, `ts`, `html-multi`, `htmlmulti`, `html-only`, `jsonl`, `sh`, `pl`, `rb`, `rs`, `golang`, `pwsh`, `ps1`, `asm`, `yml`, and `dosini`. The default `html` mode highlights JavaScript in `<script>` blocks and `on*` attributes, and CSS in `<style>` blocks and `style` attributes. Use `htmlonly` for markup-only highlighting with embedded code kept string-colored. `/mode auto` resumes filename detection. Bare `/mode` reports whether the current mode is automatic or manual. Manual mode survives buffer switches and save-as operations. `/highlight off` disables highlighting across editor/chat switches for the current process; it does not change configuration.

`/reformat` requires a selection and expands it to complete touched lines. `/reformat-all` reformats the complete buffer and keeps the cursor on its logical line. Both commands change leading indentation only, preserve blank lines and all other bytes, and are one undo step. They use the active language mode and current tab width/style; YAML always uses spaces. Comments, strings, heredocs, Markdown fences, YAML block scalars, and other multiline protected regions do not influence nesting. Reformatting runs in a cancellable background job: press `Esc` to cancel. You may continue editing or switch buffers; stale results are discarded safely.

## Configuration

`[editor]` in `pkchat` config:

- `undo_limit` — undo depth (default 5)
- `huge_file_size_warning` — confirm before loading huge files (default 1 GiB)
- `file_size_limit` — optional hard load cap
- `auto-save-mode` — `on`/`off` (default `on`)
- `auto-save-postfix` — suffix appended to the file name for backup copies (default `~`)
- `auto-save-threshold` — auto-save after this many changed bytes (default `300`)
- `auto-save-timeout` — auto-save after this many idle seconds when changes are pending (default `30`)
- `auto-save-size-limit` — skip auto-save above this buffer size; supports `k`/`M`/`G`/`T` suffixes (default `10M`)
- `tab-width` — fallback indentation and display tab width from 1 through 32 (default `4`)
- `tab-style` — fallback `spaces` or `tab` style (default `spaces`)
- `linebreak` — default `lf`, `cr`, or `crlf` for new, empty, no-ending, and mixed-ending files (default `lf`)

`[input] auto-convert-html-to-md` controls `/insert URL` and defaults to `yes`. URL insertion accepts only HTTP(S), retains the existing private-address, proxy, timeout, TLS, content-type, and response-size protections, and requires UTF-8 HTML. Set it to `no` to insert raw HTML. Local `/insert` accepts every file ending, but rejects NUL-containing, invalid UTF-8, unreadable, and oversized content. CR and CRLF are normalized to the editor's internal LF representation and use the buffer's chosen linebreak mode when saved.

When you open a file whose auto-save backup (for example `notes.txt~`) is newer than the saved file, pkchat asks whether to recover the backup instead. At startup this prompt appears on stderr before the editor UI; when opening another file in the editor, the minibuffer asks `y` to recover or `n` to load the saved file.

Existing files and recovered backups detect indentation from at most the first 20 physical lines. A consistent space step selects that tab width, while consistently tab-indented lines select tab style. Ambiguous, mixed, unindented, and one-line files retain the configured fallbacks. `/tab-width` and `/tab-style` always override the detected values for the active buffer.

Repeatable `[command]` blocks add or override editor commands. `string` and `prompt` are required; `modes` defaults to `selection, all, newbuffer, insert` when omitted. `continue` is continue-only when explicitly selected. Prompts may use escaped multiline values such as:

```conf
[command]
string = summarize
prompt = """
Summarize the selected text.
Keep the result concise.
"""
```

Legacy slash-prefixed `string` values remain valid. See `README.md` for examples.

## Tips

- After a document word or symbol prefix, `Tab` searches every open editor buffer. The first press inserts a unique match or the matches' common prefix; further presses rotate through multiple full matches. Lowercase prefixes match case-insensitively with Unicode folding, while prefixes containing uppercase letters are case-sensitive. If there is no match, `Tab` performs indentation. A selected block always uses indentation.
- Document word completion, command/path completion in the command minibuffer, and chat-input completion use separate state and candidate domains.
- LF, CR, and CRLF files retain their detected style on save and auto-save. Mixed endings produce a warning and use the configured default.
- Scratch buffers (no path) prompt to save on quit when modified.
- Bracketed paste is enabled for reliable paste detection.
- Invalid UTF-8 in files is preserved; the renderer shows a visible placeholder for bad bytes.
