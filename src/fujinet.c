#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>  /* For va_start, va_end */

#include "config.h" /* For HAVE_SOCKET etc. */

#ifdef USE_FUJINET

#ifdef HAVE_SOCKET
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h> /* For close() */
#define SOCKET int
#define INVALID_SOCKET (-1)
#define closesocket close
#define SOCKET_ERROR (-1)
#else /* Windows */
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#define SOCKET SOCKET
#endif /* HAVE_SOCKET */

#include "atari.h" /* For Atari->Exit() */
#include "sio.h"   /* For SIO command codes and responses */
#include "log.h"
#include "util.h" /* For Util_strdup */
#include "fujinet.h"

#define FUJINET_DEFAULT_HOST "127.0.0.1"
#define FUJINET_DEFAULT_PORT 9996  /* NetSIO TCP service port */
#define FUJINET_TIMEOUT_SEC 5 /* Timeout for socket operations */

/* Altirra Device Protocol Event IDs */
#define EVENT_CONNECTED      0x01
#define EVENT_COMMAND_ON     0x11 /* Value from test client */
#define EVENT_COMMAND_OFF    0x18 /* Value from test client (COMMAND_OFF_SYNC) */
#define EVENT_DATA_TO_DEVICE 0x02 /* Value from test client (DATA_BLOCK) */
#define EVENT_RESET          0xF0
#define EVENT_BOOT           0xF1

/* Altirra Device Protocol Response Events */
#define EVENT_ERROR          0x90
#define EVENT_ACK            0x81 /* Value from test client (SYNC_RESPONSE, potentially used as ACK?) */
#define EVENT_DATA_FROM_DEVICE 0x92 /* Keeping original for final data response, test uses SYNC_RESPONSE */
#define EVENT_EVENT          0x93
#define EVENT_DTR_CHANGE     0x94

/* Simplified logging functions to use the Atari800 Log system */
static void log_message(const char *format, ...) {
    char buffer[FUJINET_BUFFER_SIZE];
    va_list args;
    
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Log_print("FujiNet: %s", buffer);
}

/* For non-formatted strings */
#define FUJINET_LOG(msg) do { \
    Log_print("FujiNet: %s", (msg)); \
} while(0)

/* For formatted strings */
#define FUJINET_LOG_FORMAT(msg, ...) do { \
    Log_print("FujiNet: " msg, __VA_ARGS__); \
} while(0)

/* Altirra NetSIO Sync Byte */
#define NETSIO_SYNC 0xFF

/* Conditional Logging Macros */
#ifdef DEBUG_FUJINET
#define FUJINET_LOG_DEBUG(msg, ...) do { Log_print("FujiNet DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define FUJINET_LOG_DEBUG(msg, ...) ((void)0)
#endif

#define FUJINET_LOG_ERROR(msg, ...) do { \
    Log_print("FujiNet ERROR: " msg, ##__VA_ARGS__); \
} while(0)

#define FUJINET_LOG_WARN(msg, ...) do { \
    Log_print("FujiNet WARN: " msg, ##__VA_ARGS__); \
} while(0)

/* Global variables */
static SOCKET tcp_socket = INVALID_SOCKET;
int fujinet_enabled = 0; /* Non-static to allow external access */
static int motor_state = 0;

/* Internal buffer for received data */
static uint8_t fujinet_data_buffer[FUJINET_BUFFER_SIZE];
static int fujinet_data_len = 0;
static int fujinet_data_idx = 0;
static int fujinet_expected_data_len = 0; /* How much data we expect after an ACK */

/* Forward declarations */
static int send_tcp_data(const uint8_t *data, int data_len);
static int receive_tcp_data(uint8_t *buffer, int buffer_size, int *received_len);
static uint32_t from_little_endian(uint32_t value);

/* Helper function to convert a 32-bit integer to little-endian format */
static uint32_t to_little_endian(uint32_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return  ((value & 0xFF000000) >> 24) |
            ((value & 0x00FF0000) >> 8) |
            ((value & 0x0000FF00) << 8) |
            ((value & 0x000000FF) << 24);
#endif
}

