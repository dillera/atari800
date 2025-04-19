# FujiNet Integration for Atari800 Emulator

This document describes the integration of FujiNet support into the Atari800 emulator.

## Overview

FujiNet is a WiFi networking device for Atari 8-bit computers that provides:

- Network connectivity
- Virtual disk drives
- Printer emulation
- Modern storage capabilities

This integration enables the Atari800 emulator to communicate with FujiNet hardware or software implementations through the SIO (Serial Input/Output) interface.

## Changes Made

The integration required restoring and modifying several SIO functions in the Atari800 codebase:

1. **Restored SIO Functions:**
   - `SIO_ReadSector`, `SIO_WriteSector`, `SIO_FormatDisk`
   - `SIO_ReadStatusBlock`, `SIO_WriteStatusBlock`, `SIO_DriveStatus`
   - `Command_Frame` (as a static helper function)

2. **Added Missing Definitions:**
   - SIO status codes (`SIO_OK`, `SIO_ERROR`, `SIO_COMPLETE`, `SIO_CHECKSUM_ERROR`)
   - SIO protocol bytes (`SIO_ACK`, `SIO_NAK`, `SIO_COMPLETE_FRAME`, `SIO_ERROR_FRAME`)
   - Buffer size definition (`SIO_BUFFER_SIZE`)

3. **Fixed Function Signatures:**
   - Aligned parameter types with declarations in `sio.h`
   - Added buffer parameters where needed
   - Fixed unit type conflicts (`UBYTE` vs `int`)

4. **Compatibility Improvements:**
   - Commented out incompatible VAPI struct member access
   - Fixed call patterns for `SIO_Handler`
   - Standardized on C89 compatibility
   - Removed duplicated code

5. **FujiNet Integration:**
   - Added FujiNet initialization in `SIO_Initialise`
   - Added FujiNet cleanup in `SIO_Exit`
   - Modified `Command_Frame` to check if commands should be handled by FujiNet
   - Implemented proper response handling for FujiNet commands

## Using FujiNet with Atari800

To use FujiNet with the Atari800 emulator:

1. Configure your FujiNet device/software to be accessible on your network
2. Launch the Atari800 emulator with FujiNet support enabled
3. Access FujiNet features through standard Atari disk I/O operations

## Building

The FujiNet integration is conditionally compiled based on the `USE_FUJINET` preprocessor define. When building with FujiNet support, ensure this is defined.

## Future Improvements

- Enhanced error handling for network connectivity issues
- Additional documentation for FujiNet-specific features
- Integration testing with various FujiNet implementations

## Contributors

This integration work was performed by restoring and adapting code from the original Atari800 SIO implementation to work with FujiNet.
