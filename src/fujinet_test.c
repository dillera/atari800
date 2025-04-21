#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Socket headers */
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

#include "fujinet.h"

/* NetSIO Protocol Message IDs */
#define NETSIO_DATA_BYTE                 0x01
#define NETSIO_DATA_BLOCK                0x02
#define NETSIO_DATA_BYTE_SYNC_REQUEST    0x09
#define NETSIO_COMMAND_ON                0x11
#define NETSIO_COMMAND_OFF               0x10
#define NETSIO_COMMAND_OFF_SYNC_REQUEST  0x18
#define NETSIO_MOTOR_ON                  0x21
#define NETSIO_MOTOR_OFF                 0x20
#define NETSIO_PROCEED_ON                0x31
#define NETSIO_PROCEED_OFF               0x30
#define NETSIO_INTERRUPT_ON              0x41
#define NETSIO_INTERRUPT_OFF             0x40
#define NETSIO_SPEED_CHANGE              0x80
#define NETSIO_SYNC_RESPONSE             0x81
#define NETSIO_DEVICE_CONNECTED          0xC1
#define NETSIO_DEVICE_DISCONNECTED       0xC0
#define NETSIO_PING_REQUEST              0xC2
#define NETSIO_PING_RESPONSE             0xC3
#define NETSIO_ALIVE_REQUEST             0xC4
#define NETSIO_ALIVE_RESPONSE            0xC5
#define NETSIO_CREDIT_STATUS             0xC6
#define NETSIO_CREDIT_UPDATE             0xC7
#define NETSIO_WARM_RESET                0xFE
#define NETSIO_COLD_RESET                0xFF

/* NetSIO Protocol Constants */
#define NETSIO_DEFAULT_PORT              9996
#define NETSIO_DEFAULT_HOST              "127.0.0.1"
#define NETSIO_TIMEOUT_SEC               5
#define NETSIO_MAX_PACKET_SIZE           512
#define NETSIO_ACK                       'A'
#define NETSIO_NAK                       'N'
#define NETSIO_MAX_RETRIES               3

/* Global socket for NetSIO communication */
static int netsio_socket = -1;
static int verbose_logging = 0;
static int timeout_sec = NETSIO_TIMEOUT_SEC;
static uint8_t sync_request_number = 0;

/* Stub implementations for Atari800 functions */
void Log_print(const char *format, ...) {
    va_list args;
    va_start(args, format);
    printf("LOG: ");
    vprintf(format, args);
    printf("\n");
    va_end(args);
}

char *Util_strdup(const char *s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s) + 1;
    char *new_str = malloc(len);
    if (new_str == NULL) return NULL;
    return memcpy(new_str, s, len);
}

/* Print debug message if verbose logging is enabled */
void debug_print(const char *format, ...) {
    if (verbose_logging) {
        va_list args;
        va_start(args, format);
        printf("DEBUG: ");
        vprintf(format, args);
        printf("\n");
        va_end(args);
    }
}

/* Print hex dump of buffer */
void hex_dump(const char *prefix, const uint8_t *data, int len) {
    if (!verbose_logging) return;
    
    printf("%s (%d bytes): ", prefix, len);
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
        if (i > 0 && (i + 1) % 16 == 0 && i < len - 1) {
            printf("\n                  ");
        }
    }
    printf("\n");
}

