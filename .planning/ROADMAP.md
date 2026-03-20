# Roadmap: Puppetmaster

## Overview

Puppetmaster is built in three phases. First, establish the emulator interface — connecting, launching, and sending keyboard inputs via the AI socket. Second, build the observation layer that parses raw screen and memory reads into structured game state. Third, wire everything into automated test sequences with structured reporting. Each phase is fully verifiable before the next begins.

## Phases

**Phase Numbering:**
- Integer phases (1, 2, 3): Planned milestone work
- Decimal phases (2.1, 2.2): Urgent insertions (marked with INSERTED)

Decimal phases appear between their surrounding integers in numeric order.

- [ ] **Phase 1: Emulator Interface** - Connect to and control the atari800 emulator via the AI socket
- [ ] **Phase 2: Game State Observation** - Parse screen and memory reads into structured game state
- [ ] **Phase 3: Test Automation and Reporting** - Run scripted test suites against the live server with structured output

## Phase Details

### Phase 1: Emulator Interface
**Goal**: Puppetmaster can connect to, launch, and send keyboard inputs to the atari800 emulator
**Depends on**: Nothing (first phase)
**Requirements**: EMU-01, EMU-02, EMU-03, EMU-04, EMU-05, EMU-06, EMU-07, EMU-08, EMU-09
**Success Criteria** (what must be TRUE):
  1. Running `puppetmaster` connects to an already-running atari800 instance at `/tmp/atari800_ai.sock` and confirms connection
  2. Running `puppetmaster --launch` starts the atari800 emulator with KillZone loaded using a configurable .xex path
  3. Puppetmaster sends W/A/S/D key presses that visibly move the player on screen in the KillZone client
  4. Puppetmaster sends Space and menu keys (Q, Y, N) and releases all keys after pressing them
**Plans**: 2 plans

Plans:
- [x] 01-01-PLAN.md — Create PuppetmasterController class and full CLI entry point (EMU-01 through EMU-09)
- [ ] 01-02-PLAN.md — Add --keys/--connect-only flags and human-verify live emulator control

### Phase 2: Game State Observation
**Goal**: Puppetmaster can read and parse game state from both screen output and emulator memory
**Depends on**: Phase 1
**Requirements**: SCR-01, SCR-02, SCR-03, SCR-04, SCR-05, MEM-01, MEM-02, MEM-03, MEM-04
**Success Criteria** (what must be TRUE):
  1. Puppetmaster reads the full 40x24 screen and identifies the local player marker position in the 40x20 grid
  2. Puppetmaster identifies other entities (players, mobs, hunters) present in the game grid
  3. Puppetmaster extracts player name, player count, connection status, and world ticks from the status bar rows
  4. Puppetmaster reads `current_state`, `local_player` fields, `is_connected`, and `other_players` directly from emulator memory and prints their values
  5. Puppetmaster detects combat result messages when they appear on screen
**Plans**: TBD

### Phase 3: Test Automation and Reporting
**Goal**: Puppetmaster can run scripted test suites against the live KillZone server and report results
**Depends on**: Phase 2
**Requirements**: TEST-01, TEST-02, TEST-03, TEST-04, RPT-01, RPT-02
**Success Criteria** (what must be TRUE):
  1. Puppetmaster runs a movement test that sends a direction key, waits the correct number of frames, and verifies the player position changed via both screen and memory
  2. Puppetmaster runs a connection flow test that verifies state transitions from INIT through CONNECTING, JOINING, to PLAYING
  3. Puppetmaster runs a combat test that moves into an entity, confirms a combat message appears, and handles the death state
  4. Puppetmaster runs a display verification test that asserts the game grid renders with a player marker and the status bar is present
  5. Test results print to terminal with pass/fail per test, and a timestamped session log is written to file with commands, responses, and assertions
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 1 → 2 → 3

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 1. Emulator Interface | 1/2 | In Progress|  |
| 2. Game State Observation | 0/? | Not started | - |
| 3. Test Automation and Reporting | 0/? | Not started | - |
