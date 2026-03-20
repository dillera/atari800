<!-- GSD:project-start source:PROJECT.md -->
## Project

**Puppetmaster**

A Python automation and debugging tool that orchestrates the Atari800 emulator running the KillZone multiplayer game client. Puppetmaster uses the emulator's AI socket interface to monitor game state (screen + memory), send keyboard inputs (W/A/S/D movement, Space to attack), and run automated test sequences against the live KillZone client — helping the developer debug and validate the Atari client code.

**Core Value:** Puppetmaster must reliably send keyboard inputs to control the on-screen player in KillZone and read back game state (screen text + memory) to verify the client is behaving correctly.

### Constraints

- **Platform**: macOS, Python 3
- **Emulator**: Must use atari800 built with `-ai` flag and SDL 1.2
- **Protocol**: AI interface uses length-prefixed JSON over Unix domain socket at `/tmp/atari800_ai.sock`
- **Input method**: KillZone reads keyboard (not joystick), so puppetmaster must use the `key` command, not `joystick`
- **Timing**: KillZone checks input every frame; world state updates every 5 frames; status bar every 10 frames
- **Server**: Tests run against live server at fujinet.diller.org:6809
<!-- GSD:project-end -->

<!-- GSD:stack-start source:codebase/STACK.md -->
## Technology Stack

## Languages
- C (C89/C99) - Core Atari 800 emulator implementation
- Python 3 - AI client library (`atari800_ai.py`)
- JavaScript/Node.js (ES Modules) - MCP server implementation
- Shell (Bash) - Build scripts
## Runtime
- Native platform support: Linux, macOS, Windows (Cygwin/MinGW), Android, Raspberry Pi, Atari Falcon
- Current build: macOS 14.6+ with Apple Silicon (Homebrew)
- Python: 3.6+ (minimum)
- Node.js: 16+ (for MCP server)
- Node.js: npm (with `package-lock.json`)
- No Python package manifest (standalone script)
- Build: Autotools (autoconf, automake)
## Frameworks
- Autotools (autoconf/automake) - C build system
- SDL 1.2 - Primary graphics and audio output (not SDL2, due to keyboard issues)
- Optional: SDL2, X11 (with Motif/XView/SHM variants), GLES2 (Raspberry Pi), curses
- CPU/6502: Native C implementation
- Chipset: Custom C implementations (ANTIC, GTIA, POKEY, PIA)
- Model Context Protocol SDK (`@modelcontextprotocol/sdk` v0.5.0) - MCP server framework
- Node.js `net` module - Unix socket communication
- Node.js `child_process` - Process spawning
- Not detected
- GNU autotools (configure, make)
- Shell build script: `build_ai.sh`
- C compiler (GCC/Clang)
## Key Dependencies
- zlib - Compression for save files (checked in configure via `AC_CHECK_LIB(z,gzopen)`)
- pthread - Threading library (required for sound recording)
- libpng (optional) - PNG export support
- math library (libm) - Math functions
- SDL 1.2 - Window, input, audio output (`SDL_CONFIG=/opt/homebrew/bin/sdl-config`)
- X11 libraries (Linux/Unix targets)
- GLES2 (Raspberry Pi)
- Curses/NCurses/PDCurses (terminal UI fallback)
- SDL 1.2 audio (primary)
- OSS (Open Sound System) - Linux support
- ALSA (via OSS compatibility)
- lame (libmp3lame) - MP3 audio export
- libsocket/libgen (Solaris compatibility, if needed)
- Standard POSIX sockets (Unix domain sockets for AI interface)
- `@modelcontextprotocol/sdk` ^0.5.0 (single Node.js dependency)
## Configuration
- Configure options at build time: video system, sound system, platform target
- Runtime: Uses ATARI800_PATH env var to locate emulator (MCP server)
- Socket path: `/tmp/atari800_ai.sock` (hardcoded, configurable in code)
- Main: `configure.ac` - Autoconf template
- Generated: `configure` script
- Makefile generation: `Makefile.am` (src/Makefile.am, DOC/Makefile.am, tools/Makefile.am)
- Build wrapper: `build_ai.sh` - Builds with SDL 1.2 and AI interface enabled
- `--target=`: Platform target (default, android, falcon, firebee, ps2, libatari800, x11, motif, shm, xview)
- `--with-video=`: Graphics output (sdl, sdl2, curses, dosvga, x11, javanvm, no)
- `--with-sound=`: Audio output (sdl, sdl2, oss, falcon, dossb, javanvm, no)
- Feature flags: `--enable-emuos-altirra`, `--enable-pbi-mio`, `--enable-pbi-bb`, `--enable-netsio`
## Platform Requirements
- Autotools (autoconf 2.57+, automake 1.11+)
- GCC or Clang C compiler
- SDL 1.2 development headers (libsdl1.2-dev or Homebrew sdl@1.2)
- Optional: libpng, zlib dev headers
- Node.js 16+ for MCP server development
- Python 3 for AI client library
- SDL 1.2 runtime library
- zlib runtime library
- Platform-specific audio/graphics libraries (depends on configure options)
- Deployment targets:
- macOS 14.6+ with Apple Silicon
- Homebrew packages: sdl@1.2, autoconf, automake
- Works with both Intel and ARM architectures
<!-- GSD:stack-end -->

