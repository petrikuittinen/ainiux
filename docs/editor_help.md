# ainiux Editor Help v0.99

Standalone editor mode (`ainiux --editor [PATH]`) is a multiline text editor with Unicode-aware navigation, search/replace, and optional AI assist when a provider and model are configured.

## Layout

- **Main area** — file content with soft wrap (optionally split into panes)
- **Status line** (reverse video) — path, dirty flag, cursor position, quick hints
- **Minibuffer** (bottom line) — commands, prompts, AI status, and messages

### Window splits (`Ctrl+X` prefix)

Splits use an Emacs-style two-key sequence. Press **Ctrl+X**, then one of:

| Second key | Action |
|------------|--------|
| `v` or `3` | Vertical split (side by side) |
| `h` or `2` | Horizontal split (stacked) |
| `o` | Focus other / next pane |
| `0` | Close the focused pane |
| `1` | Maximize the focused pane (close others) |
| `Esc` or `Ctrl+X` | Cancel the window command |
| `Ctrl+B` | Page up in the **other** pane (last focused, without moving focus) |
| `Ctrl+D` | Page down in the **other** pane (same target as Ctrl+B) |

Slash commands (via `Esc` then the command):

| Command | Action |
|---------|--------|
| `/vsplit` | Vertical split (side by side); same as `Ctrl+X v` |
| `/hsplit` | Horizontal split (stacked); same as `Ctrl+X h` |
| `/closesplit` | Close the focused pane; same as `Ctrl+X 0` |
| `/maximize` | Keep only the focused pane; same as `Ctrl+X 1` |
| `/nosplit` | Alias for `/maximize` |

Both panes of a new split show the same buffer at first. Open another file in one pane (`Ctrl+O`) or switch buffers (`Ctrl+L`) to compare two files. Status shows `[N panes]` while more than one pane is open.

**Other-pane scrolling:** after a split, `Ctrl+B`/`Ctrl+D` page the new sibling. After `Ctrl+X o`, they page the pane you left. With three or more panes, the target is always the last pane you left (not every non-focused pane). Cancel for minibuffers and replace mode is **Esc** only (not Ctrl+X).

## Getting help

| Action | Effect |
|--------|--------|
| `Ctrl+H` | Open this help in read-only view |
| `Ctrl+H` again | Close help and return to your file |
| `Esc` then `/help` | Same as `Ctrl+H` |
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
| `Esc` then `replace-string` or `/replace` | Replace (search, then replace each match) |
| `Ctrl+Q` | Quit |

Replace mode after `replace-string` / `/replace`: `Space` replaces match, `s` skips, `a` replaces all remaining, `Esc` ends replace.

## File locking and external changes

Writable file buffers hold an advisory `FILE.LOCK` directory for their complete lifetime, including buffer and chat-mode switches. Opening an actively or unverifiably locked existing file produces a `[RO]` status. Search, selection, copy, settings, and AI output to a new buffer remain available; content-changing operations are blocked. The first attempted edit retries the lock. If the on-disk file changed while the buffer was read-only, accept the reload prompt to use the current disk contents and repeat the edit, or decline to remain read-only.

Before saving the current path, ainiux compares the file with its load/save fingerprint and asks before overwriting an external change, replacement, or deletion. It rechecks after confirmation. Save As acquires the destination lock first; it is also the way to turn a read-only buffer into a writable buffer at a new path. Auto-save is disabled for read-only buffers.

Dead same-host lock owners are recovered automatically. Remote, live, malformed, missing-metadata, token-mismatched, and unexpectedly nonempty locks are left in place. Only after verifying that no ainiux process owns the file, manually remove the known `FILE.LOCK/owner` file and then its empty `FILE.LOCK` directory.

## Editing

