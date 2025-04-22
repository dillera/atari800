# FujiNet Integration for Atari800 Emulator

## Overview

This document describes the integration of FujiNet support into the Atari800 emulator, enabling the emulator to communicate with a FujiNet device via UDP and the NetSIO protocol.

**Project Codename:** Able Archer

## What is FujiNet?

FujiNet is a Wi-Fi networking device for the Atari 8-bit line of computers, providing access to network resources like disk images, printers, and more. It acts as a peripheral device for the Atari, communicating via the SIO (Serial Input/Output) interface.

## Architecture

The integration follows a modular approach:

```
┌─────────────────┐     ┌─────────────────┐     ┌─────────────────┐
│   atari800      │     │      NetSIO     │     │     FujiNet     │
│   emulator      │◄───►│       Hub       │◄───►│     device      │
└─────────────────┘     └─────────────────┘     └─────────────────┘
     (Emulator)              (Bridge)              (Peripheral)
```

- **Atari800 Emulator**: Modified to redirect SIO operations to FujiNet via UDP
- **NetSIO Hub**: Acts as a protocol bridge (external component)
- **FujiNet Device**: The physical or virtual device that provides services to the emulated Atari

### Components

The implementation is split into three main modules:

1. **fujinet.c/h**: High-level interface that integrates with the emulator's SIO system
2. **fujinet_udp.c/h**: Low-level UDP socket handling (initialization, sending, receiving)
3. **fujinet_netsio.c/h**: NetSIO protocol implementation (handshaking, commands, responses)

## Technical Details

### Protocol

- **NetSIO**: Protocol for translating SIO operations over UDP
- **Port**: Communicates with the NetSIO Hub on UDP port 9997

### Network Message Types

- **Handshaking**: PING_REQUEST(0xC2), ALIVE_REQUEST(0xC4), CREDIT_STATUS(0xC6)
- **Commands**: COMMAND_ON(0x11), COMMAND_OFF(0x10), COMMAND_OFF_SYNC(0x18)
- **Data**: DATA_BLOCK(0x02)
- **Responses**: SYNC_RESPONSE(0x81)

## Building with FujiNet Support

### Prerequisites

- Standard build tools (make, gcc)
- libSDL development libraries
- On Windows: WinSock2 libraries

### Compilation

FujiNet support is enabled by default with the new build system:

```bash
./configure
make
```

To explicitly enable or disable FujiNet support:

```bash
# Enable FujiNet (default)
./configure --enable-fujinet

# Disable FujiNet
./configure --disable-fujinet
```

For debugging, you can add additional flags:

```bash
make CFLAGS="-DUSE_FUJINET -DDEBUG_FUJINET"
```

## Running with FujiNet

### Basic Setup

1. Start the NetSIO Hub
   ```bash
   cd fujinet-bridge
   python -m netsiohub
   ```

2. Launch the Atari800 emulator
   ```bash
   ./atari800
   ```

The emulator will automatically attempt to connect to the NetSIO Hub on localhost port 9997.

### Configuration

The default configuration connects to the NetSIO Hub on `localhost:9997`. This can be changed in the emulator's configuration file.

## Troubleshooting

If you encounter issues with the FujiNet connection:

1. Verify the NetSIO Hub is running
2. Check if any firewall is blocking UDP port 9997
3. Enable debug output with `-DDEBUG_FUJINET` to see detailed logs

## Development Notes

### Implementation Details

- The FujiNet integration is conditionally compiled with the `USE_FUJINET` preprocessor flag
- All networking is done via non-blocking UDP sockets
- NetSIO protocol requires careful management of command credits and synchronization

### Future Improvements

- Support for configuring the NetSIO Hub address and port
- Enhanced error handling and recovery
- More comprehensive debugging options

## Credits

- Atari800 development team
- FujiNet project contributors
- NetSIO protocol designers
