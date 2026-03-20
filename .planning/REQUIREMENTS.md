# Requirements: Puppetmaster

**Defined:** 2026-03-20
**Core Value:** Reliably send keyboard inputs to control the KillZone player and read back game state to verify client behavior

## v1 Requirements

Requirements for initial release. Each maps to roadmap phases.

### Emulator Control

- [ ] **EMU-01**: Puppetmaster can connect to running atari800 emulator via AI socket at `/tmp/atari800_ai.sock`
- [ ] **EMU-02**: Puppetmaster can launch atari800 emulator with `-ai -xl -run [path]` flags and a configurable KillZone .xex path
- [ ] **EMU-03**: Puppetmaster can send W key press to move player up
- [ ] **EMU-04**: Puppetmaster can send A key press to move player left
- [ ] **EMU-05**: Puppetmaster can send S key press to move player down
- [ ] **EMU-06**: Puppetmaster can send D key press to move player right
- [ ] **EMU-07**: Puppetmaster can send Space key press for attack action
- [ ] **EMU-08**: Puppetmaster can send Q, Y, N key presses for menu navigation
- [ ] **EMU-09**: Puppetmaster can release keys after pressing them

### Screen Monitoring

- [ ] **SCR-01**: Puppetmaster can read the 40x24 ASCII screen via `screen_ascii` command
- [ ] **SCR-02**: Puppetmaster can parse the 40x20 game grid to identify the local player marker
- [ ] **SCR-03**: Puppetmaster can parse the game grid to identify other entities (players, mobs, hunters)
- [ ] **SCR-04**: Puppetmaster can parse the status bar rows to extract player name, player count, connection status, and world ticks
- [ ] **SCR-05**: Puppetmaster can detect combat result messages on screen

### Memory Monitoring

- [ ] **MEM-01**: Puppetmaster can read the `current_state` enum value (0=INIT, 1=CONNECTING, 2=JOINING, 3=PLAYING, 4=DEAD, 5=ERROR)
- [ ] **MEM-02**: Puppetmaster can read the `local_player` struct fields: x, y position, health, player id, and name
- [ ] **MEM-03**: Puppetmaster can read the `is_connected` flag to check TCP connection status
- [ ] **MEM-04**: Puppetmaster can read the `other_players` array and `other_player_count` to see visible entities

### Test Automation

- [ ] **TEST-01**: Puppetmaster can run a scripted movement test: send direction key, wait frames, verify player position changed via screen and memory
- [ ] **TEST-02**: Puppetmaster can run a connection flow test: verify state transitions from INIT through CONNECTING, JOINING, to PLAYING
- [ ] **TEST-03**: Puppetmaster can run a combat test: move into another entity, verify combat message appears and handle death state if lost
- [ ] **TEST-04**: Puppetmaster can run a display verification test: capture screen, assert game grid renders correctly with player marker, status bar is present

### Reporting

- [ ] **RPT-01**: Puppetmaster prints structured test results to terminal console with pass/fail status per test
- [ ] **RPT-02**: Puppetmaster writes a detailed session log to file with timestamps, commands sent, responses received, and assertions

## v2 Requirements

### Advanced Automation

- **ADV-01**: Puppetmaster can run randomized exploration (random movement sequences with state capture)
- **ADV-02**: Puppetmaster can replay recorded sessions from log files
- **ADV-03**: Puppetmaster can set breakpoints and pause emulator on specific game state conditions

### Enhanced Monitoring

- **MON-01**: Puppetmaster can read CPU registers during execution
- **MON-02**: Puppetmaster can save/load emulator state for reproducible test scenarios
- **MON-03**: Puppetmaster can monitor POKEY registers for audio state verification

## Out of Scope

| Feature | Reason |
|---------|--------|
| Network packet interception | Puppetmaster controls via emulator AI interface, not TCP protocol |
| KillZone source modification | External observer/controller only |
| Local zoneserver support | Tests target live server at fujinet.diller.org:6809 |
| GUI or web interface | Terminal-only debugging tool |
| AI/ML gameplay strategies | Debugging tool, not a game-playing bot |
| Multi-platform support | macOS only, matching dev environment |

## Traceability

Which phases cover which requirements. Updated during roadmap creation.

| Requirement | Phase | Status |
|-------------|-------|--------|
| EMU-01 | — | Pending |
| EMU-02 | — | Pending |
| EMU-03 | — | Pending |
| EMU-04 | — | Pending |
| EMU-05 | — | Pending |
| EMU-06 | — | Pending |
| EMU-07 | — | Pending |
| EMU-08 | — | Pending |
| EMU-09 | — | Pending |
| SCR-01 | — | Pending |
| SCR-02 | — | Pending |
| SCR-03 | — | Pending |
| SCR-04 | — | Pending |
| SCR-05 | — | Pending |
| MEM-01 | — | Pending |
| MEM-02 | — | Pending |
| MEM-03 | — | Pending |
| MEM-04 | — | Pending |
| TEST-01 | — | Pending |
| TEST-02 | — | Pending |
| TEST-03 | — | Pending |
| TEST-04 | — | Pending |
| RPT-01 | — | Pending |
| RPT-02 | — | Pending |

**Coverage:**
- v1 requirements: 24 total
- Mapped to phases: 0
- Unmapped: 24 ⚠️

---
*Requirements defined: 2026-03-20*
*Last updated: 2026-03-20 after initial definition*