| Key | Action |
|-----|--------|
| `Ctrl+C` | Copy selection |
| `Ctrl+V` | Paste (internal clipboard first, then desktop/OSC 52 clipboard) |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo |
| `Ctrl+Y` | Redo |
| `Ctrl+A` | Select all (entire buffer) |
| `Home` | Beginning of line |
| `End` | End of line |
| `Ctrl+Home` | Beginning of buffer |
| `Ctrl+End` | End of buffer |
| `Backspace` | Delete before cursor; with a selection, cut to the clipboard |
| `Delete` (`Esc [3~`) | Delete at cursor; with a selection, cut to the clipboard |
| `Enter` | New line |
| `Tab` | Complete a word/symbol from open buffers; if none matches, insert indentation; with a selection, indent every touched line |
| `Shift+Tab` | Outdent the current line, or every touched line in a selection |

There is no dedicated cut key (`Ctrl+X` is the window-command prefix). Delete a
selection with Backspace or Delete to place it on the clipboard, then paste with
`Ctrl+V`. Copy, selection-delete, and line kill retain text in Ainiux's
process-wide clipboard and also publish it through a native desktop helper and
OSC 52 when available. With an empty internal clipboard, `Ctrl+V` reads text
asynchronously from `pbpaste`, `wl-paste`, `xclip`, `xsel`, Termux, or WSL. SSH
sessions query the terminal first. Reads are limited to 16 MiB and are applied
only if the buffer, cursor, selection, and input mode have not changed. Terminal
bracketed paste remains the fallback when clipboard access is unavailable or
denied.

## Selection

Hold `Shift` while using arrow keys, `Page Up`/`Page Down`, `Home`/`End`, or `Ctrl+Home`/`Ctrl+End` to extend the selection. Selected text is highlighted in reverse video.

`Ctrl+E` is not used in standalone editor mode. In chat TUI mode, `Ctrl+E` copies the last user or assistant message into the input for editing; `Enter` saves and a bare `Esc` cancels.

## Provider and model

Start the editor with a provider shortcut or profile to discover its models immediately — the same flow as `--chat`:

```text
ainiux openrouter --editor notes.txt
ainiux lmstudio --editor draft.md
```

When a provider is set but no model is chosen yet, ainiux loads `/v1/models`. A sole result is selected automatically; multiple results open the colored model selector. File editing still works while discovery runs, and AI commands stay disabled until a model is selected.

Use `Esc` then `/provider` or `/model` to change provider or model. `/provider` with no argument opens the same colored selector widget used by chat and the editor buffer list. Choosing a provider clears the previous provider's model and immediately loads `/v1/models`; multiple results open the model selector, while one result is selected automatically. `/model` with no argument loads the same endpoint and selector directly. Each model change also refreshes its context window from `/v1/models` unless `--context` or `/context TOKENS` set an override. `/context auto` clears the override. When model metadata has no context window, usage shows only the estimated token count without a percentage.

`/reasoning` with no argument opens a model-aware selector from `models.conf`, with Auto first and the documented provider default shown when known. `/reasoning auto` clears the override; `/reasoning VALUE` accepts a bounded ASCII value; and `/reasoning TOKENS` accepts an exact non-negative token budget. If the current model family matches but the direct value is not listed, the editor warns and asks for y/n confirmation. Confirming permits a future provider value; declining keeps the current setting. Changing the actual provider or model resets reasoning to Auto. The editor remembers its last provider, model, API, and reasoning selection globally in SQLite, loads it after configuration defaults, and still gives explicit CLI options final precedence. It does not store endpoint URLs or credentials in app state; a custom provider is restored only if its endpoint is still configured.

`ainiux --provider none --editor` (or plain `ainiux --editor`) runs as a local editor with no startup picker or model request. Use `/provider` later to discover and select a model for AI assist.

## AI continue (`Ctrl+Space`)

Requires a configured provider **and** model. If either is missing, `Ctrl+Space` and other AI commands report what to configure next.