<!-- GSD:conventions-start source:CONVENTIONS.md -->
## Conventions

## Naming Patterns
- C source files: `lowercase_with_underscores.c` and `.h` (e.g., `antic.c`, `gtia.h`, `log.c`)
- JavaScript/Node.js: `camelCase.js` (e.g., `index.js`) or `lowercase_with_underscores.js`
- Python: `lowercase_with_underscores.py` (e.g., `atari800_ai.py`)
- C: `snake_case` (e.g., `ANTIC_Initialise`, `Log_print`, `ANTIC_Frame`)
- JavaScript: `camelCase` for functions (e.g., `sendCommand`, `formatScreen`, `isEmulatorRunning`)
- JavaScript: `PascalCase` for class/constructor names (e.g., `Server`, `Atari800AI`)
- Python: `snake_case` for functions (e.g., `connect`, `disconnect`, `_send`)
- C: `UPPERCASE` for constants and globals (e.g., `SOCKET_PATH`, `ANTIC_CHACTL`, `GTIA_M0PL`), `camelCase` for local/static variables (e.g., `consol_override`, `GTIA_speaker`)
- JavaScript: `camelCase` for variables (e.g., `emulatorProcess`, `expectedLength`, `SOCKET_PATH` for constants)
- Python: `snake_case` for variables (e.g., `socket_path`, `self.sock`), `UPPERCASE_SNAKE_CASE` for constants (e.g., `AKEY_A`, `DEFAULT_SOCKET`)
- C: No formal type naming convention; typedef structs use descriptive names (see symbol table in `monitor.c`: `symtable_rec`)
- JavaScript: Classes use `PascalCase` (e.g., `Atari800AI`, `Server`)
- Python: Classes use `PascalCase` (e.g., `Atari800AI`)
- Custom types in C: `UBYTE` (unsigned byte), `UWORD` (unsigned word) - defined in `atari.h`
## Code Style
- **Indentation**: Tabs in C files (observed in `log.c`, `monitor.c`, `gtia.c`)
- **Indentation**: 2 spaces in JavaScript (observed in `mcp-server/index.js`)
- **Indentation**: 4 spaces in Python (observed in `atari800_ai.py`)
- **Line length**: No strict limit enforced; pragmatic line wrapping observed
- **Brace style**: Opening braces on same line (JavaScript), K&R style in C
- No ESLint, Prettier, or similar tool detected in JavaScript/TypeScript
- No Pylint, Black, or similar tool detected in Python
- C code uses traditional GCC conventions without automated linting
- Manual code review appears to be the primary quality mechanism
## Import Organization
## Error Handling
- Try-catch blocks for exception handling
- Promise-based error handling with `.catch()` or try-await patterns
- Errors returned as response objects with `isError: true` flag
- Error messages included in response content for user-friendly feedback
- Custom exceptions for connection errors (e.g., `ConnectionError`, `TimeoutError`)
- Context managers (`__enter__`/`__exit__`) for resource management
- Defensive checks before operations (e.g., `if not self.sock:`)
- Return codes for error indication (not observed in examined files, but standard in C)
- Inline error checking with conditional logic
- Preprocessor conditionals for platform-specific error handling
## Logging
- Uses `console.error()` for status messages
- Only one logging call observed: `console.error('Atari 800 MCP Server running')`
- No structured logging framework in use
- Uses `print()` for user-facing output
- Includes formatted error messages with `print(f"...")`
- Error output goes to stdout, following shell conventions
- Custom logging via `Log_print()` function in `log.c`
- Supports platform-specific output (macOS: `ControlManagerMessagePrint`, Android: `__android_log_write`, default: `printf`)
- Optional buffered logging with `BUFFERED_LOG` configuration
## Comments
- Above function definitions explaining purpose and parameters (docstrings preferred)
- Before complex logic blocks explaining algorithm or workaround
- Inline comments for non-obvious decisions (rare in observed code)
- Not used in JavaScript files
- Block comments document function purposes
- Module-level docstring with usage examples (see `atari800_ai.py` top-level docstring)
- Class docstring explaining purpose: `"""Client for Atari800 AI interface"""`
- Function docstrings with one-line summary: `"""Connect to the emulator"""`
- Type hints for function signatures observed throughout
- File header with copyright and license (GPL 2)
- Section headers with comment blocks: `/* GTIA Registers ---------------------------------------------------------- */`
- Inline comments for important constants: `/* Number of cycles per scanline. */`
## Function Design
- JavaScript: Use object destructuring in MCP callbacks: `const { name, arguments: args } = request.params;`
- Python: Explicit parameters with type hints preferred: `def key(self, code: int, shift: bool = False) -> bool:`
- C: Minimal parameters; use global state for configuration
- JavaScript: Return consistent response object structures: `{ content: [{ type: 'text', text: '...' }], isError?: boolean }`
- Python: Return boolean for success/failure operations, dict/list for data queries
- C: Mix of return codes and side effects through globals
## Module Design
- JavaScript: Use ES6 named and default exports; `mcp-server/index.js` is entry point
- Python: Module-level functions and classes available for import; example script in `if __name__ == "__main__":`
- C: Header files declare exports; implementation in `.c` files
- JavaScript: Single file modules (e.g., `index.js` contains server setup and tool handlers)
- Python: Class-based organization with private methods (prefix `_`): `Atari800AI` class encapsulates all emulator interactions
- C: Modular organization with paired `.h`/`.c` files; public APIs declared in headers, implementation in source
- Python: `_send()` method uses underscore prefix to indicate private/internal method
- C: Static functions for private scope, extern declarations for public APIs
- JavaScript: All functions are module-level private unless explicitly exported
<!-- GSD:conventions-end -->

