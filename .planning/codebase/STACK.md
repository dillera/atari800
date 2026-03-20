# Technology Stack

**Analysis Date:** 2026-03-20

## Languages

**Primary:**
- C (C89/C99) - Core Atari 800 emulator implementation
- Python 3 - AI client library (`atari800_ai.py`)
- JavaScript/Node.js (ES Modules) - MCP server implementation

**Secondary:**
- Shell (Bash) - Build scripts

## Runtime

**Environment:**
- Native platform support: Linux, macOS, Windows (Cygwin/MinGW), Android, Raspberry Pi, Atari Falcon
- Current build: macOS 14.6+ with Apple Silicon (Homebrew)
- Python: 3.6+ (minimum)
- Node.js: 16+ (for MCP server)

**Package Manager:**
- Node.js: npm (with `package-lock.json`)
- No Python package manifest (standalone script)
- Build: Autotools (autoconf, automake)

## Frameworks

**Core:**
- Autotools (autoconf/automake) - C build system

**Graphics/Audio:**
- SDL 1.2 - Primary graphics and audio output (not SDL2, due to keyboard issues)
- Optional: SDL2, X11 (with Motif/XView/SHM variants), GLES2 (Raspberry Pi), curses

**Emulation:**
- CPU/6502: Native C implementation
- Chipset: Custom C implementations (ANTIC, GTIA, POKEY, PIA)

**Integration/MCP:**
- Model Context Protocol SDK (`@modelcontextprotocol/sdk` v0.5.0) - MCP server framework
- Node.js `net` module - Unix socket communication
- Node.js `child_process` - Process spawning

**Testing:**
- Not detected

**Build/Dev:**
- GNU autotools (configure, make)
- Shell build script: `build_ai.sh`
- C compiler (GCC/Clang)

## Key Dependencies

**Critical:**
- zlib - Compression for save files (checked in configure via `AC_CHECK_LIB(z,gzopen)`)
- pthread - Threading library (required for sound recording)
- libpng (optional) - PNG export support
- math library (libm) - Math functions

**Graphics:**
- SDL 1.2 - Window, input, audio output (`SDL_CONFIG=/opt/homebrew/bin/sdl-config`)
- X11 libraries (Linux/Unix targets)
- GLES2 (Raspberry Pi)
- Curses/NCurses/PDCurses (terminal UI fallback)

**Audio:**
- SDL 1.2 audio (primary)
- OSS (Open Sound System) - Linux support
- ALSA (via OSS compatibility)
- lame (libmp3lame) - MP3 audio export

**Networking:**
- libsocket/libgen (Solaris compatibility, if needed)
- Standard POSIX sockets (Unix domain sockets for AI interface)

**MCP Server:**
- `@modelcontextprotocol/sdk` ^0.5.0 (single Node.js dependency)

## Configuration

**Environment:**
- Configure options at build time: video system, sound system, platform target
- Runtime: Uses ATARI800_PATH env var to locate emulator (MCP server)
- Socket path: `/tmp/atari800_ai.sock` (hardcoded, configurable in code)

**Build:**
- Main: `configure.ac` - Autoconf template
- Generated: `configure` script
- Makefile generation: `Makefile.am` (src/Makefile.am, DOC/Makefile.am, tools/Makefile.am)
- Build wrapper: `build_ai.sh` - Builds with SDL 1.2 and AI interface enabled

**Build Options (from configure.ac):**
- `--target=`: Platform target (default, android, falcon, firebee, ps2, libatari800, x11, motif, shm, xview)
- `--with-video=`: Graphics output (sdl, sdl2, curses, dosvga, x11, javanvm, no)
- `--with-sound=`: Audio output (sdl, sdl2, oss, falcon, dossb, javanvm, no)
- Feature flags: `--enable-emuos-altirra`, `--enable-pbi-mio`, `--enable-pbi-bb`, `--enable-netsio`

## Platform Requirements

**Development:**
- Autotools (autoconf 2.57+, automake 1.11+)
- GCC or Clang C compiler
- SDL 1.2 development headers (libsdl1.2-dev or Homebrew sdl@1.2)
- Optional: libpng, zlib dev headers
- Node.js 16+ for MCP server development
- Python 3 for AI client library

**Production:**
- SDL 1.2 runtime library
- zlib runtime library
- Platform-specific audio/graphics libraries (depends on configure options)
- Deployment targets:
  - Linux/macOS/Windows: Standalone executable
  - Android: Via Android NDK/build system
  - Web-based: Via libatari800 target (not primary)
  - Raspberry Pi: With GLES2 and specialized build

**Current Build Machine:**
- macOS 14.6+ with Apple Silicon
- Homebrew packages: sdl@1.2, autoconf, automake
- Works with both Intel and ARM architectures

---

*Stack analysis: 2026-03-20*
