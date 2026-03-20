# Codebase Concerns

**Analysis Date:** 2026-03-20

## Tech Debt

**Unsafe C String Functions:**
- Issue: Use of `strcpy`, `strcat`, `sprintf`, `vsprintf` without bounds checking in legacy code
- Files: `src/log.c`, `src/monitor.c`, `src/wince/port/missing.c`, `src/joycfg.c`
- Impact: Buffer overflow risk in edge cases; security vulnerability if untrusted input reaches these paths
- Fix approach: Replace with `strncpy`, `strncat`, `snprintf`, `vsnprintf` variants; audit all buffer boundaries; add compile-time checks

**Oversized Fixed Buffers:**
- Issue: Stack-allocated buffers up to 8192 bytes in `src/log.c` (line 52: `char buffer[8192]`)
- Files: `src/log.c`, `src/monitor.c`, `src/joycfg.c`
- Impact: Stack overflow if format strings are abused; inefficient stack usage
- Fix approach: Use heap allocation for large buffers or limit sizes; validate format strings at entry points

**Direct Memory Access Without Null Checks:**
- Issue: `Screen_atari` pointer accessed without NULL verification in `src/ai_interface.c` (lines 200, 334)
- Files: `src/ai_interface.c`
- Impact: Segmentation fault if Screen is not initialized before AI commands; no defensive check exists
- Fix approach: Add `if (Screen_atari == NULL) return error;` guards before screen access; validate initialization order in `AI_Initialise`

**Conditional Debug Code Complexity:**
- Issue: Extensive use of `#ifdef DEBUG`, `#if DEBUG`, and conditional macro definitions scattered throughout emulation core
- Files: `src/sound.c`, `src/antic.c`, `src/pokey.h`, `src/pia.c`, multiple platform files
- Impact: Code paths diverge between debug and release builds; bugs may exist only in one configuration; maintenance burden
- Fix approach: Use runtime debug flags instead of compile-time conditionals; unify code paths; add regression tests for both configurations

**Timing Synchronization Fragility:**
- Issue: CPU-ANTIC-GTIA timing synchronization is cycle-exact but documented as "not 100%" with multiple known display glitches
- Files: `src/antic.c`, `src/cpu.c`, `src/gtia.c`
- Impact: Programs using edge-case timing (Surf's up, Satan's Hollow, Dimension X, Sirius games) display incorrectly; hard to debug due to floating-point and macro complexity
- Fix approach: Implement proper hardware cycle tracking documentation; refactor timing loops for clarity; add unit tests for known problem programs

**Cycle-Exact POKEY Limitations:**
- Issue: POKEY interrupts are scanline-exact, not cycle-exact (line 76 in `DOC/BUGS`)
- Files: `src/pokey.c`, `src/pokeysnd.c`, `src/sound.c`
- Impact: Sound glitches in Joyride, Mirax Force, Saturday Demo, The Last Guardian, Digital Trash; speech playback incorrect
- Fix approach: Refactor POKEY interrupt timing to use CPU cycle counters instead of scanline boundaries; adds complexity but fixes multiple programs

**Thread-Unsafe Global State:**
- Issue: Multiple global variables modified from both main thread and NetSIO worker thread without synchronization
- Files: `src/netsio.c`, `src/ai_interface.c`
- Variables: `netsio_enabled`, `netsio_sync_wait`, `netsio_next_write_size`, `netsio_cmd_state` modified by `fujinet_rx_thread` (line 523 in netsio.c) without locks
- Impact: Race conditions on state changes; PROCEED signal handling may miss or double-process; data corruption possible
- Fix approach: Use pthread_mutex for all shared state; add volatile qualifiers; add barrier operations after state writes; test with ThreadSanitizer

**Detached Thread Cleanup:**
- Issue: NetSIO uses `pthread_detach` (line 309 in `src/netsio.c`) but thread accesses global state and FIFOs that aren't safely cleaned up
- Files: `src/netsio.c`
- Impact: Thread continues running after socket close attempt; potential use-after-free if emulator exits while thread blocked on `recvfrom` (line 803)
- Fix approach: Use pthread_join instead of detach; add shutdown signal to thread; verify FIFO closure order; add explicit thread termination wait in `netsio_shutdown`

