# FujiNet NetSIO Test Application

This document provides information on the FujiNet NetSIO Test Application, which is designed to test communication with the NetSIO hub using the Altirra Custom Device Protocol.

## Overview

The NetSIO Test Application (`fujinet_netsio_test.c`) is a stand-alone utility that:

1. Establishes a TCP connection to the NetSIO hub
2. Sends a test SIO command sequence following the Altirra protocol
3. Waits for and processes SIO response data from the hub
4. Verifies the response format and prints the results

This tool is useful for:
- Verifying connectivity to the NetSIO hub
- Testing the Altirra Custom Device Protocol implementation
- Debugging communication issues between Atari800 emulator and NetSIO peripherals

## Building the Application

To compile the application, run:

```bash
gcc src/fujinet_netsio_test.c -o fujinet_netsio_test -Wall
```

## Usage

```
fujinet_netsio_test [-h host] [-p port] [-v] [-?]
```

### Options

- `-h host`: Specify the NetSIO hub hostname/IP (default: 127.0.0.1)
- `-p port`: Specify the NetSIO hub TCP port (default: 9996)
- `-v`: Enable verbose debugging output
- `-?`: Show usage information

### Exit Codes

- **0**: Success (command sent and response received)
- **1**: Failure (command failed or response not properly received)

## Example Usage

### Basic Test

```bash
./fujinet_netsio_test
```

This will connect to the NetSIO hub at 127.0.0.1:9996 and send a test "Get Status" (0x4E) command to device ID 0x31.

### Verbose Output

```bash
./fujinet_netsio_test -v
```

Enables detailed output showing all message headers, payloads, and response data.

### Custom Server

```bash
./fujinet_netsio_test -h 192.168.1.100 -p 8000
```

Connect to a NetSIO hub running at 192.168.1.100 on port 8000.

## Protocol Details

The application communicates with the NetSIO hub using the Altirra Custom Device Protocol. This protocol consists of:

### Message Format

Each message has:
- 8-byte header: total_length (4 bytes), timestamp (4 bytes)
- Payload: event (1 byte), arg (1 byte), data (variable)

### SIO Command Sequence

The test application sends:

1. `COMMAND_ON (0x11)` with device ID
2. `DATA_BLOCK (0x02)` with command byte, aux1, aux2
3. `COMMAND_OFF_SYNC (0x18)` with checksum

### SIO Response

The application expects the hub to respond with:

1. `SYNC_RESPONSE (0x81)` acknowledging command completion
2. A sequence of `DATA_BYTE (0x01)` messages containing response bytes

For the "Get Status" command (0x4E), it expects:
- Status byte 'C' (0x43)
- 128 bytes of status data

## Troubleshooting

### Common Issues

1. **Connection Refused**: Ensure the NetSIO hub is running and listening on the specified port
2. **Timeout Errors**: Check if the hub is correctly processing the Altirra protocol messages
3. **Unexpected Event Types**: Verify that the hub is correctly implementing the Altirra protocol

### Debugging

Use the `-v` option for verbose output to see:
- Detailed message headers and payloads
- Raw bytes being sent and received
- Parsing and processing of messages

## Relationship to Atari800 and FujiNet

The NetSIO Test Application simulates the communication that occurs between:
1. The Atari800 emulator with NetSIO support
2. The FujiNet NetSIO hub
3. FujiNet peripheral devices

It can be used as a standalone tool to verify hub functionality without needing the full Atari800 emulator.
