#include "config.h"

#ifdef USE_FUJINET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SOCKET
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#else /* Assume Windows */
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif /* HAVE_SOCKET */

#include "log.h"
#include "fujinet.h"
#include "util.h" // For Util_strdup

#define FUJINET_DEFAULT_HOST "127.0.0.1"
#define FUJINET_DEFAULT_PORT 9996  /* NetSIO TCP service port */
#define FUJINET_BUFFER_SIZE 1024
#define FUJINET_TIMEOUT_SEC 5 /* Timeout for socket operations */

/* NetSIO Protocol Message IDs (Based on Altirra Custom Device) */
#define NETSIO_DEVICE_CONNECTED      0xC1
#define NETSIO_COLD_RESET            0xFF
#define NETSIO_COMMAND_ON            0xD1
#define NETSIO_COMMAND_OFF_SYNC_REQ  0xD2
#define NETSIO_COMMAND_OFF_ASYNC     0xD3
#define NETSIO_DATA_BLOCK            0xD4
#define NETSIO_ACK                   0xD0
#define NETSIO_NAK                   0xA0
#define NETSIO_STATUS                0xE0

/* SIO Protocol Message IDs (New for clarity) */
#define NETSIO_SIO_COMMAND           0xB1 /* Client sends SIO command */
#define NETSIO_SIO_RESPONSE          0xB2 /* Server sends SIO response */
#define NETSIO_SIO_COMPLETE          0xB3 /* Server signals SIO complete */

