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
| `Ctrl+C` | Copy input selection; with no selection, copy the last user/assistant message |
| `Ctrl+V` | Paste (internal clipboard first, then desktop/OSC 52 clipboard) |
| `Ctrl+K` | Kill from cursor to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo last edit (typing, delete, cut, paste) |
| `Ctrl+Y` | Redo |
| `Backspace` | Delete before cursor (with a selection: cut to clipboard) |
| `Delete` | Delete at cursor (with a selection: cut to clipboard) |
| `Tab` | Slash-command completion (start of first line) or path completion after `/insert`, `/attach`, `/save`, `/load` |

Copy and selection-delete (Backspace/Delete on a range) publish to native desktop helpers and OSC 52 when available. With no input selection, `Ctrl+C` copies the last user/assistant message from the stored transcript, preserving its original newlines and excluding soft-wrap padding, labels, and scrollbar glyphs. This is the reliable way to copy history from full-screen terminal clients whose mouse selection copies painted screen rows. With an empty internal clipboard, `Ctrl+V` reads external text asynchronously; SSH prefers a terminal OSC 52 query. Bracketed terminal paste (middle-click or Shift+Insert in many terminals) remains the fallback and is also undoable with `Ctrl+Z` / `Ctrl+U`. There is no dedicated cut key; delete the selection with Backspace or Delete, then paste with `Ctrl+V`.

The mouse wheel scrolls one rendered visual row per event. Chat and agent scroll only while the pointer is over history (or the visible help panel); the editor scrolls only the focused pane's content and does not move the caret. Native terminal selection may require `Shift+drag` while full-screen mouse reporting is active.

### Chat-specific actions

| Shortcut | Action |
|----------|--------|
| `Ctrl+H` | Toggle mode help panel (same as `/help`) |
| `Ctrl+E` | Copy last user/assistant message into input for editing (`/edit`) |
| `Ctrl+R` | Regenerate last answer (resend last user prompt) |
| `Alt+Ctrl+T` | Toggle thinking-trace display (`/thinking show` / `hide`) |
| `Ctrl+W` | Toggle traces in ordinary chat; close buffer in editor |
| `Ctrl+L` | Open saved-thread picker (`/list`; also shown on chat startup) |
| `Ctrl+P` | Open provider picker (same as bare `/provider`; model picker follows when needed) |
| `Ctrl+O` | Toggle history scrollbar visibility (`/scrollbar show|hide`) |
| `Ctrl+G` | Toggle chat → editor; the next Ctrl+G returns editor → chat |

Chat mode opens the **thread selector** on startup (same UI as `Ctrl+L` / `/list`). Choose an existing thread or press **Tab** / **Insert** for a new one. If the CLI did not set a provider/model and the selected thread has them saved, those values are restored.

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
- `Ctrl+Z`/`Ctrl+U`, `Ctrl+Y`, copy/paste — same as normal input editing

**Thread list** (`Ctrl+L` / `/list`):

- `↑`/`↓`, `PageUp`/`PageDown`, `Home`/`End` — move selection
- Letter/number keys — jump to the next label containing that character (wraps; no match keeps selection)
- `Enter` — open thread
- `Tab` / `Insert` — new thread
- `DEL` — delete selected thread (prompts y/n)
- `Ctrl+H` — toggle help
- `Esc` — cancel
- `Ctrl+Q` — quit

**Provider/model pickers** — `↑`/`↓`, type-to-jump (same character search), `Enter`, `Esc`, `Ctrl+Q`. **Remove confirm** / **model confirm** — `y`/`n`/`Esc`, `Ctrl+Q` as appropriate.

### Slash commands (type in input, `Enter` to run if single-line)

`/help`, `/quit`, `/exit`, `/clear`, `/edit`, `/list`, `/new`, `/provider`, `/models`, `/model`, `/system`, `/setting`, `/clone`, `/save`, `/load`, `/remove`, `/remove-empty`, `/pop`, `/response`, `/insert`, `/attach`, `/fetch`, `/search`, `/theme`, `/scrollbar`, `/thinking`

