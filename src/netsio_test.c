#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>
#include <ctype.h> // For isprint
#include <stdbool.h>

#define NETSIO_PORT 9997
#define BUFFER_SIZE 1024

// NetSIO Message Types (Based on netsio.py)
#define NETSIO_DATA_BYTE        0x01
#define NETSIO_DATA_BLOCK       0x02
#define NETSIO_DATA_BYTE_SYNC   0x09
#define NETSIO_COMMAND_OFF      0x10
#define NETSIO_COMMAND_ON       0x11 // Command Assert
#define NETSIO_COMMAND_OFF_SYNC 0x18 // Command Deassert + Sync Request
#define NETSIO_MOTOR_OFF        0x20
#define NETSIO_MOTOR_ON         0x21
#define NETSIO_PROCEED_OFF      0x30
#define NETSIO_PROCEED_ON       0x31
#define NETSIO_INTERRUPT_OFF    0x40
#define NETSIO_INTERRUPT_ON     0x41
#define NETSIO_SPEED_CHANGE     0x80
// #define NETSIO_SYNC_RESPONSE    0x81 // Misleading in netsio.py
#define NETSIO_BUS_IDLE         0x88
#define NETSIO_CANCEL           0x89

// --- Connection Management (>= 0xC0) ---
#define NETSIO_DEVICE_DISCONNECT 0xC0
#define NETSIO_DEVICE_CONNECT   0xC1
#define NETSIO_PING_REQUEST     0xC2
#define NETSIO_PING_RESPONSE    0xC3
#define NETSIO_ALIVE_REQUEST    0xC4
#define NETSIO_ALIVE_RESPONSE   0xC5
#define NETSIO_CREDIT_STATUS    0xC6
#define NETSIO_CREDIT_UPDATE    0xC7
// --- End Connection Management ---

#define NETSIO_WARM_RESET       0xFE
#define NETSIO_COLD_RESET       0xFF

// Actual Sync response per netsio.md
#define NETSIO_REAL_SYNC_RESPONSE 0x82
#define NETSIO_SYNC_RESPONSE 0x81

#define NETSIO_HUB_PORT 9997
#define DEFAULT_CREDITS 3

// SIO Command Format: Device ID, Command, Aux1, Aux2, Checksum
// STATUS command for D1: (0x31), Checksum = 0x31 + 0x53 = 0x84
unsigned char sio_status_cmd_d1[] = { 0x31, 0x53, 0x00, 0x00, 0x84 };
// STATUS command for FujiNet Device (0x70), Checksum = 0x70 + 0x53 = 0xC3
unsigned char sio_status_cmd_fuji[] = { 0x70, 0x53, 0x00, 0x00, 0xC3 };

// Helper to print buffer as hex
void print_hex(const unsigned char *buf, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        printf("%02X ", buf[i]);
    }
    printf(" | ");
    for (size_t i = 0; i < len; ++i) {
        printf("%c", isprint(buf[i]) ? buf[i] : '.');
    }
    printf("\n");
}

