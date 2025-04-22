// src/fujinet_netsio.c
#ifdef USE_FUJINET

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>   // isprint
#include <errno.h>
#include <stdbool.h>

#include "log.h"
#include "fujinet_netsio.h"
#include "fujinet.h"

// --- NetSIO Protocol Codes --- 
#define NETSIO_COMMAND_ON        0x11
#define NETSIO_COMMAND_OFF       0x10
#define NETSIO_COMMAND_OFF_SYNC  0x18
#define NETSIO_DATA_BLOCK        0x02
#define NETSIO_DATA_BYTE_SYNC    0x09
#define NETSIO_DEVICE_DISCONNECT 0xC0
#define NETSIO_DEVICE_CONNECT    0xC1
#define NETSIO_PING_REQUEST      0xC2
#define NETSIO_PING_RESPONSE     0xC3
#define NETSIO_ALIVE_REQUEST     0xC4
#define NETSIO_ALIVE_RESPONSE    0xC5
#define NETSIO_CREDIT_STATUS     0xC6
#define NETSIO_CREDIT_UPDATE     0xC7
#define NETSIO_SYNC_RESPONSE     0x81
#define NETSIO_REAL_SYNC_RESPONSE 0x82 // Unused for now
#define NETSIO_SPEED_CHANGE      0x80 // Unused for now

// --- NetSIO State Variables --- 
static struct sockaddr_in current_client_addr;
static socklen_t current_client_len = sizeof(current_client_addr);
static bool client_known = false;
static bool initial_credit_sent = false;
static uint8_t current_sync_number = 0;
static int available_credits = 0;

// --- Function Implementations --- 
// Initialize NetSIO state variables.
void FujiNet_NetSIO_InitState(void) {
    Log_print("FujiNet_NetSIO: Resetting state.");
    client_known = false;
    initial_credit_sent = false;
    current_sync_number = 0;
    available_credits = 0;
    memset(&current_client_addr, 0, sizeof(current_client_addr));
    current_client_len = sizeof(current_client_addr);
}

// Check if a client is known and the initial handshake is complete.
BOOL FujiNet_NetSIO_IsClientConnected(void) {
    // Consider connected only after handshake completes (initial credit sent)
    return client_known && initial_credit_sent;
}

// Get the current client address (if known).
// Returns TRUE if client is known, FALSE otherwise.
BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len) {
    if (client_known) {
        memcpy(client_addr, &current_client_addr, sizeof(current_client_addr));
        *client_len = current_client_len;
        return TRUE;
    } else {
        return FALSE;
    }
}

