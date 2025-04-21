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
#include "fujinet_network.h"
#include "fujinet_sio.h"
#include "log.h"
#include "util.h"
#include "atari.h"
#include "sio.h" /* For SIO command status codes */

/* Response buffer for SIO commands */
static UBYTE fujinet_response_buffer[FUJINET_BUFFER_SIZE];
static int fujinet_response_buffer_pos = 0;
static int fujinet_response_buffer_size = 0;

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

    /* In NetSIO protocol, we handle all device IDs, not just FujiNet device */
    /* Reset receive buffers for new command */
    /* Reset any status indicators */

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

    /* Get the components of the SIO command frame */
    uint8_t devid = command_frame[0];
    uint8_t command = command_frame[1];
    uint8_t aux1 = command_frame[2];
    uint8_t aux2 = command_frame[3];
    uint8_t frame_checksum = command_frame[4];
    
    SIO_LOG_DEBUG("Sending SIO command frame for device 0x%02X (cmd=0x%02X, aux1=0x%02X, aux2=0x%02X, chksum=0x%02X)...",
                devid, command, aux1, aux2, frame_checksum);
    
    /* Enhanced error handling for network send operation */
    int send_attempts = 0;
    int max_attempts = 3;
    int success = 0;
    
    while (send_attempts < max_attempts && !success) {
        send_attempts++;
        
        /* Follow NetSIO protocol sequence for SIO commands:
           1. COMMAND_ON with device ID
           2. DATA_BLOCK with command, aux1, aux2
           3. COMMAND_OFF_SYNC with checksum and sync request counter */
        
        /* Step 1: Send COMMAND_ON with device ID */
        if (!Network_SendAltirraMessage(NETSIO_COMMAND_ON, devid, NULL, 0)) {
            if (send_attempts < max_attempts) {
                SIO_LOG_WARN("Failed to send COMMAND_ON, retrying (attempt %d/%d)...", 
                            send_attempts, max_attempts);
                continue;
            } else {
                SIO_LOG_ERROR("Failed to send COMMAND_ON after %d attempts", max_attempts);
                return FUJINET_SIO_ERROR;
            }
        }
        
        /* Cascade Change: Remove sleep */
        /* struct timespec ts = {0, 10000000}; // 10ms */
        /* nanosleep(&ts, NULL); */
        /* End Cascade Change */
        
        /* Step 2: Send DATA_BLOCK with command, aux1, aux2 */
        uint8_t data_block[3] = {command, aux1, aux2};
        if (!Network_SendAltirraMessage(NETSIO_DATA_BLOCK, 3, data_block, 3)) {
            if (send_attempts < max_attempts) {
                SIO_LOG_WARN("Failed to send DATA_BLOCK, retrying entire sequence (attempt %d/%d)...", 
                            send_attempts, max_attempts);
                continue;
            } else {
                SIO_LOG_ERROR("Failed to send DATA_BLOCK after %d attempts", max_attempts);
                return FUJINET_SIO_ERROR;
            }
        }
        
        /* Cascade Change: Remove sleep */
        /* Another short delay between messages */
        /* nanosleep(&ts, NULL); */
        /* End Cascade Change */
        
        /* Step 3: Send COMMAND_OFF_SYNC with sync request counter and checksum */
        /* The sync request counter is necessary for the NetSIO protocol to pause/resume emulation */
        /* First, get the current counter value from the network module */
        uint8_t sync_number = Network_GetSyncCounter();
        
        /* For COMMAND_OFF_SYNC, the sync number is the arg, and checksum is sent as payload */
        if (!Network_SendAltirraMessage(NETSIO_COMMAND_OFF_SYNC, sync_number, &frame_checksum, 1)) {
            if (send_attempts < max_attempts) {
                SIO_LOG_WARN("Failed to send COMMAND_OFF_SYNC, retrying entire sequence (attempt %d/%d)...", 
                            send_attempts, max_attempts);
                continue;
            } else {
                SIO_LOG_ERROR("Failed to send COMMAND_OFF_SYNC after %d attempts", max_attempts);
                return FUJINET_SIO_ERROR;
            }
        }
        
        /* After sending COMMAND_OFF_SYNC, set the waiting flag to pause CPU execution */
        /* This is required by the NetSIO protocol to ensure proper timing for ACK/NAK responses */
        Network_SetWaitingForSync(sync_number);
        
        /* All three messages sent successfully */
        success = 1;
    }
    
    if (!success) {
        SIO_LOG_ERROR("Failed to send Altirra message for SIO command after %d attempts", max_attempts);
        return FUJINET_SIO_ERROR;
    }
    
    SIO_LOG_DEBUG("SIO command sent successfully");
    
    /* ------ RESPONSE HANDLING PHASE ------ */

    /* First, reset the response buffer */
    fujinet_response_buffer_pos = 0;
    fujinet_response_buffer_size = 0;
    memset(fujinet_response_buffer, 0, FUJINET_BUFFER_SIZE);

    /* Determine expected SIO response size based on command */
    int is_read_command = 0;
    int expected_data_bytes = 0;
    switch (command) {
        case 0x52: /* 'R' Read Sector */
            is_read_command = 1;
            expected_data_bytes = 129; /* ACK/NAK + 128 data */
            break;
        case 0x53: /* 'S' Get Status */
            is_read_command = 1;
            expected_data_bytes = 129; /* Status 'C' byte + 128 data bytes */
            break;
        case 0x4E: /* 'N' Read Percom Block */
            is_read_command = 1;
            expected_data_bytes = 13; /* ACK/NAK + 12 data */
            break;
        case 0x50: /* 'P' Put Sector */
        case 0x4F: /* 'O' Format Drive */
        case 0x57: /* 'W' Write Sector */
        case 0x21: /* '!' Format Drive (Non-FMS) */
            is_read_command = 0; /* These expect ACK/NAK/COMPLETE only */
            expected_data_bytes = 1;
            break;
        default:
            /* Assume only ACK/NAK/COMPLETE */
            is_read_command = 0;
            expected_data_bytes = 1;
    }
    SIO_LOG_DEBUG("Expecting %d total SIO response bytes for command 0x%02X", expected_data_bytes, command);

    /* --- Cascade: Loop to receive SIO response bytes using Network_GetByte --- */
    SIO_LOG_DEBUG("Receiving SIO response bytes from NetSIO hub...");
    uint8_t received_byte;
    int get_byte_result;

    while (fujinet_response_buffer_size < expected_data_bytes) {
        get_byte_result = Network_GetByte(&received_byte);

        if (get_byte_result == 1) {
            /* Successfully received a byte */
            if (fujinet_response_buffer_size < FUJINET_BUFFER_SIZE) {
                fujinet_response_buffer[fujinet_response_buffer_size++] = received_byte;
                SIO_LOG_DEBUG("Received SIO byte 0x%02X (%d/%d)", 
                              received_byte, fujinet_response_buffer_size, expected_data_bytes);
            } else {
                SIO_LOG_WARN("FujiNet response buffer overflow! Discarding byte 0x%02X", received_byte);
                /* Continue trying to receive the rest to clear the network buffer, but flag error later */
            }
        } else {
            /* get_byte_result is 0 (timeout/error) or -1 (should not happen) */
            if (!Network_IsConnected()) {
                 SIO_LOG_ERROR("Network disconnected while waiting for SIO response");
            } else {
                 SIO_LOG_ERROR("Timeout or error waiting for SIO response byte (%d/%d received)", 
                               fujinet_response_buffer_size, expected_data_bytes);
            }
            break; /* Exit the loop on timeout or error */
        }
    }

    /* Check if we received the expected amount */
    if (fujinet_response_buffer_size < expected_data_bytes) {
        SIO_LOG_ERROR("Incomplete SIO response: Received %d bytes, expected %d", 
                      fujinet_response_buffer_size, expected_data_bytes);
        return FUJINET_SIO_ERROR; /* Return error if response incomplete */
    }

    /* Check for buffer overflow during receive */
    if (fujinet_response_buffer_size >= FUJINET_BUFFER_SIZE && expected_data_bytes >= FUJINET_BUFFER_SIZE) {
         SIO_LOG_ERROR("FujiNet response buffer overflow occurred during receive.");
         return FUJINET_SIO_ERROR;
    }

    SIO_LOG_DEBUG("Full SIO response received (%d bytes). First byte (status): 0x%02X", 
                  fujinet_response_buffer_size, fujinet_response_buffer[0]);

    /* Return the first byte, which is the SIO status code (ACK/NAK/COMPLETE/ERROR) */
    return fujinet_response_buffer[0];
    /* --- End Cascade Changes --- */
}