---

## `--editor` mode (standalone `ainiux --editor`)

Press `Ctrl+E`, `Esc`, or `Alt+X` to open the command minibuffer. `Ctrl+E` is the recommended timing-independent shortcut; `Alt+X` remains available. Enhanced Escape sequences are accepted, and key-repeat from holding `Esc` is ignored until the repeat burst stops or another key is typed; a later `Esc` cancels normally.

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
| `Esc` then `replace-string` / `/replace` | Replace (search, then replace each match) |
| `F3` | Search next |
| `Shift+F3` | Search previous |
| In replace mode: `Space` | Replace current match |
| In replace mode: `s` | Skip |
| In replace mode: `a` | Replace all remaining |
| In replace mode: `Esc` | End replace |

### Window splits (`Ctrl+X` prefix)

| Shortcut | Action |
|----------|--------|
| `Ctrl+X` `v` or `Ctrl+X` `3` | Vertical split (left/right) |
| `Ctrl+X` `h` or `Ctrl+X` `2` | Horizontal split (top/bottom) |
| `Ctrl+X` `o` | Focus next pane |
| `Ctrl+X` `0` | Close focused pane |
| `Ctrl+X` `1` | Maximize focused pane |
| `Esc` or `Ctrl+X` after prefix | Cancel window command |
| `Ctrl+B` | Page up in the other pane (last focused; does not move focus) |
| `Ctrl+D` | Page down in the other pane (same target as Ctrl+B) |
| `/vsplit` | Vertical split (same as `Ctrl+X v`) |
| `/hsplit` | Horizontal split (same as `Ctrl+X h`) |
| `/closesplit` | Close focused pane (same as `Ctrl+X 0`) |
| `/maximize` or `/nosplit` | Maximize focused pane (same as `Ctrl+X 1`) |

### Editing

| Shortcut | Action |
|----------|--------|
| `Ctrl+C` / `Ctrl+V` | Copy / paste |
| `Ctrl+K` | Kill to end of line |
| `Ctrl+Z` or `Ctrl+U` | Undo last edit (typing, delete, cut, paste) |
| `Ctrl+Y` | Redo |
| `Ctrl+A` | Select all |
| `Backspace` | Delete before cursor (with a selection: cut to clipboard) |
| `Delete` | Delete at cursor (with a selection: cut to clipboard) |
| `Enter` | Insert newline |
| `Tab` | Complete a word/path/command where applicable; otherwise insert indentation |

Bracketed terminal paste is undoable with `Ctrl+Z` / `Ctrl+U`.

### AI assist (provider + model configured)

| Shortcut | Action |
|----------|--------|
| `Ctrl+Space` | Run mode-aware `/continue`: prose in text/Markdown, gap completion in code modes |
| `Esc` (during generation) | Cancel AI request (keeps streamed text) |
| `Esc` (idle) | Open command minibuffer (`Command:`) |
| `Ctrl+E` (idle) | Open command minibuffer (recommended portable shortcut) |
| `Alt+X` (idle) | Open command minibuffer without relying on bare-Escape timing |
| `Ctrl+H` | Toggle read-only help view (same as `Esc` `/help`) |
| `Esc` `/help` | Toggle read-only help view |
| `Esc` | Cancel minibuffer / command entry |

### Mode switching

| Shortcut | Action |
|----------|--------|
| `Ctrl+G` | Return to the mode that opened the editor (chat or agent); standalone editor defaults to chat |
| `Ctrl+P` | Open provider picker (same as bare `/provider`; model picker follows when needed) |

Editor mode accepts `chat`, `agent`, and `editor` with or without a leading
slash. Chat and agent modes use `/chat`, `/agent`, and `/editor`.

### Cursor and selection

