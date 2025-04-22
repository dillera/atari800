/*
 * fujinet_netsio.c - FujiNet NetSIO integration
 *
 * Copyright (C) 2023-2024 Atari800 development team
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
#include <time.h>

#include "config.h"
#include "log.h"
#include "util.h"
#include "atari.h"
#include "fujinet.h"
#include "fujinet_sio.h"
#include "netsio/netsio.h"
#include "fujinet_netsio.h"

/* Response buffer for SIO commands */
static UBYTE fujinet_response_buffer[FUJINET_BUFFER_SIZE];
static int fujinet_response_buffer_pos = 0;
static int fujinet_response_buffer_size = 0;

/* Logging macros */
#ifdef DEBUG_FUJINET
#define NETSIO_DEBUG_LOG(msg, ...) do { Log_print("FujiNet NetSIO DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define NETSIO_DEBUG_LOG(msg, ...) ((void)0)
#endif
#define NETSIO_ERROR_LOG(msg, ...) do { Log_print("FujiNet NetSIO ERROR: " msg, ##__VA_ARGS__); } while(0)
#define NETSIO_INFO_LOG(msg, ...) do { Log_print("FujiNet NetSIO: " msg, ##__VA_ARGS__); } while(0)

/* Internal state */
static int netsio_initialized = 0;
static int last_ack_type = 0;
static int last_ack_byte = 0;

/* Initialize the FujiNet NetSIO integration */
int FujiNet_NetSIO_Initialize(const char *host, int port) {
    if (netsio_initialized) {
        NETSIO_INFO_LOG("NetSIO already initialized");
        return 1;
    }
    
    NETSIO_INFO_LOG("Initializing NetSIO integration with host=%s, port=%d", host ? host : "localhost", port);
    
    /* Initialize the NetSIO module */
    if (NetSIO_Initialize(host, port) != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to initialize NetSIO");
        return 0;
    }
    
    /* Reset the response buffer */
    fujinet_response_buffer_pos = 0;
    fujinet_response_buffer_size = 0;
    
    /* Register our message handlers */
    FujiNet_NetSIO_RegisterHandlers();
    
    /* Start the NetSIO listener */
    if (NetSIO_StartListener() != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to start NetSIO listener");
        NetSIO_Shutdown();
        return 0;
    }
    
    /* Send a device connected message to announce ourselves to FujiNet */
    if (NetSIO_SendDeviceConnected() != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to send device connected message");
        /* Continue anyway, as FujiNet might ping us later */
    } else {
        NETSIO_INFO_LOG("Sent device connected message to FujiNet");
    }
    
    netsio_initialized = 1;
    NETSIO_INFO_LOG("NetSIO integration initialized successfully");
    
    return 1;
}

/* Shutdown the FujiNet NetSIO integration */
void FujiNet_NetSIO_Shutdown(void) {
    if (!netsio_initialized) {
        return;
    }
    
    NETSIO_INFO_LOG("Shutting down NetSIO integration");
    
    /* Stop the NetSIO listener */
    NetSIO_StopListener();
    
    /* Shutdown the NetSIO module */
    NetSIO_Shutdown();
    
    netsio_initialized = 0;
    NETSIO_INFO_LOG("NetSIO integration shutdown complete");
}

/* Process incoming NetSIO messages */
void FujiNet_NetSIO_ProcessMessages(void) {
    if (!netsio_initialized) {
        return;
    }
    
    /* Process any incoming messages */
    while (NetSIO_HandleIncomingMessages() > 0) {
        /* Messages are processed in NetSIO_HandleIncomingMessages */
    }
}

