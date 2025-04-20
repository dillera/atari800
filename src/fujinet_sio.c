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
    
    SIO_LOG_DEBUG("Sending SIO command frame for device 0x%02X...", devid);
    
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
        
        /* Short delay between messages */
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
        
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
        
        /* Another short delay between messages */
        nanosleep(&ts, NULL);
        
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
    
    /* For the actual SIO protocol, the response structure depends on the command:
     * 1. ACK/NAK: One byte indicating if the command was accepted (0x41) or rejected (0x4E)
     * 2. COMPLETE/ERROR: One byte indicating if the command completed successfully
     * 3. DATA: Command-specific data bytes (only for read commands)
     */
    
    /* First, reset the response buffer */
    fujinet_response_buffer_pos = 0;
    fujinet_response_buffer_size = 0;
    memset(fujinet_response_buffer, 0, FUJINET_BUFFER_SIZE);
    
    /* Wait for SIO response from the NetSIO hub */
    SIO_LOG_DEBUG("Waiting for SIO response from NetSIO hub...");
    
    /* Actively poll for responses from the NetSIO hub */
    struct timeval start_time, current_time;
    gettimeofday(&start_time, NULL);
    int timeout_ms = 1000; /* Increase timeout to 1 second */
    int received_response = 0;
    uint8_t response_byte;
    
    /* Wait for at least the first byte (ACK/NAK) */
    while (!received_response) {
        /* Check for timeout */
        gettimeofday(&current_time, NULL);
        long elapsed_ms = ((current_time.tv_sec - start_time.tv_sec) * 1000) + 
                          ((current_time.tv_usec - start_time.tv_usec) / 1000);
        
        if (elapsed_ms >= timeout_ms) {
            SIO_LOG_ERROR("Timeout waiting for SIO response");
            return FUJINET_SIO_ERROR_NAK; /* Timeout is treated as NAK */
        }
        
        /* Actively process multiple messages from the network in a tight loop */
        /* This helps ensure we don't miss any messages */
        for (int i = 0; i < 10; i++) {
            int result = Network_ProcessAltirraMessage();
            
            if (result > 0) {
                /* We got data from the network, so fetch it */
                while (Network_GetByte(&response_byte)) {
                    /* Got a data byte */
                    fujinet_response_buffer[fujinet_response_buffer_pos++] = response_byte;
                    fujinet_response_buffer_size++;
                    received_response = 1;
                    
                    SIO_LOG_DEBUG("Received response byte: 0x%02X", response_byte);
                    
                    /* Check if this is a NAK response */
                    if (response_byte == 0x4E) {
                        SIO_LOG_WARN("Received NAK (0x4E) response for command");
                        return FUJINET_SIO_ERROR_NAK;
                    }
                }
            }
            
            /* Small pause between polling attempts */
            usleep(1000); /* 1ms */
        }
        
        /* If no data yet, small sleep to avoid CPU spinning */
        if (!received_response) {
            usleep(10000); /* 10ms sleep */
        }
    }
    
    /* For read commands, wait for additional data */
    /* For these commands, we need to receive all data bytes */
    int is_read_command = 0;
    int expected_data_bytes = 0;
    
    /* Check if this is a read command that expects data */
    /* The common read commands are:
     * - 'R' (0x52): Read Sector - expects 128 bytes of data
     * - 'S' (0x53): Get Status - expects 4 bytes of status data
     * - 'N' (0x4E): Read Percom Block - expects 12 bytes of data
     */
    switch (command) {
        case 0x52: /* 'R' Read Sector */
            is_read_command = 1;
            expected_data_bytes = 128; /* Standard sector size */
            break;
        case 0x53: /* 'S' Get Status */
            is_read_command = 1;
            expected_data_bytes = 4; /* Status block size */
            break;
        case 0x4E: /* 'N' Read Percom Block */
            is_read_command = 1;
            expected_data_bytes = 12; /* Percom block size */
            break;
        default:
            /* Not a read command or unknown data size */
            is_read_command = 0;
            expected_data_bytes = 0;
    }
    
    /* If we're expecting data, try to receive it with timeout */
    if (is_read_command && expected_data_bytes > 0) {
        SIO_LOG_DEBUG("Expecting %d data bytes for command 0x%02X", expected_data_bytes, command);
        
        /* Reset timeout for data phase */
        gettimeofday(&start_time, NULL);
        timeout_ms = 1000; /* 1 second timeout for data */
        
        /* Process messages until we get all expected data or timeout */
        while (fujinet_response_buffer_size < expected_data_bytes + 1) { /* +1 for ACK byte */
            /* Check for timeout */
            gettimeofday(&current_time, NULL);
            long elapsed_ms = ((current_time.tv_sec - start_time.tv_sec) * 1000) + 
                              ((current_time.tv_usec - start_time.tv_usec) / 1000);
            
            if (elapsed_ms >= timeout_ms) {
                SIO_LOG_ERROR("Timeout waiting for data bytes (%d/%d received)", 
                              fujinet_response_buffer_size - 1, expected_data_bytes);
                
                /* If we have at least the ACK, return success but log a warning */
                if (fujinet_response_buffer_size >= 1) {
                    SIO_LOG_WARN("Returning partial data (%d bytes)", fujinet_response_buffer_size);
                    return FUJINET_SIO_COMPLETE;
                }
                
                return FUJINET_SIO_ERROR_GENERAL;
            }
            
            /* Actively process messages from the network */
            int result = Network_ProcessAltirraMessage();
            
            if (result > 0) {
                /* We got data from the network, so fetch it */
                while (Network_GetByte(&response_byte)) {
                    /* Got a data byte */
                    fujinet_response_buffer[fujinet_response_buffer_pos++] = response_byte;
                    fujinet_response_buffer_size++;
                    
                    SIO_LOG_DEBUG("Received data byte %d: 0x%02X", 
                                  fujinet_response_buffer_size - 1, response_byte);
                    
                    /* Reset timeout after receiving data */
                    gettimeofday(&start_time, NULL);
                    
                    /* Check if we have all expected data */
                    if (fujinet_response_buffer_size >= expected_data_bytes + 1) {
                        break;
                    }
                }
            } 
            else if (result < 0) {
                /* Error in receiving data */
                SIO_LOG_ERROR("Error receiving data bytes");
                
                /* If we have at least the ACK, return success but log a warning */
                if (fujinet_response_buffer_size >= 1) {
                    SIO_LOG_WARN("Returning partial data (%d bytes)", fujinet_response_buffer_size);
                    return FUJINET_SIO_COMPLETE;
                }
                
                return FUJINET_SIO_ERROR_GENERAL;
            }
            
            /* Small sleep to avoid CPU spinning if no data */
            usleep(5000); /* 5ms sleep */
        }
        
        /* Check for complete byte */
        if (fujinet_response_buffer_size >= expected_data_bytes + 1) {
            SIO_LOG_DEBUG("Received all expected data bytes (%d bytes total)", 
                         fujinet_response_buffer_size);
        }
    }
    
    /* Reset the response buffer position for subsequent reads */
    fujinet_response_buffer_pos = 0;
    
    /* Return success */
    return FUJINET_SIO_COMPLETE;
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
