/*
 * netsio.c - NetSIO protocol implementation
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
#include <errno.h>
#include <stdarg.h>
#include <time.h>

#include "config.h"
#include <stdio.h>
#define Log_print printf
#define Log_flushlog() fflush(stdout)
#include "util.h"
#include "atari.h"
#include "netsio.h"

/* Basic socket platform compatibility layer */
#ifdef HAVE_WINDOWS_H
#include <windows.h>
#include <winsock2.h>
typedef int socklen_t;
typedef SOCKET socket_t;
#define INVALID_SOCKET INVALID_SOCKET
#define closesocket closesocket
#define SOCKET_ERROR SOCKET_ERROR
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/time.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#endif

/* Message handler function pointers */
static void (*data_byte_handler)(uint8_t) = NULL;
static void (*data_block_handler)(const uint8_t *, uint16_t) = NULL;
static void (*sync_response_handler)(uint8_t, uint8_t, uint8_t, uint16_t) = NULL;

/* Socket and connection state */
static socket_t udp_socket = INVALID_SOCKET;
static struct sockaddr_in fujinet_addr;
static socklen_t fujinet_addr_len = sizeof(struct sockaddr_in);
static int have_fujinet_addr = 0;

/* Connection state */
static NetSIO_ConnectionState connection_state = {0, 0, 0, 0};

/* Receive buffer for incoming messages */
static uint8_t rx_buffer[NETSIO_BUFFER_SIZE + 8]; /* Extra space for header */
static int rx_buffer_len = 0;

/* Helper functions for little-endian conversion */
static uint16_t to_little_endian_16(uint16_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 8) | ((value & 0xFF00) >> 8);
#else
    return value;
#endif
}

static uint16_t from_little_endian_16(uint16_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 8) | ((value & 0xFF00) >> 8);
#else
    return value;
#endif
}

static uint32_t to_little_endian_32(uint32_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 24) | 
           ((value & 0xFF00) << 8) | 
           ((value & 0xFF0000) >> 8) | 
           ((value & 0xFF000000) >> 24);
#else
    return value;
#endif
}

static uint32_t from_little_endian_32(uint32_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 24) | 
           ((value & 0xFF00) << 8) | 
           ((value & 0xFF0000) >> 8) | 
           ((value & 0xFF000000) >> 24);
#else
    return value;
#endif
}

/* Compatibility function for ARM macOS */
#if defined(__APPLE__) && defined(__arm64__)
int Util_stricmp(const char *str1, const char *str2) {
    int retval;
    while ((retval = tolower((unsigned char)*str1) - tolower((unsigned char)*str2++)) == 0) {
        if (*str1++ == '\0')
            break;
    }
    return retval;
}
#endif

/* Set socket to non-blocking mode */
static int set_nonblocking(socket_t sock) {
#ifdef HAVE_WINDOWS_H
    u_long mode = 1;  /* 1 = non-blocking */
    return (ioctlsocket(sock, FIONBIO, &mode) == 0) ? 0 : -1;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* Initialize the NetSIO module */
int NetSIO_Initialize(const char *host, int port) {
    struct hostent *hp;
    int result;

    NETSIO_LOG_INFO("Initializing NetSIO UDP communication");

    /* Initialize platform-specific socket library if needed */
#ifdef HAVE_WINDOWS_H
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        NETSIO_LOG_ERROR("WSAStartup failed");
        return NETSIO_STATUS_ERROR;
    }
#endif

    /* Create UDP socket */
    udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket == INVALID_SOCKET) {
        NETSIO_LOG_ERROR("Failed to create UDP socket: %s (errno=%d)", strerror(errno), errno);
        return NETSIO_STATUS_ERROR;
    }

    /* Set up local address structure for binding */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);

    /* Bind the socket to the local address */
    result = bind(udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if (result == SOCKET_ERROR) {
        NETSIO_LOG_ERROR("Failed to bind UDP socket: %s (errno=%d)", strerror(errno), errno);
        closesocket(udp_socket);
        udp_socket = INVALID_SOCKET;
        return NETSIO_STATUS_ERROR;
    }

    /* Set up FujiNet address structure if host is provided */
    if (host && *host) {
        memset(&fujinet_addr, 0, sizeof(fujinet_addr));
        fujinet_addr.sin_family = AF_INET;
        fujinet_addr.sin_port = htons(port);
        
        /* Resolve hostname to IP address */
        hp = gethostbyname(host);
        if (hp == NULL) {
            NETSIO_LOG_ERROR("Failed to resolve hostname %s: %s (errno=%d)", host, strerror(errno), errno);
            /* Don't fail initialization, we'll wait for incoming packets to set the address */
        } else {
            memcpy(&fujinet_addr.sin_addr, hp->h_addr, hp->h_length);
            have_fujinet_addr = 1;
            NETSIO_LOG_INFO("FujiNet address set to %s:%d", host, port);
        }
    }

    /* Set socket to non-blocking mode */
    if (set_nonblocking(udp_socket) < 0) {
        NETSIO_LOG_WARN("Failed to set UDP socket to non-blocking mode: %s (errno=%d)", strerror(errno), errno);
        /* Continue anyway, we'll handle blocking I/O */
    }

    /* Initialize connection state */
    connection_state.connected = 0;
    connection_state.sync_counter = 0;
    connection_state.waiting_for_sync = 0;
    connection_state.waiting_sync_num = 0;

    NETSIO_LOG_INFO("NetSIO initialized successfully, listening on UDP port %d", port);
    
    /* Send a device connected message if we have the FujiNet address */
    if (have_fujinet_addr) {
        NetSIO_SendDeviceConnected();
    }

    return NETSIO_STATUS_OK;
}

