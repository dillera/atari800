# Puppetmaster

A Python automation and debugging tool that orchestrates the full KillZone multiplayer game stack — server, emulator, and Atari client — and acts as an AI-controlled player.

## Overview

Puppetmaster coordinates three components:

1. **ZoneServer** — A Node.js TCP game server that manages the KillZone multiplayer world
2. **Atari800 emulator** — The Atari 800 XL emulator, built with `-ai` (socket control) and `-netsio` (FujiNet networking)
3. **KillZone client** — The Atari game program (`.xex`) running inside the emulator

Puppetmaster connects to the emulator via a Unix domain socket (`/tmp/atari800_ai.sock`) and controls it by sending keyboard inputs, reading screen memory, and monitoring the game server's log output — all to simulate a player joining and playing the game.

## Architecture

```
┌─────────────┐         ┌──────────────────┐        ┌─────────────┐
│ Puppetmaster │────────▶│  Atari800 (-ai)  │◀──────▶│ FujiNet-PC  │
│  (Python)    │  Unix   │   running KZ     │ NetSIO │  Bridge     │
│              │  socket │   client .xex    │  UDP   │  (port 9997)│
└──────┬───────┘         └──────────────────┘        └──────┬──────┘
       │                                                     │
       │  subprocess                                    TCP  │
       │  stdout/stderr                              port 3001
       ▼                                                     ▼
┌──────────────┐                                    ┌──────────────┐
│  ZoneServer  │◀───────────────────────────────────│  KillZone    │
│  (Node.js)   │         TCP game protocol          │  client      │
│  port 3001   │                                    │  (via FujiNet)│
└──────────────┘                                    └──────────────┘
```

### Communication Layers

- **Puppetmaster → Emulator**: Length-prefixed JSON over Unix domain socket at `/tmp/atari800_ai.sock`. Commands include `key`, `run`, `peek`, `screen_ascii`, etc.
- **Emulator → FujiNet-PC**: NetSIO protocol over UDP port 9997. The emulator's `-netsio` flag enables this, allowing the Atari SIO bus to communicate with FujiNet-PC running on the host.
- **KillZone client → ZoneServer**: TCP connection on port 3001, routed through the FujiNet-PC bridge. The Atari client uses the FujiNet network device (`N:TCP://host:port`) to establish a standard TCP socket.
- **Puppetmaster → ZoneServer**: Subprocess management (stdin/stdout). Puppetmaster captures the server's log output in a background thread for monitoring and verification.

## Smart Startup Sequence

The core intelligence of puppetmaster is its startup flow, which verifies each step before proceeding to the next:

### Step 1: Launch ZoneServer
- Starts `node src/server.js` as a subprocess
- Captures stdout/stderr in a background thread
- Waits for the log line `KillZone TCP Server running on port 3001` before continuing
- Installs npm dependencies automatically if `node_modules/` is missing

### Step 2: Launch Emulator
- Runs: `./src/atari800 -ai -netsio 9997 -xl -run killzone.xex`
  - `-ai` enables the AI socket interface
  - `-netsio 9997` connects to the FujiNet-PC bridge for networking
  - `-xl` selects Atari 800 XL mode
  - `-run` auto-loads the KillZone `.xex` binary
- Polls the Unix socket until the emulator is ready (up to 15s)

### Step 3: Verify Client TCP Connection
- **Reads the Atari screen** from screen RAM to show boot progress
- Polls the ZoneServer's captured log output for `TCP Client connected`
- This confirms the KillZone Atari client successfully connected to the server through the FujiNet-PC bridge
- If the connection doesn't appear within the timeout, runs diagnostics:
  - Checks if the server process is still alive
  - Checks if the server ever started listening on port 3001
  - Reads the current Atari screen for error messages or clues
  - Dumps recent server log lines

### Step 4: Wait for "Enter player name:" Prompt
- Polls the Atari's **screen RAM** (not pixel data) for the text `player name`
- Screen text is read by peeking at SAVMSC (`$58/$59`) to find the screen memory address, then reading 960 bytes (40 columns x 24 rows) and converting Atari internal character codes to ASCII

### Step 5: Enter Player Name
- Types the player name character by character using Atari key codes
- Presses Return to submit

### Step 6: Verify Join in Server Logs
- Checks server logs for `TCP Join Request`, `TCP Join:`, or `TCP Rejoin:`
- Confirms the server accepted the player

### Step 7: Wait for Game Screen
- Polls screen RAM for `WASD=Move` — the command help text on line 23 of the gameplay screen
- This text only appears once the full game screen is rendered: play area (40x20 grid of `.` floor tiles, `@` player, `*` enemies), status bar (player name, connection status, world ticks), and help line
- Only after this check passes does puppetmaster begin sending movement commands

## Reading the Atari Screen

Puppetmaster has two screen reading modes:

