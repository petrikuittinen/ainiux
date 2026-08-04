# Getting started

Ainiux builds as a C++17 terminal application with libcurl and SQLite. Ubuntu x86-64 and ARM64 are the primary tested baseline. Native Windows 10 1903+/Windows 11 x64 builds use GNU Make in MSYS2 UCRT64; other POSIX-like systems are targeted where practical but are not guaranteed.

## Install on Ubuntu or Debian

The supported convenience path installs packages, builds, and installs to `/usr/local`:

```sh
git clone https://github.com/petrikuittinen/ainiux.git
cd ainiux
./scripts/install.sh --with-deps -y
ainiux --version
```

Useful variants:

```sh
./scripts/install-deps.sh -y
./scripts/install.sh
./scripts/install.sh --optimized
./scripts/install.sh --user
./scripts/uninstall.sh
```

`--user` installs below `~/.local`. Uninstall removes the selected-prefix
installation; user configuration under `~/.config/ainiux/` is left untouched.

## Build manually

Install a C++17 compiler, Make, `pkg-config`, Git, SQLite, and libcurl development files:

```sh
sudo apt update
sudo apt install -y build-essential pkg-config git libsqlite3-dev libcurl4-openssl-dev
make
./ainiux --version
```

Some Ubuntu releases call the curl runtime package `libcurl4t64`; `scripts/install-deps.sh` detects the available name. `make optimized` uses release-oriented compiler settings. `sudo make install PREFIX=/usr/local` installs the binary and refreshable bundled configuration documents below `/usr/local/share/ainiux/`.

On another POSIX-like system, provide a C++17 compiler plus development headers and link libraries for libcurl and SQLite. The repository does not claim continuous testing on BSD or macOS, and terminal behavior can differ across emulators.

## Build on native Windows x64

Open the MSYS2 **UCRT64** shell, install the UCRT64 GCC, curl, SQLite, Python,
and ZIP packages, then run `make`. Do not use the plain MSYS shell: it produces
MSYS-runtime programs rather than the supported native UCRT target.

```sh
pacman -S --needed base-devel git zip \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-sqlite3 \
  mingw-w64-ucrt-x86_64-python
make
./ainiux.exe --version
make package-windows
```

Full-screen modes run in Windows Terminal or modern conhost. mintty is not a
supported full-screen host. See [Native Windows](windows.md) for packaging,
PowerShell, clipboard, path, and test details.

## Choose a first provider

For a local LM Studio server at its default address:

```sh
ainiux lmstudio --list-models
ainiux lmstudio -p "Hello"
```

For OpenAI:

```sh
export OPENAI_API_KEY=...
ainiux openai --list-models
ainiux openai -m MODEL -p "Hello"
```

For an arbitrary OpenAI-compatible server:

```sh
ainiux http://localhost:8000/v1 --list-models
ainiux http://localhost:8000/v1 -m MODEL -p "Hello"
```

Do not put long-lived keys directly on the command line. Use provider environment variables, `--key-env`, `--key-file`, or `--key-stdin`. The complete profile and key table is in the [project README](../README.md#provider-profiles-and-credentials).

## Try each surface

```sh
ainiux lmstudio -c                  # saved-thread chat
ainiux none -e notes.md             # offline editor
ainiux lmstudio -m MODEL -a         # interactive project agent
ainiux lmstudio -m MODEL -r "inspect this project"
```

The editor and conversion paths can use `none` without inventing a model endpoint. Chat and AI assistance need a provider and model. Agent mode is separate from chat because it can use project tools.

## Where data goes

User chat threads and media are stored under `~/.ainiux/`. Interactive and one-shot agent state stays within the current project under `.ainiux-pr/`. Installed defaults live under the prefix's `share/ainiux/` directory; user configuration normally lives under `~/.config/ainiux/`. Windows keeps the same layout and derives `HOME` from `USERPROFILE` when needed.

## Next steps

- Read [CLI and scripting](cli.md) for pipelines and output formats.
- Read [Chat TUI](chat.md) or [Editor help](editor_help.md) for interactive use.
- Read [Agent workflows](agent.md) before granting workspace permissions.
- Use [Configuration](configuration.md) for endpoints, models, themes, and custom commands.

Related documentation: [documentation index](README.md), [security](security.md), [license](../LICENSE).