/* Shutdown the NetSIO module */
void NetSIO_Shutdown(void) {
    NETSIO_LOG_INFO("Shutting down NetSIO");
    
    /* Send device disconnected message if connected */
    if (connection_state.connected && have_fujinet_addr) {
        NetSIO_SendDeviceDisconnected();
    }
    
    /* Close the socket */
    if (udp_socket != INVALID_SOCKET) {
        closesocket(udp_socket);
        udp_socket = INVALID_SOCKET;
    }
    
    /* Reset connection state */
    connection_state.connected = 0;
    connection_state.waiting_for_sync = 0;
    
#ifdef HAVE_WINDOWS_H
    WSACleanup();
#endif

    NETSIO_LOG_INFO("NetSIO shutdown complete");
}

/* Check if connected to FujiNet */
int NetSIO_IsConnected(void) {
    return connection_state.connected;
}

/* Send a NetSIO message */
static int send_netsio_message(uint8_t message_type, uint8_t parameter, const uint8_t *data, uint16_t data_length) {
    uint8_t buffer[NETSIO_BUFFER_SIZE + 4]; /* Header (4) + Data */
    uint16_t le_data_length;
    int result;
    
    /* Check if we have the FujiNet address */
    if (!have_fujinet_addr) {
        NETSIO_LOG_WARN("Cannot send message: FujiNet address not set");
        return NETSIO_STATUS_ERROR;
    }
    
    /* Check if socket is valid */
    if (udp_socket == INVALID_SOCKET) {
        NETSIO_LOG_ERROR("Cannot send message: UDP socket not initialized");
        return NETSIO_STATUS_ERROR;
    }
    
    /* Check data length */
    if (data_length > NETSIO_BUFFER_SIZE) {
        NETSIO_LOG_ERROR("Data length %d exceeds maximum buffer size %d", data_length, NETSIO_BUFFER_SIZE);
        return NETSIO_STATUS_ERROR;
    }
    
    /* Prepare message header */
    buffer[0] = message_type;
    buffer[1] = parameter;
    le_data_length = to_little_endian_16(data_length);
    memcpy(buffer + 2, &le_data_length, 2);
    
    /* Copy data if present */
    if (data_length > 0 && data != NULL) {
        memcpy(buffer + 4, data, data_length);
    }
    
    /* Send the message */
    result = sendto(udp_socket, (const char*)buffer, 4 + data_length, 0, 
                   (struct sockaddr*)&fujinet_addr, sizeof(fujinet_addr));
    
    if (result == SOCKET_ERROR) {
        NETSIO_LOG_ERROR("Failed to send NetSIO message: %s (errno=%d)", strerror(errno), errno);
        return NETSIO_STATUS_ERROR;
    }
    
    NETSIO_LOG_DEBUG("Sent NetSIO message: type=0x%02X, param=0x%02X, data_len=%d", 
                    message_type, parameter, data_length);
    
    return NETSIO_STATUS_OK;
}

