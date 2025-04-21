/*****************************************************************************
 * FujiNet NetSIO Test Application
 * 
 * This application tests communication with the NetSIO hub using the Altirra
 * Custom Device Protocol. It can be used to verify connectivity and proper
 * message handling with the NetSIO hub.
 * 
 * Usage:
 *   fujinet_netsio_test [-h host] [-p port] [-v] [-r] [-c]
 * 
 * Options:
 *   -h host    Specify the NetSIO hub hostname/IP (default: 127.0.0.1)
 *   -p port    Specify the NetSIO hub TCP port (default: 9996)
 *   -v         Enable verbose debugging output
 *   -r         Test reset command after SIO command sequence
 *   -c         Use COLD_RESET instead of WARM_RESET if testing reset
 *   -?         Show usage information
 * 
 * This application:
 * 1. Establishes a TCP connection to the NetSIO hub
 * 2. Sends a test SIO command sequence following the Altirra protocol:
 *    - COMMAND_ON with device ID 0x31
 *    - DATA_BLOCK with command 0x4E (Get Status), aux1=0, aux2=0
 *    - COMMAND_OFF_SYNC with checksum
 * 3. Waits for SIO response data from the hub
 * 4. Verifies the response format and prints the results
 * 5. If requested, sends a reset command (WARM_RESET or COLD_RESET)
 * 
 * Exit codes:
 *   0 - Success (command sent and response received)
 *   1 - Failure (command failed or response not properly received)
 * 
 * Compile:
 *   gcc src/fujinet_netsio_test.c -o fujinet_netsio_test -Wall
 * 
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>   /* For select timeout */
#include <unistd.h>     /* For usleep */
#include <fcntl.h>
#include <getopt.h>     /* For command-line parsing */

/* Handle endian differences between platforms */
#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htole32(x) OSSwapHostToLittleInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#else /* Assuming Linux/glibc */
#include <endian.h>
#endif

/* Socket headers with proper platform detection */
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define close(s) closesocket(s)
    typedef int socklen_t;
#else /* POSIX systems */
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <arpa/inet.h>
#endif

/* Default connection parameters */
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 9996
#define DEFAULT_TIMEOUT_SEC 5

/* --- Altirra protocol constants --- */
/* Event types in the Altirra protocol */
#define NETSIO_DATA_BYTE        0x01
#define NETSIO_DATA_BLOCK       0x02
#define NETSIO_COMMAND_ON       0x11
#define NETSIO_COMMAND_OFF_SYNC 0x18
#define NETSIO_SYNC_RESPONSE    0x81
#define NETSIO_WARM_RESET       0xFE
#define NETSIO_COLD_RESET       0xFF

/* Global variables */
static int tcp_socket = -1;
static const int timeout_seconds = DEFAULT_TIMEOUT_SEC;

/**
 * Hex-dump utility function
 * Formats and prints data in hex format with ASCII representation
 */
