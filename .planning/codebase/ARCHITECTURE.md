# Architecture

**Analysis Date:** 2026-03-20

## Pattern Overview

**Overall:** Modular Hardware Emulation Architecture

**Key Characteristics:**
- Frame-based emulation cycle with synchronized chip components
- Device-centric design where each Atari hardware component is a separate module
- Clean separation between core emulation, platform-specific code, and AI interface
- CPU-driven execution model where instruction cycle limits per scanline enforce hardware timing
- Register-based state management for all chips (ANTIC, GTIA, POKEY, PIA, CPU)

## Layers

**Core Emulation Layer:**
- Purpose: Implements the Atari 800 hardware behavior (CPU, memory, chips)
- Location: `src/` (core .c/.h files)
- Contains: CPU emulation (`cpu.c`), chip simulators (ANTIC, GTIA, POKEY, PIA), memory management, device I/O
- Depends on: System abstractions (nothing below it)
- Used by: Platform layer and AI interface

**Platform Abstraction Layer:**
- Purpose: Provides cross-platform support (SDL, X11, Amiga, Android, etc.) and UI integration
- Location: `src/sdl/`, `src/amiga/`, `src/android/`, `src/x11/`, etc.
- Contains: Video output, input handling, timing synchronization, UI implementation
- Depends on: Core emulation layer
- Used by: Main executable entry point

**AI Interface Layer:**
- Purpose: Exposes emulator control via JSON socket API for external automation
- Location: `src/ai_interface.c`, `src/ai_interface.h`
- Contains: Socket server, command parser, chip state readers, input injection, screenshot/memory access
- Depends on: Core emulation layer (reads/writes to chips, memory, input)
- Used by: External AI agents, MCP server

**MCP Server Bridge:**
- Purpose: Adapts emulator AI interface to Claude MCP protocol for tool integration
- Location: `mcp-server/`
- Contains: MCP server implementation, socket communication handler, tool definitions
- Depends on: AI interface socket protocol
- Used by: Claude agents via MCP

## Data Flow

**Emulation Cycle:**

1. Main loop calls `Atari800_Frame()` from `src/atari.c`
2. `AI_Frame()` processes any pending commands from socket clients
3. Platform reads input (keyboard, mouse, joystick)
4. `INPUT_Frame()` processes input state and writes to PIA hardware
5. `AI_ApplyInput()` overlays any AI-controlled inputs (joystick/trigger overrides)
6. Chip frames execute: `GTIA_Frame()`, `ANTIC_Frame()`, `POKEY_Frame()`
7. CPU executes instructions up to cycle limits defined by ANTIC state
8. Screen updates if `Atari800_display_screen` flag is set
9. Response sent back to waiting AI client if one is paused

**Command Processing Flow (AI Interface):**

1. Client connects to `/tmp/atari800_ai.sock`
2. Sends JSON command: `<length>\n<json>`
3. `AI_Frame()` polls for pending commands
4. Command handler parses JSON and executes (e.g., read CPU state, set joystick)
5. Response JSON generated and buffered
6. `AI_SendResponse()` sends: `<length>\n<json_response>`

**Input Override Pattern:**

1. `INPUT_Frame()` reads hardware state (keyboard, joystick) and updates PIA
2. `AI_ApplyInput()` called immediately after to re-apply any AI overrides
3. `AI_joy_override[port]` replaces PIA joystick bits if >= 0
4. `AI_trig_override[port]` replaces GTIA trigger values if >= 0
5. GTIA reads final values in `GTIA_Frame()`

**State Management:**

- **CPU State:** `cpu.h` defines CPU_A, CPU_X, CPU_Y, CPU_PC, CPU_SP, CPU_P
- **Memory:** 64KB address space in `MEMORY_mem[65536]` with page-based read/write handlers
- **Chip Registers:** ANTIC at $D4xx, GTIA at $D0xx, POKEY at $D2xx, PIA at $D3xx
- **Hardware State:** GTIA_TRIG[], PIA_PORT_input[], POKEY registers for audio/keyboard
- **AI Overrides:** Stored in `AI_joy_override[]` and `AI_trig_override[]` arrays

## Key Abstractions

**Hardware Component Module Pattern:**
- Purpose: Encapsulate each Atari chip as a standalone state machine
- Examples: `antic.c` (display), `gtia.c` (graphics), `pokey.c` (sound/keyboard), `pia.c` (ports)
- Pattern: Each module has register state arrays, a `*_Frame()` function, and `*_GetByte()` / `*_PutByte()` accessors
- Memory-mapped I/O handled by `cpu_*_read_map[]` and `cpu_*_write_map[]` function pointers

**Instruction Cycle Enforcement:**
- Purpose: Maintain cycle-accurate timing between CPU and ANTIC
- Examples: `ANTIC_xpos_limit` constrains CPU execution per scanline, `ANTIC_xpos` tracks cycle position
- Pattern: CPU loops until reaching `ANTIC_xpos_limit`, then scanline advances

**Socket Server Pattern (AI Interface):**
- Purpose: Non-blocking JSON command interface via Unix domain socket
- Examples: `AI_Frame()` polls socket, handlers parse and respond to commands
- Pattern: Length-prefixed JSON messages, synchronous request/response, paused execution model

**Memory Access Abstraction:**
- Purpose: Provide chip-to-chip and AI-to-memory interaction
- Examples: `cpu_read_map[]` provides read functions per page, `Memory_SetByte()`, `Memory_GetByte()`
- Pattern: Each memory page can have custom read/write handler (ROM, RAM, chip I/O)

## Entry Points

**`src/sdl/main.c` - main():**
- Location: `src/sdl/main.c:163`
- Triggers: Binary execution `./src/atari800 [args]`
- Responsibilities: Initialize SDL platform, call `Atari800_Initialise()`, run main emulation loop

**`src/atari.c` - Atari800_Initialise():**
- Location: `src/atari.c`
- Triggers: Called from platform main()
- Responsibilities: Parse command-line args, initialize all hardware modules, load ROMs/cartridges, set up memory map

**`src/atari.c` - Atari800_Frame():**
- Location: `src/atari.c:1322`
- Triggers: Once per frame from main loop
- Responsibilities: Orchestrate one complete emulation frame (AI commands, input, chip execution, display)

**`src/ai_interface.c` - AI_Initialise():**
- Location: `src/ai_interface.c`
- Triggers: Called from `Atari800_Initialise()` when `-ai` flag present
- Responsibilities: Create Unix socket server, set up non-blocking I/O, initialize state

**`src/ai_interface.c` - AI_Frame():**
- Location: `src/ai_interface.c`
- Triggers: Called at start of `Atari800_Frame()` before input processing
- Responsibilities: Accept socket connections, parse and execute JSON commands, pause/resume emulation

## Error Handling

**Strategy:** Silent fallback with logging

**Patterns:**
- Chip register writes to invalid addresses are ignored (no-op)
- Missing ROM files default to cartridge or built-in BASIC
- Invalid AI commands return JSON error response without crashing
- Socket errors logged via `Log_print()` but emulation continues
- Memory violations (illegal addresses) wrapped by memory management layer

## Cross-Cutting Concerns

**Logging:** Implemented via `Log_print()` in `src/log.c` - all subsystems use centralized logger

**Validation:** Command validation in AI interface: check ranges for addresses, port numbers, frame counts; JSON parsing with fallback defaults

**Authentication:** None - AI interface assumes trusted local socket; no credentials needed

**Input Coordination:** Input system uses priority (hardware input first, then AI overrides) to allow AI to take control without breaking normal input