/* Initialize NetSIO connection using TCP */
int netsio_init(const char *host, int port) {
    printf("Initializing NetSIO TCP connection to %s:%d...\n", host, port);
    
    /* Create TCP socket */
    netsio_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (netsio_socket < 0) {
        printf("Error creating socket: %s\n", strerror(errno));
        return 0;
    }
    
    /* Set socket timeout */
    struct timeval tv;
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(netsio_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        printf("Warning: Could not set socket receive timeout: %s\n", strerror(errno));
    }
    
    if (setsockopt(netsio_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        printf("Warning: Could not set socket send timeout: %s\n", strerror(errno));
    }
    
    /* Set up server address */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    /* Try to resolve hostname */
    struct hostent *he = gethostbyname(host);
    if (he == NULL) {
        /* Try direct IP address parsing */
        if (inet_addr(host) == INADDR_NONE) {
            printf("Error resolving hostname: %s\n", host);
            close(netsio_socket);
            netsio_socket = -1;
            return 0;
        }
        server_addr.sin_addr.s_addr = inet_addr(host);
    } else {
        memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    /* Connect to the server */
    printf("Connecting to NetSIO server...\n");
    if (connect(netsio_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("Error connecting to server: %s\n", strerror(errno));
        close(netsio_socket);
        netsio_socket = -1;
        return 0;
    }
    
    printf("NetSIO TCP socket connected successfully\n");
    return 1;
}

/* Close NetSIO connection */
void netsio_close() {
    if (netsio_socket >= 0) {
        close(netsio_socket);
        netsio_socket = -1;
        printf("NetSIO socket closed\n");
    }
}

/* Send a NetSIO message over TCP */
int netsio_send_message(uint8_t message_id, const uint8_t *data, int data_len) {
    if (netsio_socket < 0) {
        printf("Error: NetSIO socket not initialized\n");
        return 0;
    }
    
    /* Prepare message buffer */
    uint8_t buffer[NETSIO_MAX_PACKET_SIZE + 1]; /* +1 for message ID */
    buffer[0] = message_id;
    
    if (data != NULL && data_len > 0) {
        if (data_len > NETSIO_MAX_PACKET_SIZE) {
            printf("Error: Data length exceeds maximum packet size\n");
            return 0;
        }
        memcpy(buffer + 1, data, data_len);
    }
    
    /* Send message */
    int total_len = data_len + 1; /* +1 for message ID */
    
    hex_dump("Sending NetSIO packet", buffer, total_len);
    
    int sent = send(netsio_socket, buffer, total_len, 0);
    
    if (sent < 0) {
        printf("Error sending NetSIO message: %s\n", strerror(errno));
        return 0;
    }
    
    if (sent != total_len) {
        printf("Warning: Sent only %d of %d bytes\n", sent, total_len);
    }
    
    printf("Sent NetSIO message ID 0x%02X (%d bytes)\n", message_id, sent);
    return 1;
}

/* Receive a NetSIO message over TCP */
int netsio_receive_message(uint8_t *message_id, uint8_t *data, int *data_len) {
    if (netsio_socket < 0) {
        printf("Error: NetSIO socket not initialized\n");
        return 0;
    }
    
    /* Receive message ID first */
    uint8_t id_byte;
    int received = recv(netsio_socket, &id_byte, 1, 0);
    
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("Timeout waiting for NetSIO response\n");
        } else {
            printf("Error receiving NetSIO message: %s\n", strerror(errno));
        }
        return 0;
    }
    
    if (received == 0) {
        printf("Connection closed by server\n");
        return 0;
    }
    
    *message_id = id_byte;
    
    /* Now try to receive data if any */
    int max_data_len = *data_len;
    *data_len = 0;
    
    /* For TCP, we need to determine how much data to read based on the message type */
    int expected_data_len = 0;
    
    switch (id_byte) {
        case NETSIO_DATA_BYTE:
        case NETSIO_DATA_BYTE_SYNC_REQUEST:
        case NETSIO_COMMAND_OFF_SYNC_REQUEST:
        case NETSIO_CREDIT_STATUS:
        case NETSIO_CREDIT_UPDATE:
            expected_data_len = 1;
            break;
        case NETSIO_SYNC_RESPONSE:
            expected_data_len = 5; /* sync_number, ack_type, ack_byte, write_size (2 bytes) */
            break;
        case NETSIO_SPEED_CHANGE:
            expected_data_len = 4; /* 4 bytes for baud rate */
            break;
        case NETSIO_DATA_BLOCK:
            /* For data block, we need to read as much as available */
            expected_data_len = max_data_len;
            break;
        default:
            /* Most messages have no data */
            expected_data_len = 0;
            break;
    }
    
    if (expected_data_len > 0) {
        int to_read = expected_data_len < max_data_len ? expected_data_len : max_data_len;
        received = recv(netsio_socket, data, to_read, 0);
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                printf("Timeout waiting for message data\n");
            } else {
                printf("Error receiving message data: %s\n", strerror(errno));
            }
            return 0;
        }
        
        if (received == 0) {
            printf("Connection closed by server while reading data\n");
            return 0;
        }
        
        *data_len = received;
    }
    
    hex_dump("Received NetSIO packet", &id_byte, 1 + *data_len);
    
    printf("Received NetSIO message ID 0x%02X (%d bytes of data)\n", *message_id, *data_len);
    return 1;
}

