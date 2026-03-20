# External Integrations

**Analysis Date:** 2026-03-20

## APIs & External Services

**Model Context Protocol (MCP):**
- Model Context Protocol - AI assistant integration framework
  - SDK: `@modelcontextprotocol/sdk` v0.5.0
  - Auth: None (uses stdio transport)
  - Purpose: Provides MCP tools for Claude/other AI to control emulator

**FujiNet (Optional):**
- Network I/O device support (NetSIO feature)
  - Files: `src/netsio.h`, `src/netsio.c` (referenced in configure)
  - Purpose: Enables network connectivity simulation in emulated Atari
  - Implementation: Custom network socket layer (`AI_SOCKET_PATH` uses Unix domain sockets)

## Data Storage

**Databases:**
- None - Pure emulator, no persistent data layer

**File Storage:**
- Local filesystem only
  - Emulator ROM files (required at startup)
  - Cartridge files (.CAR format)
  - Disk images (.ATR, .XFD, .PRO, .DCM)
  - Tape images (.CAS)
  - Program files (.XEX, .COM, .BAS binary)
  - Save states (`.sta` files via `statesav.h`)
  - Configuration files (`~/.atari800/atari800.cfg`)

**Save State Format:**
- Binary format with gzip compression (`zlib`)
- Files: `src/statesav.c`, `src/statesav.h`
- Used by MCP tools: `atari_save_state`, `atari_load_state`

**Caching:**
- None - All data operations are direct file I/O

## Authentication & Identity

**Auth Provider:**
- Custom/Internal only
- No external authentication required
- MCP server uses stdio transport (inherits caller's auth)

**AI Interface Authentication:**
- Socket-based (Unix domain socket `/tmp/atari800_ai.sock`)
- Authentication: None (file system permissions control access)
- Implementation: `src/ai_interface.c`

## Monitoring & Observability

**Error Tracking:**
- None - Console/stderr output only

**Logs:**
- Custom logging via `src/log.c`, `src/log.h`
- Log level controlled at compile time
- No external logging service integration

**Debug Output:**
- AI interface debug buffer: `ai_debug_buffer` in `src/ai_interface.c` (4KB buffer)
- Console output to stderr for development

## CI/CD & Deployment

**Hosting:**
- Not web-hosted - Desktop/embedded emulator
- Distribution: Compiled binary executable
- Custom targets support: Android, Raspberry Pi, Windows, Linux, macOS

**CI Pipeline:**
- Travis CI (`.travis.yml` present but legacy)
- GitHub Actions (`.github/` directory present)
- Build script: `build_ai.sh` (manual macOS build with AI interface)
- No npm build pipeline detected for MCP server (runs as standalone)

**Artifact Management:**
- No package registry
- Build produces: `src/atari800` executable
- MCP server runs via Node.js: `node mcp-server/index.js`

## Environment Configuration

**Required env vars:**
- `ATARI800_PATH` (optional) - Path to atari800 emulator binary for MCP server
  - Default: `../src/atari800` relative to mcp-server directory
  - Files: `mcp-server/index.js` line 27

**Optional env vars:**
- Standard build variables: `CFLAGS`, `LDFLAGS`, `CC` (for autotools configure)
- `SDL_CONFIG` - Path to SDL 1.2 config script (used in build_ai.sh)

**Secrets location:**
- No secrets management - Emulator is self-contained
- Configuration file: `~/.atari800/atari800.cfg` (user's home directory)
- Socket: `/tmp/atari800_ai.sock` (temp directory, Unix domain socket)

## Webhooks & Callbacks

**Incoming:**
- None - Command/response protocol via socket, not webhooks

**Outgoing:**
- None - No external service callbacks

**Protocol:**
- AI Interface Protocol (custom JSON over Unix domain socket)
  - Request format: `{length}\n{json_command}`
  - Response format: Same format
  - Commands: ping, run, step, pause, reset, load, key, joystick, peek, poke, cpu, screen_ascii, gtia, pokey, antic, pia, save_state, load_state
  - Files: `src/ai_interface.c`, `atari800_ai.py`, `mcp-server/index.js`

## Socket Communication Protocol

**AI Interface Socket:**
- Path: `/tmp/atari800_ai.sock`
- Type: Unix domain socket (AF_UNIX)
- Transport: Raw socket with length-prefixed JSON frames
- Server: `src/ai_interface.c` (emulator side)
- Client implementations:
  - Python: `atari800_ai.py` (Atari800AI class)
  - JavaScript: `mcp-server/index.js` (sendCommand function)

**Supported Commands:**

Control:
- `load` - Load program file
- `run` - Execute N frames
- `step` - Execute N CPU instructions
- `pause` - Pause execution
- `reset` - Cold reset

Input:
- `key` - Press Atari key code
- `key_release` - Release all keys
- `joystick` - Set joystick direction and fire
- `consol` - Press console buttons (Start, Select, Option)

State Inspection:
- `cpu` - Get CPU registers and flags
- `memory` (peek/poke) - Read/write memory
- `screen_ascii` - Get 40x24 character display
- `gtia` - Get graphics chip state
- `pokey` - Get sound chip state
- `antic` - Get display list controller state
- `pia` - Get parallel interface adapter state

Save/Restore:
- `save_state` - Save emulator state file
- `load_state` - Load emulator state file

Utility:
- `ping` - Check connection

## External Resource Dependencies

**ROM Requirements:**
- Atari 800 System ROM (required)
- Atari 800 XL/130XE ROM (for -xl mode)
- BASIC ROM (optional, for Atari BASIC)
- Game/Demo cartridges (user-provided)

**No Cloud Dependencies:**
- Emulator is fully offline
- No internet connectivity required (except optional NetSIO for network simulation)

---

*Integration audit: 2026-03-20*