`Ctrl+T` cycles the selected model's catalog-backed reasoning setting from lower
to higher values, then Auto, and around again. For toggle-only Qwen 3.5/3.6 and
Gemma 4 models it switches thinking off and on. With no selected model it does
nothing. `Alt+Ctrl+T` toggles whether thinking traces are shown.

`Ctrl+Space` runs **`/continue`** in **continue** mode:

1. In `text` and `markdown` modes, sends bounded context before and after the cursor. It requests a natural, developed bridge into an existing postfix. At the buffer end (including a whitespace-only remainder), it requests substantial continuation: concrete examples and supported numbers for factual text, and brave, vivid, specific development for creative writing. It asks for the document itself, not suggestions or an outline, and prohibits recap/restart behavior.
2. In every other `/mode`, sends bounded UTF-8-character context before and after the cursor and requests only insertion code for the canonical active language. This mode split applies even when `/highlight off` is set.
3. Streams the result at the original cursor without changing the existing postfix or normalizing generated whitespace. Prose accepts raw insertion text or an optional `<content>` wrapper. Code may remove a matching or unlabeled Markdown fence; a mismatched leading fence is rejected.
4. Omits postfix data when the postfix limit is `0` or the complete remainder is empty/whitespace-only (spaces, tabs, CR/LF, form feed, or vertical tab).
5. Keeps thinking traces out of the buffer and shows `thinking...` / `writing.` status.
6. `Esc` cancels generation but keeps partial output; the stream remains one undoable edit.

Context settings live under `[editor]` in `config.conf` (and optional environment overrides):

| Side | Default | Config | Environment |
|------|---------|--------|-------------|
| Prose prefix | 16384 | `continue_prose_prefix_max_chars` | `MAX_CONTINUE_PROSE_PREFIX` |
| Prose postfix | 4096 | `continue_prose_postfix_max_chars` | `MAX_CONTINUE_PROSE_POSTFIX` |
| Code prefix | 4000 | `continue_prefix_max_chars` | `MAX_CONTINUE_PREFIX` |
| Code postfix | 2000 | `continue_postfix_max_chars` | `MAX_CONTINUE_POSTFIX` |
| Output tokens | 32768 | `continue_max_tokens` | `MAX_AI_CONTINUE_TOKENS` |

