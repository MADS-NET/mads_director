# director

`director` is a simple C++20 process supervisor with a Dear ImGui desktop UI for launching and controlling multiple processes from a TOML file.

## Features

- TOML-driven process definitions
- Required field per process section: `command` (the section name is the process name)
- Optional fields:
  - `after = "process_name"`: start this process only after another process has started
  - `workdir = "path"`: run process in this working directory (default: `director` working directory)
  - `enabled = <bool>`: include process but skip starting it when `false` (default: `true`)
  - `scale = <int>`: spawn N instances of this process
  - `relaunch = <bool>`: auto-restart on non-zero exit
  - `tty = <bool>`: run child under PTY and allow interactive `attach`/`detach`
- If a process working directory contains `.venv`, it is exposed to launched Python commands.
- Interactive UI:
  - select process
  - start, stop, restart
  - for `tty=true`, choose either:
    - send input from GUI
    - open a new terminal window attached to the running process
  - send stdin input to non-tty processes
  - view buffered output logs per process with ANSI color support

## Build

```bash
cmake -Bbuild -G Ninja
cmake --build build
```

## Run

```bash
./build/mads-director ./examples/director.toml
```

Show built-in example TOML:

```bash
./build/mads-director
# or
./build/mads-director --example
```

Internal attach client mode (used by GUI): `--attach-socket <path>`

Executable naming:
- default output name: `mads-director`
- set `-DMADS_CMD_PREFIX=` to build plain `director`

## Config format

Use one top-level section per process. The section name is the process name:

```toml
[api]
command = "./my-service --port 9000"
after = "db"
workdir = "services/api"
enabled = true
scale = 2
relaunch = true
tty = false
```

Scaled processes are named as `name[1]`, `name[2]`, etc.

Scaling behavior:
- `scale = 1` (default): a single process entry is created with the section name (for example `api`)
- `scale = N` (`N > 1`): `N` entries are created (`api[1]` ... `api[N]`)
- if a process has `after = "db"` and `db` is scaled, each dependent instance waits for all `db[...]` instances to be running

Interactive external terminal attach is currently implemented for POSIX terminals (macOS/Linux).