void send_sio_status_command(int sockfd, const struct sockaddr_in client_addr, socklen_t client_len, uint8_t sync_number) {
    unsigned char cmd_buf[10]; // Buffer for NetSIO command packets
    ssize_t sent_bytes;
    int cmd_len;

    printf("\n==> Sending SIO STATUS sequence (sync %d) for FujiNet (0x70)...\n", sync_number);

    // --- Command Sequence for Device 0x70 --- 

    // 1. Command Assert (ON)
    cmd_buf[0] = NETSIO_COMMAND_ON; // 0x11
    cmd_len = 1;
    printf("    [0x70] Sending COMMAND_ON (0x%02X)\n", cmd_buf[0]);
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto COMMAND_ON failed");

    // 2. Data Block (SIO Command)
    cmd_buf[0] = NETSIO_DATA_BLOCK; // 0x02
    memcpy(cmd_buf + 1, sio_status_cmd_fuji, sizeof(sio_status_cmd_fuji));
    cmd_len = 1 + sizeof(sio_status_cmd_fuji);
    printf("    [0x70] Sending DATA_BLOCK (0x%02X + %zd bytes): ", cmd_buf[0], sizeof(sio_status_cmd_fuji));
    print_hex(cmd_buf + 1, sizeof(sio_status_cmd_fuji));
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto DATA_BLOCK failed");

    // 3. Command De-assert (OFF) + Sync Request
    cmd_buf[0] = NETSIO_COMMAND_OFF_SYNC; // 0x18
    cmd_buf[1] = sync_number;
    cmd_len = 2;
    printf("    [0x70] Sending COMMAND_OFF_SYNC (0x%02X, sync=0x%02X)\n", cmd_buf[0], cmd_buf[1]);
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto COMMAND_OFF_SYNC failed");

    printf("==> SIO STATUS sequence for 0x70 sent. Waiting for response...\n");

    // --- Command Sequence for Device 0x31 --- 
    sync_number++; // Use next sync number
    printf("\n==> Sending SIO STATUS sequence (sync %d) for Drive 1 (0x31)...\n", sync_number);

    // 1. Command Assert (ON)
    cmd_buf[0] = NETSIO_COMMAND_ON; // 0x11
    cmd_len = 1;
    printf("    [0x31] Sending COMMAND_ON (0x%02X)\n", cmd_buf[0]);
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto COMMAND_ON failed");

    // 2. Data Block (SIO Command)
    cmd_buf[0] = NETSIO_DATA_BLOCK; // 0x02
    memcpy(cmd_buf + 1, sio_status_cmd_d1, sizeof(sio_status_cmd_d1));
    cmd_len = 1 + sizeof(sio_status_cmd_d1);
    printf("    [0x31] Sending DATA_BLOCK (0x%02X + %zd bytes): ", cmd_buf[0], sizeof(sio_status_cmd_d1));
    print_hex(cmd_buf + 1, sizeof(sio_status_cmd_d1));
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto DATA_BLOCK failed");

    // 3. Command De-assert (OFF) + Sync Request
    cmd_buf[0] = NETSIO_COMMAND_OFF_SYNC; // 0x18
    cmd_buf[1] = sync_number;
    cmd_len = 2;
    printf("    [0x31] Sending COMMAND_OFF_SYNC (0x%02X, sync=0x%02X)\n", cmd_buf[0], cmd_buf[1]);
    sent_bytes = sendto(sockfd, cmd_buf, cmd_len, 0, (const struct sockaddr *)&client_addr, client_len);
    if (sent_bytes < 0) perror("    sendto COMMAND_OFF_SYNC failed");

    printf("==> SIO STATUS sequence for 0x31 sent. Waiting for response...\n");
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;
    struct sockaddr_in current_client_addr;
    socklen_t current_client_len = sizeof(current_client_addr);
    bool client_known = false;
    bool initial_credit_sent = false;
    unsigned char buffer[BUFFER_SIZE];
    ssize_t n;
    int sent_initial_command = 0; // Flag to send command only once
    uint8_t sync_number = 0; // Sync counter for commands

    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    // Allow address reuse
    int reuse = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
    }
    #ifdef SO_REUSEPORT
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt(SO_REUSEPORT) failed");
    }
    #endif

    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(NETSIO_PORT);

    // Bind the socket
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("NetSIO test server listening on UDP port %d...\n", NETSIO_PORT);

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        struct sockaddr_in recv_client_addr;
        socklen_t recv_client_len = sizeof(recv_client_addr);
        n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&recv_client_addr, &recv_client_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &recv_client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("[%s:%d] RX (%zd bytes): ", client_ip, ntohs(recv_client_addr.sin_port), n);
        print_hex(buffer, n);

        if (n > 0) {
            uint8_t response_byte = 0;
            ssize_t response_len = 0;
            int send_status_commands_now = 0;
            bool send_initial_credit = false;

            // Track current client
            if (!client_known || memcmp(&recv_client_addr, &current_client_addr, sizeof(struct sockaddr_in)) != 0) {
                printf("  -> New or changed client address detected.\n");
                memcpy(&current_client_addr, &recv_client_addr, sizeof(struct sockaddr_in));
                current_client_len = recv_client_len;
                client_known = true;
                initial_credit_sent = false; // Reset credit flag for new client
                sent_initial_command = 0;   // Reset command flag for new client
            }

            switch (buffer[0]) {
                case NETSIO_PING_REQUEST: // 0xC2
                    printf("  -> Received PING_REQUEST.\n");
                    response_byte = NETSIO_PING_RESPONSE; // 0xC3
                    response_len = 1;
                    if (!initial_credit_sent) send_initial_credit = true;
                    break;

                case NETSIO_ALIVE_REQUEST: // 0xC4
                    printf("  -> Received ALIVE_REQUEST.\n");
                    response_byte = NETSIO_ALIVE_RESPONSE; // 0xC5
                    response_len = 1;
                    // Send SIO command ONLY after first ALIVE response is sent
                    if (!sent_initial_command) {
                        send_status_commands_now = 1; 
                    }
                    if (!initial_credit_sent) send_initial_credit = true;
                    break;

                case NETSIO_DEVICE_DISCONNECT: // 0xC0
                    printf("  -> Received DEVICE_DISCONNECT from %s:%d. Closing connection.\n", client_ip, ntohs(recv_client_addr.sin_port));
                    client_known = false; // Forget client
                    initial_credit_sent = false;
                    sent_initial_command = 0; // Allow command to be sent again if reconnected
                    break;

                case NETSIO_SPEED_CHANGE: // 0x80
                    printf("  -> Received SPEED_CHANGE from %s:%d with data: ", client_ip, ntohs(recv_client_addr.sin_port));
                    if (n > 1) {
                        print_hex(buffer + 1, n - 1);
                    } else {
                        printf("[No data]\n");
                    }
                    // No specific response required from hub AFAIK
                    break;

                case NETSIO_CREDIT_STATUS: // 0xC6
                    printf("  -> Received CREDIT_STATUS from %s:%d with data: ", client_ip, ntohs(recv_client_addr.sin_port));
                    if (n > 1) {
                        print_hex(buffer + 1, n - 1);
                    } else {
                        printf("[No data]\n");
                    }
                    // Respond with Credit Update
                    printf("  -> Sending CREDIT_UPDATE (%d credits) to %s:%d...\n", DEFAULT_CREDITS, client_ip, ntohs(recv_client_addr.sin_port));
                    uint8_t credit_buf[2];
                    ssize_t sent_bytes_credit;
                    credit_buf[0] = NETSIO_CREDIT_UPDATE;
                    credit_buf[1] = DEFAULT_CREDITS;
                    sent_bytes_credit = sendto(sockfd, credit_buf, 2, 0, (const struct sockaddr *)&recv_client_addr, recv_client_len);
                    if (sent_bytes_credit < 0) perror("    sendto CREDIT_UPDATE failed");
                    else printf("  -> Sent CREDIT_UPDATE (%zd bytes)\n", sent_bytes_credit);

                    break;

                // --- Potential Command Responses ---
                case NETSIO_DATA_BYTE_SYNC: // 0x09
                    printf("  -> Received DATA_BYTE_SYNC (Code 0x%02X) from %s:%d with data: ", buffer[0], client_ip, ntohs(recv_client_addr.sin_port));
                    if (n >= 4) { // Expect code, sync, status, data
                         printf("Sync=0x%02X Status=0x%02X Data=0x%02X\n", buffer[1], buffer[2], buffer[3]);
                    } else {
                         printf("[Incomplete Data]\n");
                    }
                    sent_initial_command = 1; // Mark command as completed/responded
                    break;

                case NETSIO_DATA_BLOCK: // 0x02 (Unexpected as direct response?)
                    printf("  -> Received DATA_BLOCK (Code 0x%02X) from %s:%d with data: ", buffer[0], client_ip, ntohs(recv_client_addr.sin_port));
                     if (n > 1) {
                        print_hex(buffer + 1, n - 1);
                    } else {
                        printf("[No data]\n");
                    }
                    sent_initial_command = 1; // Mark command as completed/responded
                    break;

                case NETSIO_SYNC_RESPONSE:        // 0x81 (per netsio.py)
                case NETSIO_REAL_SYNC_RESPONSE: // 0x82 (per netsio.md)
                    printf("  -> Received SYNC_RESPONSE (Code 0x%02X) from %s:%d with data: ", buffer[0], client_ip, ntohs(recv_client_addr.sin_port));
                    if (n >= 3) { // Expect code, sync number, status
                        printf("Sync=0x%02X Status=0x%02X\n", buffer[1], buffer[2]);
                    } else if (n >= 2) { // Maybe just code and sync number?
                        printf("Sync=0x%02X [No Status Byte]\n", buffer[1]);
                    } else {
                        printf("[Incomplete Data]\n");
                    }
                    sent_initial_command = 1; // Mark command as completed/responded
                    break;

                default:
                    printf("  -> Unhandled message type 0x%02X.\n", buffer[0]);
                    break;
            }

            // Send standard response (PING/ALIVE)
            if (response_len > 0) {
                ssize_t sent_bytes = sendto(sockfd, &response_byte, response_len, 0,
                                             (const struct sockaddr *)&current_client_addr,
                                             current_client_len);
                if (sent_bytes < 0) {
                    perror("sendto response failed");
                } else {
                    printf("  -> Sent response 0x%02X (%zd byte) to %s:%d.\n", response_byte, sent_bytes, client_ip, ntohs(current_client_addr.sin_port));
                }
            }

            // Send initial credit update if triggered
            if (send_initial_credit) {
                printf("  -> Sending INITIAL CREDIT_UPDATE (%d credits) to %s:%d...\n", DEFAULT_CREDITS, client_ip, ntohs(current_client_addr.sin_port));
                uint8_t credit_buf[2];
                ssize_t sent_bytes_credit;
                credit_buf[0] = NETSIO_CREDIT_UPDATE;
                credit_buf[1] = DEFAULT_CREDITS;
                sent_bytes_credit = sendto(sockfd, credit_buf, 2, 0, (const struct sockaddr *)&current_client_addr, current_client_len);
                if (sent_bytes_credit < 0) perror("    sendto INITIAL CREDIT_UPDATE failed");
                else printf("  -> Sent INITIAL CREDIT_UPDATE (%zd bytes)\n", sent_bytes_credit);
                initial_credit_sent = true; // Mark as sent
            }

            // Send the SIO STATUS command if triggered
            if (send_status_commands_now) {
                // Pass the current sync number; the function will handle incrementing for the second command
                send_sio_status_command(sockfd, current_client_addr, current_client_len, sync_number);
                sync_number += 2; // Increment by 2 since the function sent two commands
                sent_initial_command = 1; // Mark as *attempted* - response handlers will confirm
                // We set sent_initial_command = 1 here to prevent resending on subsequent ALIVE messages
            }
        }
        printf("---\n");
    }

    close(sockfd);
    return 0;
}