/* Helper function to convert a little-endian 32-bit integer to host format */
static uint32_t from_little_endian(uint32_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return ((value & 0xFF) << 24) | ((value & 0xFF00) << 8) | 
           ((value & 0xFF0000) >> 8) | ((value & 0xFF000000) >> 24);
#endif
}

/* Helper function to send an Altirra protocol message */
static int send_altirra_message(uint8_t event, uint8_t arg, const uint8_t *data, int data_len) {
    uint8_t *message;
    uint32_t total_length;
    uint32_t length_le, timestamp_le = 0;
    int sent_ok;
    
    /* Calculate total message length (header + payload) */
    total_length = 8 + 2; /* 8-byte header + event byte + arg byte */
    if (data != NULL && data_len > 0) {
        total_length += data_len;
    }
    
    /* Allocate buffer for message */
    message = (uint8_t *)malloc(total_length);
    if (!message) {
        Log_print("FujiNet: Memory allocation failed for Altirra message");
        return 0;
    }
    
    /* Convert the 32-bit values to little-endian format */
    length_le = to_little_endian(total_length);
    timestamp_le = 0; /* We use 0 for timestamp */
    
    /* Copy the header byte-by-byte to ensure correct format */
    memcpy(message, &length_le, 4);       /* Total length (4 bytes) */
    memcpy(message + 4, &timestamp_le, 4); /* Timestamp (4 bytes) */
    
    /* Set event type and device ID */
    message[8] = event;  /* Event byte */
    message[9] = arg;    /* Arg byte (usually device ID) */
    
    /* Copy data (if any) */
    if (data != NULL && data_len > 0) {
        memcpy(message + 10, data, data_len);
    }
    
    /* Send the full message */
    sent_ok = send_tcp_data(message, total_length);
    
    free(message);
    return sent_ok;
}

/* Helper function to receive an Altirra protocol message */
static int receive_altirra_message(uint8_t expected_event, uint8_t *event, uint8_t *arg, uint8_t *data_buffer, int buffer_size, int *received_len) {
    uint8_t header[8];
    uint8_t *payload;
    uint32_t total_length;
    uint32_t payload_len;
    int header_received_len = 0;
    int data_received_actual = 0;
    int result = 0;
    
    /* Initialize received_len to 0 */
    if (received_len != NULL) {
        *received_len = 0;
    }
    
    /* Read the 8-byte header */
    if (!receive_tcp_data(header, 8, &header_received_len)) {
        Log_print("FujiNet: Failed to receive header");
        return 0;
    }
    
    /* Validate header length */
    if (header_received_len != 8) {
        Log_print("FujiNet: Incomplete header received");
        return 0;
    }
    
    /* Extract total message length from header */
    total_length = from_little_endian(*((uint32_t *)header));
    
    /* Calculate payload length */
    if (total_length < 8) {
        Log_print("FujiNet: Invalid message length");
        return 0;
    }
    
    payload_len = total_length - 8;
    
    /* Allocate buffer for payload: event (1) + arg (1) + data */
    payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        Log_print("FujiNet: Memory allocation failed for payload");
        return 0;
    }
    
    /* Read the payload */
    if (!receive_tcp_data(payload, payload_len, &data_received_actual)) {
        Log_print("FujiNet: Failed to receive payload");
        free(payload);
        return 0;
    }
    
    /* Validate payload length */
    if (data_received_actual < 2) {
        Log_print("FujiNet: Incomplete payload received");
        free(payload);
        return 0;
    }
    
    /* Extract event type and arg */
    if (event != NULL) {
        *event = payload[0];
    }
    
    if (arg != NULL) {
        *arg = payload[1];
    }
    
    /* Check if it's the expected event type */
    if (expected_event != 0 && payload[0] != expected_event) {
        Log_print("FujiNet: Received unexpected event 0x%02X (expected 0x%02X)", payload[0], expected_event);
        
        /* Special case: if we got ERROR, log it */
        if (payload[0] == EVENT_ERROR) {
            Log_print("FujiNet: Received ERROR event with arg 0x%02X", payload[1]);
        }
        
        free(payload);
        return 0;
    }
    
    /* For DATA_FROM_DEVICE, copy data to buffer if provided */
    if (payload[0] == EVENT_DATA_FROM_DEVICE && data_buffer != NULL && buffer_size > 0) {
        int data_len = data_received_actual - 2; /* Subtract event and arg bytes */
        if (data_len > buffer_size) {
            data_len = buffer_size; /* Limit to buffer size */
        }
        
        if (data_len > 0) {
            memcpy(data_buffer, payload + 2, data_len);
            if (received_len != NULL) {
                *received_len = data_len;
            }
        }
    }
    
    result = 1;
    free(payload);
    return result;
}