| Shortcut | Action |
|----------|--------|
| Arrow keys | Move cursor (**logical-line** up/down) |
| `Shift` + arrows | Extend selection |
| `Home` / `End` | Beginning / end of **current line** |
| `Ctrl+Home` / `Ctrl+End` | Beginning / end of **buffer** |
| `PageUp` / `PageDown` | Move/scroll in document |
| `Shift` + movement keys | Extend selection |

### Slash commands (via `Ctrl+E`, `Esc`, or `Alt+X`; `Tab` completes)

Built-in AI: `/spell`, `/grammar`, `/continue`, `/fact`, `/comment`, `/rewrite`, `/style-formal`, `/style-casual`, `/style-humor`, `/marketing`, `/English`, `/Chinese`, `/Finnish`, `/German`, `/French`, `/Italian`, `/Spanish`, `/Portuguese`, `/Arabic`, `/Hindi`, `/Japanese`, `/Korean`, `/Swedish`, `/Polish`, `/Russian`, `/prompt`, `/regenerate` (and many more from `editor-commands.conf`)

File/editor: `/save`, `/saveas`, `/find`, `/replace` (`replace-string`), `/open`, `/new`, `/list`, `/close`, `/vsplit`, `/hsplit`, `/closesplit`, `/maximize` (`/nosplit`), `/chat`, `/agent`, `/editor`, `/help`, `/quit`

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
| `Ctrl+H` | Toggle mode help panel (same as `/help`) |
| `Ctrl+P` | Open provider picker (same as bare `/provider`; model picker follows when needed) |
| `Ctrl+O` | Toggle history scrollbar visibility (`/scrollbar show|hide`) |
| `Ctrl+G` | Toggle agent → editor; the next Ctrl+G returns editor → agent |
| `Ctrl+Q` | Finish the project session and quit |
| `y` / `n` | Allow or deny an active Guard Ask confirmation |

Use `/chat`, `/editor`, `/agent`, or `/mode` for explicit mode handoffs.
`/cycle` follows Ctrl+G and enters the editor from chat or agent. Manual
`/compact` preserves the full transcript while compacting model-visible context. In interactive agent mode, `/plan` selects planning mode and `/act` returns to full coding mode. `/goal [condition|clear|pause|resume]` sets a persistent completion condition; the agent auto-continues until it calls `goal_met` with evidence, stalls/blocks, hits the goal turn cap, or the user interrupts. Refactor mode is not implemented yet. New agent projects require explicit `/new` (Tab/Insert do not create one).

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
| `Ctrl+C` / `Ctrl+V` / `Ctrl+K` / `Ctrl+A` | Copy / paste / kill line / select all |
| `Backspace` / `Delete` on a selection | Cut selection to clipboard |
| `Shift` + movement keys | Extend selection |

## Shortcuts that differ between modes

| Shortcut | `--chat` | `--editor` |
|----------|----------|------------|
| `Enter` | Send message | Insert newline |
| `Ctrl+S` | Send message | Save file |
| `Ctrl+L` | Thread list (`/list`) | Buffer list (`/list`) |
| `Ctrl+H` | Toggle chat help panel | Toggle editor help view |
| `Ctrl+X` | **Unused** (no dedicated cut) | Window-command prefix (splits) |
| `Ctrl+E` | Edit last chat message | **Unused** |
| `Ctrl+R` | Regenerate last answer | **Unused** (redo is `Ctrl+Y` only) |
| `Ctrl+O` | Toggle history scrollbar | Open file buffer |
| `Esc` (idle) | Cancel in-flight job | Open slash-command minibuffer |
| `Ctrl+B` / `Ctrl+D` | Scroll chat history back / forward | Page up / down in the other split pane |
| `Alt+PageUp` / `Alt+PageDown` | Scroll chat history (if terminal allows) | **Unused** |
| `Tab` | Command/path completion | Word/path/command completion or indentation |
| `↑` / `↓` | Visual-row movement (soft wrap) | Logical-line movement |