/* Receive a NetSIO message with timeout */
int NetSIO_ReceiveMessage(NetSIO_Message *message, int timeout_ms) {
    fd_set readfds;
    struct timeval tv;
    int result;
    
    /* Check if socket is valid */
    if (udp_socket == INVALID_SOCKET) {
        NETSIO_LOG_ERROR("Cannot receive message: UDP socket not initialized");
        return NETSIO_STATUS_ERROR;
    }
    
    /* Set up the file descriptor set */
    FD_ZERO(&readfds);
    FD_SET(udp_socket, &readfds);
    
    /* Set up the timeout */
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    /* Wait for data to be available */
    result = select(udp_socket + 1, &readfds, NULL, NULL, &tv);
    
    if (result == SOCKET_ERROR) {
        NETSIO_LOG_ERROR("Select failed: %s (errno=%d)", strerror(errno), errno);
        return NETSIO_STATUS_ERROR;
    }
    
    if (result == 0) {
        /* Timeout */
        return NETSIO_STATUS_TIMEOUT;
    }
    
    /* Data is available, receive it */
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len = sizeof(sender_addr);
    
    rx_buffer_len = recvfrom(udp_socket, (char*)rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr*)&sender_addr, &sender_addr_len);
    
    if (rx_buffer_len == SOCKET_ERROR) {
        NETSIO_LOG_ERROR("Failed to receive NetSIO message: %s (errno=%d)", strerror(errno), errno);
        return NETSIO_STATUS_ERROR;
    }
    
    /* Check if we received enough data for a header */
    if (rx_buffer_len < 4) {
        NETSIO_LOG_WARN("Received incomplete NetSIO message header (%d bytes)", rx_buffer_len);
        return NETSIO_STATUS_ERROR;
    }
    
    /* Parse the message header */
    message->message_type = rx_buffer[0];
    message->parameter = rx_buffer[1];
    uint16_t data_length;
    memcpy(&data_length, rx_buffer + 2, 2);
    message->data_length = from_little_endian_16(data_length);
    
    /* Check if we received all the data */
    if (rx_buffer_len < 4 + message->data_length) {
        NETSIO_LOG_WARN("Received incomplete NetSIO message data (%d bytes, expected %d)",
                       rx_buffer_len - 4, message->data_length);
        return NETSIO_STATUS_ERROR;
    }
    
    /* Copy the data */
    if (message->data_length > 0) {
        memcpy(message->data, rx_buffer + 4, message->data_length);
    }
    
    /* If we don't have the FujiNet address yet, save it */
    if (!have_fujinet_addr) {
        memcpy(&fujinet_addr, &sender_addr, sizeof(fujinet_addr));
        have_fujinet_addr = 1;
        NETSIO_LOG_INFO("FujiNet address set to %s:%d from incoming packet",
                       inet_ntoa(fujinet_addr.sin_addr), ntohs(fujinet_addr.sin_port));
    }
    
    NETSIO_LOG_DEBUG("Received NetSIO message: type=0x%02X, param=0x%02X, data_len=%d",
                    message->message_type, message->parameter, message->data_length);
    
    return NETSIO_STATUS_OK;
}

