# FujiNet Integration for Atari800 Emulator
![a800-able-archer-logo_small](https://github.com/user-attachments/assets/54b581a0-889d-4ad4-8d96-132734431bbb)


## Overview

This document describes the integration of FujiNet support into the Atari800 emulator, enabling the emulator to communicate with a FujiNet device via UDP and the NetSIO protocol.

**Project Codename:** Able Archer

## What is FujiNet?

FujiNet is a Wi-Fi networking device for the Atari 8-bit line of computers, providing access to network resources like disk images, printers, and more. It acts as a peripheral device for the Atari, communicating via the SIO (Serial Input/Output) interface.

## Architecture

The integration establishes a direct connection between the Atari800 emulator and the FujiNet device:

```
┌─────────────────┐     ┌─────────────────┐
│   atari800      │     │     FujiNet     │
│   emulator      │◄───►│     device      │
└─────────────────┘     └─────────────────┘
                        UDP:9997
```

- **Atari800 Emulator**: Modified to redirect SIO operations to FujiNet via UDP using NetSIO protocol
- **FujiNet Device**: Acts as a virtual peripheral device (floppy, printer, cassette, etc.)

### Components

The implementation is split into three main modules:

1. **fujinet.c/h**: High-level interface that integrates with the emulator's SIO system
2. **fujinet_udp.c/h**: Low-level UDP socket handling (initialization, sending, receiving)
3. **fujinet_netsio.c/h**: NetSIO protocol implementation (handshaking, commands, responses)

## Technical Details

### Protocol

- **NetSIO**: Protocol for translating SIO operations over UDP
- **Port**: Communicates directly with the FujiNet device on UDP port 9997

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

1. Make sure the FujiNet device is powered on and connected to your network

2. Launch the Atari800 emulator
   ```bash
   ./atari800
   ```

The emulator will automatically attempt to connect to the FujiNet device on UDP port 9997.

### Configuration

The default configuration connects to the FujiNet device on `localhost:9997`. For connecting to a physical FujiNet on your network, you would need to specify its IP address.

## SIO Call Flow

This section details the complete execution path of an SIO (Serial Input/Output) command from the emulator's main loop to the FujiNet device and back, including the CPU synchronization mechanism.

### 1. Program Initialization and Main Loop

The execution begins in `main()` which calls:

```
Atari800_Initialise() -> FujiNet_Initialise()
```

`FujiNet_Initialise()` sets up:
- UDP socket on port 9997 via `FujiNet_UDP_Initialize()`
- NetSIO protocol state via `FujiNet_NetSIO_Initialize()`

After initialization, the emulator enters its main loop in `main()`, which repeatedly calls `Atari800_Frame()`.

### 2. Frame Execution

The `Atari800_Frame()` function is the heart of the emulation, coordinating all subsystems for each frame:

```
Atari800_Frame()
  ↓
ANTIC_Frame() -> CPU_GO()
```

`ANTIC_Frame()` emulates the ANTIC chip and calls `CPU_GO()` to execute CPU instructions for each scanline. The CPU executes 6502 code, which may trigger SIO operations through hardware register access.

### 3. CPU Execution and SIO Patch

When an Atari program performs SIO operations (via JSR to $E459), the emulator intercepts this through the SIO patch mechanism:

```
CPU_GO()
  ↓
Check if PC == 0xE459 (SIO entry point)
  ↓
If true: call SIO_Handler() instead of executing OS SIO code
```

The CPU also checks if it should pause execution while waiting for FujiNet:

```
CPU_GO() 
  ↓
Check if fujinet_WaitingForSync == TRUE
  ↓
If true: return without executing instructions (pauses CPU)
```

### 4. SIO Command Handling

The `SIO_Handler()` function processes the SIO command from the emulated Atari:

```
SIO_Handler()
  ↓
Extract command parameters (device, command, aux1, aux2, buffer, length)
  ↓
When USE_FUJINET is defined:
  Route ALL SIO commands to FujiNet via SIO_SendCommandToFujiNet()
```

### 5. FujiNet Command Transmission

`SIO_SendCommandToFujiNet()` is a wrapper around the core FujiNet communication function:

```
SIO_SendCommandToFujiNet()
  ↓
FujiNet_SendSIOCommand()
```

`FujiNet_SendSIOCommand()` performs the actual NetSIO protocol communication:

```
FujiNet_SendSIOCommand()
  ↓
1. Set fujinet_WaitingForSync = TRUE (pauses CPU execution)
  ↓
2. Build and send NetSIO command packet via FujiNet_UDP_Send()
  ↓
3. Enter polling loop waiting for SYNC_RESPONSE
  ↓
4. When SYNC received: Set fujinet_WaitingForSync = FALSE (resumes CPU)
  ↓
5. Return result code to SIO_Handler
```

### 6. NetSIO Packet Processing

During operation, the `FujiNet_Update()` function is called regularly to process incoming packets:

```
FujiNet_Update()
  ↓
Check for incoming UDP packets
  ↓
FujiNet_NetSIO_ProcessPacket() handles protocol details:
  - PING maintenance
  - CREDIT management
  - DATA processing
```

### 7. Synchronization Mechanism

The key to proper SIO timing is the CPU pause/resume mechanism:

1. When `FujiNet_SendSIOCommand()` is called, it sets `fujinet_WaitingForSync = TRUE`
2. This causes `CPU_GO()` to return immediately without executing instructions
3. The emulator continues to call `FujiNet_Update()` during this pause
4. When a SYNC response is received, `fujinet_WaitingForSync` is set to `FALSE`
5. CPU execution resumes at normal speed

This synchronization ensures that the emulated Atari OS waits appropriately for SIO operations to complete, just as a real Atari would wait for its SIO devices.

## Troubleshooting

If you encounter issues with the FujiNet connection:

1. Verify the FujiNet device is powered on and connected to your network
2. Check if any firewall is blocking UDP port 9997
3. Enable debug output with `-DDEBUG_FUJINET` to see detailed logs

## Development Notes

### Implementation Details

- The FujiNet integration is conditionally compiled with the `USE_FUJINET` preprocessor flag
- All networking is done via non-blocking UDP sockets

## Credits

- Atari800 development team
- FujiNet project contributors
- NetSIO protocol designers (APC)
