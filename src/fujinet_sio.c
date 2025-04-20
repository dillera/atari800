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
#include <string.h>

#include "config.h"
#include "fujinet_sio.h"
#include "fujinet_network.h"
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

    /* Check if command is for the FujiNet device */
    if (command_frame[0] != FUJINET_DEVICE_ID) {
        SIO_LOG_DEBUG("Command not for FujiNet device (device ID 0x%02X)", command_frame[0]);
        return FUJINET_SIO_ERROR_NAK; /* Not for us */
    }

    /* Reset receive buffers for new command */
    /* Reset any status indicators */

    /* Send the 5-byte SIO command frame encapsulated in an Altirra Custom Device message */
    SIO_LOG_DEBUG("Sending SIO command frame...");
    if (!Network_SendAltirraMessage(EVENT_SCRIPT_POST, FUJINET_DEVICE_ID, command_frame, 5)) {
        SIO_LOG_ERROR("Failed to send Altirra message for SIO command");
        /* Consider closing/resetting connection? */
        return FUJINET_SIO_ERROR_GENERAL;
    }

    /* Command sent successfully - SIO bus timing now handled by NetSIO hub */
    return FUJINET_SIO_COMPLETE;
}

/*
 * Get a byte from the SIO interface
 * Returns:
 *   1 - Data byte available in *byte
 *  -1 - Status byte available in *byte
 *   0 - Error or timeout occurred
 */
int FujiNet_SIO_GetByte(uint8_t *byte) {
    if (!sio_enabled) {
        SIO_LOG_ERROR("SIO_GetByte called but not enabled");
        return 0;
    }
    
    return Network_GetByte(byte);
}

/*
 * Send a byte via SIO 
 */
int FujiNet_SIO_PutByte(uint8_t byte) {
    if (!sio_enabled) {
        SIO_LOG_ERROR("SIO_PutByte called but not enabled");
        return 0;
    }
    
    return Network_PutByte(byte);
}

/*
 * Update motor state for disk drives
 */
void FujiNet_SIO_SetMotorState(int on) {
    if (!sio_enabled) {
        return;
    }
    
    if (on != motor_state) {
        motor_state = on;
        SIO_LOG_DEBUG("Motor state changed to %s", on ? "ON" : "OFF");
        /* Motor state changes are currently not sent to NetSIO */
    }
}

/*
 * Check if SIO device is enabled
 */
int FujiNet_SIO_IsDeviceEnabled(void) {
    return sio_enabled;
}
