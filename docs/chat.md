# Chat TUI

Start ordinary full-screen chat with `-c`:

```sh
ainiux lmstudio -c
ainiux openrouter -m MODEL -c
```

Chat is conversation only. It does not enable agent workspace tools. Use `-a` for the separate [agent workflow](agent.md).

Native Windows full-screen modes require Windows Terminal or modern conhost;
mintty/non-console sessions are rejected with a clear error. Ainiux consumes
mouse-wheel reports, so hold `Shift` while dragging for native terminal text
selection. See [Native Windows](windows.md).

## Threads and persistence

Chat opens the thread selector at startup. Choose a saved thread or press `Tab`/`Insert` for a new one. `Ctrl+L` or `/list` reopens the selector. Threads are stored newest-first in `~/.ainiux/ainiux.db` using SQLite WAL mode.

Thread records include messages, provider, endpoint, model, generation settings, usage, attachments, and compaction events. Keys and authorization headers are not stored. Text and small canonical Markdown attachments may stay in SQLite; images and larger managed Markdown are content-addressed below `~/.ainiux/media/`.

A thread with missing managed media stays readable but becomes read-only. `/cleanup` removes old unreferenced managed media according to configuration. JSON `/save` and `/load` provide explicit import/export and are separate from the thread library.

## Input and history

`Enter` or `Ctrl+S` sends. `Esc` then `Enter` inserts a newline. The shared editor input supports grapheme-aware navigation, selection, copy/paste, undo/redo, soft wrapping, and syntax highlighting. History prose wraps on word boundaries so a long sentence does not split mid-word; Markdown fenced code keeps column wrap. Up on the first visual line and Down on the last visual line recall earlier user prompts from the current thread, including prompts loaded from the chat library. Agent-mode prompts are not mixed into chat recall.

`/clear` in chat deletes the thread messages and restores the system prompt. In agent mode the same command only hides the visible window and does not change model context.

The line above the input shows version, mode, model and reasoning, estimated token usage (and percent of the known context window), and provider credits when available (for example OpenRouter or DeepSeek). When no model is selected it shows `[choose model /model]`. Custom URL providers display as short `custom`, never as long registry ids such as `custom_openai_chat`. History navigation help (`Ctrl+B` / `Ctrl+D`) lives on the status row with TTFT or response metrics after a reply.

Common controls:

| Key | Action |
| --- | --- |
| `Ctrl+E` | Edit the last user or assistant message |
| `Ctrl+R` | Regenerate the previous answer |
| `Ctrl+P` | Choose provider, then model when needed |
| `Ctrl+L` | Open thread library |
| `Ctrl+B` / `Ctrl+D` | Scroll history backward/forward |
| `Ctrl+O` | Toggle the history scrollbar |
| `Ctrl+G` | Open the editor; press again to return |
| `Esc` | Cancel an active request or file job |
| `Ctrl+Q` | Quit |

See [Keyboard shortcuts](keyboard-shortcuts.md) for selection, clipboard, picker, and terminal-specific details.

## Commands

Core commands include `/help`, `/new`, `/list`, `/edit`, `/provider`, `/model`, `/system`, `/setting`, `/clone`, `/save`, `/load`, `/remove`, `/pop`, `/response`, `/insert`, `/attach`, `/fetch`, `/search`, `/theme`, `/scrollbar`, and `/thinking`.

`/insert` places text into the input. `/attach` adds provider context or a supported image. `/fetch` and `/search` are explicit network operations. A URL typed in ordinary prompt text is not fetched.

Editor-only `/width`, `/alignment-width`, `/left-align`, `/right-align`, `/center-align`, and `/justify` commands are rejected in chat and agent history. They operate on editor buffers only.

## Attachments and media

Text, Markdown, and HTML are converted into bounded canonical Markdown. PNG, JPEG, and GIF attachments require a compatible Chat Completions model. PDF and DOCX are not supported. Managed media cleanup never turns missing content into silent empty context; affected threads are marked read-only.

## Themes, highlighting, and thinking

`/theme` lists or selects layered themes. `/theme off`, `--theme off`, `--nocolors`, or configuration can disable color styling without breaking layout control sequences. `--theme NAME` selects a palette at startup (for example `--theme light`). `--color-mode auto|truecolor|256|16` (and `[tui] color_mode`) chooses how RGB theme colors are emitted; `auto` prefers truecolor when `COLORTERM` advertises it and otherwise uses 256-color for common `TERM` values—useful when Windows Terminal over SSH shows unreadable blue/red chrome. `/highlight on|off` controls shared syntax styling. `/thinking show|hide` and `Alt+Ctrl+T` control visible reasoning traces where the provider supplies them.

Display-only notices and thinking previews are excluded from provider context, compaction, and transcript token estimates. Raw saved messages remain the durable source.

## Mode switching

Use `/editor`, `/agent`, `/chat`, `/mode`, `/cycle`, or `Ctrl+G` for explicit handoff. Provider, model, theme, and relevant UI state follow the transition. Moving to agent mode starts separate project-local semantics; it does not retrofit tools into the current chat transcript. From agent mode, `Ctrl+G` may open the editor while a turn is still running so you can review workspace changes; return with `Ctrl+G` to reattach the agent view.

Related documentation: [documentation index](README.md), [editor help](editor_help.md), [agent workflows](agent.md), [configuration](configuration.md), [security](security.md).