For any context side, `0` disables that side; it does not mean unlimited. Setting precedence is built-in default, system config, user config, then environment. The output-token setting is shared by prose and code.

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
| `/expand` | Add relevant detail, examples, and elaboration |
| `/shorten` | Tighten text without losing important meaning or details |
| `/summarize` | Produce a high-information one-to-three-paragraph summary |
| `/simplify` | Rewrite in plain language for a typical teenager |
| `/variations` | Generate five alternative phrasings |
| `/checklist` | Convert text into a checklist or action items |
| `/table` | Convert text into a Markdown table |
| `/keypoints` | Extract the main arguments and key points |
| `/sentiment` | Analyze emotional tone, sentiment, and possible bias |
| `/quiz` | Generate 10 multiple-choice questions and an answer key |
| `/questions` | Generate 10 open-ended questions with model answers |
| `/risk` | Identify legal, ethical, and reputational risks |
| `/entities` | Extract names, dates, places, quantities, and specific terms |
| `/brainstorm` | Generate 10 diverse ideas related to the text |
| `/outline` | Create a structured outline |
| `/hooks` | Generate 10 opening hooks |
| `/title` | Suggest 10 titles |
| `/explain` | Explain code or technical concepts |
| `/fix` | Fix code errors and improve fault tolerance |
| `/refactor` | Simplify and improve code without changing behavior |
| `/tests` | Write comprehensive tests for the supplied code |
| `/plan` | Create a concrete TDD implementation plan |
| `/transliterate` | Convert text between scripts without translating it |
| `/readability` | Score readability and suggest improvements |
| `/speech` | Write a structured, compelling speech |
| `/fiction` | Write a bold short fiction piece with lively dialogue |
| `/blog` | Write an audience-aware, SEO-conscious blog post |
| `/article` | Write a comprehensive, sourced article |
| `/joke` | Write a developed joke with a strong payoff |
| `/roast` | Write a hard-hitting comedic roast |
| `/grumpyman` | Write as a blunt, old-fashioned grumpy man |
| `/Trump` | Write a clearly fictional Trump parody monologue |
| `/English` | Translate to English |
| `/Chinese` | Translate to Chinese |
| `/Finnish` | Translate to Finnish |
| `/German` | Translate to German |
| `/French` | Translate to French |
| `/Italian` | Translate to Italian |
| `/Spanish` | Translate to Spanish |
| `/Portuguese` | Translate to Portuguese |
| `/Arabic` | Translate to Arabic |
| `/Hindi` | Translate to Hindi |
| `/Japanese` | Translate to Japanese |
| `/Korean` | Translate to Korean |
| `/Swedish` | Translate to Swedish |
| `/Polish` | Translate to Polish |
| `/Russian` | Translate to Russian |
| `/prompt TEXT` | Custom one-shot AI task |
| `/regenerate` | Repeat the previous AI command with the same command options |
| `/save` | Save (same as `Ctrl+S`) |
| `/saveas [PATH]` | Save as (same as `Ctrl+Shift+S`; `Tab` completes paths after the command) |
| `/find` | Search (same as `Ctrl+F`) |
| `/replace` or `replace-string` | Replace (search, then interactive replace) |
| `/open [PATH]` | Open file (same as `Ctrl+O`; `Tab` completes paths after the command) |
| `/new` | Open a new empty editor buffer (same as `Ctrl+N`) |
| `/list` | List open editor buffers (same as `Ctrl+L`; Enter chooses, N new, DEL closes selected with y/n prompt, Esc cancels) |
| `/close` | Close the active editor buffer (same as `Ctrl+W`; prompts if modified) |
| `/vsplit` | Vertical split (side by side; same as `Ctrl+X v`) |
| `/hsplit` | Horizontal split (stacked; same as `Ctrl+X h`) |
| `/closesplit` | Close the focused pane (same as `Ctrl+X 0`) |
| `/maximize` | Keep only the focused pane (same as `Ctrl+X 1`) |
| `/nosplit` | Alias for `/maximize` |
| `/highlight [on|off]` | Show or toggle syntax highlighting for editor and chat |
| `/mode [MODE|auto]` | Show or set this buffer's syntax mode |
| `/reformat` | Reformat leading indentation in the selected lines |
| `/reformat-all` | Reformat leading indentation in the entire buffer |
| `/left-align [WIDTH]` | Word-wrap and left-align (selection or whole buffer); omit WIDTH to prompt (default from config) |
| `/right-align [WIDTH]` | Word-wrap and right-align to WIDTH columns |
| `/center-align [WIDTH]` | Word-wrap and center-align to WIDTH columns |
| `/justify [WIDTH]` | Word-wrap and justify lines (last line of each paragraph left-aligned) |
| `/alignment-width [WIDTH]` | Show or set this session's default WIDTH for alignment commands |
| `/remove-blank-lines` | Remove empty and whitespace-only lines |
| `/remove-duplicate-blank-lines` | Collapse consecutive blank lines to one |
| `/remove-duplicate-lines` | Collapse consecutive identical lines (`uniq`-style) |
| `/tab-width [1..32]` | Show or set this buffer's tab width |
| `/tab-style [spaces|tab]` | Show or set this buffer's indentation style |
| `/linebreak [lf|cr|crlf]` | Show or set this buffer's save line endings |
| `/insert FILE_OR_URL` | Insert bounded UTF-8 file text or fetched HTML at the cursor |
| `shell COMMAND` or `!COMMAND` (optional `/`) | Run a user shell command; open a **new buffer** with pure stdout; status/errors/time in the minibuffer |
| `shell-stdout COMMAND` or `!!COMMAND` (optional `/`) | Same as `shell` in the editor (new buffer + minibuffer status) |
| `/auto-convert-html-to-md [yes|no]` | Show or set URL HTML-to-Markdown conversion for this process |
| `/provider [NAME]` | Change provider (picker when omitted) |
| `/model [MODEL]` | Change model (picker when omitted) |
| `/context [auto\|TOKENS]` | Show, override, or resume automatic model context-window discovery |
| `/reasoning [auto|VALUE|TOKENS]` | Show the model-aware selector or set reasoning directly |
| `/chat` | Switch directly to ordinary chat mode |
| `/agent` | Switch directly to interactive agent mode |
| `/editor` | Stay in editor mode (reports that it is already active) |
| `/help` | Toggle this help view |
| `/quit` | Quit the editor |

