# Puppetmaster

## What This Is

A Python automation and debugging tool that orchestrates the Atari800 emulator running the KillZone multiplayer game client. Puppetmaster uses the emulator's AI socket interface to monitor game state (screen + memory), send keyboard inputs (W/A/S/D movement, Space to attack), and run automated test sequences against the live KillZone client — helping the developer debug and validate the Atari client code.

## Core Value

Puppetmaster must reliably send keyboard inputs to control the on-screen player in KillZone and read back game state (screen text + memory) to verify the client is behaving correctly.

## Requirements

### Validated

<!-- Shipped and confirmed valuable. -->

(None yet — ship to validate)

### Active

<!-- Current scope. Building toward these. -->

- [ ] Connect to running atari800 emulator via AI socket (`/tmp/atari800_ai.sock`)
- [ ] Launch emulator with KillZone binary (configurable .xex path)
- [ ] Send W/A/S/D as Atari key presses to control player movement
- [ ] Send Space/F key press for attack action
- [ ] Read screen via `screen_ascii` to capture 40x24 display
- [ ] Parse screen output to identify player position, other entities, status bar
- [ ] Read memory addresses to get precise game state (player struct, connection status, game state enum)
- [ ] Monitor KillZone state machine transitions (INIT → CONNECTING → JOINING → PLAYING → DEAD)
- [ ] Run scripted movement test sequences (send direction, verify position changed)
- [ ] Run connection flow tests (verify join sequence completes successfully)
- [ ] Run combat tests (move into entities, verify combat messages and death handling)
- [ ] Run display verification tests (capture screen, verify grid/status bar/player rendering)
- [ ] Output structured test results to terminal console
- [ ] Write detailed session log to file for later review
- [ ] Build on existing `atari800_ai.py` client library (Atari800AI class)

### Out of Scope

- Network interception/proxy — puppetmaster controls via emulator AI interface only, not the TCP protocol
- Modifying KillZone source code — puppetmaster is an external observer/controller
- Running against local zoneserver — tests target live server at fujinet.diller.org:6809
- GUI or web interface — terminal-only tool
- AI/ML gameplay strategies — this is a debugging tool, not a game-playing AI

## Context

- KillZone is a multiplayer game for Atari 8-bit computers built with cc65 and FujiNet networking
- The game client connects to a Node.js server at fujinet.diller.org:6809 via binary TCP protocol
- Game uses a 40x20 grid world with text-mode rendering
- Client state machine: INIT(0) → CONNECTING(1) → JOINING(2) → PLAYING(3) → DEAD(4) → ERROR(5)
- Player controls: W/A/S/D for movement (mapped to Atari key codes), Space/F for attack
- The atari800 emulator's AI interface exposes keyboard input (`key` command), screen reading (`screen_ascii`), and memory access (`peek`) via JSON over Unix socket
- KillZone binary is at an external path (e.g., `/Users/dillera/code/killzone/build/killzone.atari`)
- KillZone source is at `/Users/dillera/code/killzone/` — memory layout defined in `src/common/state.c`
- Game state is stored in static globals: `current_state`, `local_player` (76 bytes), `other_players[10]`, `is_connected`
- Build artifacts include `.map` and `.lbl` files that can provide symbol addresses

## Constraints

- **Platform**: macOS, Python 3
- **Emulator**: Must use atari800 built with `-ai` flag and SDL 1.2
- **Protocol**: AI interface uses length-prefixed JSON over Unix domain socket at `/tmp/atari800_ai.sock`
- **Input method**: KillZone reads keyboard (not joystick), so puppetmaster must use the `key` command, not `joystick`
- **Timing**: KillZone checks input every frame; world state updates every 5 frames; status bar every 10 frames
- **Server**: Tests run against live server at fujinet.diller.org:6809

## Key Decisions

<!-- Decisions that constrain future work. Add throughout project lifecycle. -->

| Decision | Rationale | Outcome |
|----------|-----------|---------|
| Python with atari800_ai.py | Existing client library already handles socket protocol | — Pending |
| Keyboard input, not joystick | KillZone reads W/A/S/D as key presses, not joystick directions | — Pending |
| Screen + memory monitoring | Screen gives visual context, memory gives precise state values | — Pending |
| External observer pattern | No modifications to KillZone source; puppetmaster is purely external | — Pending |

## Evolution

This document evolves at phase transitions and milestone boundaries.

**After each phase transition** (via `/gsd:transition`):
1. Requirements invalidated? → Move to Out of Scope with reason
2. Requirements validated? → Move to Validated with phase reference
3. New requirements emerged? → Add to Active
4. Decisions to log? → Add to Key Decisions
5. "What This Is" still accurate? → Update if drifted

**After each milestone** (via `/gsd:complete-milestone`):
1. Full review of all sections
2. Core Value check — still the right priority?
3. Audit Out of Scope — reasons still valid?
4. Update Context with current state

---
*Last updated: 2026-03-20 after initialization*