/* Helper function to send data over TCP, ensuring all bytes are sent */
static int send_tcp_data(const uint8_t *data, int data_len) {
    int total_sent = 0;
    int sent;
    
    if (tcp_socket == INVALID_SOCKET) { 
        return 0;
    }
    
    while (total_sent < data_len) {
        sent = send(tcp_socket, (const char *)(data + total_sent), data_len - total_sent, 0);
        if (sent == SOCKET_ERROR) {
#ifdef HAVE_SOCKET
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Interrupted or temporary error, try again */
                continue; 
            }
#else /* Windows */
            int wsa_error = WSAGetLastError();
            if (wsa_error == WSAEINTR || wsa_error == WSAEWOULDBLOCK) {
                /* Interrupted or temporary error, try again */
                continue;
            }
#endif
            /* Other error */
            Log_print("FujiNet: Send failed: %s", strerror(errno));
            return 0; /* Failure */
        }
        if (sent == 0) {
             /* Socket closed unexpectedly? */
             Log_print("FujiNet: Send returned 0, connection closed?");
             return 0; /* Failure */
        }
        total_sent += sent;
    }
    
    /* All bytes sent successfully */
    return 1; 
}

/* Helper function to receive data over TCP */
static int receive_tcp_data(uint8_t *buffer, int buffer_size, int *received_len) {
    int total_received = 0;
    int received_now;
    struct timeval tv;
    fd_set readfds;

    if (tcp_socket == INVALID_SOCKET) {
        if (received_len) *received_len = 0;
        return 0; /* Indicate failure/not connected */
    }

    while (total_received < buffer_size) {
        /* Use select() for timeout */
        FD_ZERO(&readfds);
        FD_SET(tcp_socket, &readfds);
        
        /* Set timeout (e.g., 1 second) */
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ret = select(tcp_socket + 1, &readfds, NULL, NULL, &tv);
        
        if (ret == -1) { /* Error */
            Log_print("FujiNet: select() failed: %s", strerror(errno));
            if (received_len) *received_len = total_received;
            return 0; /* Indicate failure */
        } else if (ret == 0) { /* Timeout */
            Log_print("FujiNet: Receive timeout waiting for %d bytes (got %d)", buffer_size, total_received);
            if (received_len) *received_len = total_received;
            return 0; /* Indicate failure due to timeout */
        } else { /* Data available */
#ifdef _WIN32
            received_now = recv(tcp_socket, (char*)buffer + total_received, buffer_size - total_received, 0);
#else
            received_now = recv(tcp_socket, buffer + total_received, buffer_size - total_received, 0);
#endif

            if (received_now < 0) { /* Error */
#ifdef _WIN32
                int wsa_error = WSAGetLastError();
                if (wsa_error == WSAEWOULDBLOCK) continue; /* Shouldn't happen with select, but check */
                Log_print("FujiNet: Receive failed: Error %d", wsa_error);
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* Shouldn't happen with select, but check */
                Log_print("FujiNet: Receive failed: %s", strerror(errno));
#endif
                if (received_len) *received_len = total_received;
                return 0; /* Indicate failure */
            }
            
            if (received_now == 0) { /* Connection closed */
                Log_print("FujiNet: Connection closed by server while waiting for %d bytes (got %d)", buffer_size, total_received);
                if (received_len) *received_len = total_received;
                return 0; /* Indicate connection closed */
            }
            
            total_received += received_now;
        }
    }
    
    /* Successfully received buffer_size bytes */
    if (received_len != NULL) {
        *received_len = total_received;
    }
    
    /* --- Log received bytes (moved from previous edit for clarity) --- */
    Log_print("FujiNet: receive_tcp_data successfully got %d bytes:", total_received);
    if (total_received > 0) {
        char hex_buf[16]; 
        int i;
        for (i = 0; i < total_received; ++i) {
            snprintf(hex_buf, sizeof(hex_buf), " %02X", buffer[i]);
            Log_print(hex_buf); 
        }
        Log_print("\n"); 
    }
    /* --- END LOGGING --- */
    
    return 1; /* Indicate success */
}