The leading slash is optional for these mode commands, as for other editor
commands. `Ctrl+P` toggles back to the conversational mode that opened the
editor: chat ↔ editor or agent ↔ editor. A standalone editor defaults to
editor ↔ chat.

Most commands accept a **mode** (prompted if omitted):

`/prompt TEXT` uses the same choices (including `v` and `h`).

| Mode key | Name | Input to model | Output |
|----------|------|----------------|--------|
| `s` | selection | Selected text | Replace selection in place |
| `a` | all | Whole buffer | Replace entire buffer in place |
| `n` / `newbuffer` | new buffer | Selected text | Stream into a new editor buffer |
| `v` / `vsplit` | new buffer + vertical split | Selected text | Stream into a new buffer in a side-by-side pane |
| `h` / `hsplit` | new buffer + horizontal split | Selected text | Stream into a new buffer in a stacked pane |
| `i` | insert | Selected text | Stream after cursor |

**Continue** mode (`c`) is only available on **`/continue`**, including `Ctrl+Space`. Other built-in commands do not offer continue.

Examples:

```text
/spell selection
/rewrite all
/Chinese newbuffer
/prompt Summarize the buffer in three bullets
```

`Esc` cancels the command minibuffer without running a command.

## Local editing without AI

`ainiux --provider none --editor` and plain `ainiux --editor` work offline. File editing, search, replace, undo, and clipboard still work. AI commands and `Ctrl+Space` report that a provider is required until `/provider` and `/model` are configured.

With a provider but no model, editing still works; AI commands report **No model chosen. Use /model to choose one**.

## Syntax highlighting

Highlighting is enabled by default. The editor detects Markdown, Python, C, C++, C#, Java, JavaScript/JSX, TypeScript/TSX, HTML, HTML-only, CSS, XML/SVG, JSON/JSONL, Bash, PHP, Perl, Ruby, Rust, Go, PowerShell, Assembly, SQL, TOML, YAML, and INI from common filename endings. `.html` and `.htm` select `html`; `.xhtml` selects `htmlonly`. Scratch buffers, `.txt`, and unknown endings use `text`. Markdown fenced blocks use the named language highlighter when the fence tag is recognized. In Markdown mode, headings and strong text use the terminal's bold font, emphasis uses italic, and links and URLs are underlined.

The status line shows the current language and line-ending mode compactly in parentheses, such as `(html LF)`, `(python CRLF)`, or `(text CR)`. `/mode text` disables syntax styling for the current buffer. `/mode markdown|python|c|cpp|csharp|java|javascript|typescript|html|htmlonly|css|xml|json|bash|php|perl|ruby|rust|go|powershell|assembly|sql|toml|yaml|ini` selects a manual mode. Short aliases include `md`, `py`, `c++`, `c#`, `js`, `ts`, `html-multi`, `htmlmulti`, `html-only`, `jsonl`, `sh`, `pl`, `rb`, `rs`, `golang`, `pwsh`, `ps1`, `asm`, `yml`, and `dosini`. The default `html` mode highlights JavaScript in `<script>` blocks and `on*` attributes, and CSS in `<style>` blocks and `style` attributes. Use `htmlonly` for markup-only highlighting with embedded code kept string-colored. `/mode auto` resumes filename detection. Bare `/mode` reports whether the current mode is automatic or manual. Manual mode survives buffer switches and save-as operations. `/highlight off` disables highlighting across editor/chat switches for the current process; it does not change configuration.