### `screen_text()` — Actual Text from RAM
Reads the Atari's screen memory directly:
1. Peeks at SAVMSC (`$0058-$0059`) to get the screen RAM base address
2. Reads 960 bytes in 256-byte chunks (the `peek` command is limited to 256 bytes per call)
3. Converts Atari internal character codes to ASCII:
   - Bit 7 (inverse video) is stripped
   - Codes `0x00-0x3F` map to ASCII `0x20-0x5F` (space, punctuation, digits, uppercase A-Z)
   - Codes `0x40-0x5F` map to space (control characters)
   - Codes `0x60-0x7F` map to ASCII `0x60-0x7F` (lowercase a-z)

This is what puppetmaster uses for all text matching — detecting prompts, game state, and screen content.

### `screen_ascii()` — Pixel Brightness View
Samples pixel luminance from the emulator's display buffer and maps brightness levels to ` .:-=+*#%@`. Useful for visual debugging but **cannot match text** — the word "HELLO" on the Atari screen appears as something like `=+**=` in this mode.

## KillZone Game Screen Layout

Once in-game, the KillZone screen is organized as:

```
Lines 0-19:  Play area (40x20 text grid)
             .  = empty floor
             @  = local player
             *  = enemy mob
             ^  = hunter mob
             #  = other players

Line 20:     Status: <name> P:<count> CONNECTED     T:<ticks>
Line 21:     Combat messages (temporary, 30-frame expiry)
Line 22:     ----------------------------------------
Line 23:     WASD=Move R=Refresh Q=Quit    C1.2.0|S1.2.0
```

## CLI Usage

```bash
# Full stack: launch server + emulator + join game + move around
python3 puppetmaster.py --launch --demo killzone.xex

# Full stack with custom server path
python3 puppetmaster.py --launch --server-dir /path/to/zoneserver --demo killzone.xex

# Just emulator (server already running externally)
python3 puppetmaster.py --launch --no-server --demo killzone.xex

# Connect to already-running emulator (no launch)
python3 puppetmaster.py --demo killzone.xex

# Dump the current Atari screen text and exit
python3 puppetmaster.py --launch --screen killzone.xex

# Smoke-test all keys (W A S D Space Q Y N)
python3 puppetmaster.py --launch --keys killzone.xex

# Just connect, confirm it works, and exit
python3 puppetmaster.py --launch --connect-only killzone.xex
```

### CLI Options

| Flag | Description |
|------|-------------|
| `xex` | Path to the KillZone `.xex` file (required positional arg) |
| `--launch` | Launch the emulator (and server unless `--no-server`) |
| `--emulator PATH` | Path to the atari800 binary (default: `./src/atari800`) |
| `--socket PATH` | AI socket path (default: `/tmp/atari800_ai.sock`) |
| `--netsio-port PORT` | NetSIO UDP port for FujiNet-PC (default: `9997`) |
| `--no-server` | Skip launching the ZoneServer |
| `--server-dir PATH` | Path to the zoneserver directory |
| `--name NAME` | Player name for the join prompt (default: `puppet`) |
| `--no-startup` | Skip the KillZone startup sequence (assume already in-game) |
| `--demo` | Run a demo movement sequence (up/down/left/right) |
| `--keys` | Send one press of each game key |
| `--screen` | Dump the emulator screen and exit |
| `--server-log` | Show recent server log lines and exit |
| `--connect-only` | Connect to emulator, confirm, and exit |

## Components

### `ZoneServerManager`
Manages the KillZone game server as a subprocess:
- Starts `node src/server.js` and captures output in a background thread
- Provides `has_log_match(text)` for checking server events (connection, join, errors)
- Provides `get_recent_log(n)` for diagnostics
- Handles graceful shutdown (SIGTERM, then SIGKILL after 5s)

### `PuppetmasterController`
Controls the Atari emulator via the AI socket:
- Launches the emulator with the correct flags (`-ai -netsio -xl -run`)
- Wraps `Atari800AI` with named methods: `move_up()`, `move_down()`, `move_left()`, `move_right()`, `attack()`
- Reads screen text from Atari RAM via `screen_text()`
- Polls for specific text on screen via `wait_for_screen_text(text, timeout)`
- Runs the smart startup sequence via `wait_for_game_startup()`
- Diagnoses connection failures with `_diagnose_connection()`

### `Atari800AI` (in `atari800_ai.py`)
Low-level client library for the emulator's AI socket protocol:
- Length-prefixed JSON over Unix domain socket
- Key input: `key(code)`, `key_release()`, `type_string(text)`
- Screen: `screen_text()` (RAM text), `screen_ascii()` (pixel brightness)
- Memory: `peek(addr, len)`, `poke(addr, data)`
- Control: `run(frames)`, `pause()`, `reset()`, `load(path)`
- Chips: `antic()`, `gtia()`, `pokey()`, `pia()`, `cpu()`

## Prerequisites

- **macOS** with Python 3.6+
- **atari800** emulator built with AI interface: `./build_ai.sh`
- **FujiNet-PC** bridge running on port 9997 (for network connectivity)
- **Node.js 16+** for the ZoneServer
- **KillZone `.xex`** client binary compiled for the Atari