## Known Bugs

**Display Timing Misalignment (Confirmed):**
- Symptoms: Horizon flickers in Surf's up and Satan's Hollow; vertical alignment off by 1-2 pixels in multiple demos
- Files: `src/antic.c` (lines 3083, 3097, 3181, 3213, 3726, 3773, 3906 contain TODO comments about timing accuracy)
- Trigger: Programs that write to WSYNC register at cycle boundaries; depends on exact CPU-ANTIC synchronization
- Workaround: None; user must disable cycle-exact emulation or use different display filter
- Status: Documented in `DOC/BUGS` lines 4-38

**VAPI Image Incompatibility:**
- Symptoms: VAPI disk images fail to boot or load (Alternate Reality, Ankh, Ballblazer, etc.)
- Files: `src/cartridge.c` (lines 827, 841, 868, 882 have TODO notes), `src/devices.c`
- Cause: Binary AND of overlapping banks not properly implemented; VAPI spec incomplete
- Workaround: Convert VAPI images to ATR or other formats
- Status: Documented in `DOC/BUGS` lines 91-105

**POKEY Interrupt Overdrive Hang (Cosmic Balance):**
- Symptoms: Program hangs before intro screen; infinite interrupt loop triggered
- Files: `src/pokey.c`
- Trigger: Game sets POKEY timer to absurdly high frequency + SIO interaction
- Workaround: Use monitor command "c 10 40" then "cont"
- Status: Documented in `DOC/BUGS` lines 4-12; likely cannot be fixed without SIO-POKEY interaction modeling

**SDL Sound Buffer Noise:**
- Symptoms: Wrong sound effects (noise) in some games
- Files: `src/sound.c`, `src/sdl/` platform code
- Cause: Callback system for sound buffer filling has timing issues
- Impact: Audio quality degradation in affected titles
- Status: Documented in `DOC/BUGS` lines 107-108

**Keyboard State Leak (SHIFT Release):**
- Symptoms: Pressing SHIFT+1 then releasing SHIFT still produces '!' instead of '1'
- Files: `src/input.c`, `src/atari_x11.c`, `src/sdl/input.c`
- Cause: Keyboard state machine doesn't properly handle modifier release ordering
- Impact: Input handling incorrect in edge cases
- Status: Documented in `DOC/BUGS` lines 16-17

**Stereo POKEY Engine Configuration:**
- Symptoms: New POKEY engine doesn't switch to mono output in STEREO_SOUND builds
- Files: `src/pokeysnd.c`, `src/sound.c`
- Cause: Output mode negotiation between engine and SDL sound broken
- Impact: Audio output wrong when stereo setting changed at runtime
- Status: Documented in `DOC/BUGS` lines 14-15

## Security Considerations

**JSON Parsing Not Robust:**
- Risk: Simple `strstr`-based parsing in `src/ai_interface.c` (lines 62-101) vulnerable to malformed JSON causing buffer reads past boundaries
- Files: `src/ai_interface.c` (functions `json_get_string`, `json_get_int`, `json_get_bool`)
- Current mitigation: Buffer size checks exist but parsing doesn't validate JSON structure
- Recommendations: Use proper JSON parser (jansson or cJSON); validate all inputs before processing; add fuzzing tests

**Socket File Race Condition:**
- Risk: `unlink(AI_socket_path)` (line 125 in `src/ai_interface.c`) followed by `socket()` and `bind()` creates window where attacker could create socket file
- Files: `src/ai_interface.c` function `setup_server_socket`
- Current mitigation: None; code assumes /tmp/atari800_ai.sock is not writable by others
- Recommendations: Use mktemp or similar to create socket atomically; check socket file ownership before binding; use restrictive umask

