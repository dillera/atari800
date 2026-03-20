#!/bin/bash
# Build atari800 with AI interface

set -e

cd "$(dirname "$0")"

# Regenerate build system (ai_interface.c is in Makefile.am)
echo "Running autoreconf..."
autoreconf -if

# Configure with SDL 1.2 (NOT SDL2 - SDL2 has broken keyboard trigger!)
echo "Configuring with SDL 1.2..."
./configure \
    --with-video=sdl \
    --with-sound=sdl \
    SDL_CONFIG=/opt/homebrew/bin/sdl-config \
    CFLAGS="-O2 -g"

# Build (just the emulator, not docs/tools)
echo "Building..."
make -C src -j4

echo ""
echo "Build complete!"
echo "Binary: src/atari800"
echo ""
echo "Usage:"
echo "  ./src/atari800 -ai -xl -run your_program.xex"
echo ""
echo "Then connect to socket: /tmp/atari800_ai.sock"
echo "Send JSON commands with length prefix (e.g., '14\n{\"cmd\":\"ping\"}')"