/* Send an SIO command to FujiNet via NetSIO */
int FujiNet_NetSIO_SendCommand(const UBYTE *command_frame) {
    if (!netsio_initialized) {
        NETSIO_ERROR_LOG("NetSIO not initialized");
        return 0;
    }
    
    if (!command_frame) {
        NETSIO_ERROR_LOG("NULL command frame");
        return 0;
    }
    
    NETSIO_DEBUG_LOG("Sending SIO command: %02X %02X %02X %02X %02X",
                    command_frame[0], command_frame[1], command_frame[2],
                    command_frame[3], command_frame[4]);
    
    /* Reset the response buffer */
    fujinet_response_buffer_pos = 0;
    fujinet_response_buffer_size = 0;
    
    /* Get the components of the SIO command frame */
    uint8_t devid = command_frame[0];
    uint8_t command = command_frame[1];
    uint8_t aux1 = command_frame[2];
    uint8_t aux2 = command_frame[3];
    uint8_t checksum = command_frame[4];
    
    /* Follow NetSIO protocol sequence for SIO commands:
       1. COMMAND_ON with device ID
       2. DATA_BLOCK with command, aux1, aux2
       3. COMMAND_OFF_SYNC with checksum and sync request counter */
    
    /* Step 1: Send COMMAND_ON with device ID */
    if (NetSIO_SendCommandOn(devid) != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to send COMMAND_ON");
        return 0;
    }
    
    /* Step 2: Send DATA_BLOCK with command, aux1, aux2 */
    uint8_t data_block[3] = {command, aux1, aux2};
    if (NetSIO_SendDataBlock(data_block, 3) != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to send DATA_BLOCK");
        return 0;
    }
    
    /* Step 3: Send COMMAND_OFF_SYNC with sync request counter and checksum */
    uint8_t sync_number = NetSIO_GetSyncCounter();
    if (NetSIO_SendCommandOffSync(sync_number) != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to send COMMAND_OFF_SYNC");
        return 0;
    }
    
    /* After sending COMMAND_OFF_SYNC, set the waiting flag to pause CPU execution */
    NetSIO_SetWaitingForSync(sync_number);
    
    /* Wait for the sync response */
    int timeout_ms = 5000; /* 5 seconds timeout */
    int start_time = (int)time(NULL);
    
    while (NetSIO_IsWaitingForSync()) {
        /* Process incoming messages to check for sync response */
        FujiNet_NetSIO_ProcessMessages();
        
        /* Check for timeout */
        if ((int)time(NULL) - start_time > timeout_ms / 1000) {
            NETSIO_ERROR_LOG("Timeout waiting for sync response");
            NetSIO_ClearWaitingForSync();
            return 0;
        }
        
        /* Small delay to prevent CPU hogging */
        struct timespec ts = {0, 1000000}; /* 1ms */
        nanosleep(&ts, NULL);
    }
    
    /* Check the acknowledgment type */
    if (last_ack_type == NETSIO_ACK_TYPE_ACK) {
        NETSIO_DEBUG_LOG("Command ACKed");
        return 1; /* Success */
    } else if (last_ack_type == NETSIO_ACK_TYPE_NAK) {
        NETSIO_DEBUG_LOG("Command NAKed");
        return 0; /* NAK */
    } else {
        NETSIO_DEBUG_LOG("Command error (ack_type=0x%02X)", last_ack_type);
        return -1; /* Error */
    }
}

/* Get a byte from the response buffer */
int FujiNet_NetSIO_GetByte(uint8_t *byte) {
    if (!netsio_initialized) {
        NETSIO_ERROR_LOG("NetSIO not initialized");
        return 0;
    }
    
    /* Check if we have data in the buffer */
    if (fujinet_response_buffer_pos < fujinet_response_buffer_size) {
        *byte = fujinet_response_buffer[fujinet_response_buffer_pos++];
        NETSIO_DEBUG_LOG("Read byte 0x%02X from buffer (%d/%d)",
                        *byte, fujinet_response_buffer_pos, fujinet_response_buffer_size);
        return 1;
    }
    
    /* No data in buffer, process incoming messages */
    FujiNet_NetSIO_ProcessMessages();
    
    /* Check again if we have data */
    if (fujinet_response_buffer_pos < fujinet_response_buffer_size) {
        *byte = fujinet_response_buffer[fujinet_response_buffer_pos++];
        NETSIO_DEBUG_LOG("Read byte 0x%02X from buffer after processing (%d/%d)",
                        *byte, fujinet_response_buffer_pos, fujinet_response_buffer_size);
        return 1;
    }
    
    NETSIO_DEBUG_LOG("No data available");
    return 0;
}