/*
 * Read a byte from the SIO command response buffer
 * Returns:
 *   1 = Byte read successfully (byte is stored in *byte)
 *   0 = No data available
 *  -1 = Error
 */
int FujiNet_SIO_GetByte(uint8_t *byte) {
    /* Check if we have valid response data */
    if (fujinet_response_buffer_size <= 0 || fujinet_response_buffer_pos >= fujinet_response_buffer_size) {
        /* No data available */
        return 0;
    }
    
    /* Get the next byte from the response buffer */
    *byte = fujinet_response_buffer[fujinet_response_buffer_pos++];
    
    SIO_LOG_DEBUG("SIO_GetByte: Returning byte 0x%02X at position %d/%d", 
                 *byte, fujinet_response_buffer_pos - 1, fujinet_response_buffer_size - 1);
    
    return 1; /* Success */
}

/*
 * Send a byte to the FujiNet device
 * Returns:
 *   1 = Byte sent successfully
 *   0 = No data sent (device not ready)
 *  -1 = Error
 */
int FujiNet_SIO_PutByte(uint8_t byte) {
    /* For now, we only support sending bytes as part of command frames */
    /* The main SIO module will build the full command frame and call FujiNet_SIO_ProcessCommand */
    /* Individual byte transmission is not needed for most SIO operations */
    
    SIO_LOG_DEBUG("SIO_PutByte called with byte 0x%02X - operation not supported", byte);
    
    /* If we need to send data to the host (e.g., for write operations), 
       we'll need to implement a buffer and proper message formatting */
    
    /* Return success for now to avoid blocking the SIO process */
    return 1;
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

/*
 * Get the current position within the response buffer
 */
int FujiNet_SIO_GetResponseBufferPos(void) {
    return fujinet_response_buffer_pos;
}

/*
 * Get the total size of the data currently in the response buffer
 */
int FujiNet_SIO_GetResponseBufferSize(void) {
    return fujinet_response_buffer_size;
}
