/*
 * fujinet_sio.c - FujiNet SIO command handling functions
 *
 * Copyright (C) 2020-2023 AtariGeekyJames
 * Copyright (C) 2023 DigiDude
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>  /* For memset */
#include <time.h>  /* For nanosleep */
#include <sys/time.h> /* For gettimeofday */

#include "config.h"
#include "fujinet.h"
#include "fujinet_netsio.h"
#include "fujinet_sio.h"
#include "log.h"
#include "util.h"
#include "atari.h"
#include "sio.h" /* For SIO command status codes */

/* Logging macros */
#define SIO_LOG_ERROR(msg, ...) do { Log_print("FujiNet SIO ERROR: " msg, ##__VA_ARGS__); } while(0)
#define SIO_LOG_WARN(msg, ...) do { Log_print("FujiNet SIO WARN: " msg, ##__VA_ARGS__); } while(0)
#ifdef DEBUG_FUJINET
#define SIO_LOG_DEBUG(msg, ...) do { Log_print("FujiNet SIO DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define SIO_LOG_DEBUG(msg, ...) ((void)0)
#endif

/* Device state tracking */
static int motor_state = 0;
static int sio_enabled = 0;

/* 
 * SIO module initialization
 */
void FujiNet_SIO_Initialize(void) {
    motor_state = 0;
    sio_enabled = 1;
    SIO_LOG_DEBUG("SIO module initialized");
}

/*
 * SIO module shutdown 
 */
void FujiNet_SIO_Shutdown(void) {
    sio_enabled = 0;
    SIO_LOG_DEBUG("SIO module shut down");
}

/*
 * Process SIO command
 */
UBYTE FujiNet_SIO_ProcessCommand(const UBYTE *command_frame) {
    if (!sio_enabled) {
        SIO_LOG_ERROR("ProcessCommand called but SIO not enabled");
        return FUJINET_SIO_ERROR_FRAME;
    }
    
    if (!command_frame) {
        SIO_LOG_ERROR("ProcessCommand called with NULL command_frame");
        return FUJINET_SIO_ERROR_FRAME;
    }
    
    SIO_LOG_DEBUG("SIO_ProcessCommand called for device 0x%02X, cmd 0x%02X", 
                  command_frame[0], command_frame[1]);

    /* First, calculate and verify checksum before sending */
    UBYTE checksum = 0;
    for (int i = 0; i < 4; i++) {
        checksum += command_frame[i];
    }
    
    if (checksum != command_frame[4]) {
        SIO_LOG_WARN("SIO command has invalid checksum: calculated 0x%02X, got 0x%02X", 
                    checksum, command_frame[4]);
        /* Continue anyway - might be an intentional non-standard checksum */
    }

    /* Send the command to FujiNet via NetSIO */
    int result = FujiNet_NetSIO_SendCommand(command_frame);
    
    if (result == 1) {
        /* Command accepted (ACK) */
        return FUJINET_SIO_ACK;
    } else if (result == 0) {
        /* Command rejected (NAK) */
        return FUJINET_SIO_NAK;
    } else {
        /* Error */
        return FUJINET_SIO_ERROR;
    }
}

/*
 * Read a byte from the SIO command response buffer
 * Returns:
 *   1 = Byte read successfully (byte is stored in *byte)
 *   0 = No data available
 *  -1 = Error
 */
int FujiNet_SIO_GetByte(uint8_t *byte) {
    if (!sio_enabled) {
        SIO_LOG_ERROR("GetByte called but SIO not enabled");
        return -1;
    }
    
    if (!byte) {
        SIO_LOG_ERROR("GetByte called with NULL byte pointer");
        return -1;
    }
    
    /* Get a byte from the NetSIO response buffer */
    int result = FujiNet_NetSIO_GetByte(byte);
    
    if (result == 1) {
        SIO_LOG_DEBUG("Read byte 0x%02X from NetSIO", *byte);
        return 1;
    } else if (result == 0) {
        SIO_LOG_DEBUG("No data available from NetSIO");
        return 0;
    } else {
        SIO_LOG_ERROR("Error reading byte from NetSIO");
        return -1;
    }
}

/*
 * Send a byte to the FujiNet device
 * Returns:
 *   1 = Byte sent successfully
 *   0 = No data sent (device not ready)
 *  -1 = Error
 */
int FujiNet_SIO_PutByte(uint8_t byte) {
    if (!sio_enabled) {
        SIO_LOG_ERROR("PutByte called but SIO not enabled");
        return -1;
    }
    
    /* Send the byte via NetSIO */
    int result = FujiNet_NetSIO_PutByte(byte);
    
    if (result == 1) {
        SIO_LOG_DEBUG("Sent byte 0x%02X via NetSIO", byte);
        return 1;
    } else {
        SIO_LOG_ERROR("Failed to send byte 0x%02X via NetSIO", byte);
        return -1;
    }
}

/*
 * Update motor state for disk drives
 */
void FujiNet_SIO_SetMotorState(int on) {
    if (!sio_enabled) {
        return;
    }
    
    if (motor_state != on) {
        motor_state = on;
        SIO_LOG_DEBUG("Motor state changed to %s", on ? "ON" : "OFF");
        
        /* Update motor state via NetSIO */
        FujiNet_NetSIO_SetMotorState(on);
    }
}

/*
 * Check if SIO device is enabled
 */
int FujiNet_SIO_IsDeviceEnabled(void) {
    return sio_enabled;
}

/*
 * Get the current position within the response buffer
 */
int FujiNet_SIO_GetResponseBufferPos(void) {
    return FujiNet_NetSIO_GetResponseBufferPos();
}

/*
 * Get the total size of the data currently in the response buffer
 */
int FujiNet_SIO_GetResponseBufferSize(void) {
    return FujiNet_NetSIO_GetResponseBufferSize();
}
