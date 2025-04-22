#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <netdb.h>     /* For gethostbyname */
#include <errno.h>     /* For errno */

#define NETSIO_PORT 9997
#define BUFFER_SIZE 1024

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

/* Global variables */
static int udp_socket = -1;
static struct sockaddr_in fujinet_addr;
static int have_fujinet_addr = 0;
static uint8_t sync_counter = 0;

/* Function prototypes */
int initialize_netsio(const char *host, int port);
void shutdown_netsio(void);
int send_netsio_message(uint8_t message_type, uint8_t parameter, const uint8_t *data, uint16_t data_length);
int send_device_connected(void);
int send_command_on(uint8_t device_id);
int send_data_block(const uint8_t *data, uint16_t data_length);
int send_command_off_sync(uint8_t sync_number);
int handle_incoming_messages(void);

/* Initialize NetSIO UDP communication */
int initialize_netsio(const char *host, int port) {
    struct hostent *hp;
    
    printf("Initializing NetSIO UDP communication\n");
    
    /* Create UDP socket */
    udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_socket < 0) {
        perror("Failed to create UDP socket");
        return -1;
    }
    
    /* Set up local address structure for binding */
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    local_addr.sin_port = htons(port);  /* Bind directly to the specified port */
    
    /* Bind the socket to the local address */
    if (bind(udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        perror("Failed to bind UDP socket");
        close(udp_socket);
        udp_socket = -1;
        return -1;
    }
    
    printf("Bound to local port %d\n", port);
    
    /* Set up FujiNet address structure if host is provided */
    if (host && *host) {
        memset(&fujinet_addr, 0, sizeof(fujinet_addr));
        fujinet_addr.sin_family = AF_INET;
        fujinet_addr.sin_port = htons(port);
        
        /* Try to parse as IP address first */
        if (inet_pton(AF_INET, host, &fujinet_addr.sin_addr) != 1) {
            /* If not an IP address, try to resolve hostname */
            hp = gethostbyname(host);
            if (hp == NULL) {
                printf("Failed to resolve hostname %s\n", host);
                /* Don't fail initialization, we'll wait for incoming packets */
            } else {
                memcpy(&fujinet_addr.sin_addr, hp->h_addr, hp->h_length);
                have_fujinet_addr = 1;
                printf("FujiNet address set to %s:%d\n", host, port);
            }
        } else {
            have_fujinet_addr = 1;
            printf("FujiNet address set to %s:%d\n", host, port);
        }
    }
    
    /* Set socket to non-blocking mode */
    int flags = fcntl(udp_socket, F_GETFL, 0);
    if (flags < 0) {
        perror("Failed to get socket flags");
    } else if (fcntl(udp_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("Failed to set socket to non-blocking mode");
    }
    
    printf("NetSIO initialized successfully, listening on UDP port %d\n", port);
    return 0;
}

/* Shutdown NetSIO UDP communication */
void shutdown_netsio(void) {
    printf("Shutting down NetSIO\n");
    
    if (udp_socket >= 0) {
        close(udp_socket);
        udp_socket = -1;
    }
    
    printf("NetSIO shutdown complete\n");
}

/* Send a NetSIO message */
int send_netsio_message(uint8_t message_type, uint8_t parameter, const uint8_t *data, uint16_t data_length) {
    uint8_t buffer[BUFFER_SIZE + 4]; /* Header (4) + Data */
    uint16_t le_data_length;
    int result;
    
    /* Check if we have the FujiNet address */
    if (!have_fujinet_addr) {
        printf("Cannot send message: FujiNet address not set\n");
        return -1;
    }
    
    /* Check if socket is valid */
    if (udp_socket < 0) {
        printf("Cannot send message: UDP socket not initialized\n");
        return -1;
    }
    
    /* Check data length */
    if (data_length > BUFFER_SIZE) {
        printf("Data length %d exceeds maximum buffer size %d\n", data_length, BUFFER_SIZE);
        return -1;
    }
    
    /* Prepare message header */
    buffer[0] = message_type;
    buffer[1] = parameter;
    le_data_length = data_length; /* Assuming little-endian system */
    memcpy(buffer + 2, &le_data_length, 2);
    
    /* Copy data if present */
    if (data_length > 0 && data != NULL) {
        memcpy(buffer + 4, data, data_length);
    }
    
    /* Send the message */
    result = sendto(udp_socket, buffer, 4 + data_length, 0, 
                   (struct sockaddr*)&fujinet_addr, sizeof(fujinet_addr));
    
    if (result < 0) {
        perror("Failed to send NetSIO message");
        return -1;
    }
    
    printf("Sent NetSIO message: type=0x%02X, param=0x%02X, data_len=%d\n", 
           message_type, parameter, data_length);
    
    return 0;
}

/* Send a device connected message */
int send_device_connected(void) {
    return send_netsio_message(NETSIO_DEVICE_CONNECTED, 0, NULL, 0);
}

/* Send a command on message */
int send_command_on(uint8_t device_id) {
    return send_netsio_message(NETSIO_COMMAND_ON, device_id, NULL, 0);
}

/* Send a data block message */
int send_data_block(const uint8_t *data, uint16_t data_length) {
    return send_netsio_message(NETSIO_DATA_BLOCK, 0, data, data_length);
}

/* Send a command off sync message */
int send_command_off_sync(uint8_t sync_number) {
    return send_netsio_message(NETSIO_COMMAND_OFF_SYNC, sync_number, NULL, 0);
}

/* Handle incoming NetSIO messages */
int handle_incoming_messages(void) {
    uint8_t buffer[BUFFER_SIZE + 4]; /* Header (4) + Data */
    struct sockaddr_in sender_addr;
    socklen_t sender_addr_len = sizeof(sender_addr);
    int result;
    
    /* Check if socket is valid */
    if (udp_socket < 0) {
        printf("Cannot receive message: UDP socket not initialized\n");
        return -1;
    }
    
    /* Try to receive a message */
    result = recvfrom(udp_socket, buffer, sizeof(buffer), 0,
                     (struct sockaddr*)&sender_addr, &sender_addr_len);
    
    if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No data available */
            return 0;
        }
        perror("Failed to receive NetSIO message");
        return -1;
    }
    
    /* Check if we received enough data for a header */
    if (result < 4) {
        printf("Received incomplete NetSIO message header (%d bytes)\n", result);
        return -1;
    }
    
    /* Parse the message header */
    uint8_t message_type = buffer[0];
    uint8_t parameter = buffer[1];
    uint16_t data_length;
    memcpy(&data_length, buffer + 2, 2);
    
    /* Check if we received all the data */
    if (result < 4 + data_length) {
        printf("Received incomplete NetSIO message data (%d bytes, expected %d)\n",
               result - 4, data_length);
        return -1;
    }
    
    /* If we don't have the FujiNet address yet, save it */
    if (!have_fujinet_addr) {
        memcpy(&fujinet_addr, &sender_addr, sizeof(fujinet_addr));
        have_fujinet_addr = 1;
        printf("FujiNet address set to %s:%d from incoming packet\n",
               inet_ntoa(fujinet_addr.sin_addr), ntohs(fujinet_addr.sin_port));
    }
    
    printf("Received NetSIO message: type=0x%02X, param=0x%02X, data_len=%d from %s:%d\n",
           message_type, parameter, data_length, 
           inet_ntoa(sender_addr.sin_addr), ntohs(sender_addr.sin_port));
    
    /* Handle different message types */
    switch (message_type) {
        case NETSIO_DEVICE_CONNECTED:
            printf("*** VALIDATION: Device connected message received from FujiNet! ***\n");
            /* Send a response to acknowledge the connection */
            send_device_connected();
            return 1;
            
        case NETSIO_DEVICE_DISCONNECTED:
            printf("Device disconnected message received\n");
            return 1;
            
        case NETSIO_PING_REQUEST:
            printf("Ping request received, sending response\n");
            send_netsio_message(NETSIO_PING_RESPONSE, 0, NULL, 0);
            return 1;
            
        case NETSIO_PING_RESPONSE:
            printf("Ping response received\n");
            return 1;
            
        case NETSIO_ALIVE_REQUEST:
            printf("Alive request received, sending response\n");
            send_netsio_message(NETSIO_ALIVE_RESPONSE, 0, NULL, 0);
            return 1;
            
        case NETSIO_ALIVE_RESPONSE:
            printf("Alive response received\n");
            return 1;
            
        case NETSIO_SYNC_RESPONSE:
            printf("*** VALIDATION: Sync response received from FujiNet! sync_number=%d ***\n", parameter);
            
            /* Parse the sync response data */
            if (data_length >= 1) {
                uint8_t ack_type = buffer[4];
                printf("  Ack Type: 0x%02X ('%c')\n", ack_type, ack_type);
            }
            
            if (data_length >= 2) {
                uint8_t ack_byte = buffer[5];
                printf("  Ack Byte: 0x%02X\n", ack_byte);
            }
            
            if (data_length >= 4) {
                uint16_t write_size;
                memcpy(&write_size, buffer + 6, 2);
                printf("  Write Size: %d\n", write_size);
            }
            
            return 1;
            
        case NETSIO_DATA_BYTE:
            printf("Received data byte: 0x%02X\n", parameter);
            return 1;
            
        case NETSIO_DATA_BLOCK:
            printf("*** VALIDATION: Data block received from FujiNet! %d bytes ***\n", data_length);
            printf("  First few bytes: ");
            for (int i = 0; i < (data_length > 16 ? 16 : data_length); i++) {
                printf("%02X ", buffer[4 + i]);
            }
            printf("\n");
            return 1;
            
        case NETSIO_PROCEED_ON:
            printf("Proceed ON received\n");
            return 1;
            
        case NETSIO_PROCEED_OFF:
            printf("Proceed OFF received\n");
            return 1;
            
        case NETSIO_INTERRUPT_ON:
            printf("Interrupt ON received\n");
            return 1;
            
        case NETSIO_INTERRUPT_OFF:
            printf("Interrupt OFF received\n");
            return 1;
            
        case NETSIO_WARM_RESET:
            printf("Warm reset received\n");
            return 1;
            
        case NETSIO_COLD_RESET:
            printf("Cold reset received\n");
            return 1;
            
        default:
            printf("Received unknown NetSIO message type: 0x%02X\n", message_type);
            return 1;
    }
    
    return 0;
}

