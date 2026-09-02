# Native Windows

Ainiux has a native x64 target for Windows 10 version 1903 or later and Windows
11. The supported build environment is the MSYS2 **UCRT64** shell with GNU Make;
the resulting `ainiux.exe` is a Win32/UCRT program, not an MSYS executable. MSYS2
is needed to build, not to run the portable package.

Windows ARM64, MSVC/CMake, installers, older Windows releases, and full-screen
operation under mintty are outside this target. One-shot CLI, conversion,
benchmark, grade, index, security-review, and headless agent paths can be run from
ordinary shells. Chat, editor/dired, and interactive agent require Windows
Terminal or modern conhost; an unsupported pseudo-terminal is rejected before
terminal state changes.

## Build with MSYS2 UCRT64

Install [MSYS2](https://www.msys2.org/), open its UCRT64 shell, and install the
native toolchain and dependencies:

```sh
pacman -Syu
pacman -S --needed \
  base-devel git zip \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-sqlite3 \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-python

git clone https://github.com/petrikuittinen/ainiux.git
cd ainiux
make
./ainiux.exe --version
```

[MSYS2 recommends UCRT64](https://www.msys2.org/docs/environments/) when there is
no reason to select another environment; it supplies the x64 GCC/UCRT toolchain
used here. Do not build from the plain MSYS or MINGW64 shell.

## Test and package

```sh
make test-unit
make test-unit-faults
make test-integration-smoke
make test-integration-sqlite
make test-windows-conpty
make package-windows
```

`make package-windows` creates
`ainiux-VERSION-windows-x86_64.zip` and a matching `.sha256`. The archive contains
`ainiux.exe`, recursively discovered non-system UCRT64 DLL dependencies, bundled
configuration, prompts, benchmarks, help, documentation, and licenses. The
packager rejects an accidental dependency on `msys-2.0.dll`.

Portable Windows artifacts should be published only after the manual UCRT64 CI
workflow and native parity checklist pass. That checklist includes streaming and
conversion; REPL; chat; editor/dired; benchmark and grade; SQLite; indexing;
security review; one-shot and interactive Act/Plan agents; cancellation;
PowerShell; and clipboard behavior.

The comprehensive shell integration retains CLI, REPL, provider, benchmark,
grade, index, and security-review coverage under UCRT64. Unix-only PTY, signal,
and symlink cases are replaced on Windows by native subprocess/interrupt,
reparse-point, SQLite, and ConPTY tests rather than importing POSIX `pty` into a
nominally native job.

## Terminal and clipboard behavior

The Windows terminal backend saves console modes and code pages, enables
[Win32 virtual-terminal input/output](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences),
and restores the original state on exit. Existing
bracketed-paste, SGR mouse-wheel, modifier-key, alternate-screen, resize, and
UTF-8 parsers are shared with POSIX. The mouse wheel is delivered to Ainiux;
hold `Shift` while dragging when native terminal selection is needed.

Desktop copy/paste uses `CF_UNICODETEXT` directly. Text is converted strictly
between UTF-16 and UTF-8, CRLF is normalized only at the clipboard boundary, and
reads are limited to 16 MiB. A busy clipboard is retried for up to two seconds
and remains cancellable.

`!COMMAND` and `/shell COMMAND` use the built-in Windows PowerShell 5.1 with
`-NoLogo -NoProfile -NonInteractive` and an encoded UTF-16 command. Ainiux sets
PowerShell/native output to UTF-8 and reports the final command exit status.

## Paths and data

Ainiux keeps its Unix-style profile layout on Windows:

- user configuration: `$HOME/.config/ainiux/`
- chat database and media: `$HOME/.ainiux/`
- project agent state: `PROJECT/.ainiux-pr/`

If `HOME` is absent, `USERPROFILE` becomes `HOME` at process startup. Explicit
XDG and Ainiux overrides continue to apply. Portable resources are found below
`share/ainiux/` beside `ainiux.exe`.

Native path handling follows the documented
[Windows file-naming rules](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)
and accepts drive-rooted and UNC paths, slash or backslash,
Unicode, spaces, and long paths. Security-sensitive operations reject ambiguous
drive-relative paths (`C:foo`), alternate data streams, reserved device names,
caller-supplied NT namespace paths, and reparse-point escapes. Persisted
project-relative paths continue using `/` and the casing returned by directory
enumeration.

Private state uses protected DACLs for the current user and SYSTEM, flushed
temporary siblings, and write-through atomic replacement. SQLite and JSON
schemas are unchanged between Windows and POSIX.

Agent subprocess trees use
[Windows Job Objects](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects),
and the terminal smoke uses Microsoft's
[ConPTY pseudoconsole flow](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session).

Related documentation: [getting started](getting-started.md),
[security](security.md), [testing](../TESTING.md), and
[architecture decisions](decisions.md).
