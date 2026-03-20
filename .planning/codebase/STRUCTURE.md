# Codebase Structure

**Analysis Date:** 2026-03-20

## Directory Layout

```
atari800/
├── src/                     # Core emulation + platform implementations
│   ├── *.c / *.h            # Core hardware emulators (69 .c files, 66 .h files)
│   ├── ai_interface.c/h     # AI socket interface (NEW)
│   ├── sdl/                 # SDL platform backend
│   ├── amiga/               # Amiga platform backend
│   ├── android/             # Android platform backend
│   ├── x11/                 # X11 platform backend
│   ├── atari_ntsc/          # NTSC color generation
│   ├── codecs/              # Media codecs (PNG, etc.)
│   ├── libatari800/         # Library interface for embedding
│   ├── roms/                # Built-in ROM data
│   └── [other platforms]/   # Falcon, DOS, WinCE, DC, etc.
├── mcp-server/              # MCP bridge to Claude
│   ├── index.js             # MCP server implementation
│   └── package.json         # MCP dependencies
├── DOC/                     # Documentation (original project)
├── tools/                   # Build/utility scripts
├── util/                    # Utilities
├── data/                    # Game data/assets
├── emuos/                   # Embedded OS ROM sources
└── configure.ac / Makefile  # Build system (autotools)
```

## Directory Purposes

**`src/`:**
- Purpose: All emulation core and platform-specific implementations
- Contains: C source files for CPU, chips, memory, input, screen, devices, AI interface, and platform backends
- Key files: `atari.c`, `cpu.c`, `memory.c`, `antic.c`, `gtia.c`, `pokey.c`, `pia.c`, `input.c`, `ai_interface.c`

**`src/sdl/`:**
- Purpose: SDL 1.2 platform implementation (default build target)
- Contains: Video output (`video.c`), input handling (`input.c`), sound output, main loop
- Key files: `src/sdl/main.c:163`, `src/sdl/video.c`, `src/sdl/input.c`

**`src/ai_interface.c/h`:**
- Purpose: NEW - JSON socket API for external AI/automation control
- Contains: Unix socket server, command parser, JSON helpers, response builders
- Key exports: `AI_Initialise()`, `AI_Frame()`, `AI_SendResponse()`, `AI_ApplyInput()`, `AI_DebugWrite()`

**`src/atari_ntsc/`:**
- Purpose: NTSC color signal generation and artifact simulation
- Contains: Lookup tables, color generation code
- Used by: Display rendering

**`src/codecs/`:**
- Purpose: Media codec support (PNG screenshot saving, etc.)
- Contains: PNG encoder/decoder
- Used by: Screen capture in AI interface

**`mcp-server/`:**
- Purpose: Model Context Protocol server bridge
- Contains: Node.js MCP server that wraps AI interface socket calls
- Entry point: `mcp-server/index.js` - spawns emulator and exposes MCP tools

**`DOC/`:**
- Purpose: Original atari800 documentation (hardware reference, user guides)
- Generated: No - committed
- Note: Reference documentation, not code

**`tools/`:**
- Purpose: Build scripts and utilities
- Key files: None directly used by emulation

**`emuos/`:**
- Purpose: Embedded OS ROM source code
- Contains: Atari OS assembler source, test code
- Generated: No

## Key File Locations

**Entry Points:**
- `src/sdl/main.c:163` - `main()` function, SDL platform entry point
- `src/atari.c` - `Atari800_Initialise()` and `Atari800_Frame()` core functions
- `mcp-server/index.js` - MCP server entry point (Node.js)

**Configuration:**
- `configure.ac` - Build configuration with AI flag support (`--enable-ai`)
- `build_ai.sh` - Convenience build script with correct SDL 1.2 flags
- `atari800.config` - Qt Creator project config (development convenience)

**Core Logic:**
- `src/atari.c:1322` - `Atari800_Frame()` - main emulation cycle orchestrator
- `src/cpu.c` - 6502 CPU emulator
- `src/memory.c` - Memory management (RAM, ROM, I/O mapping)
- `src/antic.c` - ANTIC chip (display/DMA controller)
- `src/gtia.c` - GTIA chip (graphics, collision, triggers)
- `src/pokey.c` - POKEY chip (sound, timers, keyboard)
- `src/pia.c` - PIA chip (ports, I/O)
- `src/input.c` - Input handling (keyboard, joystick, mouse)
- `src/ai_interface.c` - AI socket interface and command handlers