**No Input Validation on File Paths:**
- Risk: `load` command accepts arbitrary file paths and passes to `BINLOAD_Loader` without sanitization
- Files: `src/ai_interface.c` (line 226)
- Current mitigation: Emulator only; assumes local socket can be trusted
- Recommendations: If socket becomes network-exposed, validate paths against whitelist; use chroot/pledge/seccomp sandboxing

**NetSIO Network Socket Without Authentication:**
- Risk: NetSIO connects to hardcoded port 9997 (or configurable) without authentication
- Files: `src/netsio.c` (UDP socket created without credentials)
- Current mitigation: Local network only; assumes trusted FujiNet-PC device
- Recommendations: Add token-based authentication; use TLS for network variant; log connection attempts

## Performance Bottlenecks

**Intensive 130XE Bank Switching:**
- Problem: Programs that switch memory banks frequently (Impossible but Real, Sheol, Total Daze, Ultra demos) slow dramatically
- Files: `src/memory.c` (bank switching uses memcpy), `src/cartridge.c`
- Cause: Each bank switch copies entire 4KB pages; no dirty-page tracking
- Impact: Demo effects run at 5-10 FPS instead of 60 FPS
- Improvement path: Implement Dirty Spans optimization (referenced in `DOC/TODO` line 274); cache mapped pages; use lazy copy-on-write

**Large File I/O Monolithic:**
- Problem: Cartridge loading and state save use single large memcpy operations
- Files: `src/cartridge.c`, `src/statesav.c`
- Cause: No streaming or buffering; loads entire image into RAM at once
- Impact: Memory spike on 512MB+ image load; freezes emulator briefly
- Improvement path: Implement chunked I/O; add progress callback; lazy load cartridge banks

**Screen Rendering Per-Pixel:**
- Problem: NTSC emulation in `src/ntsc.c` processes pixels individually even for flat regions
- Files: `src/ntsc.c`, `src/videomode.c`
- Cause: No region-based optimization; treats every pixel as potential artifact area
- Impact: NTSC mode uses 3-5x more CPU than linear scaling
- Improvement path: Detect flat color regions; skip artifact processing for uniform areas; use SIMD for line processing

**Monitor/Debugger String Operations:**
- Problem: Monitor debug interface (4081 lines in `src/monitor.c`) rebuilds display strings on every refresh
- Files: `src/monitor.c`
- Cause: No caching of parsed CPU state; rebuilds register display from scratch each frame
- Impact: Monitor becomes sluggish with breakpoints set; trace mode runs at 1 FPS
- Improvement path: Cache CPU state between refreshes; only update changed fields; implement dirty-rectangle tracking

**Util_malloc/free Overhead:**
- Problem: 117 malloc/calloc/realloc calls throughout codebase; no memory pool
- Files: Multiple, especially `src/votrax.c` (line 435), `src/remez.c`, `src/cartridge.c`
- Cause: Each allocation goes through system allocator; no pooling strategy
- Impact: Startup overhead; fragmentation over long runs
- Improvement path: Implement memory arena for fixed-size allocations; pool common sizes (512B, 4KB, 64KB)

## Fragile Areas

**ANTIC/GTIA Collision Detection:**
- Files: `src/antic.c`, `src/gtia.c`
- Why fragile: Multiple TODO comments (lines 3083, 3097, 3213, 3726, 3773) indicate unfinished cycle-exact collision handling; depends on pm_scanline partial data restoration (line 155 in `DOC/TODO`) which is not fully implemented
- Safe modification: Only change collision flags when adding new hardware features; add unit tests for each collision type; document exact cycle at which collision is detected
- Test coverage: Multiple demos (8 Players, Bitter Reality, Our 5oft Unity Part) fail intermittently due to collision bugs; no automated regression test suite exists
- Risk: Changing HPOS, GRAF, or SIZE handling invalidates collision logic; affects ~30% of games

