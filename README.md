# director

`director` is a simple C++20 process supervisor with a Dear ImGui desktop UI for launching and controlling multiple processes from a TOML file.

## Features

- TOML-driven process definitions
- Required fields: `name`, `command`
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
./build/director ./examples/director.toml
```

Show built-in example TOML:

```bash
./build/director
# or
./build/director --example
```

Internal attach client mode (used by GUI): `--attach-socket <path>`

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

Interactive `attach` is currently implemented for POSIX terminals (macOS/Linux).