/* Main function */
int main(int argc, char *argv[]) {
    const char *host = "localhost";
    int port = NETSIO_PORT;
    
    /* Parse command line arguments */
    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    
    printf("NetSIO Test Program\n");
    printf("Initializing NetSIO with host=%s, port=%d...\n", host, port);
    
    /* Initialize NetSIO */
    if (initialize_netsio(host, port) < 0) {
        printf("Failed to initialize NetSIO\n");
        return 1;
    }
    
    printf("NetSIO initialized successfully\n");
    
    /* Send device connected message */
    printf("Sending device connected message...\n");
    if (send_device_connected() < 0) {
        printf("Failed to send device connected message\n");
    } else {
        printf("Device connected message sent successfully\n");
    }
    
    /* Example: Send an SIO command for device 0x31 (D1:) */
    printf("Sending example SIO command for device 0x31 (D1:)...\n");
    
    /* Step 1: Send COMMAND_ON with device ID */
    printf("Step 1: Sending COMMAND_ON with device ID 0x31...\n");
    if (send_command_on(0x31) < 0) {
        printf("Failed to send COMMAND_ON message\n");
    } else {
        printf("COMMAND_ON message sent successfully\n");
    }
    
    /* Step 2: Send DATA_BLOCK with command, aux1, aux2 */
    printf("Step 2: Sending DATA_BLOCK with command='R', aux1=1, aux2=0...\n");
    uint8_t data_block[3] = {0x52, 0x01, 0x00}; /* 'R' command, sector 1 */
    if (send_data_block(data_block, 3) < 0) {
        printf("Failed to send DATA_BLOCK message\n");
    } else {
        printf("DATA_BLOCK message sent successfully\n");
    }
    
    /* Step 3: Send COMMAND_OFF_SYNC with sync request counter */
    printf("Step 3: Sending COMMAND_OFF_SYNC with sync_counter=%d...\n", sync_counter);
    if (send_command_off_sync(sync_counter++) < 0) {
        printf("Failed to send COMMAND_OFF_SYNC message\n");
    } else {
        printf("COMMAND_OFF_SYNC message sent successfully\n");
    }
    
    /* Main loop: Handle incoming messages */
    printf("Waiting for incoming messages (press Ctrl+C to exit)...\n");
    int count = 0;
    while (1) {
        int result = handle_incoming_messages();
        if (result > 0) {
            printf("Processed an incoming message\n");
        } else if (result < 0) {
            printf("Error processing incoming message\n");
        }
        
        /* Print a status message every 10 seconds */
        if (count++ % 1000 == 0) {
            printf("Still waiting for messages... (count=%d)\n", count);
        }
        
        usleep(10000); /* Sleep for 10ms to avoid high CPU usage */
    }
    
    /* Cleanup */
    shutdown_netsio();
    return 0;
}
