# pkchat Editor Help

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
| `Ctrl+O` | Load another file |
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
| `Ctrl+U` | Undo |
| `Ctrl+R` | Redo |
| `Ctrl+A` | Select all (entire buffer) |
| `Home` | Beginning of line |
| `End` | End of line |
| `Alt+Home` | Beginning of buffer |
| `Alt+End` | End of buffer |
| `Backspace` | Delete before cursor |
| `Delete` (`Esc [3~`) | Delete at cursor |
| `Enter` | New line |

## Selection

Hold `Shift` while using arrow keys, `Page Up`/`Page Down`, `Home`/`End`, or `Alt+Home`/`Alt+End` to extend the selection. Selected text is highlighted in reverse video.

`Ctrl+E` is not used in standalone editor mode. In chat TUI mode, `Ctrl+E` copies the last user or assistant message into the input for editing; `Enter` saves and a bare `Esc` cancels.

## AI continue (`Ctrl+Space`)

Requires a configured provider and model (`pkchat --provider lmstudio -m MODEL --editor file.txt`, or a base URL before `--editor`).

`Ctrl+Space` runs **`/continue`** in **continue** mode:

1. Sends up to 4096 characters immediately before the cursor as context
2. Streams new text after the cursor (thinking traces stay out of the buffer)
3. Shows status in the minibuffer: `thinking...`, `writing.`, `stopped and ready`
4. `Esc` during generation cancels the request but keeps text already streamed

Local endpoints (`lmstudio`, `ollama`, `vllm`, loopback URLs) can auto-pick the first model from `/v1/models` when `--model` is omitted.

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
| `/save` | Save (same as `Ctrl+S`) |
| `/saveas [PATH]` | Save as (same as `Ctrl+Shift+S`; `Tab` completes paths after the command) |
| `/find` | Search (same as `Ctrl+F`) |
| `/replace` | Replace (same as `Ctrl+H`) |
| `/open [PATH]` | Open file (same as `Ctrl+O`; `Tab` completes paths after the command) |
| `/help` | Toggle this help view |
| `/quit` | Quit the editor |

Most commands accept a **mode** (prompted if omitted):

| Mode key | Name | Input to model | Output |
|----------|------|----------------|--------|
| `s` | selection | Selected text | Replace selection in place |
| `a` | all | Whole buffer | Replace entire buffer in place |
| `c` | continue | Text before cursor | Stream after cursor |
| `i` | insert | Selected text | Stream after cursor |

Examples:

```text
/spell selection
/rewrite all
/continue continue
/prompt Summarize the buffer in three bullets
```

`Esc` or `Ctrl+G` cancels the command minibuffer without running a command.

## Provider without AI

Editor mode works without a model. File editing, search, replace, undo, and clipboard still work. AI commands and `Ctrl+Space` report that a provider is required.

## Configuration

`[editor]` in `pkchat` config:

- `undo_limit` — undo depth (default 5)
- `huge_file_size_warning` — confirm before loading huge files (default 1 GiB)
- `file_size_limit` — optional hard load cap

Repeatable `[command]` blocks add or override slash commands. See `README.md` for examples.

## Tips

- Tab completion is **disabled** in standalone editor mode (unlike the chat TUI input).
- Scratch buffers (no path) prompt to save on quit when modified.
- Bracketed paste is enabled for reliable paste detection.
- Invalid UTF-8 in files is preserved; the renderer shows a visible placeholder for bad bytes.