int FujiNet_Initialise(const char *host_port) {
    char host[256] = FUJINET_DEFAULT_HOST;
    int port = FUJINET_DEFAULT_PORT;
    struct sockaddr_in server;
    struct hostent *he;
    struct timeval tv;
    /* fd_set readfds; */ /* Unused in this function */
    int result;
    char *host_port_copy;
    char *port_str;

    /* Reset state */
    fujinet_enabled = 0;
    tcp_socket = INVALID_SOCKET;
    motor_state = 0;
    
    /* Parse host:port string if provided */
    if (host_port != NULL && host_port[0] != '\0') {
        host_port_copy = Util_strdup(host_port);
        
        if (host_port_copy == NULL) {
            Log_print("FujiNet: Memory allocation failed");
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
                    Log_print("FujiNet: Invalid port number: %s, using default: %d", port_str, port);
                }
            }
        }
        
        if (strlen(host_port_copy) > 0) {
            strncpy(host, host_port_copy, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
        }
        
        free(host_port_copy);
    }
    
    Log_print("FujiNet: Initializing with host: %s, port: %d", host, port);
    
    /* Create TCP socket */
#ifdef HAVE_SOCKET
    tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else /* Windows */
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        Log_print("FujiNet: WSAStartup failed");
        return 0;
    }
    tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    
    if (tcp_socket == INVALID_SOCKET) {
        Log_print("FujiNet: Socket creation failed");
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
        Log_print("FujiNet: Failed to set socket timeout");
        /* Non-fatal, continue */
    }
    
