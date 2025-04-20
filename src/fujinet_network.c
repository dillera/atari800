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
    if (tcp_socket != INVALID_SOCKET) {
        Log_print("FujiNet Network: Closing connection");
        
        /* Send shutdown event if connected */
        if (network_connected) {
            Network_SendAltirraMessage(EVENT_SHUTDOWN, 0, NULL, 0);
        }
        
        /* Close socket */
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
        network_connected = 0;
    }
    
#ifdef HAVE_WINDOWS_H
    WSACleanup();
#endif
}

int Network_IsConnected(void) {
    return network_connected;
}

/* Send raw data over TCP, ensuring all bytes are sent */
int Network_SendData(const uint8_t *data, int data_len) {
    int total_sent = 0;
    int sent;
    
    if (tcp_socket == INVALID_SOCKET || !network_connected) {
        NETWORK_LOG_ERROR("Attempted to send data while not connected.");
        return 0;
    }

#ifdef DEBUG_FUJINET
    /* Debug log for hex dump of data being sent */
    char hex_buffer[100] = "";
    int len_to_log = data_len < 16 ? data_len : 16; /* Limit data dump to first 16 bytes */
    int i;
    
    for (i = 0; i < len_to_log; i++) {
        sprintf(hex_buffer + i * 3, "%02X ", data[i]);
    }
    if (len_to_log > 0) hex_buffer[len_to_log * 3 - 1] = '\0'; else hex_buffer[0] = '\0'; /* Trim trailing space */
    NETWORK_LOG_DEBUG("send_tcp_data: Sending %d bytes, starting with: %s%s", 
                       len_to_log, hex_buffer, len_to_log < data_len ? "..." : "");
#endif

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
int Network_SendAltirraMessage(uint8_t event, uint8_t arg, const uint8_t *data, int data_len) {
    uint8_t header[8];
    uint32_t total_length;
    
    /* Build the header */
    /* Fields:
       Bytes 0-3: Total length (header + payload) as little-endian 32-bit int
       Byte 4: Event ID
       Byte 5: Argument byte
       Bytes 6-7: Zero padding */
    total_length = 8; /* Header size */
    if (data && data_len > 0) {
        total_length += data_len;
    }
    
    /* Convert total length to little-endian */
    *(uint32_t*)header = to_little_endian(total_length);
    
    /* Event ID and argument */
    header[4] = event;
    header[5] = arg;
    header[6] = header[7] = 0; /* Zero padding */
    
    /* Send header */
    if (!Network_SendData(header, 8)) {
        return 0; /* Failed to send header */
    }
    
    /* Send payload if present */
    if (data && data_len > 0) {
        if (!Network_SendData(data, data_len)) {
            return 0; /* Failed to send payload */
        }
    }
    
    return 1; /* Success */
}

/* Process an incoming Altirra NetSIO message 
 * Returns:
 *   1 if a data byte was added to the buffer
 *  -1 if a status byte was received
 *   0 if no message was available or an error occurred
 */
int Network_ProcessAltirraMessage(void) {
    uint8_t header[8];
    int header_len_read;
    uint8_t event, arg;
    uint32_t total_len;
    int payload_len;
    uint8_t *payload = NULL;
    int payload_len_read;

    /* Read the 8-byte header */
    if (!Network_ReadExactBytes(header, 8, &header_len_read)) {
        NETWORK_LOG_DEBUG("Failed to read Altirra header.");
        /* Error, timeout, or connection closed by peer - don't crash, 
           just return an error that can be handled gracefully */
        if (!network_connected) {
            NETWORK_LOG_WARN("Network connection lost during header read");
        }
        return 0;
    }
    
    /* Parse header */
    total_len = from_little_endian(*(uint32_t*)header);
    event = header[4];
    arg = header[5];
    
    payload_len = total_len - 8; /* Header is 8 bytes */
    
    NETWORK_LOG_DEBUG("Received Altirra Msg: Event=0x%02X, Arg=0x%02X, PayloadLen=%d", 
                       (unsigned int)event, (unsigned int)arg, payload_len);
    
    /* Read payload if it exists */
    if (payload_len > 0) {
        payload = (uint8_t*)malloc(payload_len);
        if (!payload) {
            NETWORK_LOG_ERROR("Failed to allocate memory for payload of %d bytes", payload_len);
            return 0; /* Memory allocation error */
        }
        if (!Network_ReadExactBytes(payload, payload_len, &payload_len_read)) {
            NETWORK_LOG_DEBUG("Failed to read Altirra payload.");
            free(payload);
            /* Error, timeout, or connection closed by peer - 
               check connection status and log appropriately */
            if (!network_connected) {
                NETWORK_LOG_WARN("Network connection lost during payload read");
            }
            return 0; /* Read error */
        }
    }
    
    /* Process based on event type */
    switch (event) {
        case NETSIO_DATA_BYTE: /* 0x01 */
            if (network_rx_len < FUJINET_BUFFER_SIZE) {
                network_rx_buffer[network_rx_len++] = (uint8_t)arg; 
                NETWORK_LOG_DEBUG("Added byte 0x%02X to rx_buffer (new len=%d)", (unsigned int)arg, network_rx_len);
            } else {
                NETWORK_LOG_WARN("FujiNet RX buffer full! Discarding byte 0x%02X", (unsigned int)arg);
            }
            if (payload) free(payload);
            return 1; /* Indicate data byte received */
            
        case NETSIO_STATUS_BYTE: /* 0x02 - Final SIO status byte */
            /* Store status byte for later retrieval */
            network_sio_status = arg;
            NETWORK_LOG_DEBUG("Stored SIO status byte: 0x%02X", network_sio_status);
            if (payload) free(payload);
            return -1; /* Signal status received */
            
        case NETSIO_COMMAND_OFF: /* 0x10 - Placeholder for future status handling */
            NETWORK_LOG_DEBUG("Received NETSIO_COMMAND_OFF (0x10) - Status handling TBD.");
            /* TODO: Set a status variable (e.g., network_sio_status = SIO_COMPLETE/SIO_ERROR based on arg?) */
            break;
            
        default:
            NETWORK_LOG_DEBUG("Received unhandled event type: 0x%02X", (unsigned int)event);
            break;
    }
    
    if (payload) free(payload);
    return 0; /* No data/status processed */
}

/* 
 * Get a byte from the receive buffer or network if needed
 * Returns:
 *   1 if a data byte was retrieved and stored in *byte
 *  -1 if a status byte was retrieved and stored in *byte
 *   0 if no byte is available (error or timeout)
 */
int Network_GetByte(uint8_t *byte) {
    unsigned long start_time = Util_time(); /* For timeout */
    static unsigned long last_reconnect_attempt = 0;
    const unsigned long reconnect_cooldown_ms = 5000; /* Only try reconnecting every 5 seconds */
    
    if (!network_connected) {
        NETWORK_LOG_ERROR("Network_GetByte called but not connected");
        
        /* Check if we should attempt reconnection */
        unsigned long current_time = Util_time();
        if (current_time - last_reconnect_attempt > reconnect_cooldown_ms) {
            NETWORK_LOG_WARN("Attempting to reconnect to NetSIO hub...");
            last_reconnect_attempt = current_time;
            
            /* Attempt to reopen socket and reconnect */
            if (tcp_socket != INVALID_SOCKET) {
                closesocket(tcp_socket);
                tcp_socket = INVALID_SOCKET;
            }
            
            /* Create new socket */
            tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
            if (tcp_socket == INVALID_SOCKET) {
                NETWORK_LOG_ERROR("Failed to create socket for reconnection");
                return 0;
            }
            
            /* Try to connect to localhost on the default port */
            struct sockaddr_in server;
            struct hostent *hp;
            
            memset(&server, 0, sizeof(server));
            server.sin_family = AF_INET;
            server.sin_port = htons(FUJINET_DEFAULT_PORT);
            
            hp = gethostbyname(FUJINET_DEFAULT_HOST);
            if (hp) {
                memcpy(&server.sin_addr, hp->h_addr, hp->h_length);
                
                if (connect(tcp_socket, (struct sockaddr*)&server, sizeof(server)) == 0) {
                    NETWORK_LOG_DEBUG("Reconnected to NetSIO hub successfully");
                    network_connected = 1;
                    network_rx_len = 0;
                    network_rx_idx = 0;
                    network_sio_status = 0;
                } else {
                    NETWORK_LOG_ERROR("Failed to reconnect: %s", strerror(errno));
                    closesocket(tcp_socket);
                    tcp_socket = INVALID_SOCKET;
                }
            } else {
                NETWORK_LOG_ERROR("Failed to resolve hostname for reconnection");
                closesocket(tcp_socket);
                tcp_socket = INVALID_SOCKET;
            }
        }
        
        return 0; /* Error */
    }
    
    while (1) { /* Loop until byte/status returned or timeout */
        int process_result;
        
        /* Priority 1: Check if final SIO status byte was already received */
        if (network_sio_status != 0) {
            NETWORK_LOG_DEBUG("Returning stored SIO status 0x%02X", network_sio_status);
            *byte = network_sio_status;
            network_sio_status = 0; /* Clear status */
            return -1; /* Indicate final status byte */
        }
        
        /* Priority 2: Check if data is available in the buffer */
        if (network_rx_idx < network_rx_len) {
            *byte = network_rx_buffer[network_rx_idx++];
            NETWORK_LOG_DEBUG("Returning buffered byte 0x%02X (idx=%d, len=%d)", *byte, network_rx_idx, network_rx_len);
            return 1; /* Indicate data byte */
        }
        
        /* Priority 3: Try to read new data from the socket if buffer is empty */
        if (network_rx_idx >= network_rx_len && network_sio_status == 0) {
            NETWORK_LOG_DEBUG("No data in buffer, attempting receive_tcp_data...");
            process_result = Network_ProcessAltirraMessage(); 
            
            if (process_result == 1) { /* Data byte added to buffer */
                NETWORK_LOG_DEBUG("Data byte added to buffer.");
                start_time = Util_time(); 
                continue;
            } else if (process_result == -1) { /* SIO status byte received */
                NETWORK_LOG_DEBUG("SIO status byte received.");
                start_time = Util_time();
                continue; /* Status is now stored in network_sio_status, will be handled next loop */
            } else { /* process_result == 0: Error or no message ready */
                NETWORK_LOG_DEBUG("Checking for timeout...");
                if (Util_time() - start_time > FUJINET_TIMEOUT_MS) {
                    NETWORK_LOG_WARN("Network_GetByte: Timeout (%lums) waiting for data/status from hub.", FUJINET_TIMEOUT_MS);
                    return 0; /* Indicate timeout/error */
                }
                /* loop will check Util_time timeout shortly. */
            }
        }
    }
    
    /* Should not be reached if timeout logic inside loop is correct */
    return 0; /* Error/Timeout */
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