<!-- GSD:architecture-start source:ARCHITECTURE.md -->
## Architecture

## Pattern Overview
- Frame-based emulation cycle with synchronized chip components
- Device-centric design where each Atari hardware component is a separate module
- Clean separation between core emulation, platform-specific code, and AI interface
- CPU-driven execution model where instruction cycle limits per scanline enforce hardware timing
- Register-based state management for all chips (ANTIC, GTIA, POKEY, PIA, CPU)
## Layers
- Purpose: Implements the Atari 800 hardware behavior (CPU, memory, chips)
- Location: `src/` (core .c/.h files)
- Contains: CPU emulation (`cpu.c`), chip simulators (ANTIC, GTIA, POKEY, PIA), memory management, device I/O
- Depends on: System abstractions (nothing below it)
- Used by: Platform layer and AI interface
- Purpose: Provides cross-platform support (SDL, X11, Amiga, Android, etc.) and UI integration
- Location: `src/sdl/`, `src/amiga/`, `src/android/`, `src/x11/`, etc.
- Contains: Video output, input handling, timing synchronization, UI implementation
- Depends on: Core emulation layer
- Used by: Main executable entry point
- Purpose: Exposes emulator control via JSON socket API for external automation
- Location: `src/ai_interface.c`, `src/ai_interface.h`
- Contains: Socket server, command parser, chip state readers, input injection, screenshot/memory access
- Depends on: Core emulation layer (reads/writes to chips, memory, input)
- Used by: External AI agents, MCP server
- Purpose: Adapts emulator AI interface to Claude MCP protocol for tool integration
- Location: `mcp-server/`
- Contains: MCP server implementation, socket communication handler, tool definitions
- Depends on: AI interface socket protocol
- Used by: Claude agents via MCP
## Data Flow
- **CPU State:** `cpu.h` defines CPU_A, CPU_X, CPU_Y, CPU_PC, CPU_SP, CPU_P
- **Memory:** 64KB address space in `MEMORY_mem[65536]` with page-based read/write handlers
- **Chip Registers:** ANTIC at $D4xx, GTIA at $D0xx, POKEY at $D2xx, PIA at $D3xx
- **Hardware State:** GTIA_TRIG[], PIA_PORT_input[], POKEY registers for audio/keyboard
- **AI Overrides:** Stored in `AI_joy_override[]` and `AI_trig_override[]` arrays
## Key Abstractions
- Purpose: Encapsulate each Atari chip as a standalone state machine
- Examples: `antic.c` (display), `gtia.c` (graphics), `pokey.c` (sound/keyboard), `pia.c` (ports)
- Pattern: Each module has register state arrays, a `*_Frame()` function, and `*_GetByte()` / `*_PutByte()` accessors
- Memory-mapped I/O handled by `cpu_*_read_map[]` and `cpu_*_write_map[]` function pointers
- Purpose: Maintain cycle-accurate timing between CPU and ANTIC
- Examples: `ANTIC_xpos_limit` constrains CPU execution per scanline, `ANTIC_xpos` tracks cycle position
- Pattern: CPU loops until reaching `ANTIC_xpos_limit`, then scanline advances
- Purpose: Non-blocking JSON command interface via Unix domain socket
- Examples: `AI_Frame()` polls socket, handlers parse and respond to commands
- Pattern: Length-prefixed JSON messages, synchronous request/response, paused execution model
- Purpose: Provide chip-to-chip and AI-to-memory interaction
- Examples: `cpu_read_map[]` provides read functions per page, `Memory_SetByte()`, `Memory_GetByte()`
- Pattern: Each memory page can have custom read/write handler (ROM, RAM, chip I/O)
## Entry Points
- Location: `src/sdl/main.c:163`
- Triggers: Binary execution `./src/atari800 [args]`
- Responsibilities: Initialize SDL platform, call `Atari800_Initialise()`, run main emulation loop
- Location: `src/atari.c`
- Triggers: Called from platform main()
- Responsibilities: Parse command-line args, initialize all hardware modules, load ROMs/cartridges, set up memory map
- Location: `src/atari.c:1322`
- Triggers: Once per frame from main loop
- Responsibilities: Orchestrate one complete emulation frame (AI commands, input, chip execution, display)
- Location: `src/ai_interface.c`
- Triggers: Called from `Atari800_Initialise()` when `-ai` flag present
- Responsibilities: Create Unix socket server, set up non-blocking I/O, initialize state
- Location: `src/ai_interface.c`
- Triggers: Called at start of `Atari800_Frame()` before input processing
- Responsibilities: Accept socket connections, parse and execute JSON commands, pause/resume emulation
## Error Handling
- Chip register writes to invalid addresses are ignored (no-op)
- Missing ROM files default to cartridge or built-in BASIC
- Invalid AI commands return JSON error response without crashing
- Socket errors logged via `Log_print()` but emulation continues
- Memory violations (illegal addresses) wrapped by memory management layer
## Cross-Cutting Concerns
<!-- GSD:architecture-end -->

<!-- GSD:workflow-start source:GSD defaults -->
## GSD Workflow Enforcement

Before using Edit, Write, or other file-changing tools, start work through a GSD command so planning artifacts and execution context stay in sync.

Use these entry points:
- `/gsd:quick` for small fixes, doc updates, and ad-hoc tasks
- `/gsd:debug` for investigation and bug fixing
- `/gsd:execute-phase` for planned phase work

Do not make direct repo edits outside a GSD workflow unless the user explicitly asks to bypass it.
<!-- GSD:workflow-end -->



<!-- GSD:profile-start -->
## Developer Profile

> Profile not yet configured. Run `/gsd:profile-user` to generate your developer profile.
> This section is managed by `generate-claude-profile` -- do not edit manually.
<!-- GSD:profile-end -->
