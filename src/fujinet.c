/*
 * fujinet.c - FujiNet device emulation - main interface
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
#include <errno.h>

#include "config.h"
#include "fujinet.h"
#include "fujinet_network.h"
#include "fujinet_sio.h"
#include "log.h"
#include "util.h"

/* FujiNet global state */
int fujinet_enabled = 0;

/* Main logging macros */
#define FUJINET_LOG(msg) do { Log_print("FujiNet: %s", (msg)); } while(0)
#define FUJINET_LOG_ERROR(msg, ...) do { Log_print("FujiNet ERROR: " msg, ##__VA_ARGS__); } while(0)
#define FUJINET_LOG_WARN(msg, ...) do { Log_print("FujiNet WARN: " msg, ##__VA_ARGS__); } while(0)

/*
 * Initialize the FujiNet device emulation
 */
int FujiNet_Initialise(const char *host_port) {
    FUJINET_LOG("Initializing FujiNet device emulation");
    
    /* Initialize the network module */
    if (!Network_Initialize(host_port)) {
        FUJINET_LOG("Failed to initialize network connection");
        return 0;
    }
    
    /* Initialize the SIO module */
    FujiNet_SIO_Initialize();
    
    fujinet_enabled = 1;
    FUJINET_LOG("FujiNet device emulation initialized successfully");
    return 1;
}

/*
 * Shut down the FujiNet device emulation
 */
void FujiNet_Shutdown(void) {
    FUJINET_LOG("Shutting down FujiNet device emulation");
    
    if (fujinet_enabled) {
        FujiNet_SIO_Shutdown();
        Network_Shutdown();
        fujinet_enabled = 0;
    }
}

/*
 * Process an SIO command
 */
UBYTE FujiNet_ProcessCommand(const UBYTE *command_frame) {
    if (!fujinet_enabled) {
        FUJINET_LOG_ERROR("ProcessCommand called but not enabled");
        return FUJINET_SIO_ERROR_FRAME;
    }
    
    return FujiNet_SIO_ProcessCommand(command_frame);
}

/*
 * Get a byte from the device
 */
int FujiNet_GetByte(uint8_t *byte) {
    if (!fujinet_enabled) {
        FUJINET_LOG_ERROR("GetByte called but not enabled");
        return 0;
    }
    
    return FujiNet_SIO_GetByte(byte);
}

/*
 * Send a byte to the device
 */
int FujiNet_PutByte(uint8_t byte) {
    if (!fujinet_enabled) {
        FUJINET_LOG_ERROR("PutByte called but not enabled");
        return 0;
    }
    
    return FujiNet_SIO_PutByte(byte);
}

/*
 * Set the motor state
 */
void FujiNet_SetMotorState(int on) {
    if (!fujinet_enabled) {
        return;
    }
    
    FujiNet_SIO_SetMotorState(on);
}