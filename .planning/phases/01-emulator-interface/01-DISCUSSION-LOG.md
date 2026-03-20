# Phase 1: Emulator Interface - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-03-20
**Phase:** 1-Emulator Interface
**Areas discussed:** Launch strategy, Connection flow

---

## Launch Strategy

### How should puppetmaster start the emulator?

| Option | Description | Selected |
|--------|-------------|----------|
| Subprocess | Puppetmaster launches atari800 as a child process, waits for socket, then controls it | ✓ |
| Assume running | User starts emulator separately, puppetmaster just connects to the existing socket | |
| Both modes | --launch flag spawns emulator, otherwise connect to already-running one | |

**User's choice:** Subprocess
**Notes:** None

### Where should puppetmaster find the atari800 binary?

| Option | Description | Selected |
|--------|-------------|----------|
| Hardcode path | Default to ./src/atari800 (the build output in this repo) | |
| CLI argument | Required --emulator /path/to/atari800 flag | |
| Smart default | Default to ./src/atari800, override with --emulator flag if needed | ✓ |

**User's choice:** Smart default
**Notes:** None

### Should the emulator window stay visible?

| Option | Description | Selected |
|--------|-------------|----------|
| Yes, visible | Keep SDL window showing so you can watch the game while puppetmaster controls it | ✓ |
| Headless | Run without display if possible | |
| You decide | Claude picks the best approach | |

**User's choice:** Yes, visible
**Notes:** None

---

## Connection Flow

### What should happen if the emulator takes a while to create the socket after launch?

| Option | Description | Selected |
|--------|-------------|----------|
| Poll with timeout | Use wait_for_emulator() pattern — retry every 100ms, fail after N seconds | |
| Fixed delay | Wait a set number of seconds after launch, then try once | |
| You decide | Claude picks the best retry approach | ✓ |

**User's choice:** You decide (Claude's discretion)
**Notes:** None

### If puppetmaster ultimately can't connect, what should it do?

| Option | Description | Selected |
|--------|-------------|----------|
| Exit with error | Print clear error message and exit with non-zero code | ✓ |
| Retry indefinitely | Keep trying until user Ctrl+C's | |

**User's choice:** Exit with error
**Notes:** None

---

## Claude's Discretion

- Connection retry strategy (polling interval, timeout)
- Key hold timing
- Script structure and CLI argument parsing
- Whether to import atari800_ai.py or adapt its patterns

## Deferred Ideas

None — discussion stayed within phase scope.
