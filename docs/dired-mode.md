# Dired mode (directory browser)

Dired is a full-screen directory browser inside the **standalone editor**. It is inspired by GNU Emacs dired, but the key map and goals are Ainiux-specific: quick navigation, read-only file preview with syntax highlight, light file operations, and content-hash markers for files that changed since you last reviewed the listing.

This guide is the full reference. The editor’s embedded help (`Ctrl+H`) and [keyboard shortcuts](keyboard-shortcuts.md) summarize the same bindings.

## Start dired

| Entry | Notes |
| --- | --- |
| `ainiux -d` or `ainiux --dired` | Open editor and enter dired in the **current directory** |
| `ainiux -d PATH` or `ainiux --dired PATH` | Directory or simple glob (e.g. `src/`, `src/*.cpp`) |
| `ainiux --dired=PATH` | Equals form (long option) |
| `F4` | From an already running editor |
| `Ctrl+X` then `d` | Emacs-style window prefix |
| `dired` / `/dired [PATH\|glob]` | Command minibuffer (`Ctrl+E`, `Esc`, or `Alt+X`); **Tab** completes paths |

Provider profiles may precede startup flags, for example:

```sh
ainiux -d
ainiux none -d src/
ainiux lmstudio --dired .
```

`-d` / `--dired` implies editor mode. It uses the same mutual-exclusion rules as `--editor` (not combined with `--chat`, `--agent`, `--repl`, and so on).

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

Windows builds omit mode, owner, and group columns. They use native hidden,
read-only, directory, and reparse-point attributes; `.com`, `.exe`, `.bat`, and
`.cmd` are executable types. Name sort and simple glob matching are
case-insensitive with deterministic original-name ties. ACL-denied operations
still return their ordinary filesystem error instead of inventing POSIX metadata.

### Listing colors

Dired reuses existing theme roles (no extra palette keys). Typical mapping:

| Kind | Theme role | Notes |
| --- | --- | --- |
| Help lines | `panel_hint` | Key cheat sheet |
| Mode / owner / size / mtime | `muted` | Secondary metadata |
| Parent `../` | `muted` | Navigation |
| Directory | `user_label` | Distinct from files |
| Hidden directory (`.git`, …) | `muted` | Dimmer than normal directories |
| Reviewed / clean file | `panel_body` | Default listing text |
| Hidden file (`.env`, …) | `muted` | Dimmer than normal files |
| Executable file | `assistant_label` | Green-style cue when mode has `x`/`s`/`t` |
| Dirty file name | `syntax_emphasis` | Warm emphasis vs clean body |
| Dirty `*` marker | `error` | High-contrast attention mark |
| Selection | reverse + highlight on `>` | Keeps type colors on the selected row |

The status bar shows the dired path/selection summary (not editor `(language LF)` chrome).

Example (Linux):

```text
  RET view  o edit  g refresh  r rename  c copy  d del  n file  m dir
  t touch  f|/ find  p pass  SPC/b page  ←=parent  →=enter  q quit
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
| `Space` | Same as PageDown (less-style) |
| `b` | Same as PageUp / “back” (less-style) |
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
| `f` or `/` | Find (only meaningful while **viewing** a file; `/` is a less-style alias) |
| `F3` | Search next (after a find) |
| `Shift+F3` | Search previous |

In list focus, `f` or `/` reminds you to open a file with Enter first.

### Review / dirty markers

| Key | Action |
| --- | --- |
| `p` | **Toggle** the selected **file**: dirty → reviewed, or reviewed → dirty (“pass”) |

**Dirty** means the file’s **content hash** differs from the review baseline, or you manually marked it dirty with `p`—not merely that mtime changed, and not requiring git. The baseline is established on the first dired open in the session (when empty). Refresh (`g`) recomputes hashes and updates markers without changing the baseline. Pressing `p` only affects the **selected file** (directories and `../` are not tracked). `p` is used instead of `*` so non-US keyboard layouts do not need Shift for a common action.

Note: listing **type** colors (directories, executables, hidden names) are separate from dirty/reviewed. Dirty files show a `*` and use the dirty name color; `p` only toggles that reviewed state.

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
- New theme color roles dedicated to dired (listing reuses existing semantic colors)

## Tips for agentic coding

1. After an agent turn, open dired on the project root (`F4` or `ainiux -d .`).
2. Use `g` to refresh and scan for `*` dirty files; press **`p`** on each file to mark it reviewed (or again to mark dirty).
3. **Enter** to preview one file at a time with highlight; **Enter** again for the list. In a file, `f` or `/` finds text; Space/`b` page like less.
4. **`o`** when you need to edit; that exits dired into the normal multi-buffer editor.
5. Use `←` / `→` to walk the tree without hunting for `../`.

## Related documentation

- [Editor help](editor_help.md) — full editor reference (embedded in the binary via install)
- [Keyboard shortcuts](keyboard-shortcuts.md) — chat, editor, agent, and dired tables
- [CLI and scripting](cli.md) — other command-line modes
- [Documentation index](README.md)
