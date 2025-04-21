/*
 * fujinet_network.c - FujiNet network communication functions
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
#include <stdarg.h>

#include "config.h"
#include "fujinet.h"
#include "fujinet_network.h"
#include "log.h"
#include "util.h"
#include "atari.h"

/* Basic socket platform compatibility layer */
#ifdef HAVE_WINDOWS_H
#include <windows.h>
#include <winsock2.h>
typedef int socklen_t;
typedef SOCKET socket_t;
#define INVALID_SOCKET INVALID_SOCKET
#define closesocket closesocket
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
typedef int socket_t;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

/* Buffer for receiving data from the NetSIO hub */
/* Using FUJINET_BUFFER_SIZE from fujinet.h */
static uint8_t network_rx_buffer[FUJINET_BUFFER_SIZE];
static int network_rx_len = 0;
static int network_rx_idx = 0;

/* Socket and connection state */
static socket_t tcp_socket = INVALID_SOCKET;
static int network_connected = 0;
static uint8_t network_sio_status = 0; /* Last status byte received */

/* Sync request counter for NetSIO protocol */
static uint8_t sync_request_counter = 0;

/* Sync waiting state */
static int waiting_for_sync = 0;
static uint8_t waiting_sync_num = 0;

/* Logging macros */
#define NETWORK_LOG_ERROR(msg, ...) do { Log_print("FujiNet Network ERROR: " msg, ##__VA_ARGS__); } while(0)
#define NETWORK_LOG_WARN(msg, ...) do { Log_print("FujiNet Network WARN: " msg, ##__VA_ARGS__); } while(0)
#ifdef DEBUG_FUJINET
#define NETWORK_LOG_DEBUG(msg, ...) do { Log_print("FujiNet Network DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define NETWORK_LOG_DEBUG(msg, ...) ((void)0)
#endif

/* Helper functions for little-endian conversion */
static uint32_t to_little_endian(uint32_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 24) | 
           ((value & 0xFF00) << 8) | 
           ((value & 0xFF0000) >> 8) | 
           ((value & 0xFF000000) >> 24);
#else
    return value;
#endif
}

static uint32_t from_little_endian(uint32_t value) {
#ifdef WORDS_BIGENDIAN
    return ((value & 0xFF) << 24) | 
           ((value & 0xFF00) << 8) | 
           ((value & 0xFF0000) >> 8) | 
           ((value & 0xFF000000) >> 24);
#else
    return value;
#endif
}

/* Set the waiting for sync flag with the sync number we're waiting for */
void Network_SetWaitingForSync(uint8_t sync_num) {
    waiting_for_sync = 1;
    waiting_sync_num = sync_num;
    NETWORK_LOG_DEBUG("Now waiting for sync response #%d", (unsigned int)sync_num);
}

/* Clear the waiting for sync flag */
void Network_ClearWaitingForSync(void) {
    if (waiting_for_sync) {
        NETWORK_LOG_DEBUG("No longer waiting for sync response #%d", (unsigned int)waiting_sync_num);
    }
    waiting_for_sync = 0;
}

/* Check if we're waiting for a sync response */
int Network_IsWaitingForSync(void) {
    return waiting_for_sync;
}

/* Get the sync number we're waiting for */
uint8_t Network_GetWaitingSyncNum(void) {
    return waiting_sync_num;
}

/* Network operations implementation */