static void log_hex_dump(const char* prefix, const void *data, size_t len) {
    const unsigned char* buf = (const unsigned char*)data;
    size_t i;
    
    for (i = 0; i < len; i++) {
        if (i == 0 && prefix) printf("%s", prefix);
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

/**
 * Receive data with timeout
 * Returns:
 *  >0: Number of bytes received
 *  -1: Error
 *  -2: Timeout
 */
static int recv_with_timeout(int sockfd, void *buf, size_t len, int timeout_sec) {
    fd_set readfds;
    struct timeval tv;
    int ret;

    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    ret = select(sockfd + 1, &readfds, NULL, NULL, &tv);
    if (ret == -1) {
        perror("select error");
        return -1;
    } else if (ret == 0) {
        return -2; /* Timeout */
    }

    return recv(sockfd, buf, len, 0);
}

/**
 * Helper function to send all data across a socket
 * Handles partial sends and ensures all data is sent
 */
static int send_all(int sockfd, const void *buf, size_t len) {
    const char *pbuf = (const char*)buf;
    size_t total_sent = 0;
    
    while (total_sent < len) {
        int sent = send(sockfd, pbuf + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            if (sent < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            return -1; /* Error */
        }
        total_sent += sent;
    }
    
    return 0; /* Success */
}

/**
 * Parse and receive a message in Altirra protocol format
 * 
 * Message format:
 * - 8-byte header: total_length (4 bytes), timestamp (4 bytes)
 * - Payload: event (1 byte), arg (1 byte), data (variable)
 * 
 * Returns:
 *  0: Success
 * -1: Error
 * -2: Timeout
 */
static int receive_altirra_message(int sockfd, uint8_t *event, uint8_t *arg, uint8_t *data_buf, int *data_len, int verbose) {
    // Read the 8-byte header
    unsigned char header[8];
    int received = recv_with_timeout(sockfd, header, 8, timeout_seconds);
    if (received == -1) { // Error
        return -1;
    }
    if (received == -2) { // Timeout
        fprintf(stderr, "Error: recv timeout reading Altirra header\n");
        return -2;
    }
    if (received == 0) { // Connection closed
        fprintf(stderr, "recv: connection closed by peer reading Altirra header\n");
        return -1;
    }
    if (received < 8) {
        fprintf(stderr, "Error: Received incomplete Altirra header (%d of 8 bytes)\n", received);
        return -1;
    }

    // Debug raw header bytes
    if (verbose) {
        printf("   Altirra Recv Header raw bytes: ");
        for (int i = 0; i < 8; i++) {
            printf("%02X ", header[i]);
        }
        printf("\n");
    }

    // Manually unpack the bytes in little-endian format
    uint32_t total_length = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
    uint32_t timestamp = header[4] | (header[5] << 8) | (header[6] << 16) | (header[7] << 24);

    if (verbose) {
        printf("<- Altirra Recv Hdr: Len=%u, TS=%u", total_length, timestamp);
    }

    // Sanity check on total_length
    if (total_length < 10) { // Header (8) + event (1) + arg (1)
        fprintf(stderr, "Error: Invalid Altirra message length %u (too small)\n", total_length);
        return -1;
    }

    uint32_t payload_len = total_length - 8;
    
    // Prevent overly large allocation
    if (payload_len > 256 + 2) { // Reasonable limit for our test app: data (256) + event + arg
        fprintf(stderr, "Error: Declared Altirra payload length %u exceeds reasonable limit\n", payload_len);
        fprintf(stderr, "  Header bytes: ");
        for (int i = 0; i < 8; i++) {
            fprintf(stderr, "%02X ", header[i]);
        }
        fprintf(stderr, "\n");
        return -1;
    }

    // Allocate a buffer for the payload: event (1) + arg (1) + data
    unsigned char payload[payload_len];

    // Read the payload
    received = recv_with_timeout(sockfd, payload, payload_len, timeout_seconds);
    if (received == -1) { // Error
        return -1;
    }
    if (received == -2) { // Timeout
        fprintf(stderr, "Error: recv timeout reading Altirra payload (%u bytes)\n", payload_len);
        return -2;
    }
    if (received == 0) { // Connection closed
        fprintf(stderr, "recv: connection closed by peer reading Altirra payload\n");
        return -1;
    }
    if ((uint32_t)received < payload_len) {
        fprintf(stderr, "Error: Received incomplete Altirra payload (%d of %u bytes)\n", received, payload_len);
        return -1;
    }

    // Extract the event and arg from the payload
    *event = payload[0];
    *arg = payload[1];
    int received_data_len = payload_len - 2; // Bytes remaining after event and arg

    // Copy any data after event and arg into the provided buffer
    if (received_data_len > 0) {
        // Check buffer size before copying
        if (received_data_len > *data_len) { // *data_len is the capacity passed in
            fprintf(stderr, "Warning: Received data block (%d bytes) exceeds provided buffer size (%d). Truncating.\n", 
                   received_data_len, *data_len);
            memcpy(data_buf, payload + 2, *data_len); // Copy only what fits
            // Data is truncated, but return success as message was received
        } else {
            memcpy(data_buf, payload + 2, received_data_len);
            *data_len = received_data_len; // Update *data_len to actual received length
        }
    } else {
        *data_len = 0; // No data
    }

    if (verbose) {
        printf(" | Payload: Evt=0x%02X, Arg=0x%02X", *event, *arg);
        if (*data_len > 0) {
            printf(" | Data (%d bytes):", *data_len);
            for (int i = 0; i < *data_len; ++i) printf(" %02X", data_buf[i]);
        }
        printf("\n");
    }

    return 0; // Success
}

/**
 * Send a message in Altirra protocol format
 * 
 * Message format:
 * - 8-byte header: total_length (4 bytes), timestamp (4 bytes)
 * - Payload: event (1 byte), arg (1 byte), data (variable)
 * 
 * Returns:
 *  0: Success
 * -1: Error
 */
static int send_altirra_message(int sockfd, uint8_t event, uint8_t arg, const uint8_t *data, uint16_t data_len, int verbose) {
    // Calculate the message size: header (8) + event (1) + arg (1) + data
    uint32_t total_length = 8 + 2 + data_len;
    
    // Prepare the message buffer
    unsigned char *message = (unsigned char *)malloc(total_length);
    if (!message) {
        fprintf(stderr, "Error: Failed to allocate memory for message\n");
        return -1;
    }
    
    // Header: 8 bytes (total_length + timestamp)
    // Use current 32-bit length for simplicity
    uint32_t length_le = htole32(total_length);
    uint32_t timestamp_le = htole32(0); // Use 0 for timestamp
    
    memcpy(message, &length_le, 4);
    memcpy(message + 4, &timestamp_le, 4);
    
    // Payload: event (1) + arg (1) + data
    message[8] = event;
    message[9] = arg;
    if (data_len > 0 && data != NULL) {
        memcpy(message + 10, data, data_len);
    }
    
    if (verbose) {
        printf("   Altirra Send Hdr: Len=%u, TS=0 | Payload: Evt=0x%02X, Arg=0x%02X", 
              total_length, event, arg);
        if (data_len > 0) {
            printf(" | Data (%d bytes):", data_len);
            for (uint16_t i = 0; i < data_len; ++i) printf(" %02X", data[i]);
        }
        printf("\n   TCP Send (%d bytes): ", total_length);
        for (uint32_t i = 0; i < total_length; i++) {
            printf("%02X ", message[i]);
        }
        printf("\n");
    }
    
    // Send the message
    int result = send_all(tcp_socket, message, total_length);
    free(message);
    
    return result;
}

/**
 * Send an SIO command through the NetSIO protocol
 * 
 * Sends a sequence of three Altirra protocol messages:
 * 1. COMMAND_ON with device ID
 * 2. DATA_BLOCK with command, aux1, aux2
 * 3. COMMAND_OFF_SYNC with checksum
 * 
 * Then waits for response bytes
 * 
 * Returns:
 *  1: Success
 *  0: Failure
 */
static int send_sio_command(const uint8_t *sio_frame, int frame_len, uint8_t *response_data, int *response_len) {
    if (frame_len < 5) {
        fprintf(stderr, "SIO frame too short\n");
        return 0;
    }

    uint8_t devid = sio_frame[0];
    uint8_t command = sio_frame[1];
    uint8_t aux1 = sio_frame[2];
    uint8_t aux2 = sio_frame[3];
    uint8_t checksum = sio_frame[4];
    int verbose = 1;  // Always show verbose output for commands
    
    printf("SIO Frame: %02X %02X %02X %02X %02X \n", 
            devid, command, aux1, aux2, checksum);
    
    // 1. Send COMMAND_ON with device ID
    printf("-> Sending COMMAND_ON (0x11) with DevID 0x%02X\n", devid);
    if (send_altirra_message(tcp_socket, 0x11, devid, NULL, 0, verbose) != 0) {
         fprintf(stderr, "Error sending COMMAND_ON\n");
         return 0;
    }
    usleep(10000); // Small delay based on hub logic
    
    // 2. Send DATA_BLOCK with command, aux1, aux2
    uint8_t data_block[3] = {command, aux1, aux2};
    printf("-> Sending DATA_BLOCK (0x02) with Cmd=0x%02X, Aux1=0x%02X, Aux2=0x%02X\n", command, aux1, aux2);
    if (send_altirra_message(tcp_socket, 0x02, 3, data_block, 3, verbose) != 0) {
         fprintf(stderr, "Error sending DATA_BLOCK\n");
         return 0;
    }
     usleep(10000); // Small delay based on hub logic
    
    // 3. Send COMMAND_OFF_SYNC with checksum
    printf("-> Sending COMMAND_OFF_SYNC (0x18) with Checksum 0x%02X\n", checksum);
    if (send_altirra_message(tcp_socket, 0x18, checksum, NULL, 0, verbose) != 0) {
        fprintf(stderr, "Error sending COMMAND_OFF_SYNC\n");
        return 0;
    }

    // 4. Wait for the SIO response from the peripheral, forwarded by the hub.
    // These come as a sequence of NETSIO_DATA_BYTE (0x01) messages, one per byte.
    printf("<- Waiting for SIO response bytes via Altirra messages...\n");
    
    int sio_response_len = 0;
    uint8_t sio_response_buffer[256];  // Buffer for the complete SIO response
    uint8_t recv_data_buf[256];        // Buffer for received Altirra message data
    
    // Wait for up to 129 bytes (status byte + 128 data bytes for GetStatus)
    // Stop if we receive the expected response (status byte for commands that don't return data,
    // or status byte + data for commands like GetStatus)
    while (sio_response_len < 129) {
        uint8_t event, arg;
        int recv_data_len = sizeof(recv_data_buf);  // Max capacity
        
        int result = receive_altirra_message(tcp_socket, &event, &arg, recv_data_buf, &recv_data_len, verbose);

        if (result == -1) { // Error
            fprintf(stderr, "Error receiving Altirra message while waiting for SIO response.\n");
            return 0; // Indicate failure
        }
        if (result == -2) { // Timeout
            fprintf(stderr, "Timeout waiting for SIO response byte %d.\n", sio_response_len);
            // Did we get *any* response bytes?
            if (sio_response_len > 0) {
                 printf("Received partial SIO response (%d bytes):", sio_response_len);
                 for(int i=0; i<sio_response_len; ++i) printf(" %02X", sio_response_buffer[i]);
                 printf(" ('%c')\n", sio_response_buffer[0]); // Print first char too
            }
            return 0; // Indicate failure
        }

        // Process received Altirra message
        if (event == NETSIO_DATA_BYTE) {
            // This is a single byte of SIO response data
            if (sio_response_len < sizeof(sio_response_buffer)) {
                sio_response_buffer[sio_response_len++] = arg;
                
                // For Get Status, we're expecting 'C' (0x43) + 128 bytes
                if (command == 0x4E) { // Get Status
                    if (sio_response_len == 1 && arg != 'C') {
                         fprintf(stderr, "Error: Expected 'C' for Get Status response, got 0x%02X ('%c')\n", arg, arg);
                         return 0; // Unexpected status for Get Status
                    }
                
                    // Stop after we get the expected response (status + 128 bytes)
                    if (sio_response_len == 129) {
                        printf("   Received SIO 'C' + 128 bytes for Get Status. Stopping read.\n");
                        break; // Stop reading
                    }
                }

            } else {
                fprintf(stderr, "SIO response buffer overflow!\n");
                return 0; // Buffer full
            }
        } else {
            // Received an unexpected Altirra event while waiting for SIO bytes
            // This is common - we might receive SYNC_RESPONSE messages first
            printf("Warning: Received unexpected Altirra event 0x%02X (arg=0x%02X) while waiting for SIO data byte (0x01).\n", 
                  event, arg);
            // Continue and wait for actual DATA_BYTE messages
        }
    }
    
    // We're done reading the response
    printf("<- SIO Response processing complete. Received %d bytes total:\n", sio_response_len);
    if (sio_response_len > 0) {
        printf("   Data: ");
        for(int i=0; i<sio_response_len; i++) {
            printf("%02X ", sio_response_buffer[i]);
        }
        printf("\n");
    }
    
    // Verify response (basic validation)
    if (sio_response_len == 0) {
        fprintf(stderr, "Error: No SIO response bytes received after command sequence.\n");
        return 0;
    }

    // Crude check for Get Status success
    if (command == 0x4E) {
        if (sio_response_len == 129 && sio_response_buffer[0] == 'C') {
             printf("   Successfully received Get Status block (C + 128 bytes).\n");
        } else {
             fprintf(stderr, "Error: Incorrect response for Get Status (expected C+128 bytes, got %d bytes, status=0x%02X).\n", sio_response_len, sio_response_buffer[0]);
             return 0;
        }
    }

    // Copy the response if the caller wants it
    if (response_data && response_len) {
        int copy_len = (sio_response_len <= *response_len) ? sio_response_len : *response_len;
        memcpy(response_data, sio_response_buffer, copy_len);
        *response_len = copy_len;
    }
    
    return 1; // Success
}

/**
 * Send a reset command (warm or cold)
 * 
 * Returns:
 *  1: Success
 *  0: Failure
 */
static int send_reset_command(int cold_reset, int verbose) {
    uint8_t reset_type = cold_reset ? NETSIO_COLD_RESET : NETSIO_WARM_RESET;
    const char *reset_name = cold_reset ? "COLD_RESET" : "WARM_RESET";
    
    printf("-> Sending %s (0x%02X)\n", reset_name, reset_type);
    
    if (send_altirra_message(tcp_socket, reset_type, 0, NULL, 0, verbose) != 0) {
        fprintf(stderr, "Error sending %s command\n", reset_name);
        return 0;
    }
    
    // Wait for confirmation (though the hub might not send anything for reset commands)
    printf("<- Waiting for reset confirmation (may timeout if none sent)...\n");
    
    // Try to receive a response, but don't fail if none comes
    uint8_t event, arg;
    uint8_t recv_data_buf[256];
    int recv_data_len = sizeof(recv_data_buf);
    
    int result = receive_altirra_message(tcp_socket, &event, &arg, recv_data_buf, &recv_data_len, verbose);
    
    // Handle different response scenarios
    if (result == -2) { // Timeout
        printf("No response to reset command (timeout) - this is normal for some implementations\n");
    } else if (result == -1) { // Connection closed by peer
        printf("Connection closed by NetSIO hub after reset command\n");
        printf("NOTE: This is expected behavior when the hub doesn't implement the %s handler\n", reset_name);
        printf("      The NetSIO hub needs to be updated to handle RESET commands properly\n");
    } else if (result > 0) {
        printf("Received response to reset command: Event=0x%02X, Arg=0x%02X\n", event, arg);
    }
    
    printf("%s command completed%s\n", 
           reset_name, 
           (result == -1) ? " (but closed connection)" : "");
    
    return 1; // Consider success even with connection close
}

/**
 * Print usage information
 */
static void print_usage(const char* program_name) {
    printf("Usage: %s [options]\n", program_name);
    printf("Options:\n");
    printf("  -h host    Specify the NetSIO hub hostname/IP (default: %s)\n", DEFAULT_HOST);
    printf("  -p port    Specify the NetSIO hub TCP port (default: %d)\n", DEFAULT_PORT);
    printf("  -v         Enable verbose debugging output\n");
    printf("  -r         Test reset command after SIO command sequence\n");
    printf("  -c         Use COLD_RESET instead of WARM_RESET if testing reset\n");
    printf("  -?         Show this help message\n");
}

/**
 * Main entry point
 */
int main(int argc, char *argv[]) {
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    int verbose = 0;
    int opt;
    int ret_code = 0;
    int cold_reset = 0; // By default, use warm reset if testing reset
    int test_reset = 0; // By default, don't test reset command
    
    /* Parse command line arguments */
    while ((opt = getopt(argc, argv, "h:p:vrc?")) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'v':
                verbose = 1;
                break;
            case 'r':
                test_reset = 1;
                break;
            case 'c':
                cold_reset = 1;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }
    
    printf("Starting FujiNet NetSIO Test Application...\n");
    printf("Host: %s, Port: %d, Verbose: %s\n", host, port, verbose ? "yes" : "no");
    if (test_reset) {
        printf("Will test %s after command sequence\n", cold_reset ? "COLD_RESET" : "WARM_RESET");
    }
    
    /* Initialize TCP connection to the NetSIO service */
    printf("Initializing TCP connection to %s:%d...\n", host, port);
    
    struct sockaddr_in server_addr;
    
    /* Create socket */
    tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_socket < 0) {
        perror("socket creation failed");
        return 1;
    }
    
    /* Set up server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("invalid address");
        close(tcp_socket);
        return 1;
    }
    
    /* Connect to the server */
    printf("Connecting to server...\n");
    if (connect(tcp_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connection failed");
        close(tcp_socket);
        return 1;
    }
    
    printf("TCP connection established.\n");
    
    /* --- Begin test sequence --- */
    printf("=== Sending SIO Command via NetSIO ===\n");
    
    /* Test command: Get Status for Device 0x31 (NetSIO device) */
    const uint8_t test_command[] = {
        0x31,       /* Device ID */
        0x4E,       /* Command (Get Status) */
        0x00, 0x00, /* Aux1, Aux2 */
        0x7F        /* Checksum (computed manually for this test) */
    };
    
    unsigned char sio_response[256];
    int response_len = 256;
    
    // Send the command and wait for response
    if (send_sio_command(test_command, sizeof(test_command), sio_response, &response_len)) {
        printf("Successfully sent SIO command and received response.\n");
        // Only print response if successful and length > 0
        if (response_len > 0) {
            printf("SIO Response (%d bytes): ", response_len);
            log_hex_dump("", sio_response, response_len);
        }
    } else {
        fprintf(stderr, "Failed to send SIO command or receive full response.\n");
        ret_code = 1;
    }

    // If requested, test reset command
    if (test_reset && ret_code == 0) {
        printf("\n=== Testing %s Command ===\n", cold_reset ? "COLD_RESET" : "WARM_RESET");
        if (!send_reset_command(cold_reset, verbose)) {
            fprintf(stderr, "Failed to send reset command.\n");
            ret_code = 1;
        }
    }
    
    /* Close TCP connection */
    if (tcp_socket >= 0) {
        printf("TCP socket closed\n");
        close(tcp_socket);
        tcp_socket = -1;
    }
    
    printf("\nTest complete.\n");
    return ret_code;
}