// Process a received packet buffer.
// Checks for new clients, handles handshake (PING/ALIVE), credit status, etc.
// Determines if a response packet needs to be sent.
// Returns TRUE if a response packet was generated, FALSE otherwise.
BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *recv_buffer, ssize_t recv_len,
                                  const struct sockaddr_in *recv_addr, socklen_t recv_addr_len,
                                  unsigned char *response_buffer, ssize_t *response_len)
{
    *response_len = 0; // Default: no response needed
    if (recv_len <= 0) return FALSE; // Ignore empty/error packets

    uint8_t msg_type = recv_buffer[0];

    // Log received packet for debugging
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(recv_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    Log_print("FujiNet_NetSIO: RX from %s:%d - Type 0x%02X (%zd bytes)", client_ip, ntohs(recv_addr->sin_port), msg_type, recv_len);
    // print_hex_fuji(recv_buffer, recv_len); // Optional: Implement and call hex dump helper

    // Handle new client or message from existing client
    if (!client_known) {
        // First message must be PING or ALIVE to establish connection
        if (msg_type == NETSIO_PING_REQUEST || msg_type == NETSIO_ALIVE_REQUEST) {
            Log_print("FujiNet_NetSIO: New client detected %s:%d.", client_ip, ntohs(recv_addr->sin_port));
            memcpy(&current_client_addr, recv_addr, recv_addr_len);
            current_client_len = recv_addr_len;
            client_known = true;
            initial_credit_sent = false; // Mark handshake as incomplete
            available_credits = 0; // Reset credits for new client
        } else {
            Log_print("FujiNet_NetSIO: Ignoring message 0x%02X from unknown client %s:%d.", msg_type, client_ip, ntohs(recv_addr->sin_port));
            return FALSE; // Ignore messages from unknown sources if not handshake
        }
    } else {
        // Check if message is from the *known* client
        if (current_client_addr.sin_addr.s_addr != recv_addr->sin_addr.s_addr ||
            current_client_addr.sin_port != recv_addr->sin_port) {
            char known_client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(current_client_addr.sin_addr), known_client_ip, INET_ADDRSTRLEN);
            Log_print("FujiNet_NetSIO: Ignoring message 0x%02X from unexpected source %s:%d (expected %s:%d).", 
                      msg_type, client_ip, ntohs(recv_addr->sin_port),
                      known_client_ip, ntohs(current_client_addr.sin_port));
            return FALSE; // Message not from the client we are talking to
        }
    }

    // Process message based on type
    switch (msg_type) {
        case NETSIO_PING_REQUEST:
            Log_print("FujiNet_NetSIO: PING received. Responding with PING_RESPONSE.");
            response_buffer[0] = NETSIO_PING_RESPONSE;
            *response_len = 1;
            return TRUE;

        case NETSIO_ALIVE_REQUEST:
            Log_print("FujiNet_NetSIO: ALIVE received. Responding with ALIVE_RESPONSE.");
            response_buffer[0] = NETSIO_ALIVE_RESPONSE;
            *response_len = 1;
            // Do not send initial credits here, wait for CREDIT_STATUS
            return TRUE;

        case NETSIO_CREDIT_STATUS:
            Log_print("FujiNet_NetSIO: CREDIT_STATUS received. Sending initial CREDIT_UPDATE (%d credits).", NETSIO_DEFAULT_CREDITS);
            available_credits = NETSIO_DEFAULT_CREDITS;
            response_buffer[0] = NETSIO_CREDIT_UPDATE;
            response_buffer[1] = available_credits; // Send initial credits
            *response_len = 2;
            initial_credit_sent = true; // Mark handshake as complete
            Log_print("FujiNet_NetSIO: Client handshake complete. Available credits: %d", available_credits);
            return TRUE;

         case NETSIO_DEVICE_DISCONNECT:
            Log_print("FujiNet_NetSIO: Received DEVICE_DISCONNECT. Resetting client state.");
            FujiNet_NetSIO_InitState(); // Reset all NetSIO state
            return FALSE; // No response needed

        case NETSIO_SYNC_RESPONSE: // Handled specifically by FujiNet_SendSIOCommand wait loop
            // We still need to process it here to manage credits
            if (recv_len >= 3) {
                 Log_print("FujiNet_NetSIO: Received SYNC_RESPONSE (handled by sender). Sync#%d, Status:0x%02X", recv_buffer[1], recv_buffer[2]);
                // Decrease credits - assumes sync response consumes one credit
                 if (available_credits > 0) {
                     available_credits--;
                     Log_print("FujiNet_NetSIO: Decremented credits to %d.", available_credits);
                 } else {
                     Log_print("FujiNet_NetSIO: Warning - Received SYNC_RESPONSE with 0 credits available.");
                 }
                // TODO: Check if credit update needs to be sent proactively?
            } else {
                 Log_print("FujiNet_NetSIO: Received short SYNC_RESPONSE (%zd bytes). Ignoring.", recv_len);
            }
            return FALSE; // Don't generate an immediate response here

        case NETSIO_DATA_BLOCK: // Data coming FROM fujinet device
            if (recv_len >= 3) { 
                Log_print("FujiNet_NetSIO: Received DATA_BLOCK (sync %d, len %d). Storing...", recv_buffer[1], recv_buffer[2]);
                // TODO: Store received data in a buffer for FujiNet_SendSIOCommand
                if (available_credits > 0) {
                     available_credits--;
                     Log_print("FujiNet_NetSIO: Decremented credits to %d (for DATA_BLOCK).", available_credits);
                 } else {
                     Log_print("FujiNet_NetSIO: Warning - Received DATA_BLOCK with 0 credits available.");
                 }
            } else {
                Log_print("FujiNet_NetSIO: Received short DATA_BLOCK (%zd bytes). Ignoring.", recv_len);
            }
            return FALSE; // No immediate response needed

        case NETSIO_COMMAND_ON: // Usually part of a sequence we send
        case NETSIO_COMMAND_OFF:
        case NETSIO_COMMAND_OFF_SYNC:
             Log_print("FujiNet_NetSIO: Received SIO command packet 0x%02X (likely echo or unexpected). Ignoring.", msg_type);
             return FALSE;

        case NETSIO_CREDIT_UPDATE: // Client telling us its credits? Should not happen from Hub.
             Log_print("FujiNet_NetSIO: Received unexpected CREDIT_UPDATE from client. Ignoring.");
             return FALSE;

        case NETSIO_DEVICE_CONNECT: // Client telling us it connected? Should not happen from Hub.
             Log_print("FujiNet_NetSIO: Received unexpected DEVICE_CONNECT from client. Ignoring.");
             return FALSE;

        default:
            Log_print("FujiNet_NetSIO: Received unknown message type 0x%02X. Ignoring.", msg_type);
            return FALSE;
    }
    // Should not reach here
    return FALSE;
}