#ifdef HAVE_SOCKET
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
#else /* Windows */
    if (setsockopt(tcp_socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv)) < 0) {
#endif
        /* Non-fatal, continue */
    }
    
    /* Set up server address */
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    
    /* Try to resolve hostname */
    he = gethostbyname(host);
    if (he == NULL) {
        /* Try direct IP address parsing */
        server.sin_addr.s_addr = inet_addr(host);
        if (server.sin_addr.s_addr == INADDR_NONE) {
            Log_print("FujiNet: Failed to resolve hostname: %s", host);
            closesocket(tcp_socket);
            tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
            WSACleanup();
#endif
            return 0;
        }
    } else {
        memcpy(&server.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    /* Connect to the server */
    Log_print("FujiNet: Connecting to server...");
    result = connect(tcp_socket, (struct sockaddr*)&server, sizeof(server));
    if (result < 0) {
        Log_print("FujiNet: Failed to connect: %s", strerror(errno));
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    
    Log_print("FujiNet: Connected to NetSIO hub");

    /* Send EVENT_CONNECTED immediately after connecting */
    uint8_t connected_event = EVENT_CONNECTED;
    if (send_tcp_data(&connected_event, 1) <= 0) {
        Log_print("FujiNet: Failed to send EVENT_CONNECTED");
        /* Decide if this is fatal? Maybe log and continue? */
        /* For now, let's continue and see what happens */
        // closesocket(tcp_socket);
        // tcp_socket = INVALID_SOCKET;
        // WSACleanup();
        // return 0; 
    }

    fujinet_enabled = 1; /* Set enabled *after* successful connection and initial message */
    Log_print("FujiNet: Device initialized successfully (assuming connection implies readiness)");
    return 1;
}

void FujiNet_Shutdown(void) {
    if (!fujinet_enabled) {
        return;
    }
    
    Log_print("FujiNet: Shutting down");
    
    /* Close socket */
    if (tcp_socket != INVALID_SOCKET) {
        /* Send RESET event to notify server we're disconnecting */
        send_altirra_message(EVENT_RESET, 0, NULL, 0);
        
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
    }
    
#ifndef HAVE_SOCKET
    WSACleanup();
#endif
    
    fujinet_enabled = 0;
}

/* Processes a 5-byte SIO command frame and returns the immediate SIO response byte (A, N, C, E). */
UBYTE FujiNet_ProcessCommand(const UBYTE *command_frame /* Pointer to the 5-byte SIO command frame */) {
#ifdef DEBUG_FUJINET
    if (!command_frame) {
        FUJINET_LOG_ERROR("ProcessCommand called with NULL command_frame.");
        return SIO_ERROR_FRAME;
    }
    FUJINET_LOG_DEBUG("FujiNet_ProcessCommand called. Frame: %02X %02X %02X %02X %02X",
                      command_frame[0], command_frame[1], command_frame[2], command_frame[3], command_frame[4]);
#endif

    /* Extract command details from the frame */
    UBYTE sio_command = command_frame[1];
    /* UBYTE sio_devic = command_frame[0]; */ /* Unused */
    /* UBYTE sio_aux1 = command_frame[2]; */ /* Unused */
    /* UBYTE sio_aux2 = command_frame[3]; */ /* Unused */

    /* Check if FujiNet is enabled and connected */
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        FUJINET_LOG_ERROR("ProcessCommand called but FujiNet not enabled or connected.");
        return SIO_NAK; /* NAK seems appropriate for device not ready */
    }

    /* Clear any stale data from previous operations */
    fujinet_expected_data_len = 0;
    fujinet_data_len = 0;
    fujinet_data_idx = 0;

    /* Altirra Protocol: Send Command Sequence (4 bytes + sync) */
    UBYTE command_sequence[5];
    command_sequence[0] = NETSIO_SYNC;          /* Sync byte */
    command_sequence[1] = command_frame[1];     /* Command */
    command_sequence[2] = command_frame[2];     /* Aux1 */
    command_sequence[3] = command_frame[3];     /* Aux2 */
    command_sequence[4] = command_frame[1] ^ command_frame[2] ^ command_frame[3] ^ NETSIO_SYNC; /* Checksum (XOR) */

    FUJINET_LOG_DEBUG("Sending command sequence: %02X %02X %02X %02X %02X",
                      command_sequence[0], command_sequence[1], command_sequence[2], command_sequence[3], command_sequence[4]);

    if (!send_tcp_data(command_sequence, 5)) {
        FUJINET_LOG_ERROR("Failed to send command sequence to FujiNet device.");
        /* Consider closing/resetting connection? */
        return SIO_ERROR_FRAME; /* Error sending command */
    }

    /* Altirra Protocol: Receive Immediate SIO Response (1 byte: A/C/N/E) */
    UBYTE immediate_response = SIO_NAK; /* Initialize to prevent using uninitialized value if recv fails */
    int bytes_received = recv(tcp_socket, &immediate_response, 1, 0);

    if (bytes_received <= 0) {
        if (bytes_received == 0) {
            FUJINET_LOG_ERROR("FujiNet device closed connection while waiting for immediate response.");
        } else {
#ifdef _WIN32
            FUJINET_LOG_ERROR("recv error waiting for immediate response: %d", WSAGetLastError());
#else
            FUJINET_LOG_ERROR("recv error waiting for immediate response: %s", strerror(errno));
#endif
        }
        /* Consider closing/resetting connection? */
        return SIO_ERROR_FRAME; /* Error receiving response */
    }

    FUJINET_LOG_DEBUG("Received immediate response: %c (0x%02X)", (immediate_response >= 32 && immediate_response <= 126) ? immediate_response : '?', immediate_response);

    /* Handle response - potentially buffer data for read commands */
    switch (immediate_response) {
        case SIO_ACK:
            /* Command acknowledged, data phase will follow (handled by GetByte/PutByte) */
            /* Determine expected data length based on command */
            switch (sio_command) {
                case SIO_CMD_READ_SECTOR:       /* 0x52 */
                case SIO_CMD_READ_SECTOR_HS:    /* 0xD2 */
                    /* TODO: Get actual sector size from disk image? Assume 128 for now */
                    fujinet_expected_data_len = 128;
                    break;
                case SIO_CMD_STATUS_BLOCK:      /* 0x4E */
                    /* SpartaDOS Status Block? Check typical size */
                     fujinet_expected_data_len = 12; /* Common size */
                     break;
                case SIO_CMD_DRIVE_STATUS:      /* 0x53 */
                case SIO_CMD_DRIVE_STATUS_HS:   /* 0xD3 */
                    fujinet_expected_data_len = 4;
                    break;
                 /* Commands that have a data phase FROM Atari TO FujiNet */
                case SIO_CMD_WRITE_SECTOR:      /* 0x50 */
                case SIO_CMD_WRITE_VERIFY:      /* 0x57 */
                case SIO_CMD_WRITE_SECTOR_HS:   /* 0xD0 */
                case SIO_CMD_WRITE_VERIFY_HS:   /* 0xD7 */
                case SIO_CMD_FORMAT_DISK:       /* 0x21 */
                case SIO_CMD_FORMAT_ENHANCED:   /* 0x22 */
                case SIO_CMD_FORMAT_DISK_HS:    /* 0xA1 */
                case SIO_CMD_FORMAT_ENHANCED_HS:/* 0xA2 */
                     /* TODO: Determine expected size for writes/formats */
                     fujinet_expected_data_len = 128; /* Assume 128 for now */
                     break;
                default:
                    /* Other commands might have ACK but no data phase, or variable data */
                    FUJINET_LOG_DEBUG("Command %02X ACKed, assuming no data phase follows.", sio_command);
                    fujinet_expected_data_len = 0;
                    break;
            }
            FUJINET_LOG_DEBUG("Command ACKed (%c), expecting %d data bytes.", immediate_response, fujinet_expected_data_len);
            break;

        case SIO_NAK:             /* NAK ('N') */
        case SIO_ERROR_FRAME:     /* Error ('E') */
        case SIO_COMPLETE_FRAME:  /* Complete ('C') - Should only happen after data phase? */
            FUJINET_LOG_DEBUG("Command finished immediately with response %c.", immediate_response);
            fujinet_expected_data_len = 0; /* No data phase */
            break;

        default:
            FUJINET_LOG_WARN("Received unexpected immediate response: 0x%02X", immediate_response);
            fujinet_expected_data_len = 0; /* No data expected */
            /* Treat unexpected as NAK? Or return the byte as is? Returning it for now. */
            break;
    }

    return immediate_response; /* Return the A/C/N/E code */

}

 int FujiNet_PutByte(uint8_t byte) {
    uint8_t data[1];
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0; /* Not connected */
    }

    /* TODO: Protocol for sending data bytes needs definition. */
    /* Maybe send single bytes via DATA_TO_DEVICE? Or buffer and send block? */
    /* For now, assume sending single byte via DATA_TO_DEVICE is placeholder */
    data[0] = byte;
    Log_print("FujiNet_PutByte: Sending byte 0x%02X (Placeholder Impl)", byte);
    if (!send_altirra_message(EVENT_DATA_TO_DEVICE, 0, data, 1)) {
        Log_print("FujiNet_PutByte: Failed to send byte");
        return 0;
    }
    /* Need ACK mechanism? */
     return 1;
}

int FujiNet_GetByte(uint8_t *byte) {
     if (!fujinet_enabled) {
        return 0; /* Not enabled */
    }
    if (fujinet_data_idx < fujinet_data_len) {
        *byte = fujinet_data_buffer[fujinet_data_idx++];
        /* Log_print("FujiNet_GetByte: Returning byte %d/%d: 0x%02X", fujinet_data_idx, fujinet_data_len, *byte); */
        return 1; /* Byte available */
    } else {
         /* Log_print("FujiNet_GetByte: No more data available (idx=%d, len=%d)", fujinet_data_idx, fujinet_data_len); */
         return 0; /* No more data */
    }
}

void FujiNet_SetMotorState(int on) {
    if (!fujinet_enabled) {
        return;
    }
    
    if (on != motor_state) {
        motor_state = on;
        /* Motor state changes are currently not sent to NetSIO */
    }
}

#endif /* USE_FUJINET */