**Testing:**
- No dedicated test directory in core
- Note: Tests would be in `mcp-server/` or external

## Naming Conventions

**Files:**
- Hardware modules: `{chip_name}.c` and `{chip_name}.h` (e.g., `antic.c`, `gtia.c`, `pokey.c`)
- Core modules: `{subsystem}.c` and `{subsystem}.h` (e.g., `memory.c`, `input.c`, `screen.c`)
- Platform-specific: `{platform}/{component}.c` (e.g., `sdl/video.c`, `amiga/main.c`)

**Functions:**
- Core chip functions: `{CHIP}_Frame()`, `{CHIP}_GetByte()`, `{CHIP}_PutByte()`
  - Examples: `ANTIC_Frame()`, `GTIA_GetByte()`, `POKEY_PutByte()`
- Initialization: `{MODULE}_Initialise()`, `{MODULE}_Init()`
- AI interface: `AI_Frame()`, `AI_SendResponse()`, `AI_ApplyInput()`
- Platform: `PLATFORM_{Action}()` or `SDL_{Component}_{Action}()`

**Variables:**
- Global state: ALL_CAPS prefixed by module, e.g., `GTIA_TRIG[]`, `ANTIC_xpos`, `CPU_PC`
- Chip registers: Named after hardware register, e.g., `ANTIC_DMACTL`, `POKEY_AUDF1`
- Arrays: Plural with index, e.g., `PIA_PORT_input[2]`, `GTIA_TRIG[4]`

**Types:**
- Use atari.h definitions: `UBYTE` (unsigned 8-bit), `UWORD` (unsigned 16-bit), `ULONG` (unsigned 32-bit)
- Struct names: `struct {chip}_state` (internally only, not exposed)

## Where to Add New Code

**New Feature (e.g., breakpoint system):**
- Primary code: `src/` (new .c module or extended existing module)
- Header: `src/{feature}.h` with public API
- Integration point: Call from `Atari800_Frame()` or relevant chip frame function

**New Hardware Component (e.g., new cartridge type):**
- Implementation: `src/{component}.c` following module pattern
- Header: `src/{component}.h`
- Memory mapping: Register read/write handlers in `src/memory.c`
- Frame function: Add `{COMPONENT}_Frame()` call to `Atari800_Frame()` if timing-sensitive

**AI Command:**
- Handler: Add to `src/ai_interface.c` in command dispatcher (look for "if (strcmp(cmd, ...)")
- State access: Use existing chip APIs (`CPU_GetX()`, `Memory_GetByte()`, etc.)
- Response: Build JSON response using `snprintf()` into `ai_response` buffer
- Example: See `AI_Frame()` for how "cpu" command reads and formats response

**Platform Support (e.g., new video backend):**
- New directory: `src/{newplatform}/`
- Entry: `src/{newplatform}/main.c` with `PLATFORM_Initialise()`, main loop
- I/O handlers: `PLATFORM_Keyboard()`, `PLATFORM_DisplayScreen()`, `PLATFORM_ConfigInit()`

**MCP Tool:**
- Location: `mcp-server/index.js` in tool definition handler
- Pattern: Tool calls `sendCommand()` with JSON command, formats response for Claude
- No changes needed to C code for basic tools - just wrap existing commands

## Special Directories

**`src/.deps/`:**
- Purpose: Build dependency tracking (automake generated)
- Generated: Yes - created by automake
- Committed: No

**`autom4te.cache/`:**
- Purpose: Autoconf macro cache (build acceleration)
- Generated: Yes - created by autoconf
- Committed: No

**`emuos/src_*/ emuos/src_5200/ emuos/src_basic/`:**
- Purpose: Embedded ROM source code in 6502 assembly
- Generated: No - source committed, compiled to ROM blobs
- Note: ROM binaries embedded in `src/roms/` during build

**`src/roms/`:**
- Purpose: Embedded ROM binary data
- Generated: Yes (compiled from emuos assembly)
- Committed: Yes - pre-built ROM blobs for distribution

**`.planning/codebase/`:**
- Purpose: Architecture documentation for AI-guided development
- Generated: No - manually authored by analysis tools
- Committed: Yes - guides `/gsd:plan-phase` and `/gsd:execute-phase`