/* Send Device Connected message */
int netsio_device_connected() {
    printf("Sending Device Connected message...\n");
    return netsio_send_message(NETSIO_DEVICE_CONNECTED, NULL, 0);
}

/* Send Device Disconnected message */
int netsio_device_disconnected() {
    printf("Sending Device Disconnected message...\n");
    return netsio_send_message(NETSIO_DEVICE_DISCONNECTED, NULL, 0);
}

/* Send Ping Request message */
int netsio_ping_request() {
    printf("Sending Ping Request message...\n");
    return netsio_send_message(NETSIO_PING_REQUEST, NULL, 0);
}

/* Send a SIO command using NetSIO protocol */
int netsio_send_sio_command(const uint8_t *command_frame, int command_len) {
    if (command_len < 5) {
        printf("Error: SIO command frame must be at least 5 bytes\n");
        return 0;
    }
    
    printf("Sending SIO command using NetSIO protocol...\n");
    printf("Command frame: ");
    for (int i = 0; i < command_len; i++) {
        printf("%02X ", command_frame[i]);
    }
    printf("\n");
    
    /* Step 1: Send Command ON */
    if (!netsio_send_message(NETSIO_COMMAND_ON, NULL, 0)) {
        printf("Failed to send Command ON\n");
        return 0;
    }
    
    /* Step 2: Send Data Block with command frame */
    if (!netsio_send_message(NETSIO_DATA_BLOCK, command_frame, command_len)) {
        printf("Failed to send Data Block\n");
        return 0;
    }
    
    /* Step 3: Send Command OFF with Sync request */
    uint8_t sync_number = ++sync_request_number;
    if (!netsio_send_message(NETSIO_COMMAND_OFF_SYNC_REQUEST, &sync_number, 1)) {
        printf("Failed to send Command OFF with Sync request\n");
        return 0;
    }
    
    /* Step 4: Wait for Sync response */
    printf("Waiting for Sync response...\n");
    uint8_t message_id;
    uint8_t response_data[16];
    int response_len = sizeof(response_data);
    
    if (!netsio_receive_message(&message_id, response_data, &response_len)) {
        printf("Failed to receive Sync response\n");
        return 0;
    }
    
    if (message_id != NETSIO_SYNC_RESPONSE) {
        printf("Unexpected response message ID: 0x%02X (expected 0x%02X)\n", 
               message_id, NETSIO_SYNC_RESPONSE);
        return 0;
    }
    
    if (response_len < 4) {
        printf("Sync response too short (%d bytes)\n", response_len);
        return 0;
    }
    
    uint8_t recv_sync_number = response_data[0];
    uint8_t ack_type = response_data[1];
    uint8_t ack_byte = response_data[2];
    uint16_t write_size = response_data[3] | (response_data[4] << 8);
    
    printf("Sync response received: sync_number=%d, ack_type=%d, ack_byte=0x%02X, write_size=%d\n",
           recv_sync_number, ack_type, ack_byte, write_size);
    
    if (recv_sync_number != sync_number) {
        printf("Warning: Sync number mismatch (sent %d, received %d)\n", 
               sync_number, recv_sync_number);
    }
    
    if (ack_type == 0) {
        printf("Device not interested in this command (empty acknowledgment)\n");
        return 0;
    }
    
    printf("Command acknowledged with byte: 0x%02X ('%c')\n", ack_byte, 
           (ack_byte >= 32 && ack_byte <= 126) ? ack_byte : '?');
    
    return 1;
}

/* Print usage information */
void print_usage(const char *program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h <host>    NetSIO host (default: %s)\n", NETSIO_DEFAULT_HOST);
    printf("  -p <port>    NetSIO port (default: %d)\n", NETSIO_DEFAULT_PORT);
    printf("  -t <seconds> Timeout in seconds (default: %d)\n", NETSIO_TIMEOUT_SEC);
    printf("  -v           Enable verbose logging\n");
    printf("  -?           Show this help\n");
}

