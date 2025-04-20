#include "config.h" /* For HAVE_SOCKET etc. */

#ifdef USE_FUJINET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>  /* For va_start, va_end */

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
#include "util.h" /* For Util_strdup */

#define FUJINET_DEFAULT_HOST "127.0.0.1"
#define FUJINET_DEFAULT_PORT 9996  /* NetSIO TCP service port */
#define FUJINET_BUFFER_SIZE 1024
#define FUJINET_TIMEOUT_SEC 5 /* Timeout for socket operations */

/* Altirra Device Protocol Event IDs */
#define EVENT_CONNECTED      0x01
#define EVENT_COMMAND_ON     0x10
#define EVENT_COMMAND_OFF    0x11
#define EVENT_DATA_TO_DEVICE 0x12
#define EVENT_RESET          0xF0
#define EVENT_BOOT           0xF1

/* Altirra Device Protocol Response Events */
#define EVENT_ERROR          0x90
#define EVENT_ACK            0x91
#define EVENT_DATA_FROM_DEVICE 0x92
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

/* Global variables */
static SOCKET tcp_socket = INVALID_SOCKET;
int fujinet_enabled = 0; /* Non-static to allow external access */
static int motor_state = 0;

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
    uint32_t timestamp;
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
    
    /* Set the message length in little-endian format */
    *((uint32_t *)message) = to_little_endian(total_length);
    
    /* Set timestamp (we use 0) */
    timestamp = 0;
    *((uint32_t *)(message + 4)) = to_little_endian(timestamp);
    
    /* Set event type and device ID */
    message[8] = event;
    message[9] = arg;
    
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

/* Helper function to send data over TCP */
static int send_tcp_data(const uint8_t *data, int data_len) {
    int sent;
    
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0;
    }
    
    sent = send(tcp_socket, (const char *)data, data_len, 0);
    if (sent == SOCKET_ERROR) {
        Log_print("FujiNet: Send failed: %s", strerror(errno));
        return 0;
    }
    
    if (sent != data_len) {
        Log_print("FujiNet: Sent only %d of %d bytes", sent, data_len);
    }
    
    return 1;
}

/* Helper function to receive data over TCP */
static int receive_tcp_data(uint8_t *buffer, int buffer_size, int *received_len) {
    int received;
    
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0;
    }
    
    received = recv(tcp_socket, (char *)buffer, buffer_size, 0);
    if (received == SOCKET_ERROR) {
#ifdef HAVE_SOCKET
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            Log_print("FujiNet: Receive timeout");
        } else {
            Log_print("FujiNet: Receive failed: %s", strerror(errno));
        }
#else /* Windows */
        int wsa_error = WSAGetLastError();
        if (wsa_error == WSAETIMEDOUT) {
            Log_print("FujiNet: Receive timeout");
        } else {
            Log_print("FujiNet: Receive failed: Error %d", wsa_error);
        }
#endif
        return 0;
    }
    
    if (received == 0) {
        Log_print("FujiNet: Connection closed by server");
        return 0;
    }
    
    if (received_len != NULL) {
        *received_len = received;
    }
    
    return 1;
}