int Network_Initialize(const char *host_port) {
    char host[256] = FUJINET_DEFAULT_HOST;
    int port = FUJINET_DEFAULT_PORT;
    struct sockaddr_in server;
    int result;

    /* Parse host:port string if provided */
    if (host_port && *host_port) {
        char *colon = strchr(host_port, ':');
        if (colon) {
            size_t host_len = colon - host_port;
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, host_port, host_len);
            host[host_len] = '\0';
            port = atoi(colon + 1);
        } else {
            strncpy(host, host_port, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
    }

    Log_print("FujiNet Network: Initializing connection to NetSIO hub at %s:%d", host, port);

    /* Initialize platform-specific socket library if needed */
#ifdef HAVE_WINDOWS_H
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        Log_print("FujiNet Network: WSAStartup failed");
        return 0;
    }
#endif

    /* Create socket */
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket == INVALID_SOCKET) {
        Log_print("FujiNet Network: Failed to create socket: %s (errno=%d)", strerror(errno), errno);
        return 0;
    }

    /* Set up server address structure */
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    
    /* Resolve hostname to IP address */
    struct hostent *hp = gethostbyname(host);
    if (hp == NULL) {
        Log_print("FujiNet Network: Failed to resolve hostname %s: %s (errno=%d)", host, strerror(errno), errno);
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
        return 0;
    }
    memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
    
    /* Connect to the server */
    Log_print("FujiNet Network: Connecting to NetSIO hub at %s:%d...", host, port);
    NETWORK_LOG_DEBUG("Connecting to server...");
    result = connect(tcp_socket, (struct sockaddr*)&server, sizeof(server));
    NETWORK_LOG_DEBUG("Connected to server.");
    
    if (result < 0) {
        Log_print("FujiNet Network: Failed to connect to NetSIO hub: %s (errno=%d)", strerror(errno), errno);
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
        return 0;
    }

    /* Initialize receive buffer */
    network_rx_len = 0;
    network_rx_idx = 0;
    network_sio_status = 0;
    network_connected = 1;

    Log_print("FujiNet Network: Successfully connected to NetSIO hub");
    
    /* Note: We explicitly DO NOT send EVENT_CONNECTED message
       The NetSIO hub doesn't expect this message, and the TCP connection
       establishment is sufficient signal. */
    
    return 1;
}

void Network_Shutdown(void) {
    NETWORK_LOG_DEBUG("Network shutdown");
    
    if (network_connected) {
        /* No need to send a RESET command during normal operation */
        NETWORK_LOG_DEBUG("Closing network connection gracefully");
        
        /* Close socket */
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
        network_connected = 0;
    }
}

int Network_IsConnected(void) {
    return network_connected;
}

/* Send raw data over TCP, ensuring all bytes are sent */
int Network_SendData(const uint8_t *data, int data_len) {
    // --- Add Logging Start ---
    if (data_len > 0) {
        char hex_preview[40] = {0}; // Buffer for hex preview
        int preview_len = data_len < 8 ? data_len : 8; // Show up to 8 bytes
        for (int i = 0; i < preview_len; ++i) {
            snprintf(hex_preview + i*3, 4, "%02X ", data[i]);
        }
        // Remove trailing space if any
        if (preview_len > 0) hex_preview[preview_len*3 - 1] = '\0'; 

        NETWORK_LOG_DEBUG("Network_SendData: Sending %d bytes starting with: %s", data_len, hex_preview);
    } else {
        NETWORK_LOG_DEBUG("Network_SendData: Called with 0 bytes");
    }
    // --- Add Logging End ---
    
    int total_sent = 0;
    int sent;
    
    if (tcp_socket == INVALID_SOCKET || !network_connected) {
        NETWORK_LOG_ERROR("Attempted to send data while not connected.");
        return 0;
    }

    while (total_sent < data_len) {
        sent = send(tcp_socket, (const char *)(data + total_sent), data_len - total_sent, 0);
        if (sent <= 0) {
            NETWORK_LOG_ERROR("Failed to send data: %s (errno=%d)", strerror(errno), errno);
            network_connected = 0; /* Mark as disconnected on error */
            return 0;
        }
        total_sent += sent;
    }
    
    return 1; /* Success */
}