// Prepare the NetSIO sequence for an SIO command.
// Returns the sync number used for this command, or -1 on error.
int FujiNet_NetSIO_PrepareSIOCommandSequence(
                            UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                            const UBYTE *output_buffer, int output_len,
                            unsigned char *on_cmd_buf, size_t *on_cmd_len,
                            unsigned char *data_cmd_buf, size_t *data_cmd_len,
                            unsigned char *data_out_buf, size_t *data_out_len, 
                            unsigned char *off_sync_buf, size_t *off_sync_len)
{
    if (!FujiNet_NetSIO_IsClientConnected()) {
        Log_print("FujiNet_NetSIO: Cannot prepare SIO command - client not connected.");
        return -1; // Error: Not connected
    }

    if (available_credits <= 0) {
        Log_print("FujiNet_NetSIO: Cannot prepare SIO command - no credits available.");
        return -2; // Error: No credits
    }
    // TODO: Check if already waiting for a sync response?

    // Increment and wrap sync number (0-255)
    current_sync_number++;

    // COMMAND_ON: 0x11, dev_id, command, aux1, aux2
    on_cmd_buf[0] = NETSIO_COMMAND_ON;
    on_cmd_buf[1] = device_id;
    on_cmd_buf[2] = command;
    on_cmd_buf[3] = aux1;
    on_cmd_buf[4] = aux2;
    *on_cmd_len = 5;

    // DATA_BLOCK: 0x02, sync#, len, <data...>
    data_cmd_buf[0] = NETSIO_DATA_BLOCK;
    data_cmd_buf[1] = current_sync_number;
    data_cmd_buf[2] = (UBYTE)output_len; // Assuming output_len fits in UBYTE
    *data_cmd_len = 3;
    if (output_len > 0) {
        if (output_len > NETSIO_MAX_PACKET_SIZE - 3) { // Check buffer size
             Log_print("FujiNet_NetSIO: SIO output buffer too large (%d bytes).", output_len);
             return -3; // Error: Data too large
        }
        memcpy(data_out_buf, output_buffer, output_len);
        *data_out_len = output_len;
    } else {
        *data_out_len = 0;
    }

    // COMMAND_OFF_SYNC: 0x18, sync#
    off_sync_buf[0] = NETSIO_COMMAND_OFF_SYNC;
    off_sync_buf[1] = current_sync_number;
    *off_sync_len = 2;

    // Credits are decremented in ProcessPacket when SYNC_RESPONSE arrives

    Log_print("FujiNet_NetSIO: Prepared SIO Seq: ON(%02X %02X %02X %02X), DATA(Sync %d, Len %d), OFF_SYNC(Sync %d)", 
                device_id, command, aux1, aux2, current_sync_number, output_len, current_sync_number);

    return current_sync_number;
}

// --- Remaining Stubs ---
BOOL FujiNet_NetSIO_CheckSyncResponse(const unsigned char *recv_buffer, ssize_t recv_len,
                                      int expected_sync, uint8_t *status_code)
{
    if (recv_buffer == NULL || recv_len < 3) {
        return FALSE;
    }

    // Check for NETSIO_SYNC_RESPONSE (0x81), expected sync number, and extract status
    if (recv_buffer[0] == NETSIO_SYNC_RESPONSE && recv_buffer[1] == (uint8_t)expected_sync) {
        *status_code = recv_buffer[2]; // Extract status code
        Log_print("FujiNet_NetSIO: Matched SYNC_RESPONSE for sync %d, status 0x%02X", expected_sync, *status_code);
        return TRUE;
    }

    return FALSE;
}

#endif /* USE_FUJINET */
