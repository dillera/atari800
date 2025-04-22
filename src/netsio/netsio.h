/*
 * netsio.h - NetSIO protocol implementation
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

#ifndef NETSIO_H_
#define NETSIO_H_

#include <stdint.h>
#include "atari.h" /* For UBYTE type */

#ifdef __cplusplus
extern "C" {
#endif

/* NetSIO Configuration */
#define NETSIO_DEFAULT_PORT 9997
#define NETSIO_BUFFER_SIZE 512
#define NETSIO_TIMEOUT_MS 500

/* NetSIO Protocol Message Types */
#define NETSIO_DATA_BYTE        0x01
#define NETSIO_DATA_BLOCK       0x02
#define NETSIO_DATA_BYTE_SYNC   0x09
#define NETSIO_COMMAND_ON       0x11
#define NETSIO_COMMAND_OFF      0x10
#define NETSIO_COMMAND_OFF_SYNC 0x18
#define NETSIO_MOTOR_ON         0x21
#define NETSIO_MOTOR_OFF        0x20
#define NETSIO_PROCEED_ON       0x31
#define NETSIO_PROCEED_OFF      0x30
#define NETSIO_INTERRUPT_ON     0x41
#define NETSIO_INTERRUPT_OFF    0x40
#define NETSIO_SPEED_CHANGE     0x80
#define NETSIO_SYNC_RESPONSE    0x81

/* Connection Management Message Types */
#define NETSIO_DEVICE_CONNECTED    0xC1
#define NETSIO_DEVICE_DISCONNECTED 0xC0
#define NETSIO_PING_REQUEST        0xC2
#define NETSIO_PING_RESPONSE       0xC3
#define NETSIO_ALIVE_REQUEST       0xC4
#define NETSIO_ALIVE_RESPONSE      0xC5
#define NETSIO_CREDIT_STATUS       0xC6
#define NETSIO_CREDIT_UPDATE       0xC7

/* Notification Message Types */
#define NETSIO_WARM_RESET       0xFE
#define NETSIO_COLD_RESET       0xFF

/* Sync Response Acknowledgment Types */
#define NETSIO_ACK_TYPE_ACK     0x41  /* 'A' */
#define NETSIO_ACK_TYPE_NAK     0x4E  /* 'N' */
#define NETSIO_ACK_TYPE_COMPLETE 0x43 /* 'C' */
#define NETSIO_ACK_TYPE_ERROR   0x45  /* 'E' */

/* Status Codes */
#define NETSIO_STATUS_OK        0
#define NETSIO_STATUS_ERROR     -1
#define NETSIO_STATUS_TIMEOUT   -2

/* Logging macros */
#ifdef DEBUG_FUJINET
#define NETSIO_LOG_DEBUG(msg, ...) do { Log_print("NetSIO DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define NETSIO_LOG_DEBUG(msg, ...) ((void)0)
#endif
#define NETSIO_LOG_ERROR(msg, ...) do { Log_print("NetSIO ERROR: " msg, ##__VA_ARGS__); } while(0)
#define NETSIO_LOG_WARN(msg, ...) do { Log_print("NetSIO WARN: " msg, ##__VA_ARGS__); } while(0)
#define NETSIO_LOG_INFO(msg, ...) do { Log_print("NetSIO: " msg, ##__VA_ARGS__); } while(0)

/* NetSIO Message Structure */
typedef struct {
    uint8_t message_type;
    uint8_t parameter;
    uint16_t data_length;
    uint8_t data[NETSIO_BUFFER_SIZE];
} NetSIO_Message;

/* Connection State */
typedef struct {
    int connected;
    uint8_t sync_counter;
    int waiting_for_sync;
    uint8_t waiting_sync_num;
} NetSIO_ConnectionState;

/* Function Declarations */

/* Initialization and Shutdown */
int NetSIO_Initialize(const char *host, int port);
void NetSIO_Shutdown(void);
int NetSIO_IsConnected(void);

/* Message Sending */
int NetSIO_SendDataByte(uint8_t data_byte);
int NetSIO_SendDataBlock(const uint8_t *data, uint16_t data_length);
int NetSIO_SendDataByteSync(uint8_t data_byte, uint8_t sync_number);
int NetSIO_SendCommandOn(uint8_t device_id);
int NetSIO_SendCommandOff(void);
int NetSIO_SendCommandOffSync(uint8_t sync_number);
int NetSIO_SendMotorOn(void);
int NetSIO_SendMotorOff(void);
int NetSIO_SendSpeedChange(uint32_t baud_rate);
int NetSIO_SendSyncResponse(uint8_t sync_number, uint8_t ack_type, uint8_t ack_byte, uint16_t write_size);

/* Connection Management */
int NetSIO_SendDeviceConnected(void);
int NetSIO_SendDeviceDisconnected(void);
int NetSIO_SendPingRequest(void);
int NetSIO_SendPingResponse(void);
int NetSIO_SendAliveRequest(void);
int NetSIO_SendAliveResponse(void);

/* Notifications */
int NetSIO_SendWarmReset(void);
int NetSIO_SendColdReset(void);

/* Message Receiving */
int NetSIO_ReceiveMessage(NetSIO_Message *message, int timeout_ms);
int NetSIO_ProcessReceivedMessage(const NetSIO_Message *message);

/* Synchronization */
uint8_t NetSIO_GetSyncCounter(void);
void NetSIO_SetWaitingForSync(uint8_t sync_num);
void NetSIO_ClearWaitingForSync(void);
int NetSIO_IsWaitingForSync(void);
uint8_t NetSIO_GetWaitingSyncNum(void);

/* UDP Socket Management */
int NetSIO_StartListener(void);
int NetSIO_StopListener(void);
int NetSIO_HandleIncomingMessages(void);

/* Message handler registration functions */
void NetSIO_RegisterDataByteHandler(void (*handler)(uint8_t));
void NetSIO_RegisterDataBlockHandler(void (*handler)(const uint8_t *, uint16_t));
void NetSIO_RegisterSyncResponseHandler(void (*handler)(uint8_t, uint8_t, uint8_t, uint16_t));

#ifdef __cplusplus
}
#endif

#endif /* NETSIO_H_ */
