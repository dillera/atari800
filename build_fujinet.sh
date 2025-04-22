#!/bin/bash
# Manual build script for Atari800 with FujiNet support
# This bypasses the configure script issues

# Set compiler flags
CFLAGS="-g -DUSE_FUJINET -DDEBUG_FUJINET -I/opt/homebrew/include"
LDFLAGS="-L/opt/homebrew/lib"

# Add include paths for the Atari800 headers
CFLAGS="$CFLAGS -I/Users/dillera/code/atari800/src -I/Users/dillera/code/atari800/src/netsio"

# Create a simple config.h for basic compilation
mkdir -p build_fujinet/include
cat > build_fujinet/include/config.h << EOF
/* Minimal config.h for manual build */
#define HAVE_STDINT_H 1
#define HAVE_STRCASECMP 1
#define HAVE_ARPA_INET_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_TERMIOS_H 1
#define USE_FUJINET 1
#define DEBUG_FUJINET 1
EOF

# Create a minimal atari.h for basic compilation
cat > build_fujinet/include/atari.h << EOF
/* Minimal atari.h for standalone testing */
#ifndef ATARI_H
#define ATARI_H

#include <stdint.h>

/* Basic types used throughout the codebase */
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int8_t SBYTE;
typedef int16_t SWORD;
typedef int32_t SLONG;

#endif /* ATARI_H */
EOF

# Add the generated config.h to include path
CFLAGS="$CFLAGS -I/Users/dillera/code/atari800/build_fujinet/include"

# SDL detection - macOS typically needs this
if [ -x "$(command -v sdl2-config)" ]; then
    CFLAGS="$CFLAGS $(sdl2-config --cflags)"
    LDFLAGS="$LDFLAGS $(sdl2-config --libs)"
elif [ -x "$(command -v sdl-config)" ]; then
    CFLAGS="$CFLAGS $(sdl-config --cflags)"
    LDFLAGS="$LDFLAGS $(sdl-config --libs)"
else
    echo "SDL development libraries not found. Please install SDL first."
    exit 1
fi

# Create a build directory
mkdir -p build_fujinet

# Create a standalone version of netsio.c that doesn't rely on atari800 logging
echo "Creating standalone netsio.c..."
cp src/netsio/netsio.c build_fujinet/netsio_standalone.c
sed -i '' 's/#include "log.h"/#include <stdio.h>\n#define Log_print printf\n#define Log_flushlog() fflush(stdout)/g' build_fujinet/netsio_standalone.c

# Compile NetSIO module first
echo "Compiling NetSIO module..."
gcc $CFLAGS -c build_fujinet/netsio_standalone.c -o build_fujinet/netsio.o

# Compile FujiNet components
echo "Compiling FujiNet components..."
gcc $CFLAGS -c src/fujinet.c -o build_fujinet/fujinet.o || true
gcc $CFLAGS -c src/fujinet_netsio.c -o build_fujinet/fujinet_netsio.o || true
gcc $CFLAGS -c src/fujinet_network.c -o build_fujinet/fujinet_network.o || true
gcc $CFLAGS -c src/fujinet_sio.c -o build_fujinet/fujinet_sio.o || true
gcc $CFLAGS -c src/fujinet_sio_handler.c -o build_fujinet/fujinet_sio_handler.o || true

# Compile a test executable for FujiNet/NetSIO
echo "Compiling NetSIO test..."
gcc $CFLAGS $LDFLAGS src/netsio/netsio_test.c build_fujinet/netsio.o -o build_fujinet/netsio_test \
    -lm -lz || true

echo "Build completed. Check for any errors above."
echo "The test executable is at build_fujinet/netsio_test"