/* Enhanced logging with file output */
#define FUJINET_LOG(msg, ...) do { \
    fprintf(stdout, "FujiNet: " msg "\n", ##__VA_ARGS__); \
    fflush(stdout); \
    Log_print("FujiNet: " msg, ##__VA_ARGS__); \
    FILE *f = fopen("fujinet_debug.log", "a"); \
    if (f) { \
        fprintf(f, "FujiNet: " msg "\n", ##__VA_ARGS__); \
        fclose(f); \
    } \
} while(0)

/* Global variables */
static SOCKET tcp_socket = INVALID_SOCKET;
static int fujinet_enabled = 0;
static int motor_state = 0;

/* --- NetSIO Protocol Helpers --- */

/* Send a NetSIO formatted message */
static int send_netsio_message(uint8_t message_id, const uint8_t *data, uint16_t data_len) {
    uint8_t header[3];
    
    header[0] = message_id;
    header[1] = data_len & 0xFF;         /* Length (little-endian) */
    header[2] = (data_len >> 8) & 0xFF;
    
    FUJINET_LOG("Sending NetSIO message: ID=0x%02X, Len=%d", message_id, data_len);
    if (data_len > 0 && data != NULL) {
        log_hex_dump("  Data", data, data_len);
    }

    /* Send header */
    if (!send_tcp_data(header, 3)) {
        FUJINET_LOG("Failed to send NetSIO message header");
        return 0;
    }
    
    /* Send data payload (if any) */
    if (data_len > 0 && data != NULL) {
        if (!send_tcp_data(data, data_len)) {
            FUJINET_LOG("Failed to send NetSIO message data");
            return 0;
        }
    }
    
    return 1;
}

/* Receive a NetSIO formatted message, checking for expected ID */
static int receive_netsio_message(uint8_t expected_message_id, uint8_t *data_buffer, uint16_t buffer_size, uint16_t *received_len) {
    uint8_t header[3];
    int header_received_len;
    uint8_t received_message_id;
    uint16_t message_data_len;
    
    FUJINET_LOG("Waiting for NetSIO message (expecting ID=0x%02X)...", expected_message_id);

    /* Receive header */
    if (!receive_tcp_data(header, 3, &header_received_len)) {
        FUJINET_LOG("Failed to receive NetSIO message header");
        return 0;
    }
    
    if (header_received_len != 3) {
        FUJINET_LOG("Received incomplete NetSIO header (%d bytes)", header_received_len);
        return 0;
    }
    
    received_message_id = header[0];
    message_data_len = header[1] | (header[2] << 8); /* Length (little-endian) */

    FUJINET_LOG("Received NetSIO message: ID=0x%02X, Len=%d", received_message_id, message_data_len);

    /* Verify message ID */
    if (received_message_id != expected_message_id) {
        FUJINET_LOG("Error: Received unexpected NetSIO message ID 0x%02X (expected 0x%02X)", received_message_id, expected_message_id);
        // TODO: Handle unexpected messages? Maybe drain the data?
        return 0;
    }

    /* Special handling for ACK/NAK/STATUS which might have zero length */
    if (message_data_len == 0) {
        if (expected_message_id == NETSIO_ACK || expected_message_id == NETSIO_NAK || expected_message_id == NETSIO_STATUS) {
             FUJINET_LOG("Received expected zero-length message (ID=0x%02X)", received_message_id);
            *received_len = 0;
            return 1; // Success, no data to read
        } else {
             FUJINET_LOG("Error: Received unexpected zero-length message (ID=0x%02X, expected data)", received_message_id);
             return 0;
        }
    } else { 
        /* Check if buffer is large enough for data */
        if (message_data_len > buffer_size) {
            FUJINET_LOG("Error: NetSIO message data (%d bytes) larger than buffer (%d bytes)", message_data_len, buffer_size);
            // TODO: Handle this? Drain data?
            return 0;
        }
    }

    /* Receive data payload (if any) */
    if (message_data_len > 0) {
        int data_received_actual;
        if (!receive_tcp_data(data_buffer, message_data_len, &data_received_actual)) {
            FUJINET_LOG("Failed to receive NetSIO message data");
            return 0;
        }
        
        if (data_received_actual != message_data_len) {
             FUJINET_LOG("Received incomplete NetSIO data (%d bytes, expected %d)", data_received_actual, message_data_len);
             return 0;
        }
        *received_len = data_received_actual;
        log_hex_dump("  Data", data_buffer, *received_len);
    } else {
        *received_len = 0;
    }

    return 1;
}

/* --- End NetSIO Protocol Helpers --- */

/* Helper function to send data over TCP */
static int send_tcp_data(const uint8_t *data, int data_len) {
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        FUJINET_LOG("Cannot send data - FujiNet not initialized");
        return 0;
    }
    
    FUJINET_LOG("Sending %d bytes", data_len);
    
    int sent = send(tcp_socket, (const char *)data, data_len, 0);
    if (sent == SOCKET_ERROR) {
        FUJINET_LOG("send failed: %s", strerror(errno));
        return 0;
    }
    
    if (sent != data_len) {
        FUJINET_LOG("Warning: Sent only %d of %d bytes", sent, data_len);
    }
    
    return 1;
}

/* Helper function to receive data over TCP */
static int receive_tcp_data(uint8_t *buffer, int buffer_size, int *received_len) {
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        FUJINET_LOG("Cannot receive data - FujiNet not initialized");
        return 0;
    }
    
    int received = recv(tcp_socket, (char *)buffer, buffer_size, 0);
    if (received == SOCKET_ERROR) {
#ifdef HAVE_SOCKET
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            FUJINET_LOG("recv timeout - no response from NetSIO");
        } else {
            FUJINET_LOG("recv failed: %s", strerror(errno));
        }
#else // Windows
        int wsa_error = WSAGetLastError();
        if (wsa_error == WSAETIMEDOUT) {
            FUJINET_LOG("recv timeout - no response from NetSIO");
        } else {
            FUJINET_LOG("recv failed: WSAError %d", wsa_error);
        }
#endif
        return 0;
    }
    
    if (received == 0) {
        FUJINET_LOG("Connection closed by server");
        return 0;
    }
    
    *received_len = received;
    return 1;
}