/* Send a byte to FujiNet via NetSIO */
int FujiNet_NetSIO_PutByte(uint8_t byte) {
    if (!netsio_initialized) {
        NETSIO_ERROR_LOG("NetSIO not initialized");
        return 0;
    }
    
    NETSIO_DEBUG_LOG("Sending byte 0x%02X", byte);
    
    /* Send the byte */
    if (NetSIO_SendDataByte(byte) != NETSIO_STATUS_OK) {
        NETSIO_ERROR_LOG("Failed to send data byte");
        return 0;
    }
    
    return 1;
}

/* Set motor state via NetSIO */
void FujiNet_NetSIO_SetMotorState(int on) {
    if (!netsio_initialized) {
        return;
    }
    
    NETSIO_DEBUG_LOG("Setting motor state: %s", on ? "ON" : "OFF");
    
    if (on) {
        NetSIO_SendMotorOn();
    } else {
        NetSIO_SendMotorOff();
    }
}

/* Get the response buffer position */
int FujiNet_NetSIO_GetResponseBufferPos(void) {
    return fujinet_response_buffer_pos;
}

/* Get the response buffer size */
int FujiNet_NetSIO_GetResponseBufferSize(void) {
    return fujinet_response_buffer_size;
}

/* Handle a received data byte */
void FujiNet_NetSIO_HandleDataByte(uint8_t byte) {
    NETSIO_DEBUG_LOG("Received data byte: 0x%02X", byte);
    /* Add the byte to the response buffer */
    if (fujinet_response_buffer_size < FUJINET_BUFFER_SIZE) {
        fujinet_response_buffer[fujinet_response_buffer_size++] = byte;
    } else {
        NETSIO_ERROR_LOG("Response buffer full, dropping byte 0x%02X", byte);
    }
}

/* Handle a received data block */
void FujiNet_NetSIO_HandleDataBlock(const uint8_t *data, uint16_t data_length) {
    NETSIO_DEBUG_LOG("Received data block: %d bytes", data_length);
    
    /* Add the data to the response buffer */
    int i;
    for (i = 0; i < data_length && fujinet_response_buffer_size < FUJINET_BUFFER_SIZE; i++) {
        fujinet_response_buffer[fujinet_response_buffer_size++] = data[i];
    }
    
    if (i < data_length) {
        NETSIO_ERROR_LOG("Response buffer full, dropped %d bytes", data_length - i);
    }
}

/* Handle a received sync response */
void FujiNet_NetSIO_HandleSyncResponse(uint8_t sync_number, uint8_t ack_type, uint8_t ack_byte, uint16_t write_size) {
    NETSIO_DEBUG_LOG("Received sync response: sync_number=%d, ack_type=0x%02X, ack_byte=0x%02X, write_size=%d",
                    sync_number, ack_type, ack_byte, write_size);
    
    /* Store the acknowledgment information */
    last_ack_type = ack_type;
    last_ack_byte = ack_byte;
    
    /* Check if we're waiting for this sync response */
    if (NetSIO_IsWaitingForSync() && NetSIO_GetWaitingSyncNum() == sync_number) {
        /* Clear the waiting flag */
        NetSIO_ClearWaitingForSync();
    }
}

/* Register message handlers with the NetSIO module */
void FujiNet_NetSIO_RegisterHandlers(void) {
    extern void NetSIO_RegisterDataByteHandler(void (*handler)(uint8_t));
    extern void NetSIO_RegisterDataBlockHandler(void (*handler)(const uint8_t *, uint16_t));
    extern void NetSIO_RegisterSyncResponseHandler(void (*handler)(uint8_t, uint8_t, uint8_t, uint16_t));
    
    NETSIO_INFO_LOG("Registering NetSIO message handlers");
    
    /* Register our handlers with the NetSIO module */
    NetSIO_RegisterDataByteHandler(FujiNet_NetSIO_HandleDataByte);
    NetSIO_RegisterDataBlockHandler(FujiNet_NetSIO_HandleDataBlock);
    NetSIO_RegisterSyncResponseHandler(FujiNet_NetSIO_HandleSyncResponse);
}