int main(int argc, char *argv[]) {
    const char *host = NETSIO_DEFAULT_HOST;
    int port = NETSIO_DEFAULT_PORT;
    int opt;
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "h:p:t:v?")) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    printf("Invalid port number: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 't':
                timeout_sec = atoi(optarg);
                if (timeout_sec <= 0) {
                    printf("Invalid timeout: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'v':
                verbose_logging = 1;
                break;
            case '?':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    printf("Starting NetSIO Test Program (TCP mode)...\n");
    printf("Host: %s, Port: %d, Timeout: %d seconds, Verbose: %s\n",
           host, port, timeout_sec, verbose_logging ? "yes" : "no");
    
    /* Initialize NetSIO connection */
    if (!netsio_init(host, port)) {
        printf("Failed to initialize NetSIO connection\n");
        return 1;
    }
    
    /* Connect to NetSIO hub */
    if (!netsio_device_connected()) {
        printf("Failed to connect to NetSIO hub\n");
        netsio_close();
        return 1;
    }
    
    /* Send a ping request */
    printf("\n=== Testing Ping Request ===\n");
    int ping_success = 0;
    
    for (int retry = 0; retry < NETSIO_MAX_RETRIES && !ping_success; retry++) {
        if (retry > 0) {
            printf("Retrying ping (attempt %d of %d)...\n", retry + 1, NETSIO_MAX_RETRIES);
        }
        
        if (netsio_ping_request()) {
            /* Wait for ping response */
            uint8_t message_id;
            uint8_t response_data[16];
            int response_len = sizeof(response_data);
            
            if (netsio_receive_message(&message_id, response_data, &response_len)) {
                if (message_id == NETSIO_PING_RESPONSE) {
                    printf("Ping successful! NetSIO hub is responding.\n");
                    ping_success = 1;
                } else {
                    printf("Received unexpected response to ping (ID: 0x%02X)\n", message_id);
                }
            } else {
                printf("No response to ping request\n");
            }
        }
        
        if (!ping_success && retry < NETSIO_MAX_RETRIES - 1) {
            printf("Waiting before retry...\n");
            sleep(1); /* Wait 1 second before retrying */
        }
    }
    
    if (!ping_success) {
        printf("Warning: Ping test failed after %d attempts\n", NETSIO_MAX_RETRIES);
        printf("Continuing with SIO command tests anyway...\n");
    }
    
    /* Test SIO Reset Command */
    printf("\n=== Testing SIO Reset Command ===\n");
    uint8_t reset_command[5] = {0x70, 0xFF, 0x00, 0x00, 0x6F}; // SIO Reset command
    for (int retry = 0; retry < NETSIO_MAX_RETRIES; retry++) {
        if (retry > 0) {
            printf("Retrying reset command (attempt %d of %d)...\n", retry + 1, NETSIO_MAX_RETRIES);
        }
        
        if (netsio_send_sio_command(reset_command, sizeof(reset_command))) {
            printf("Reset command successful!\n");
            break;
        }
        
        if (retry < NETSIO_MAX_RETRIES - 1) {
            printf("Waiting before retry...\n");
            sleep(1); /* Wait 1 second before retrying */
        }
    }
    
    /* Test SIO Read Command */
    printf("\n=== Testing SIO Read Command ===\n");
    uint8_t read_command[5] = {0x31, 0x52, 0x01, 0x00, 0x84}; // SIO Read command
    for (int retry = 0; retry < NETSIO_MAX_RETRIES; retry++) {
        if (retry > 0) {
            printf("Retrying read command (attempt %d of %d)...\n", retry + 1, NETSIO_MAX_RETRIES);
        }
        
        if (netsio_send_sio_command(read_command, sizeof(read_command))) {
            printf("Read command successful!\n");
            break;
        }
        
        if (retry < NETSIO_MAX_RETRIES - 1) {
            printf("Waiting before retry...\n");
            sleep(1); /* Wait 1 second before retrying */
        }
    }
    
    /* Send Device Disconnected message before closing */
    netsio_device_disconnected();
    
    /* Close NetSIO connection */
    netsio_close();
    
    printf("Test complete.\n");
    return 0;
}
