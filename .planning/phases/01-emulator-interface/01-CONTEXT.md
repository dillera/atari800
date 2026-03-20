# Phase 1: Emulator Interface - Context

**Gathered:** 2026-03-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Puppetmaster can connect to, launch, and send keyboard inputs to the atari800 emulator via the AI socket. This phase delivers the foundational control layer — no screen parsing, memory reading, or test automation (those are Phases 2 and 3).

</domain>

<decisions>
## Implementation Decisions

### Launch Strategy
- **D-01:** Puppetmaster launches atari800 as a subprocess (not connect-to-existing)
- **D-02:** Default emulator path is `./src/atari800`, overridable with `--emulator` CLI flag
- **D-03:** KillZone .xex path is a required CLI argument
- **D-04:** Emulator SDL window stays visible — user watches the game while puppetmaster controls it
- **D-05:** Launch command: `atari800 -ai -xl -run <killzone.xex>`

### Connection Flow
- **D-06:** On connection failure, exit with clear error message and non-zero exit code (fail fast)

### Input Method
- **D-07:** Use `key` command (not `joystick`) — KillZone reads W/A/S/D as keyboard presses
- **D-08:** Key codes from existing `atari800_ai.py`: W=46, A=63, S=62, D=58, Space=33, Q=47, Y=43, N=35
- **D-09:** Key release after each press using `key_release` command

### Claude's Discretion
- Connection retry strategy after launch (polling interval, timeout duration)
- Key hold timing (frames to hold before release)
- Script structure and CLI argument parsing approach
- Whether to import `atari800_ai.py` directly or adapt its patterns

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Emulator AI Interface
- `README.md` §Socket Protocol, §API Reference — Full command protocol, key codes, joystick encoding
- `src/ai_interface.c` — Server-side socket implementation, command handlers
- `src/ai_interface.h` — API header with command documentation

### Python Client Library
- `atari800_ai.py` — Existing `Atari800AI` class with `key()`, `key_release()`, `type_char()`, `run()`, `wait_for_emulator()`, and all Atari key code constants

### KillZone Controls
- `/Users/dillera/code/killzone/src/common/input.c` — Key-to-command mapping (W/A/S/D/Q/Y/N/Space)
- `/Users/dillera/code/killzone/src/common/constants.h` — Server host, display dimensions, client version

### Build
- `build_ai.sh` — Build script with SDL 1.2 flags and AI interface enabled

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `atari800_ai.py:Atari800AI` — Complete socket client with `key()`, `key_release()`, `type_char()`, `type_string()`, `run()`, `screen_ascii()`, `peek()` methods
- `atari800_ai.py:wait_for_emulator()` — Polls socket with configurable timeout (default 10s), returns connected client
- `atari800_ai.py:AKEY_*` constants — All Atari key codes already defined as class attributes

### Established Patterns
- Length-prefixed JSON over Unix socket (`/tmp/atari800_ai.sock`)
- Context manager pattern (`with Atari800AI() as atari:`)
- `type_char()` holds key for 5 frames, releases, waits 2 frames — proven timing for Atari input

### Integration Points
- Puppetmaster will live alongside `atari800_ai.py` in the repo root
- Emulator binary at `./src/atari800` after build
- Socket at `/tmp/atari800_ai.sock` created when emulator starts with `-ai` flag

</code_context>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 01-emulator-interface*
*Context gathered: 2026-03-20*
