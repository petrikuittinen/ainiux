# pkchat Editor Help v0.94

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

1. Sends up to `continue_read_chars` characters immediately before the cursor as context (default 16384; `0` means unlimited)
2. Streams new text after the cursor (thinking traces stay out of the buffer)
3. Shows status in the minibuffer: `thinking...`, `writing.`, `stopped and ready`
4. `Esc` during generation cancels the request but keeps text already streamed

## Slash commands (`Esc` then type command)

Press **`Esc`** to open the command minibuffer (`Command:`). Type a slash command and press `Enter`. **`Tab`** completes commands and mode variants.

### Built-in commands

| Command | Purpose |
|---------|---------|
| `/spell` | Fix spelling |
| `/grammar` | Fix grammar |
| `/continue` | Continue writing from cursor context |
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
| `/list` | List open editor buffers (same as `Ctrl+L`; Enter chooses, Esc cancels) |
| `/close` | Close the active editor buffer (same as `Ctrl+W`; prompts if modified) |
| `/highlight [on|off]` | Show or toggle syntax highlighting for editor and chat |
| `/mode [auto|text|markdown|md]` | Show or set this buffer's syntax mode |
| `/provider [NAME]` | Change provider (picker when omitted) |
| `/model [MODEL]` | Change model (picker when omitted) |
| `/help` | Toggle this help view |
| `/quit` | Quit the editor |

Most commands accept a **mode** (prompted if omitted):

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

## Markdown highlighting preview

Markdown highlighting is enabled by default for `.md`, `.markdown`, `.mdown`, and `.mkd` files. The status line shows the current language and whether detection is automatic or manual. `/mode text` disables syntax styling for the current buffer, `/mode markdown` forces Markdown, and `/mode auto` resumes filename detection. Manual mode survives buffer switches and save-as operations. `/highlight off` disables highlighting across editor/chat switches for the current process; it does not change configuration.

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

When you open a file whose auto-save backup (for example `notes.txt~`) is newer than the saved file, pkchat asks whether to recover the backup instead. At startup this prompt appears on stderr before the editor UI; when opening another file in the editor, the minibuffer asks `y` to recover or `n` to load the saved file.

Repeatable `[command]` blocks add or override slash commands. See `README.md` for examples.

## Tips

- Tab completion is **disabled** in standalone editor mode (unlike the chat TUI input).
- Scratch buffers (no path) prompt to save on quit when modified.
- Bracketed paste is enabled for reliable paste detection.
- Invalid UTF-8 in files is preserved; the renderer shows a visible placeholder for bad bytes.