`/reformat` requires a selection and expands it to complete touched lines. `/reformat-all` reformats the complete buffer and keeps the cursor on its logical line. Both commands change leading indentation only, preserve blank lines and all other bytes, and are one undo step. They use the active language mode and current tab width/style; YAML always uses spaces. Comments, strings, heredocs, Markdown fences, YAML block scalars, and other multiline protected regions do not influence nesting. Reformatting runs in a cancellable background job: press `Esc` to cancel. You may continue editing or switch buffers; stale results are discarded safely.

### Text alignment and line cleanup

`/left-align`, `/right-align`, `/center-align`, and `/justify` reflow text so lines fit a maximum display width (cells, not bytes). They apply to the **selection if present** (expanded to complete physical lines), otherwise the **whole buffer**. Blank lines separate paragraphs and are preserved; words are packed with ordinary spaces. **WIDTH** must be greater than 20 and at most 1000. If any word is wider than WIDTH, the command fails without changing the buffer. Omitting WIDTH opens a minibuffer prompt: `Enter width for the text-alignment (N default):` with the current default prefilled—press Enter to accept it. The default comes from `[editor] alignment-width` in config (78) and can be changed for the session with `/alignment-width WIDTH`. Justify stretches spaces between words on non-final paragraph lines (Word/CSS-like); the last line of each paragraph stays left-aligned. Each successful run is one undo step and works offline (`--provider none`).

`/remove-blank-lines` drops empty and whitespace-only lines. `/remove-duplicate-blank-lines` collapses runs of consecutive blank lines to a single blank line. `/remove-duplicate-lines` keeps the first of each run of consecutive identical lines. All three use the same selection-or-whole-buffer scope and one undo step.

### Chat and agent history display alignment

In **chat** and **agent** TUI history (not the editor buffer), the same config default drives **display-only** reflow of every message role (user and assistant, and other history rows). Stored transcripts are not rewritten.

| Command | Purpose |
|---------|---------|
| `/width [N\|-1]` | Show or set history/table column width. **`-1`** disables prose reflow (unlimited). Positive N must be > 20 and ≤ 1000. Alias: `/alignment-width`. |
| `/left-align` | History align mode: left |
| `/right-align` | History align mode: right (useful for Arabic and other RTL-leaning layouts) |
| `/center-align` | History align mode: center |
| `/justify-align` or `/justify` | History align mode: justify |

Default mode is left-align; default width is `[editor] alignment-width` (78). On a wide terminal, prose wraps near that column so lines stay readable. Pretty Markdown tables are also capped to the effective width (session `/width` or the terminal content width, whichever is smaller): columns shrink and cell text word-wraps onto following lines so the table never runs past the margin. Fenced code is left intact; overlong prose words occupy their own line instead of failing.

## Configuration

`[editor]` in `ainiux` config:

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

User shell (`shell` / `/shell` / `!` / `shell-stdout` / `/shell-stdout` / `!!`) runs `/bin/sh -c` in the process working directory. In the editor the leading `/` is optional (as with other commands). Every form opens a **new buffer** containing pure stdout only; the minibuffer shows success (exit, elapsed ms, byte count) or a clear failure (exit/stderr snippet). Esc cancels an in-flight shell job. Unlike chat/agent, `shell-stdout` is not draft-fill here—both forms use the new-buffer path.

When you open a file whose auto-save backup (for example `notes.txt~`) is newer than the saved file, ainiux asks whether to recover the backup instead. At startup this prompt appears on stderr before the editor UI; when opening another file in the editor, the minibuffer asks `y` to recover or `n` to load the saved file.

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
