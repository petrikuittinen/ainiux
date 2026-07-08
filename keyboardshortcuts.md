# pkchat keyboard shortcuts

Current bindings as implemented in `src/tui/run.cpp`, `src/tui/input_handlers.cpp`, `src/editor/run_editor.cpp`, and `docs/editor_help.md`.

---

## `--chat` mode (full-screen chat TUI)

### Send, quit, and cancel

| Shortcut | Action |
|----------|--------|
| `Enter` | Send input (or run a single-line `/command`) |
| `Ctrl+S` | Send input |
| `Esc` then `Enter` | Insert newline in input |
| `Alt+Enter` | Newline where the terminal emits it (same intent as `Esc`+`Enter`; shown in status line) |
| `Ctrl+Q` | Quit chat mode |
| `Ctrl+D` | Quit when input is empty |
| `Esc` (alone, no follow-up key within ~25 ms) | Cancel active model request or file job |

### Input editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+A` | Select entire input buffer |
| `Ctrl+C` | Copy selection |
| `Ctrl+X` | Cut selection |
| `Ctrl+V` | Paste (internal clipboard, then bracketed terminal paste) |
| `Ctrl+K` | Kill from cursor to end of line |
| `Ctrl+U` | Undo |
| `Ctrl+R` | Redo |
| `Backspace` | Delete before cursor |
| `Delete` | Delete at cursor |
| `Tab` | Slash-command completion (start of first line) or path completion after `/insert`, `/attach`, `/save`, `/load` |

### Chat-specific actions

| Shortcut | Action |
|----------|--------|
| `Ctrl+E` | Copy last user/assistant message into input for editing (`/edit`) |
| `Ctrl+P` | Pop last user or assistant message (`/pop`) |
| `Ctrl+T` | Toggle thinking-trace display (`/thinking trace` / `notrace`) |
| `Ctrl+L` | Open saved-thread picker (`/list`) |
| `Esc` then `R` / `Alt+R` | Regenerate last answer |

### Cursor and selection (normal chat input)

| Shortcut | Action |
|----------|--------|
| Arrow keys | Move cursor (visual-row movement across soft-wrapped lines) |
| `Shift` + arrows | Extend selection |
| `Alt+Home` / `Alt+End` | Jump to start/end of input buffer |
| `Shift` + `PageUp`/`PageDown`/`Home`/`End` | Extend selection |
| `Home` / `End` | Scroll **chat history** to thread start / live bottom |
| `PageUp` / `PageDown` | Scroll **chat history** (half viewport step) |

### Sub-mode shortcuts

**History edit** (`Ctrl+E` / `/edit`) and **system-prompt edit** (`/system`):

- `Enter` or `Ctrl+S` — save
- Bare `Esc` — cancel
- Arrow/`Home`/`End`/etc. — edit the buffer (not history scroll)

**Thread list** (`Ctrl+L` / `/list`):

- `↑`/`↓`, `PageUp`/`PageDown`, `Home`/`End` — move selection
- `Enter` — open thread
- `N` — new thread
- `Esc` — cancel
- `Ctrl+Q` — quit

**Provider/model pickers**, **remove confirm**, **model confirm** — `↑`/`↓`, `Enter`, `y`/`n`/`Esc`, `Ctrl+Q` as appropriate.

### Slash commands (type in input, `Enter` to run if single-line)

`/help`, `/quit`, `/exit`, `/clear`, `/edit`, `/list`, `/new`, `/provider`, `/models`, `/model`, `/system`, `/setting`, `/clone`, `/save`, `/load`, `/remove`, `/remove-empty`, `/pop`, `/response`, `/insert`, `/attach`, `/fetch`, `/search`, `/theme`, `/thinking`

---

## `--editor` mode (standalone `pkchat --editor`)

### File and buffer management

| Shortcut | Action |
|----------|--------|
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save as |
| `Ctrl+O` | Open another file buffer |
| `Ctrl+N` | New empty buffer |
| `Ctrl+L` | List/switch open buffers |
| `Ctrl+W` | Close active buffer (prompt if modified) |
| `Ctrl+Q` | Quit (save prompts as needed) |

### Search and replace

| Shortcut | Action |
|----------|--------|
| `Ctrl+F` | Search |
| `Ctrl+H` | Replace |
| `F3` | Search next |
| `Shift+F3` | Search previous |
| In replace mode: `Space` | Replace current match |
| In replace mode: `s` | Skip |
| In replace mode: `a` | Replace all remaining |
| In replace mode: `Esc` / `Ctrl+G` | End replace |

### Editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+U` / `Ctrl+R` | Undo / redo |
| `Ctrl+A` | Select all |
| `Backspace` | Delete before cursor |
| `Delete` | Delete at cursor |
| `Enter` | Insert newline |
| `Tab` | Disabled (“Tab completion is disabled in editor mode”) |

### AI assist (provider + model configured)

| Shortcut | Action |
|----------|--------|
| `Ctrl+Space` | Run `/continue` at cursor |
| `Esc` (during generation) | Cancel AI request (keeps streamed text) |
| `Esc` (idle) | Open command minibuffer (`Command:`) |
| `Esc` `/help` | Toggle read-only help view |
| `Esc` or `Ctrl+G` | Cancel minibuffer / command entry |

### Cursor and selection

| Shortcut | Action |
|----------|--------|
| Arrow keys | Move cursor (**logical-line** up/down) |
| `Shift` + arrows | Extend selection |
| `Home` / `End` | Beginning / end of **current line** |
| `Alt+Home` / `Alt+End` | Beginning / end of **buffer** |
| `PageUp` / `PageDown` | Move/scroll in document |
| `Shift` + movement keys | Extend selection |

### Slash commands (via `Esc` → command minibuffer, `Tab` completes)

Built-in AI: `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, `/Finnish`, `/prompt`, `/regenerate`

File/editor: `/save`, `/saveas`, `/find`, `/replace`, `/open`, `/new`, `/list`, `/close`, `/help`, `/quit`

Also: `/provider`, `/model`, `/search QUERY`

---

## Shortcuts that differ between modes

### Same key, different action

| Shortcut | `--chat` | `--editor` |
|----------|----------|------------|
| `Enter` | Send message | Insert newline |
| `Ctrl+S` | Send message | Save file |
| `Ctrl+L` | Thread list (`/list`) | Buffer list (`/list`) |
| `Ctrl+E` | Edit last chat message | **Unused** |
| `Esc` (idle) | Cancel in-flight job | Open slash-command minibuffer |
| `Home` / `End` | Scroll chat history | Move to line start/end |
| `PageUp` / `PageDown` | Scroll chat history | Move/scroll in document |
| `Tab` | Command/path completion | Disabled |
| `↑` / `↓` | Visual-row movement (soft wrap) | Logical-line movement |

### Present in one mode only

| Shortcut | Mode | Action |
|----------|------|--------|
| `Ctrl+D` | Chat only | Quit when input empty |
| `Ctrl+E` | Chat only | Edit last message |
| `Ctrl+P` | Chat only | Pop last message |
| `Ctrl+T` | Chat only | Toggle thinking traces |
| `Esc`+`Enter` / `Alt+Enter` | Chat only | Insert newline |
| `Esc`+`R` / `Alt+R` | Chat only | Regenerate last answer |
| `Ctrl+N` | Editor only | New buffer |
| `Ctrl+O` | Editor only | Open file |
| `Ctrl+W` | Editor only | Close buffer |
| `Ctrl+Shift+S` | Editor only | Save as |
| `Ctrl+F` / `Ctrl+H` | Editor only | Search / replace |
| `F3` / `Shift+F3` | Editor only | Search next/previous |
| `Ctrl+Space` | Editor only | AI continue at cursor |
| `Ctrl+G` | Editor only | Cancel minibuffer/replace |

### Shared (same behavior in both)

`Ctrl+Q`, `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+K`, `Ctrl+U`, `Ctrl+A`, `Backspace`, `Delete`, bracketed paste, `Shift`+movement for selection, `Alt+Home`/`Alt+End` for buffer bounds.

**Note on `Ctrl+R`:** in both modes it is **redo**. Regenerate in chat is **`Esc`+`R`** or **`Alt+R`**, not `Ctrl+R`.

### Slash-command UX differs

| | `--chat` | `--editor` |
|--|----------|------------|
| How to run | Type `/command` directly in input | Press `Esc`, then type in minibuffer |
| Tab completion | Yes (commands + some paths) | Yes in command minibuffer; disabled in main buffer |
| Command sets | Chat/session oriented (`/load`, `/theme`, `/remove`, …) | File/AI oriented (`/spell`, `/open`, `/saveas`, …) |