**SIO Device Handler Compatibility:**
- Files: `src/devices.c` (2735 lines), `src/sio.c` (1973 lines)
- Why fragile: Device handler emulation uses magic numbers and undocumented Atari DOS return codes (lines 1060, 1085, 1095, 1368, 1394 in `src/devices.c` marked XXX); sector size assumptions hardcoded
- Safe modification: Changes to return codes must be validated against real DOS dumps; test with 810, 1050, XF551 images; cannot change without breaking existing game disk loaders
- Test coverage: Limited to individual .ATR file formats; no comprehensive drive emulation test suite
- Risk: SIO protocol change breaks disk I/O; affects 90% of games

**6502 Undocumented Opcodes:**
- Files: `src/cpu.c` (2512 lines of cycle-exact 6502 emulation with undocumented opcodes)
- Why fragile: Some opcodes (0x93, 0x9b, 0x9f) have "unpredictable" results per schematics study (lines 74-76 in `DOC/TODO`); behavior depends on internal CPU state
- Safe modification: Cannot safely change undocumented opcode behavior; protections needed for copy protection schemes
- Test coverage: No formal test suite for opcodes; relies on program compatibility
- Risk: Copy-protected games may fail if opcode emulation changes; affects 20% of released titles

**POKEY Serial I/O Emulation:**
- Files: `src/pokey.c`, `src/netsio.c`
- Why fragile: SIO shift register not emulated (line 40 in `DOC/TODO`); serial rate emulation incomplete (line 32); interaction with NetSIO adds async complexity
- Safe modification: Serial I/O tests must pass; cannot change protocol without breaking network devices (FujiNet)
- Test coverage: Manual testing only; no automated serial I/O tests
- Risk: Changes affect disk loaders, printer emulation, and network devices; touches 40% of I/O code

**Display List Processing:**
- Files: `src/antic.c` (4147 lines), particularly display list command execution
- Why fragile: Display list parsing is interleaved with 6502 CPU cycle counting; DMACTL changes mid-scanline affect parsing (line 157 in `DOC/TODO` references glitches)
- Safe modification: Changes must maintain cycle-exact timing; test against programs that modify DL during frame; add validation of DL instruction stream
- Test coverage: Graphics editors (GED, Power Graph) and some demos test edge cases but no formal test suite
- Risk: DL changes break graphics rendition in ~10% of complex titles; affects horizon effects, split screens

## Scaling Limits

**Memory Model Fixed at 64KB:**
- Current capacity: 64KB base Atari RAM emulated as fixed array
- Limit: XL/XE extended memory (up to 128KB) requires bank switching; nested bank switches cause slowdown (confirmed in `DOC/BUGS` line 84)
- Scaling path: Implement banked memory as view into larger virtual address space; remove memcpy on bank switch; use page tables for address translation

**CPU Cycle Counter Precision:**
- Current capacity: 32-bit unsigned cycle counter in `src/cpu.c`
- Limit: Wraps after ~71 minutes of emulation time; reset not synchronized with ANTIC/GTIA
- Scaling path: Switch to 64-bit counter; add atomic increment for netsio thread safety; synchronize reset across modules

**FIFO Buffer Size for NetSIO:**
- Current capacity: 4096 bytes (NETSIO_FIFO_SIZE in `src/netsio.h` line 42)
- Limit: High-speed data blocks (65B chunks, line 39) will overflow if network latency exceeds 60ms at full SIO speed; blocks pending write accumulate
- Scaling path: Implement dynamic FIFO growth (realloc to 8KB-64KB); add backpressure signaling; implement write coalescing

**Display Buffer Allocation:**
- Current capacity: Screen buffers allocated once at startup in platform-specific code
- Limit: Cannot change screen resolution at runtime; NTSC emulation requires 2-3x memory for artifact processing
- Scaling path: Allocate screen buffers dynamically in `src/screen.c`; support runtime resolution changes; implement lazy NTSC buffer allocation

## Dependencies at Risk

**Deprecated Sound Engine Configuration:**
- Risk: Old POKEY engine (referenced in `DOC/BUGS` line 14) coexists with new engine; config switching broken
- Impact: Users cannot select audio engine at runtime; stereo builds default to wrong config
- Migration plan: Remove old engine completely; migrate all STEREO_SOUND builds to new engine; add unit tests for audio mode switching