/* Helper function to log a hex dump of data */
static void log_hex_dump(const char *prefix, const uint8_t *data, int len) {
    char buffer[FUJINET_BUFFER_SIZE * 3]; // 3 chars per byte (2 hex + 1 space)
    int pos = 0;
    
    for (int i = 0; i < len && pos < sizeof(buffer) - 4; i++) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "%02X ", data[i]);
    }
    
    FUJINET_LOG("%s: %s", prefix, buffer);
}

int FujiNet_Initialise(const char *host_port) {
    char host[256] = FUJINET_DEFAULT_HOST;
    int port = FUJINET_DEFAULT_PORT;
    struct hostent *he;
    struct timeval tv;
    
    /* Reset state */
    fujinet_enabled = 0;
    tcp_socket = INVALID_SOCKET;
    motor_state = 0;
    
    /* Parse host:port string if provided */
    if (host_port != NULL && host_port[0] != '\0') {
        char *port_str;
        char *host_port_copy = Util_strdup(host_port);
        
        if (host_port_copy == NULL) {
            FUJINET_LOG("Memory allocation failed");
            return 0;
        }
        
        port_str = strchr(host_port_copy, ':');
        if (port_str != NULL) {
            *port_str = '\0'; /* Split the string */
            port_str++;
            
            if (strlen(port_str) > 0) {
                int parsed_port = atoi(port_str);
                if (parsed_port > 0 && parsed_port < 65536) {
                    port = parsed_port;
                } else {
                    FUJINET_LOG("Invalid port number: %s, using default: %d", port_str, port);
                }
            }
        }
        
        if (strlen(host_port_copy) > 0) {
            strncpy(host, host_port_copy, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
        
        free(host_port_copy);
    }
    
    FUJINET_LOG("Initializing with host: %s, port: %d", host, port);
    
    /* Create TCP socket */
#ifdef HAVE_SOCKET
    tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else /* Windows */
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        FUJINET_LOG("WSAStartup failed");
        return 0;
    }
    tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    
    if (tcp_socket == INVALID_SOCKET) {
        FUJINET_LOG("Socket creation failed");
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    
    /* Set socket timeout */
    tv.tv_sec = FUJINET_TIMEOUT_SEC;
    tv.tv_usec = 0;
#ifdef HAVE_SOCKET
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
#else /* Windows */
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
#endif
        FUJINET_LOG("Failed to set socket receive timeout");
        /* Non-fatal, continue */
    }
    
#ifdef HAVE_SOCKET
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
#else /* Windows */
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
#endif
        FUJINET_LOG("Failed to set socket send timeout");
        /* Non-fatal, continue */
    }
    
    /* Set up server address */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    /* Try to resolve hostname */
    he = gethostbyname(host);
    if (he == NULL) {
        /* Try direct IP address parsing */
        server_addr.sin_addr.s_addr = inet_addr(host);
        if (server_addr.sin_addr.s_addr == INADDR_NONE) {
            FUJINET_LOG("Failed to resolve hostname: %s", host);
            closesocket(tcp_socket);
            tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
            WSACleanup();
#endif
            return 0;
        }
    } else {
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    /* Connect to the server */
    FUJINET_LOG("Connecting to server...");
    if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        FUJINET_LOG("Failed to connect: %s", strerror(errno));
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    
    FUJINET_LOG("TCP connection established");
    
    /* Send NetSIO device connected message */
    if (!send_netsio_message(NETSIO_DEVICE_CONNECTED, NULL, 0)) {
        FUJINET_LOG("Failed to send NETSIO_DEVICE_CONNECTED message");
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    FUJINET_LOG("Sent NETSIO_DEVICE_CONNECTED");
    
    /* Wait for ACK response */
    uint16_t ack_len;
    if (!receive_netsio_message(NETSIO_ACK, NULL, 0, &ack_len)) {
        FUJINET_LOG("Did not receive ACK after NETSIO_DEVICE_CONNECTED");
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    FUJINET_LOG("Received ACK for device connection");
     
    fujinet_enabled = 1;
    
    FUJINET_LOG("Initialized successfully.");
    return 1;
}

void FujiNet_Shutdown(void) {
    if (!fujinet_enabled) {
        return;
    }
    
    FUJINET_LOG("Shutting down FujiNet");
    
    /* Close socket */
    if (tcp_socket != INVALID_SOCKET) {
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
    }
    
#ifndef HAVE_SOCKET
    WSACleanup();
#endif
    
    fujinet_enabled = 0;
}

int FujiNet_ProcessCommand(const unsigned char *command_frame, unsigned char *response_frame) {
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        FUJINET_LOG("Cannot process command - FujiNet not initialized");
        return 0; // Not initialized or error during init
    }
    
    FUJINET_LOG("Processing SIO command: %02X %02X %02X %02X %02X",
               command_frame[0], command_frame[1], command_frame[2], command_frame[3], command_frame[4]);
    
    log_hex_dump("Sending SIO command", command_frame, 5);
    
    uint8_t sio_device_id = command_frame[0];
    const uint8_t *sio_command_data = command_frame + 1; // Bytes 1-4 (Command, Aux1, Aux2, Checksum)
    uint16_t ack_len;
    uint8_t response_buffer[FUJINET_BUFFER_SIZE];
    uint16_t received_len;

    /* 1. Send COMMAND_ON */
    if (!send_netsio_message(NETSIO_COMMAND_ON, &sio_device_id, 1)) {
        FUJINET_LOG("Failed to send NETSIO_COMMAND_ON");
        return 0;
    }

    /* 2. Wait for ACK */
    if (!receive_netsio_message(NETSIO_ACK, NULL, 0, &ack_len)) {
        FUJINET_LOG("Did not receive ACK after COMMAND_ON");
        return 0;
    }

    /* 3. Send DATA_BLOCK */
    if (!send_netsio_message(NETSIO_DATA_BLOCK, sio_command_data, 4)) {
        FUJINET_LOG("Failed to send NETSIO_DATA_BLOCK");
        return 0;
    }

    /* 4. Wait for ACK */
    if (!receive_netsio_message(NETSIO_ACK, NULL, 0, &ack_len)) {
        FUJINET_LOG("Did not receive ACK after DATA_BLOCK");
        return 0;
    }

    /* 5. Send COMMAND_OFF_SYNC_REQ */
    if (!send_netsio_message(NETSIO_COMMAND_OFF_SYNC_REQ, NULL, 0)) {
        FUJINET_LOG("Failed to send NETSIO_COMMAND_OFF_SYNC_REQ");
        return 0;
    }

    /* 6. Wait for SIO_RESPONSE */
    if (!receive_netsio_message(NETSIO_SIO_RESPONSE, response_buffer, sizeof(response_buffer), &received_len)) {
        FUJINET_LOG("Failed to receive NETSIO_SIO_RESPONSE");
        /* TODO: Maybe server sends NAK or STATUS instead? */
         return 0;
     }

    if (received_len != 4) {
        FUJINET_LOG("NETSIO_SIO_RESPONSE data wrong size (%d bytes, expected 4)", received_len);
         // Additional check: What if the server sends SIO_COMPLETE (0xB3)?
         // Or maybe NAK (0xA0)?
         // Need to handle other possible valid NetSIO responses here.
        return 0;
    }

    /* Copy the first 4 bytes of the response */
    memcpy(response_frame, response_buffer, 4);
    
    FUJINET_LOG("SIO Response: %02X %02X %02X %02X",
               response_frame[0], response_frame[1], response_frame[2], response_frame[3]);
    
    return 1;
}

void FujiNet_SetMotor(int on) {
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return;
    }
    
    /* Only send if state has changed */
    if (on != motor_state) {
        motor_state = on;
        
        FUJINET_LOG("Setting motor %s", on ? "ON" : "OFF");
        
        /* For now, we don't send any motor commands since we're not sure
           about the protocol expected by the NetSIO service */
    }
}

int FujiNet_IsEnabled(void) {
    return fujinet_enabled;
}

#endif /* USE_FUJINET */