/* Read exactly 'buffer_size' bytes or timeout */
int Network_ReadExactBytes(uint8_t *buffer, int buffer_size, int *received_len) {
    int total_read = 0;
    int result;
    unsigned long start_time = Util_time();

    /* Initialize output parameter */
    if (received_len) *received_len = 0;
    
    if (tcp_socket == INVALID_SOCKET || !network_connected) {
        NETWORK_LOG_ERROR("Attempted to read data while not connected.");
        return 0;
    }
    
    while (total_read < buffer_size) {
        /* Use select() to check if data is available with timeout */
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(tcp_socket, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000; /* 10ms poll interval */
        
        result = select(tcp_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (result == 0) {
            /* No data available yet, check timeout */
            if (Util_time() - start_time > FUJINET_TIMEOUT_MS) {
                /* Timeout */
                NETWORK_LOG_DEBUG("Timeout waiting for data (received %d of %d bytes)",
                               total_read, buffer_size);
                if (received_len) *received_len = total_read;
                return total_read > 0 ? 1 : 0; /* Return success if we got some data */
            }
            continue; /* Keep waiting */
        } else if (result < 0) {
            /* Select error */
            NETWORK_LOG_ERROR("Error in select(): %s (errno=%d)", strerror(errno), errno);
            network_connected = 0; /* Mark as disconnected on error */
            return 0;
        }
        
        /* Data is available, read it */
        result = recv(tcp_socket, (char *)(buffer + total_read), buffer_size - total_read, 0);
        
        if (result <= 0) {
            /* Error or disconnection */
            if (result == 0) {
                NETWORK_LOG_WARN("Connection closed by peer");
            } else {
                NETWORK_LOG_ERROR("Error in recv(): %s (errno=%d)", strerror(errno), errno);
            }
            network_connected = 0; /* Mark as disconnected */
            return 0;
        }
        
        total_read += result;
    }
    
    if (received_len) *received_len = total_read;
    return 1; /* Success */
}

/* Send a message in the Altirra NetSIO Custom Protocol format */
int Network_SendAltirraMessage(uint8_t event, uint8_t arg, const uint8_t *data, uint16_t data_len) {
    uint8_t altirra_packet[17];
    uint32_t packet_id = event; // Use the event code itself as the packet ID
    uint32_t param1 = 0;     // Set param1 to 0
    uint32_t param2 = arg;
    uint32_t timestamp = 0;

    if (!network_connected || tcp_socket == INVALID_SOCKET) {
        NETWORK_LOG_ERROR("Network not connected for sending message");
        return 0;
    }

    NETWORK_LOG_DEBUG("Sending Altirra message: Cmd=0x%02X, Event(P1)=0x%08X, Arg(P2)=0x%08X, DataLen=%u",
                      packet_id, param1, param2, data_len);

    /* Format 17-byte Altirra packet (little-endian) */
    altirra_packet[0] = packet_id; // Command ID

    // Param1 (now 0) - Little Endian
    altirra_packet[1] = (param1 >> 0) & 0xFF;
    altirra_packet[2] = (param1 >> 8) & 0xFF;
    altirra_packet[3] = (param1 >> 16) & 0xFF;
    altirra_packet[4] = (param1 >> 24) & 0xFF;

    // Param2 (arg) - Little Endian
    altirra_packet[5] = (param2 >> 0) & 0xFF;
    altirra_packet[6] = (param2 >> 8) & 0xFF;
    altirra_packet[7] = (param2 >> 16) & 0xFF;
    altirra_packet[8] = (param2 >> 24) & 0xFF;

    // Timestamp (0) - Little Endian
    altirra_packet[9]  = (timestamp >> 0)  & 0xFF;
    altirra_packet[10] = (timestamp >> 8)  & 0xFF;
    altirra_packet[11] = (timestamp >> 16) & 0xFF;
    altirra_packet[12] = (timestamp >> 24) & 0xFF;
    altirra_packet[13] = (timestamp >> 32) & 0xFF;
    altirra_packet[14] = (timestamp >> 40) & 0xFF;
    altirra_packet[15] = (timestamp >> 48) & 0xFF;
    altirra_packet[16] = (timestamp >> 56) & 0xFF;

    /* Send 17-byte Altirra command packet */
    if (!Network_SendData(altirra_packet, 17)) {
        NETWORK_LOG_ERROR("Failed to send Altirra command packet");
        return 0;
    }
    NETWORK_LOG_DEBUG("Sent 17-byte Altirra command packet");

    /* Send data payload if it exists */
    if (data != NULL && data_len > 0) {
        if (!Network_SendData(data, data_len)) {
            NETWORK_LOG_ERROR("Failed to send Altirra message data payload (%u bytes)", data_len);
            return 0;
        }
        NETWORK_LOG_DEBUG("Sent %u bytes of Altirra data payload", data_len);
    }

    NETWORK_LOG_DEBUG("Altirra message sent successfully");
    return 1; /* Success */
}

/* Process an incoming Altirra NetSIO message 
 * Returns:
 *   1 if a data byte was added to the buffer
 *  -1 if a status byte was received
 *   0 if no message was available or an error occurred
 */
int Network_ProcessAltirraMessage(void) {
    if (!network_connected) {
        return -1; /* Not connected */
    }
    
    /* Receive the message header (10 bytes) */
    uint8_t header[10];
    int received = 0;
    
    if (!Network_ReadExactBytes(header, 10, &received)) {
        if (received == 0) {
            /* Socket closed or error */
            Network_Shutdown();
            return -1;
        }
        
        NETWORK_LOG_ERROR("Failed to read complete Altirra message header, got %d bytes", received);
        return -1;
    }
    
    /* Parse header fields */
    uint32_t msg_len = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
    uint8_t event = header[8];
    uint8_t arg = header[9];
    
    /* Calculate payload length (0 if none) */
    uint32_t payload_len = (msg_len > 10) ? (msg_len - 10) : 0;
    
    /* Log event details */
    NETWORK_LOG_DEBUG("Received Altirra Msg: Event=0x%02X, Arg=0x%02X, PayloadLen=%u", 
                     (unsigned int)event, (unsigned int)arg, (unsigned int)payload_len);
    
    /* Read payload if present */
    uint8_t *payload = NULL;
    int payload_len_read = 0;
    
    if (payload_len > 0) {
        payload = (uint8_t *)malloc(payload_len);
        if (!payload) {
            NETWORK_LOG_ERROR("Failed to allocate memory for payload");
            return -1;
        }
        
        if (!Network_ReadExactBytes(payload, payload_len, &payload_len_read)) {
            NETWORK_LOG_WARN("Failed to read complete payload, got %d/%u bytes", 
                             payload_len_read, (unsigned int)payload_len);
            /* Continue processing with partial payload */
        }
    }
    
    /* Process based on event type */
    switch (event) {
        case 0x00: /* 0x00 - Null event or padding */
            NETWORK_LOG_DEBUG("Received null event (0x00), likely padding - ignoring");
            /* Just free any payload and return 0 to indicate no data processed */
            if (payload) free(payload);
            return 0;
            
        case NETSIO_DATA_BYTE: /* 0x01 - Single data byte */
            /* Per NetSIO protocol and logs, the DATA_BYTE message has the actual data
             * byte in the arg field, not in the payload */
            
            /* Store the data byte (from arg) in our buffer */
            if (network_rx_len < FUJINET_BUFFER_SIZE) {
                network_rx_buffer[network_rx_len++] = (uint8_t)arg; 
                NETWORK_LOG_DEBUG("Added DATA_BYTE 0x%02X to rx_buffer (now len=%d)", 
                                 (unsigned int)arg, network_rx_len);
            } else {
                NETWORK_LOG_WARN("RX buffer full! Discarding byte 0x%02X", (unsigned int)arg);
            }
            
            if (payload) free(payload);
            return 1; /* Indicate data received */
            
        case NETSIO_DATA_BLOCK: /* 0x02 - Multiple data bytes */
            NETWORK_LOG_DEBUG("Received DATA_BLOCK (0x02) with %d bytes", payload_len_read);
            
            /* Process data block - add all payload bytes to our receive buffer */
            if (payload && payload_len_read > 0) {
                for (int i = 0; i < payload_len_read && network_rx_len < FUJINET_BUFFER_SIZE; i++) {
                    network_rx_buffer[network_rx_len++] = payload[i];
                    NETWORK_LOG_DEBUG("Added payload byte[%d]=0x%02X to rx_buffer (now len=%d)", 
                                     i, payload[i], network_rx_len);
                }
            } else {
                NETWORK_LOG_WARN("Received DATA_BLOCK with no payload data");
            }
            
            if (payload) free(payload);
            return 1; /* Indicate data received */
            
        case NETSIO_SYNC_RESPONSE: /* 0x81 - ACK/NAK response */
            /* Per the actual protocol behavior, the sync response message has:
             * - arg: the sync request number that this is responding to
             * - data[0]: the actual ACK/NAK response (0x41 for ACK, 0x4E for NAK) 
             */
            NETWORK_LOG_DEBUG("Received SYNC_RESPONSE (0x81) for sync #%d", (unsigned int)arg);
            
            /* Clear waiting-for-sync regardless of arg match (hub may not echo sync #) */
            if (Network_IsWaitingForSync()) {
                NETWORK_LOG_DEBUG("Received SYNC_RESPONSE while waiting, clearing wait state");
                Network_ClearWaitingForSync();
            }
            
            /* Check if we've received a payload with the ACK/NAK */
            if (payload && payload_len_read >= 1) {
                /* Store the ACK/NAK byte from the payload in our buffer */
                if (network_rx_len < FUJINET_BUFFER_SIZE) {
                    network_rx_buffer[network_rx_len++] = payload[0];
                    NETWORK_LOG_DEBUG("Added ACK/NAK byte 0x%02X from payload to rx_buffer", payload[0]);
                } else {
                    NETWORK_LOG_WARN("RX buffer full! Discarding ACK/NAK byte 0x%02X", payload[0]);
                }
            } else {
                /* If no payload, treat ARG as the ACK/NAK byte (hub sends it this way) */
                NETWORK_LOG_DEBUG("SYNC_RESPONSE without payload; using ARG (0x%02X) as ACK/NAK", arg);
                if (network_rx_len < FUJINET_BUFFER_SIZE) {
                    network_rx_buffer[network_rx_len++] = arg;
                }
            }
            
            if (payload) free(payload);
            return 1; /* Indicate response received */
        
        case NETSIO_COMMAND_OFF_SYNC: /* 0x18 - Command complete */
            NETWORK_LOG_DEBUG("Received COMMAND_OFF_SYNC with status 0x%02X", (unsigned int)arg);
            /* The arg is the completion status byte */
            network_sio_status = arg;
            sync_request_counter++;
            
            if (payload) free(payload);
            return -1; /* Signal completion status received */
            
        default:
            NETWORK_LOG_DEBUG("Unhandled Altirra event type 0x%02X", (unsigned int)event);
            if (payload) free(payload);
            return 0; /* No data to process */
    }
}

/* 
 * Get a byte from the receive buffer or network if needed
 * Returns:
 *   1 if a data byte was retrieved and stored in *byte
 *  -1 if a status byte was retrieved and stored in *byte
 *   0 if no byte is available (error or timeout)
 */
int Network_GetByte(uint8_t *byte) {
    if (!network_connected) {
        NETWORK_LOG_ERROR("Attempted to receive data while not connected.");
        return -1;
    }
    
    /* Check if we have buffered data from previous reads */
    if (network_rx_idx < network_rx_len) {
        *byte = network_rx_buffer[network_rx_idx++];
        NETWORK_LOG_DEBUG("Returning buffered byte: 0x%02X (idx=%d/%d)", 
                      *byte, network_rx_idx, network_rx_len);
        return 1;
    }
    
    /* Reset the buffer for a new receive operation */
    network_rx_idx = 0;
    network_rx_len = 0;
    
    if (Network_ProcessAltirraMessage() == 1) {
        *byte = network_rx_buffer[network_rx_idx++];
        return 1;
    }
    
    return 0;
}

/* Send a byte to the NetSIO hub */
int Network_PutByte(uint8_t byte) {
    /* For now, we just encapsulate the byte in an Altirra message */
    if (!network_connected) {
        NETWORK_LOG_ERROR("Network_PutByte called but not connected");
        return 0;
    }
    
    /* Need to decide on appropriate event type for sending data to NetSIO */
    if (!Network_SendAltirraMessage(NETSIO_DATA_BYTE, byte, NULL, 0)) {
        NETWORK_LOG_ERROR("Failed to send byte 0x%02X to NetSIO hub", byte);
        return 0;
    }
    
    return 1;
}

/* Get the current sync request counter value */
uint8_t Network_GetSyncCounter(void) {
    return sync_request_counter;
}