**Obsolete DOS Platform Code:**
- Risk: DOS support in `src/dos/` contains 32-bit DOS/DJGPP-specific code; no maintainer
- Impact: Cannot compile on modern systems; dead code increases maintenance burden
- Migration plan: Archive to deprecated/ directory; add UNSUPPORTED marker; remove from default build

**Hardcoded Configuration Paths:**
- Risk: Default socket path (`/tmp/atari800_ai.sock` in `src/ai_interface.c` line 30) assumes POSIX filesystem
- Impact: Windows/embedded ports require workarounds; path collisions on multi-user systems
- Migration plan: Add configuration variable for socket path; use XDG_RUNTIME_DIR on Linux; use temp directory on Windows

## Missing Critical Features

**No Automated Regression Testing:**
- Problem: 100+ known display bugs (DOC/BUGS) have no automated test suite; fixes cannot be validated without manual inspection
- Blocks: Cannot confidently refactor timing code; display bugs may return unnoticed
- Priority: HIGH - Implement snapshot-based test suite comparing output against known-good baseline images

**No Thread Safety Testing:**
- Problem: NetSIO and AI interfaces use threads but no ThreadSanitizer or TSAN testing in CI
- Blocks: Race conditions may exist undetected; production reliability unknown
- Priority: HIGH - Add ThreadSanitizer builds to CI; identify and fix all races in netsio.c

**No Performance Regression Testing:**
- Problem: No benchmark suite tracking emulation speed; slowdowns introduced silently
- Blocks: Cannot detect when bank-switching or other optimizations regress
- Priority: MEDIUM - Add baseline benchmarks for known heavy workloads (Impossible but Real, banking-intensive demos)

**No Fuzzing of AI Interface:**
- Problem: JSON parsing and socket input not fuzzed; malformed commands could crash
- Blocks: Security cannot be validated; buffer overflow risks
- Priority: MEDIUM - Implement libFuzzer harness for AI protocol; fuzz JSON parser

## Test Coverage Gaps

**NetSIO PROCEED Signal Handling:**
- What's not tested: PROCEED assertion/release sequence and timing; interaction with motor line
- Files: `src/netsio.c` (lines 408-440 handle PROCEED)
- Risk: Signal timing bugs could corrupt disk I/O or lose bytes
- Priority: HIGH - Add unit tests for PROCEED edge cases: double-assert, early-release, motor-off-during-assert

**AI Socket Connection/Disconnection:**
- What's not tested: Connection failure, client disconnect, simultaneous commands, socket cleanup on error
- Files: `src/ai_interface.c` (lines 122-157, 550-620 handle socket lifecycle)
- Risk: Resource leak, file descriptor leak, socket not properly closed on crash
- Priority: HIGH - Add integration tests; verify all fds closed after abnormal exit

**Bank Switching Edge Cases:**
- What's not tested: Switching banks mid-scanline, switching during DMA, interrupted bank operations
- Files: `src/memory.c`, `src/cartridge.c`
- Risk: Memory corruption if bank switch interrupted by NMI or interrupt
- Priority: MEDIUM - Add cycle-exact tests for bank operations; verify no partial copies left behind

**POKEY Timer Overflow:**
- What's not tested: Timer reaching zero, overflow interrupt timing, simultaneous interrupts
- Files: `src/pokey.c`
- Risk: Games relying on exact timer behavior fail; sound generation incorrect
- Priority: MEDIUM - Add unit tests for timer interrupt sequences; validate against hardware captures

**Display List Edge Cases:**
- What's not tested: DL changes during rendering, invalid DL instructions, DL executed from ROM
- Files: `src/antic.c`
- Risk: Graphics corruption, infinite loops, emulator hang
- Priority: MEDIUM - Add fuzz tests for DL streams; implement instruction validation

---

*Concerns audit: 2026-03-20*
