# Dired mode (directory browser)

Dired is a full-screen directory browser inside the **standalone editor**. It is inspired by GNU Emacs dired, but the key map and goals are Ainiux-specific: quick navigation, read-only file preview with syntax highlight, light file operations, and content-hash markers for files that changed since you last reviewed the listing.

This guide is the full reference. The editor’s embedded help (`Ctrl+H`) and [keyboard shortcuts](keyboard-shortcuts.md) summarize the same bindings.

## Start dired

| Entry | Notes |
| --- | --- |
| `ainiux --dired` | Open editor and enter dired in the **current directory** |
| `ainiux --dired PATH` | Directory or simple glob (e.g. `src/`, `src/*.cpp`) |
| `ainiux --dired=PATH` | Equals form |
| `F4` | From an already running editor |
| `Ctrl+X` then `d` | Emacs-style window prefix |
| `dired` / `/dired [PATH\|glob]` | Command minibuffer (`Ctrl+E`, `Esc`, or `Alt+X`); **Tab** completes paths |

Provider profiles may precede startup flags, for example:

```sh
ainiux none --dired src/
ainiux lmstudio --dired .
```

`--dired` implies editor mode. It uses the same mutual-exclusion rules as `--editor` (not combined with `--chat`, `--agent`, `--repl`, and so on).

## Screen layout

Dired takes the **entire** editor screen (outer splits are restored when you leave).

| Region | Content |
| --- | --- |
| Panel title | Short status: `Dired  sort:name^  [LIST]` or `[VIEW RET=list]` |
| First two body lines | One-key command cheat sheet |
| Remaining body | Directory listing |
| Status / minibuffer | Path, selection summary, prompts |

Sort markers use ASCII: **`^`** ascending, **`v`** descending (for example `name^`, `sizev`, `date^`).

### Listing columns

Each file or directory row includes:

- Selection marker (`>` on the current line)
- Dirty marker (`*` when content hash differs from the review baseline)
- **On Linux/POSIX:** mode (`-rw-r--r--`, `drwxr-xr-x`, …), owner, group (ls-style; sticky/setuid/setgid letters included)
- Size (or `DIR` for directories)
- Last modified time
- Name (directories end with `/`; `../` is parent; symlinks may show `@`)

Windows builds omit mode, owner, and group columns.

Example (Linux):

```text
  RET view  o edit  g refresh  r rename  c copy  d del  n file  m dir
  t touch  f find  * reviewed  left=parent  right=enter dir  q quit
> * -rw-r--r-- eye      eye          1234  2026-08-03 12:00  main.cpp
    drwxr-xr-x eye      eye           DIR  2026-08-03 11:50  src/
    ../
```

## Focus: list vs view

| Focus | How you get there | Navigation |
| --- | --- | --- |
| **List** (default) | Enter dired; press **Enter** while viewing a file | Up/down, page, home/end move the **selection** in the listing |
| **View** | **Enter** on a regular file | Arrows / page / home/end scroll the **file**; `Ctrl+Home` / `Ctrl+End` go to start/end of the file |

- **Enter** on a directory enters it; on `../` goes to the parent.
- **Enter** again while viewing returns to the **list**.
- Title shows `[LIST]` or `[VIEW RET=list]` so the active surface is obvious.
- While dired is open, bare **Esc** does **not** open the editor command minibuffer (it only cancels prompts or pending sort).

## Keyboard reference

### Leave dired / quit ainiux

| Key | Action |
| --- | --- |
| `q` | Leave dired and return to **editor** mode |
| `Ctrl+Q` | Quit **ainiux** (global; same save prompts as the editor) |

### Navigation

| Key | Action |
| --- | --- |
| `↑` / `↓` | Previous / next entry (list) or line (view) |
| `PageUp` / `PageDown` | Page the list or the viewed file |
| `Home` / `End` | First / last list entry, or start / end of line in view |
| `Ctrl+Home` / `Ctrl+End` | Start / end of the **viewed** file |
| `←` | Parent directory (reselects the folder you left when possible) |
| `→` | Enter selected directory when it is a directory (message if selection is a file) |
| `Enter` | Enter directory / parent, open read-only file view, or leave view back to list |
| `g` | Refresh listing and recompute dirty markers against the current baseline |

### Open and edit

| Key | Action |
| --- | --- |
| `o` | Open the selected **file** for editing and **exit dired** |
| `n` | Create a new file (path prompt; opens it in the editor and leaves dired) |
| `m` | Create a directory with `mkdir -p` semantics (e.g. `templates/poll`) |

### File operations (minibuffer prompts; **Esc** cancels)

| Key | Action |
| --- | --- |
| `r` / `R` | Rename or move (confirm before overwrite) |
| `c` | Copy (confirm before overwrite) |
| `d` | Delete (`y`/`n`; non-empty directories require recursive confirmation) |
| `t` | Touch (update modification time) |

### Sort

Press **`s`**, then a second key:

| Second key | Meaning |
| --- | --- |
| `(n)ame` | Sort by name, ascending |
| `(s)ize` | Sort by size, ascending |
| `(d)ate` | Sort by last modified, ascending |
| `(N)ame` | Sort by name, descending |
| `(S)ize` | Sort by size, descending |
| `(D)ate` | Sort by date, descending |

Directories are listed before regular files; `../` stays first. The title updates to the active criterion (for example `sort:datev`).

### Search in the viewed file

| Key | Action |
| --- | --- |
| `f` | Find (only meaningful while **viewing** a file) |
| `F3` | Search next (after a find) |
| `Shift+F3` | Search previous |

In list focus, `f` reminds you to open a file with Enter first.

### Review / dirty markers

| Key | Action |
| --- | --- |
| `*` | Mark the current listing as **reviewed** (clear dirty `*` markers) |

**Dirty** means the file’s **content hash** differs from the review baseline—not merely that mtime changed, and not requiring git. The baseline is established on the first dired open in the session (when empty) and whenever you press `*`. Refresh (`g`) recomputes hashes and updates markers without changing the baseline.

Git status coloring and freeform git commands are **not** part of this release.

## Capabilities and limits

**Supported**

- Browse directories and simple globs (`*`, `?` in the last path component)
- Read-only preview with the editor’s syntax highlighter when the extension is known
- Rename, copy, delete, touch, create file, create nested directories
- Content-hash “changed since reviewed” markers
- POSIX permission/owner/group display on non-Windows builds
- Startup via CLI, function key, Ctrl+X prefix, or slash command

**Not yet**

- Multi-file marks (operate on several selected entries at once)
- Trash / undelete
- Git status colors or in-dired git commands
- Recursive `**` globs without bounds
- New theme color roles dedicated to dired (dirty state uses a text marker)

## Tips for agentic coding

1. After an agent turn, open dired on the project root (`F4` or `ainiux --dired .`).
2. Use `g` to refresh and scan for `*` dirty files, or press `*` after a clean review pass.
3. **Enter** to preview one file at a time with highlight; **Enter** again for the list.
4. **`o`** when you need to edit; that exits dired into the normal multi-buffer editor.
5. Use `←` / `→` to walk the tree without hunting for `../`.

## Related documentation

- [Editor help](editor_help.md) — full editor reference (embedded in the binary via install)
- [Keyboard shortcuts](keyboard-shortcuts.md) — chat, editor, agent, and dired tables
- [CLI and scripting](cli.md) — other command-line modes
- [Documentation index](README.md)
