---
phase: 01-emulator-interface
plan: 01
subsystem: emulator-interface
tags: [python, puppetmaster, atari800, socket, keyboard-input, cli]
dependency_graph:
  requires: [atari800_ai.py]
  provides: [puppetmaster.py, PuppetmasterController]
  affects: []
tech_stack:
  added: [pytest]
  patterns: [TDD, context-manager, subprocess-launch, fail-fast-errors]
key_files:
  created:
    - puppetmaster.py
    - puppetmaster_test.py
  modified: []
decisions:
  - "D-05: Launch command uses -ai -xl -run flags via subprocess.Popen"
  - "D-06: Connection failure uses sys.exit(1) with stderr message"
  - "D-08: AKEY codes sourced from Atari800AI class constants (W=46, A=63, S=62, D=58, Space=33, Q=47, Y=43, N=35)"
  - "D-09: press_key sequence is key() -> run(frames) -> key_release() matching type_char timing"
metrics:
  duration_minutes: 2
  completed_date: "2026-03-20"
  tasks_completed: 2
  files_created: 2
  files_modified: 0
---

# Phase 01 Plan 01: Puppetmaster Emulator Interface Summary

**One-liner:** PuppetmasterController wrapping Atari800AI with named movement/action methods, subprocess launch, and fail-fast connection error handling via argparse CLI.

## What Was Built

`puppetmaster.py` at repo root — a Python script usable as both a library and a CLI tool that provides:

- `PuppetmasterController` class: launches or connects to atari800, sends keyboard inputs using Atari key codes, cleans up on exit
- `main()` CLI entry point with argparse for `xex` (required), `--launch`, `--emulator`, `--socket`, `--demo` flags
- `puppetmaster_test.py` with 12 unit tests, all passing

## Decisions Implemented

| Decision | Description | Implementation |
|----------|-------------|----------------|
| D-01 | Launch as subprocess | `subprocess.Popen([emulator_path, "-ai", "-xl", "-run", xex_path])` |
| D-02 | Default emulator path | `DEFAULT_EMULATOR = "./src/atari800"`, `--emulator` flag |
| D-03 | XEX as required arg | Positional `xex` argument in argparse |
| D-04 | SDL window stays visible | Not suppressed — `Popen` inherits stdout/stderr |
| D-05 | Launch flags | `-ai -xl -run` passed to subprocess |
| D-06 | Fail fast on error | `sys.exit(1)` with stderr message on `ConnectionError`/`FileNotFoundError`/`TimeoutError` |
| D-07 | Key input not joystick | Uses `Atari800AI.key()` and `key_release()`, not `joystick()` |
| D-08 | AKEY constants | Sourced from `Atari800AI` class attributes: W=46, A=63, S=62, D=58, Space=33, Q=47, Y=43, N=35 |
| D-09 | Key release after press | `press_key()`: `key()` -> `run(frames=5)` -> `key_release()` |

## PuppetmasterController API

```python
PuppetmasterController(emulator_path, xex_path, socket_path)

# Lifecycle
.connect()                    # connect to running emulator
.launch_and_connect(timeout)  # spawn subprocess + wait for socket
.disconnect()                 # clean up client + proc
.__enter__() / .__exit__()    # context manager

# Key input
.press_key(code, hold_frames=5)  # key -> run -> key_release
.move_up()     # AKEY_W = 46
.move_down()   # AKEY_S = 62
.move_left()   # AKEY_A = 63
.move_right()  # AKEY_D = 58
.attack()      # AKEY_SPACE = 33
.menu_q()      # AKEY_Q = 47
.menu_y()      # AKEY_Y = 43
.menu_n()      # AKEY_N = 35
```

## CLI Usage

```bash
python3 puppetmaster.py killzone.xex                              # connect to running
python3 puppetmaster.py --launch killzone.xex                     # launch then connect
python3 puppetmaster.py --launch --emulator /usr/local/bin/atari800 killzone.xex
python3 puppetmaster.py --launch --demo killzone.xex              # with demo sequence
```

## Test Coverage

12 tests in `puppetmaster_test.py`:
- `TestPressKey`: verifies key -> run -> key_release ordering
- `TestMovementKeys`: 4 tests for WASD codes
- `TestActionKeys`: 4 tests for Space/Q/Y/N codes
- `TestConnect`: 2 tests for connection error handling (SystemExit)
- `TestLaunch`: 1 test for subprocess args

## Implementation Choices (Claude's Discretion)

- **hold_frames default=5**: Matches `type_char()` proven timing from `atari800_ai.py`
- **pytest installed system-wide** with `--break-system-packages` (macOS, no virtualenv in project)
- **`main()` implemented alongside class** in Task 1 (CLI and class are tightly coupled; separating into two commits would have required modifying the same file twice)
- **connect() error handling**: Catches both `ConnectionError` and `FileNotFoundError` per the error types documented in the plan interfaces section
- **Demo sequence**: Runs `controller._client.run(frames=10)` between each directional press (not via press_key, since the demo is demonstrating movement with longer hold times)

## Deviations from Plan

### Auto-fixed Issues

None — plan executed exactly as written.

### Notes

- The plan specified 11 tests, but 12 were written: an additional `test_connect_exits_on_file_not_found_error` was added because both `ConnectionError` and `FileNotFoundError` are documented error types in the plan's `<interfaces>` section. This is a correctness addition.
- `main()` was implemented within Task 1's commit since the file was being created fresh and both tasks target `puppetmaster.py`. Task 2's commit showed no staged changes (already committed).

## Known Stubs

None — all implemented functionality is wired to real behavior.

## Self-Check: PASSED

- `puppetmaster.py` exists: FOUND
- `puppetmaster_test.py` exists: FOUND
- `feat(01-01)` commit `3b7cbb35`: FOUND (git log confirms)
- All 12 tests pass: CONFIRMED
