# ainiux keyboard shortcuts

Current bindings for chat, agent, and editor modes.

---

## `--chat` mode (TUI)

### Send, quit, and cancel

| Shortcut | Action |
|----------|--------|
| `Enter` | Send input (or run a single-line `/command`) |
| `Ctrl+S` | Send input |
| `Esc` then `Enter` | Insert newline in input |
| `Alt+Enter` | Newline where the terminal emits it (same intent as `Esc`+`Enter`; shown in status line) |
| `Ctrl+Q` | Quit chat mode |
| `Esc` (alone, no follow-up key within ~25 ms) | Cancel active model request or file job |

### Input editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+A` | Select entire input buffer |
| `Ctrl+C` | Copy selection |
| `Ctrl+X` | Cut selection |
| `Ctrl+V` | Paste (internal clipboard, then bracketed terminal paste) |
| `Ctrl+K` | Kill from cursor to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo last edit (typing, delete, cut, paste) |
| `Ctrl+Y` | Redo |
| `Backspace` | Delete before cursor |
| `Delete` | Delete at cursor |
| `Tab` | Slash-command completion (start of first line) or path completion after `/insert`, `/attach`, `/save`, `/load` |

Bracketed terminal paste (middle-click or Shift+Insert in many terminals) is also undoable with `Ctrl+Z` / `Ctrl+U`.

### Chat-specific actions

| Shortcut | Action |
|----------|--------|
| `Ctrl+E` | Copy last user/assistant message into input for editing (`/edit`) |
| `Ctrl+R` | Regenerate last answer (resend last user prompt) |
| `Ctrl+T` | Toggle thinking-trace display (`/thinking trace` / `notrace`) |
| `Ctrl+L` | Open saved-thread picker (`/list`) |

Use `/pop` to remove the last user or assistant message.

### Cursor and selection (normal chat input)

| Shortcut | Action |
|----------|--------|
| Arrow keys | Move cursor (visual-row movement across soft-wrapped lines) |
| `Shift` + arrows | Extend selection |
| `Home` / `End` | Beginning / end of current line |
| `Ctrl+Home` / `Ctrl+End` | Beginning / end of input buffer |
| `PageUp` / `PageDown` | Page up / down in input (same as editor) |
| `Shift` + `PageUp`/`PageDown`/`Home`/`End` | Extend selection |
| `Ctrl+B` | Scroll chat history back (older messages, half viewport) |
| `Ctrl+D` | Scroll chat history forward (toward live bottom, half viewport) |
| `Alt+Home` / `Alt+End` | Jump to oldest history / live bottom |
| `Alt+PageUp` / `Alt+PageDown` | Scroll chat history (when the terminal sends them; often blocked in SSH clients) |

### Sub-mode shortcuts

**History edit** (`Ctrl+E` / `/edit`) and **system-prompt edit** (`/system`):

- `Enter` or `Ctrl+S` — save
- Bare `Esc` — cancel
- Arrow/`Home`/`End`/etc. — edit the buffer (not history scroll)
- `Ctrl+Z`/`Ctrl+U`, `Ctrl+Y`, copy/cut/paste — same as normal input editing

**Thread list** (`Ctrl+L` / `/list`):

- `↑`/`↓`, `PageUp`/`PageDown`, `Home`/`End` — move selection
- `Enter` — open thread
- `N` — new thread
- `DEL` — delete selected thread (prompts y/n)
- `Esc` — cancel
- `Ctrl+Q` — quit

**Provider/model pickers**, **remove confirm**, **model confirm** — `↑`/`↓`, `Enter`, `y`/`n`/`Esc`, `Ctrl+Q` as appropriate.

### Slash commands (type in input, `Enter` to run if single-line)

`/help`, `/quit`, `/exit`, `/clear`, `/edit`, `/list`, `/new`, `/provider`, `/models`, `/model`, `/system`, `/setting`, `/clone`, `/save`, `/load`, `/remove`, `/remove-empty`, `/pop`, `/response`, `/insert`, `/attach`, `/fetch`, `/search`, `/theme`, `/thinking`

---

## `--editor` mode (standalone `ainiux --editor`)

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
| In replace mode: `Esc` | End replace |

### Window splits (`Ctrl+G` prefix)

| Shortcut | Action |
|----------|--------|
| `Ctrl+G` `v` or `Ctrl+G` `3` | Vertical split (left/right) |
| `Ctrl+G` `h` or `Ctrl+G` `2` | Horizontal split (top/bottom) |
| `Ctrl+G` `o` | Focus next pane |
| `Ctrl+G` `0` | Close focused pane |
| `Ctrl+G` `1` | Maximize focused pane |
| `Esc` or `Ctrl+G` after prefix | Cancel window command |
| `Ctrl+B` | Page up in the other pane (last focused; does not move focus) |
| `Ctrl+D` | Page down in the other pane (same target as Ctrl+B) |
| `/vsplit` | Vertical split (same as `Ctrl+G v`) |
| `/hsplit` | Horizontal split (same as `Ctrl+G h`) |
| `/closesplit` | Close focused pane (same as `Ctrl+G 0`) |
| `/maximize` or `/nosplit` | Maximize focused pane (same as `Ctrl+G 1`) |

### Editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo last edit (typing, delete, cut, paste) |
| `Ctrl+Y` | Redo |
| `Ctrl+A` | Select all |
| `Backspace` | Delete before cursor |
| `Delete` | Delete at cursor |
| `Enter` | Insert newline |
| `Tab` | Complete a word/path/command where applicable; otherwise insert indentation |

Bracketed terminal paste is undoable with `Ctrl+Z` / `Ctrl+U`.

### AI assist (provider + model configured)

| Shortcut | Action |
|----------|--------|
| `Ctrl+Space` | Run mode-aware `/continue`: prose in text/Markdown, gap completion in code modes |
| `Esc` (during generation) | Cancel AI request (keeps streamed text) |
| `Esc` (idle) | Open command minibuffer (`Command:`) |
| `Esc` `/help` | Toggle read-only help view |
| `Esc` | Cancel minibuffer / command entry |

### Cursor and selection

| Shortcut | Action |
|----------|--------|
| Arrow keys | Move cursor (**logical-line** up/down) |
| `Shift` + arrows | Extend selection |
| `Home` / `End` | Beginning / end of **current line** |
| `Ctrl+Home` / `Ctrl+End` | Beginning / end of **buffer** |
| `PageUp` / `PageDown` | Move/scroll in document |
| `Shift` + movement keys | Extend selection |

### Slash commands (via `Esc` → command minibuffer, `Tab` completes)

Built-in AI: `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/English`, `/Chinese`, `/Finnish`, `/German`, `/French`, `/Italian`, `/Spanish`, `/Portuguese`, `/Arabic`, `/Hindi`, `/Japanese`, `/Korean`, `/Swedish`, `/Polish`, `/Russian`, `/prompt`, `/regenerate`

File/editor: `/save`, `/saveas`, `/find`, `/replace`, `/open`, `/new`, `/list`, `/close`, `/vsplit`, `/hsplit`, `/closesplit`, `/maximize` (`/nosplit`), `/help`, `/quit`

Also: `/provider`, `/model`, `/search QUERY`

---

## `--agent` mode

Agent mode shares the terminal input editor, scrolling, provider/model/reasoning
pickers, and cancellation keys with chat. Its transcript and tools are
project-local under `.ainiux-pr/`; ordinary chat thread commands are not agent
session management.

| Shortcut | Action |
|----------|--------|
| `Enter` / `Ctrl+S` | Submit the next agent goal or follow-up |
| `Esc` | Cancel the active model/tool job |
| `Ctrl+P` | Cycle agent → chat |
| `Ctrl+Q` | Finish the project session and quit |
| `y` / `n` | Allow or deny an active Guard Ask confirmation |

Use `/chat`, `/editor`, `/agent`, `/mode`, or `/cycle` for explicit mode
handoffs. Manual `/compact`, plan mode, and refactor mode are not implemented
yet.

---

## Shared movement and editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+Z` or `Ctrl+U` | Undo |
| `Ctrl+Y` | Redo |
| `Home` / `End` | Beginning / end of current line |
| `Ctrl+Home` / `Ctrl+End` | Beginning / end of buffer |
| `PageUp` / `PageDown` | Page up / down in input or document |
| `Ctrl+Q` | Quit |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` / `Ctrl+K` / `Ctrl+A` | Copy / cut / paste / kill line / select all |
| `Shift` + movement keys | Extend selection |

## Shortcuts that differ between modes

| Shortcut | `--chat` | `--editor` |
|----------|----------|------------|
| `Enter` | Send message | Insert newline |
| `Ctrl+S` | Send message | Save file |
| `Ctrl+L` | Thread list (`/list`) | Buffer list (`/list`) |
| `Ctrl+E` | Edit last chat message | **Unused** |
| `Ctrl+R` | Regenerate last answer | **Unused** (redo is `Ctrl+Y` only) |
| `Esc` (idle) | Cancel in-flight job | Open slash-command minibuffer |
| `Ctrl+B` / `Ctrl+D` | Scroll chat history back / forward | Page up / down in the other split pane |
| `Alt+PageUp` / `Alt+PageDown` | Scroll chat history (if terminal allows) | **Unused** |
| `Tab` | Command/path completion | Word/path/command completion or indentation |
| `↑` / `↓` | Visual-row movement (soft wrap) | Logical-line movement |
