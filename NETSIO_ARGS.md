# NetSIO Command-Line Arguments

## Overview
The atari800 emulator now supports flexible NetSIO/FujiNet configuration via command-line arguments.

## Arguments

### `-enable netsio`
Enables NetSIO emulation with the default UDP port **9997**.

**Example:**
```bash
./atari800 -enable netsio
```

### `-netsio-port <port>`
Enables NetSIO emulation and sets a custom UDP port for communication with the NetSIO hub.

**Example:**
```bash
./atari800 -netsio-port 9998
./atari800 -netsio-port 9999
```

## Combined Usage

You can combine NetSIO arguments with other emulator options:

```bash
# Enable NetSIO with custom port and run a program
./atari800 -netsio-port 9998 -run myprogram.atr

# Enable NetSIO with custom port and start in monitor
./atari800 -netsio-port 9999 -monitor
```

## Default Behavior

- **Default port:** 9997 (when using `-enable netsio`)
- **Port range:** 1-65535 (standard UDP port range)
- **Invalid port:** Port 0 is rejected with an error message

## Help

View all available options including NetSIO arguments:
```bash
./atari800 -help | grep -A 2 netsio
```

## Implementation Details

Both arguments:
1. Disable patched SIO for all devices (H:, P:, R: patches)
2. Initialize the NetSIO subsystem with the specified port
3. Connect to the NetSIO hub for FujiNet peripheral support

The port is passed to `netsio_init()` which binds to that UDP port and establishes communication with the FujiNet peripheral.