/* Process a received NetSIO message */
int NetSIO_ProcessReceivedMessage(const NetSIO_Message *message) {
    if (!message) {
        NETSIO_LOG_ERROR("NULL message");
        return NETSIO_STATUS_ERROR;
    }
    
    NETSIO_LOG_DEBUG("Processing message: type=0x%02X, param=0x%02X, data_len=%d",
                    message->message_type, message->parameter, message->data_length);
    
    /* Handle different message types */
    switch (message->message_type) {
        case NETSIO_DEVICE_CONNECTED:
            NETSIO_LOG_INFO("Device connected message received");
            connection_state.connected = 1;
            /* Send a response to acknowledge the connection */
            NetSIO_SendDeviceConnected();
            return NETSIO_STATUS_OK;
            
        case NETSIO_DEVICE_DISCONNECTED:
            NETSIO_LOG_INFO("Device disconnected message received");
            connection_state.connected = 0;
            return NETSIO_STATUS_OK;
            
        case NETSIO_PING_REQUEST:
            NETSIO_LOG_DEBUG("Ping request received, sending response");
            NetSIO_SendPingResponse();
            return NETSIO_STATUS_OK;
            
        case NETSIO_PING_RESPONSE:
            NETSIO_LOG_DEBUG("Ping response received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_ALIVE_REQUEST:
            NETSIO_LOG_DEBUG("Alive request received, sending response");
            NetSIO_SendAliveResponse();
            return NETSIO_STATUS_OK;
            
        case NETSIO_ALIVE_RESPONSE:
            NETSIO_LOG_DEBUG("Alive response received");
            connection_state.connected = 1;
            return NETSIO_STATUS_OK;
            
        case NETSIO_SYNC_RESPONSE:
            NETSIO_LOG_DEBUG("Sync response received: sync_number=%d", message->parameter);
            
            /* Check if we're waiting for this sync response */
            if (connection_state.waiting_for_sync && 
                connection_state.waiting_sync_num == message->parameter) {
                
                /* Parse the sync response data */
                uint8_t ack_type = 0;
                uint8_t ack_byte = 0;
                uint16_t write_size = 0;
                
                if (message->data_length >= 1) {
                    ack_type = message->data[0];
                }
                
                if (message->data_length >= 2) {
                    ack_byte = message->data[1];
                }
                
                if (message->data_length >= 4) {
                    memcpy(&write_size, message->data + 2, 2);
                    write_size = from_little_endian_16(write_size);
                }
                
                NETSIO_LOG_DEBUG("Sync response details: ack_type=0x%02X ('%c'), ack_byte=0x%02X, write_size=%d",
                               ack_type, ack_type, ack_byte, write_size);
                
                /* Call the registered sync response handler if available */
                if (sync_response_handler) {
                    sync_response_handler(message->parameter, ack_type, ack_byte, write_size);
                }
                
                /* Clear the waiting flag */
                connection_state.waiting_for_sync = 0;
                connection_state.waiting_sync_num = 0;
            } else {
                NETSIO_LOG_WARN("Received unexpected sync response: got=%d, expected=%d, waiting=%d",
                              message->parameter, connection_state.waiting_sync_num, connection_state.waiting_for_sync);
            }
            
            return NETSIO_STATUS_OK;
            
        case NETSIO_DATA_BYTE:
            NETSIO_LOG_DEBUG("Received data byte: 0x%02X", message->parameter);
            /* Call the registered data byte handler if available */
            if (data_byte_handler) {
                data_byte_handler(message->parameter);
            }
            return NETSIO_STATUS_OK;
            
        case NETSIO_DATA_BLOCK:
            NETSIO_LOG_DEBUG("Received data block: %d bytes", message->data_length);
            /* Call the registered data block handler if available */
            if (data_block_handler) {
                data_block_handler(message->data, message->data_length);
            }
            return NETSIO_STATUS_OK;
            
        case NETSIO_PROCEED_ON:
            NETSIO_LOG_DEBUG("Proceed ON received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_PROCEED_OFF:
            NETSIO_LOG_DEBUG("Proceed OFF received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_INTERRUPT_ON:
            NETSIO_LOG_DEBUG("Interrupt ON received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_INTERRUPT_OFF:
            NETSIO_LOG_DEBUG("Interrupt OFF received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_WARM_RESET:
            NETSIO_LOG_INFO("Warm reset received");
            return NETSIO_STATUS_OK;
            
        case NETSIO_COLD_RESET:
            NETSIO_LOG_INFO("Cold reset received");
            return NETSIO_STATUS_OK;
            
        default:
            NETSIO_LOG_WARN("Received unknown NetSIO message type: 0x%02X", message->message_type);
            return NETSIO_STATUS_ERROR;
    }
    
    return NETSIO_STATUS_OK;
}

/* Start the NetSIO message listener */
int NetSIO_StartListener(void) {
    /* The socket is already created and bound in NetSIO_Initialize */
    /* This function is a placeholder for any additional setup needed */
    return NETSIO_STATUS_OK;
}

/* Stop the NetSIO message listener */
int NetSIO_StopListener(void) {
    /* The socket will be closed in NetSIO_Shutdown */
    /* This function is a placeholder for any additional cleanup needed */
    return NETSIO_STATUS_OK;
}

/* Handle incoming NetSIO messages */
int NetSIO_HandleIncomingMessages(void) {
    NetSIO_Message message;
    int result;
    
    /* Check for incoming messages with a short timeout */
    result = NetSIO_ReceiveMessage(&message, 0);
    
    if (result == NETSIO_STATUS_OK) {
        /* Process the received message */
        NetSIO_ProcessReceivedMessage(&message);
        return 1; /* Message processed */
    } else if (result == NETSIO_STATUS_TIMEOUT) {
        /* No message available */
        return 0;
    } else {
        /* Error receiving message */
        return -1;
    }
}

/* Synchronization functions */

uint8_t NetSIO_GetSyncCounter(void) {
    return connection_state.sync_counter++;
}

void NetSIO_SetWaitingForSync(uint8_t sync_num) {
    connection_state.waiting_for_sync = 1;
    connection_state.waiting_sync_num = sync_num;
    NETSIO_LOG_DEBUG("Now waiting for sync response #%d", (unsigned int)sync_num);
}

void NetSIO_ClearWaitingForSync(void) {
    if (connection_state.waiting_for_sync) {
        NETSIO_LOG_DEBUG("No longer waiting for sync response #%d", (unsigned int)connection_state.waiting_sync_num);
    }
    connection_state.waiting_for_sync = 0;
}

int NetSIO_IsWaitingForSync(void) {
    return connection_state.waiting_for_sync;
}

uint8_t NetSIO_GetWaitingSyncNum(void) {
    return connection_state.waiting_sync_num;
}

/* Message sending functions */

int NetSIO_SendDataByte(uint8_t data_byte) {
    return send_netsio_message(NETSIO_DATA_BYTE, data_byte, NULL, 0);
}

int NetSIO_SendDataBlock(const uint8_t *data, uint16_t data_length) {
    return send_netsio_message(NETSIO_DATA_BLOCK, 0, data, data_length);
}

int NetSIO_SendDataByteSync(uint8_t data_byte, uint8_t sync_number) {
    return send_netsio_message(NETSIO_DATA_BYTE_SYNC, sync_number, &data_byte, 1);
}

int NetSIO_SendCommandOn(uint8_t device_id) {
    return send_netsio_message(NETSIO_COMMAND_ON, device_id, NULL, 0);
}

int NetSIO_SendCommandOff(void) {
    return send_netsio_message(NETSIO_COMMAND_OFF, 0, NULL, 0);
}

int NetSIO_SendCommandOffSync(uint8_t sync_number) {
    return send_netsio_message(NETSIO_COMMAND_OFF_SYNC, sync_number, NULL, 0);
}

int NetSIO_SendMotorOn(void) {
    return send_netsio_message(NETSIO_MOTOR_ON, 0, NULL, 0);
}

int NetSIO_SendMotorOff(void) {
    return send_netsio_message(NETSIO_MOTOR_OFF, 0, NULL, 0);
}

int NetSIO_SendSpeedChange(uint32_t baud_rate) {
    uint32_t le_baud_rate = to_little_endian_32(baud_rate);
    return send_netsio_message(NETSIO_SPEED_CHANGE, 0, (uint8_t*)&le_baud_rate, 4);
}

int NetSIO_SendSyncResponse(uint8_t sync_number, uint8_t ack_type, uint8_t ack_byte, uint16_t write_size) {
    uint8_t data[4];
    uint16_t le_write_size = to_little_endian_16(write_size);
    
    data[0] = ack_type;
    data[1] = ack_byte;
    memcpy(data + 2, &le_write_size, 2);
    
    return send_netsio_message(NETSIO_SYNC_RESPONSE, sync_number, data, 4);
}

/* Connection management functions */

int NetSIO_SendDeviceConnected(void) {
    int result = send_netsio_message(NETSIO_DEVICE_CONNECTED, 0, NULL, 0);
    if (result == NETSIO_STATUS_OK) {
        connection_state.connected = 1;
    }
    return result;
}

int NetSIO_SendDeviceDisconnected(void) {
    int result = send_netsio_message(NETSIO_DEVICE_DISCONNECTED, 0, NULL, 0);
    if (result == NETSIO_STATUS_OK) {
        connection_state.connected = 0;
    }
    return result;
}

int NetSIO_SendPingRequest(void) {
    return send_netsio_message(NETSIO_PING_REQUEST, 0, NULL, 0);
}

int NetSIO_SendPingResponse(void) {
    return send_netsio_message(NETSIO_PING_RESPONSE, 0, NULL, 0);
}

int NetSIO_SendAliveRequest(void) {
    return send_netsio_message(NETSIO_ALIVE_REQUEST, 0, NULL, 0);
}

int NetSIO_SendAliveResponse(void) {
    return send_netsio_message(NETSIO_ALIVE_RESPONSE, 0, NULL, 0);
}

/* Notification functions */

int NetSIO_SendWarmReset(void) {
    return send_netsio_message(NETSIO_WARM_RESET, 0, NULL, 0);
}

int NetSIO_SendColdReset(void) {
    return send_netsio_message(NETSIO_COLD_RESET, 0, NULL, 0);
}

/* Register handlers for different message types */
void NetSIO_RegisterDataByteHandler(void (*handler)(uint8_t)) {
    data_byte_handler = handler;
}

void NetSIO_RegisterDataBlockHandler(void (*handler)(const uint8_t *, uint16_t)) {
    data_block_handler = handler;
}

void NetSIO_RegisterSyncResponseHandler(void (*handler)(uint8_t, uint8_t, uint8_t, uint16_t)) {
    sync_response_handler = handler;
}