int FujiNet_Initialise(const char *host_port) {
    char host[256] = FUJINET_DEFAULT_HOST;
    int port = FUJINET_DEFAULT_PORT;
    struct sockaddr_in server;
    struct hostent *he;
    struct timeval tv;
    uint8_t event, arg;
    int ack_len = 0;
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
    
    /* Send Altirra CONNECTED event */
    if (!send_altirra_message(EVENT_CONNECTED, 0, NULL, 0)) {
        Log_print("FujiNet: Failed to send CONNECTED message");
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
    
    /* Wait for ACK response */
    if (!receive_altirra_message(EVENT_ACK, &event, &arg, NULL, 0, &ack_len)) {
        Log_print("FujiNet: Did not receive ACK for connection");
        closesocket(tcp_socket);
        tcp_socket = INVALID_SOCKET;
#ifndef HAVE_SOCKET
        WSACleanup();
#endif
        return 0;
    }
     
    fujinet_enabled = 1;
    
    Log_print("FujiNet: Device initialized successfully");
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

int FujiNet_ProcessCommand(const unsigned char *command_frame, unsigned char *response_frame) {
    uint8_t sio_device_id;
    uint8_t sio_command;
    uint8_t sio_aux1;
    uint8_t sio_aux2;
    uint8_t sio_checksum;
    uint8_t event, arg;
    uint8_t sio_data[5]; /* Command, Aux1, Aux2, Checksum */
    uint8_t response_buffer[FUJINET_BUFFER_SIZE];
    int received_len;
    int result;
    
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0; /* Not initialized */
    }
    
    /* Check for FujiNet device ID (0x70) */
    sio_device_id = command_frame[0];
    if (sio_device_id != 0x70) {
        return 0; /* Not for us */
    }
    
    /* Extract command components */
    sio_command = command_frame[1];
    sio_aux1 = command_frame[2];
    sio_aux2 = command_frame[3];
    sio_checksum = command_frame[4];
    
    Log_print("FujiNet: Processing command: %02X %02X %02X %02X %02X",
             sio_device_id, sio_command, sio_aux1, sio_aux2, sio_checksum);
    
    /* Step 1: Send COMMAND_ON with device ID as arg */
    if (!send_altirra_message(EVENT_COMMAND_ON, sio_device_id, NULL, 0)) {
        Log_print("FujiNet: Failed to send COMMAND_ON");
        return 0;
    }
    
    /* Step 2: Wait for ACK */
    if (!receive_altirra_message(EVENT_ACK, &event, &arg, NULL, 0, &received_len)) {
        Log_print("FujiNet: No ACK for COMMAND_ON");
        return 0;
    }
    
    /* Step 3: Pack the command data and send DATA_TO_DEVICE */
    sio_data[0] = sio_command; /* Command */
    sio_data[1] = sio_aux1;    /* AUX1 */
    sio_data[2] = sio_aux2;    /* AUX2 */
    sio_data[3] = sio_checksum; /* Checksum */
    
    if (!send_altirra_message(EVENT_DATA_TO_DEVICE, 0, sio_data, 4)) {
        Log_print("FujiNet: Failed to send command data");
        return 0;
    }
    
    /* Step 4: Wait for ACK */
    if (!receive_altirra_message(EVENT_ACK, &event, &arg, NULL, 0, &received_len)) {
        Log_print("FujiNet: No ACK for command data");
        return 0;
    }
    
    /* Step 5: Send COMMAND_OFF */
    if (!send_altirra_message(EVENT_COMMAND_OFF, 0, NULL, 0)) {
        Log_print("FujiNet: Failed to send COMMAND_OFF");
        return 0;
    }
    
    /* Step 6: Wait for DATA_FROM_DEVICE (the SIO response) */
    result = receive_altirra_message(EVENT_DATA_FROM_DEVICE, &event, &arg, response_buffer, sizeof(response_buffer), &received_len);
    if (!result) {
        Log_print("FujiNet: No response data received");
        return 0;
    }
    
    /* Verify the response is the expected size */
    if (received_len != 4) {
        Log_print("FujiNet: Received wrong data size (%d bytes, expected 4)", received_len);
        return 0;
    }
    
    /* Copy the response data */
    memcpy(response_frame, response_buffer, 4);
    
    return 1;
}

int FujiNet_PutByte(uint8_t byte) {
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0;
    }
    
    /* Format Altirra message with the byte */
    return send_altirra_message(EVENT_DATA_TO_DEVICE, 0, &byte, 1);
}

int FujiNet_GetByte(uint8_t *byte) {
    uint8_t data[FUJINET_BUFFER_SIZE];
    uint8_t event, arg;
    int received_len;
    
    if (!fujinet_enabled || tcp_socket == INVALID_SOCKET) {
        return 0;
    }
    
    /* Wait for DATA_FROM_DEVICE */
    if (!receive_altirra_message(EVENT_DATA_FROM_DEVICE, &event, &arg, data, sizeof(data), &received_len)) {
        return 0;
    }
    
    if (received_len != 1) {
        Log_print("FujiNet: Received wrong data size (%d bytes, expected 1)", received_len);
        return 0;
    }
    
    *byte = data[0];
    return 1;
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
