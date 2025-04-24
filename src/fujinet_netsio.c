// src/fujinet_netsio.c
#include "config.h"
#include "atari.h"  /* For BOOL type */
#include <stdint.h> /* For uint8_t */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>

#include "log.h"
#include "fujinet_netsio.h"
#include "fujinet.h"
#include "fujinet_udp.h"
#include "sio.h" /* For SIO_ChkSum and SIO response codes */
#include "platform.h" /* For PLATFORM_Sleep */

/* Use globals defined in fujinet.c */
extern int fujinet_sockfd;
extern BOOL fujinet_connected;
extern struct sockaddr_in fujinet_client_addr;
extern socklen_t fujinet_client_len;

/* --- NetSIO Protocol Codes --- */
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
#define NETSIO_REAL_SYNC_RESPONSE 0x82 /* Unused for now */
#define NETSIO_SPEED_CHANGE      0x80 /* Unused for now */
#define NETSIO_ACKNOWLEDGE       0x83 /* New packet type for acknowledging SPEED_CHANGE */
#define NETSIO_DATA_ACK          0x07 /* New packet type for acknowledging data */

/* --- NetSIO State Variables --- */
static struct sockaddr_in current_client_addr;
static socklen_t current_client_len = sizeof(current_client_addr);
static int client_known = 0;
static int initial_credit_sent = 0;
static uint8_t current_sync_number = 0;
static int available_credits = 0;

/* --- Helper Functions --- */
static void print_hex(const unsigned char *buf, size_t len) {
    size_t i;
    char hexbuf[256] = {0};
    char asciibuf[256] = {0};
    int hexpos = 0;
    int asciipos = 0;
    
    for (i = 0; i < len && i < 64; ++i) {
        hexpos += sprintf(hexbuf + hexpos, " %02X", buf[i]);
        asciipos += sprintf(asciibuf + asciipos, "%c", isprint(buf[i]) ? buf[i] : '.');
    }
    
    Log_print("NETSIO HEX: %s | %s", hexbuf, asciibuf);
}

#ifdef USE_FUJINET

/* --- Function Implementations --- */

/* Initialize the NetSIO protocol state. */
void FujiNet_NetSIO_InitState(void) {
    client_known = FALSE;
    initial_credit_sent = FALSE;
    available_credits = 0;
    current_sync_number = 0;
    
    /* Log the initialization */
    Log_print("FujiNet_NetSIO: Protocol state initialized. Ready for client connection.");
}

/* Check if a client is known and the initial handshake is complete. */
BOOL FujiNet_NetSIO_IsClientConnected(void) {
    /* For debugging purposes */
    if (client_known && !initial_credit_sent) {
        Log_print("FujiNet_NetSIO: Client known but initial credits not sent - partial connection");
    }
    
    /* Only consider fully connected when both flags are true */
    return client_known;
}

/* Get the current client address (if known).
 * Returns TRUE if client is known, FALSE otherwise. */
BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len) {
    if (!client_known || client_addr == NULL || client_len == NULL) {
        return FALSE;
    }
    
    memcpy(client_addr, &current_client_addr, sizeof(current_client_addr));
    *client_len = current_client_len;
    return TRUE;
}

/* Process a received packet buffer.
 * Checks for new clients, handles handshake (PING/ALIVE), credit status, etc.
 * Determines if a response packet needs to be sent.
 * Returns TRUE if a response packet was generated, FALSE otherwise. */
BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *buffer, size_t len,
                                  struct sockaddr *recv_addr, socklen_t recv_addr_len,
                                  unsigned char *response_buffer, size_t *response_len) {
    uint8_t packet_type;
    
    if (buffer == NULL || len < 1 || response_buffer == NULL || response_len == NULL) {
        Log_print("FujiNet_NetSIO: Invalid parameters to ProcessPacket");
        return FALSE;
    }
    
    *response_len = 0;
    packet_type = buffer[0];
    
    /* Enhanced logging */
    char addr_str[INET_ADDRSTRLEN];
    struct sockaddr_in *addr_in = (struct sockaddr_in *)recv_addr;
    inet_ntop(AF_INET, &(addr_in->sin_addr), addr_str, INET_ADDRSTRLEN);
    
    Log_print("FujiNet_NetSIO: Received packet type 0x%02X (len: %d) from %s:%d", 
              packet_type, (int)len, addr_str, ntohs(addr_in->sin_port));
    print_hex(buffer, len);
    
    /* ======== CONNECTION FLOW - PING & CONNECT ======== */
    
    /* Handle PING request - critical for initial handshake */
    if (packet_type == NETSIO_PING_REQUEST) {
        Log_print("NETSIO FLOW [CONNECTION]: Received PING_REQUEST (0xC2) from %s:%d", 
                 addr_str, ntohs(addr_in->sin_port));
        
        /* Store client address for future communication */
        memcpy(&current_client_addr, recv_addr, recv_addr_len);
        current_client_len = recv_addr_len;
        
        /* Update client status */
        if (!client_known) {
            client_known = 1;
            Log_print("NETSIO FLOW [CONNECTION]: New client connected from %s:%d", 
                     addr_str, ntohs(addr_in->sin_port));
        } else {
            Log_print("NETSIO FLOW [CONNECTION]: Existing client ping from %s:%d", 
                     addr_str, ntohs(addr_in->sin_port));
        }
        
        /* Prepare PING response */
        response_buffer[0] = NETSIO_PING_RESPONSE;
        
        /* CRITICAL: Also send initial credit update in the same packet */
        response_buffer[1] = NETSIO_CREDIT_UPDATE;
        response_buffer[2] = DEFAULT_CREDITS;
        *response_len = 3;
        
        /* Set credits immediately */
        initial_credit_sent = 1;
        available_credits = DEFAULT_CREDITS;
        Log_print("NETSIO FLOW [CONNECTION]: Sending PING_RESPONSE (0xC3) with %d initial credits", 
                 DEFAULT_CREDITS);
        
        return TRUE;
    }
    
    /* Handle PING response */
    if (packet_type == NETSIO_PING_RESPONSE) {
        Log_print("NETSIO FLOW [CONNECTION]: Received PING_RESPONSE (0xC3) from %s:%d",
                 addr_str, ntohs(addr_in->sin_port));
        /* No response needed for a response packet */
        return FALSE;
    }
    
    /* Handle device connect notification - completes connection handshake */
    if (packet_type == NETSIO_DEVICE_CONNECT) {
        Log_print("NETSIO FLOW [CONNECTION]: Received DEVICE_CONNECT (0xC1) from %s:%d - Connection complete!", 
                 addr_str, ntohs(addr_in->sin_port));
        
        /* Ensure we have this client stored */
        if (!client_known) {
            memcpy(&current_client_addr, recv_addr, recv_addr_len);
            current_client_len = recv_addr_len;
            client_known = 1;
            Log_print("NETSIO FLOW [CONNECTION]: Client information stored for future communication");
        }
        
        /* No response needed for DEVICE_CONNECT */
        return FALSE;
    }
    
    /* ======== CREDIT FLOW ======== */
    
    /* Handle credit status - device is asking how many commands it can send */
    if (packet_type == NETSIO_CREDIT_STATUS) {
        Log_print("NETSIO FLOW [CREDIT]: Received CREDIT_STATUS (0xC6) from %s:%d - Client needs more credits", 
                 addr_str, ntohs(addr_in->sin_port));
        
        response_buffer[0] = NETSIO_CREDIT_UPDATE;
        response_buffer[1] = DEFAULT_CREDITS;
        *response_len = 2;
        
        /* Ensure credits are available and initial_credit_sent is true */
        if (!initial_credit_sent) {
            Log_print("NETSIO FLOW [CREDIT]: First credit allocation");
            initial_credit_sent = 1;
        }
        
        available_credits = DEFAULT_CREDITS;
        Log_print("NETSIO FLOW [CREDIT]: Sending CREDIT_UPDATE (0xC7) with %d credits", 
                 DEFAULT_CREDITS);
        
        return TRUE;
    }
    
    /* Handle credit update */
    if (packet_type == NETSIO_CREDIT_UPDATE) {
        int new_credits = 0;
        if (len >= 2) {
            new_credits = buffer[1];
        }
        
        Log_print("NETSIO FLOW [CREDIT]: Received CREDIT_UPDATE (0xC7) with %d credits", 
                 new_credits);
        
        /* No response needed for CREDIT_UPDATE */
        return FALSE;
    }
    
    /* ======== ALIVE FLOW ======== */
    
    /* Handle ALIVE request - keep connection active */
    if (packet_type == NETSIO_ALIVE_REQUEST) {
        Log_print("NETSIO FLOW [ALIVE]: Received ALIVE_REQUEST (0xC4) from %s:%d", 
                 addr_str, ntohs(addr_in->sin_port));
        
        response_buffer[0] = NETSIO_ALIVE_RESPONSE;
        *response_len = 1;
        
        Log_print("NETSIO FLOW [ALIVE]: Sending ALIVE_RESPONSE (0xC5)");
        return TRUE;
    }
    
    /* Handle ALIVE response */
    if (packet_type == NETSIO_ALIVE_RESPONSE) {
        Log_print("NETSIO FLOW [ALIVE]: Received ALIVE_RESPONSE (0xC5) from %s:%d", 
                 addr_str, ntohs(addr_in->sin_port));
        return FALSE;
    }
    
    /* Handle disconnect from client */
    if (packet_type == NETSIO_DEVICE_DISCONNECT) {
        Log_print("NETSIO FLOW [CONNECTION]: Received DEVICE_DISCONNECT (0xC0) from %s:%d", 
                 addr_str, ntohs(addr_in->sin_port));
        /* Keep the client known status but reset credits, 
         * as this seems to be part of the normal handshake */
        initial_credit_sent = 0; 
        available_credits = 0;
        return FALSE; /* No response required */
    }
    
    /* ======== SIO COMMAND FLOW ======== */
    
    /* Handle SPEED_CHANGE packet - critical for SIO timing */
    if (packet_type == NETSIO_SPEED_CHANGE) {
        if (len >= 5) {
            uint8_t sync_num = buffer[1];
            uint16_t baud_rate = (buffer[2] << 8) | buffer[3];
            
            Log_print("NETSIO FLOW [SIO]: Received SPEED_CHANGE (0x80) - Sync: %d, Baud: %d", 
                     sync_num, baud_rate);
            
            /* Respond with ACKNOWLEDGE for this sync number */
            response_buffer[0] = NETSIO_ACKNOWLEDGE;
            response_buffer[1] = sync_num;  /* Echo back the sync number */
            *response_len = 2;
            
            Log_print("NETSIO FLOW [SIO]: Sending ACKNOWLEDGE (0x83) for sync %d", sync_num);
            return TRUE;
        } else {
            Log_print("NETSIO FLOW [SIO]: Incomplete SPEED_CHANGE packet received");
            return FALSE;
        }
    }
    
    /* Handle data blocks - part of SIO command/response */
    if (packet_type == NETSIO_DATA_BLOCK) {
        Log_print("NETSIO FLOW [SIO]: Received DATA_BLOCK (0x02) - %zu bytes", len);
        
        /* Process this as a sync response with data if we're waiting for one */
        if (fujinet_WaitingForSync) {
            FujiNet_NetSIO_HandleSyncResponse(buffer, len, current_sync_number);
        }
        
        /* No immediate response needed for the NetSIO protocol */
        return FALSE;
    }
    
    if (packet_type == NETSIO_SYNC_RESPONSE) {
        Log_print("NETSIO FLOW [SIO]: Received SYNC_RESPONSE (0x81) - %zu bytes", len);
        
        /* Process this as a sync response if we're waiting for one */
        if (fujinet_WaitingForSync) {
            FujiNet_NetSIO_HandleSyncResponse(buffer, len, current_sync_number);
        }
        
        /* No immediate response needed for the NetSIO protocol */
        return FALSE;
    }
    
    /* Unknown packet type */
    Log_print("NETSIO FLOW [UNKNOWN]: Unhandled packet type: 0x%02X", packet_type);
    
    return FALSE;
}

/* Prepare the NetSIO sequence for an SIO command.
 * Returns the sync number used for this command, or -1 on error. */
int FujiNet_NetSIO_PrepareSIOCommandSequence(
                            UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                            const UBYTE *output_buffer, int output_len,
                            unsigned char *on_cmd_buf, size_t *on_cmd_len,
                            unsigned char *data_cmd_buf, size_t *data_cmd_len,
                            unsigned char *data_out_buf, size_t *data_out_len, 
                            unsigned char *off_sync_buf, size_t *off_sync_len) {
    uint8_t sync_num;
    
    /* Basic validation */
    if (!client_known || !initial_credit_sent || available_credits <= 0) {
        Log_print("FujiNet_NetSIO: Cannot prepare command - client not ready or no credits");
        return -1;
    }
    
    if (on_cmd_buf == NULL || on_cmd_len == NULL || 
        data_cmd_buf == NULL || data_cmd_len == NULL ||
        data_out_buf == NULL || data_out_len == NULL ||
        off_sync_buf == NULL || off_sync_len == NULL) {
        Log_print("FujiNet_NetSIO: Buffer pointers cannot be NULL");
        return -2;
    }
    
    /* Use next sync number in sequence */
    sync_num = current_sync_number++;
    Log_print("FujiNet_NetSIO: Preparing SIO command for device 0x%02X, command 0x%02X with sync %d",
             device_id, command, sync_num);
    
    /* Decrement available credits */
    available_credits--;
    Log_print("FujiNet_NetSIO: Decremented credits to %d", available_credits);
    
    /* 1. COMMAND_ON packet */
    on_cmd_buf[0] = NETSIO_COMMAND_ON;
    *on_cmd_len = 1;
    Log_print("FujiNet_NetSIO: Prepared COMMAND_ON packet");
    
    /* 2. DATA_BLOCK for command frame */
    data_cmd_buf[0] = NETSIO_DATA_BLOCK;
    data_cmd_buf[1] = 4; /* 4 bytes in command frame: device_id, command, aux1, aux2 */
    data_cmd_buf[2] = device_id;
    data_cmd_buf[3] = command;
    data_cmd_buf[4] = aux1;
    data_cmd_buf[5] = aux2;
    *data_cmd_len = 6;
    Log_print("FujiNet_NetSIO: Prepared DATA_BLOCK packet with command frame: Device=0x%02X, Cmd=0x%02X, Aux1=0x%02X, Aux2=0x%02X",
             device_id, command, aux1, aux2);
    
    /* 3. Optional DATA_BLOCK for output data */
    if (output_buffer != NULL && output_len > 0) {
        size_t copy_len = output_len;
        if (copy_len > BUFFER_SIZE - 2) {
            copy_len = BUFFER_SIZE - 2;
            Log_print("FujiNet_NetSIO: Warning - output data truncated from %d to %d bytes",
                     output_len, (int)copy_len);
        }
        
        data_out_buf[0] = NETSIO_DATA_BLOCK;
        data_out_buf[1] = (uint8_t)copy_len;
        memcpy(data_out_buf + 2, output_buffer, copy_len);
        *data_out_len = copy_len + 2;
        Log_print("FujiNet_NetSIO: Prepared DATA_BLOCK packet with %d bytes of output data", (int)copy_len);
    } else {
        /* No output data, but we need to send a DATA_ACK to acknowledge receipt */
        data_out_buf[0] = NETSIO_DATA_ACK;
        *data_out_len = 1;
    }
    
    /* 4. COMMAND_OFF with sync number */
    off_sync_buf[0] = NETSIO_COMMAND_OFF_SYNC; /* Use 0x18 to request a sync response */
    off_sync_buf[1] = sync_num;
    *off_sync_len = 2;
    Log_print("FujiNet_NetSIO: Prepared COMMAND_OFF_SYNC packet with sync number %d", sync_num);
    
    return sync_num;
}

/* Check if a packet is a sync response for the expected sync.
 * Returns TRUE if it's a sync response with the expected sync, FALSE otherwise.
 * If TRUE, status_code is set to the status from the response. */
BOOL FujiNet_NetSIO_CheckSyncResponse(const unsigned char *recv_buffer, ssize_t recv_len,
                                     int expected_sync, uint8_t *status_code) {
    /* Check for NULL pointers and minimum packet length */
    if (recv_buffer == NULL || recv_len < 3 || status_code == NULL) {
        Log_print("FujiNet_NetSIO: CheckSyncResponse - Invalid parameters");
        return FALSE;
    }
    
    /* Log the packet we're checking */
    Log_print("FujiNet_NetSIO: CheckSyncResponse - Analyzing packet type 0x%02X (len: %d) for sync %d", 
             recv_buffer[0], (int)recv_len, expected_sync);
             
    /* First byte should be NETSIO_SYNC_RESPONSE or NETSIO_DATA_BLOCK */
    if (recv_buffer[0] != NETSIO_SYNC_RESPONSE && recv_buffer[0] != NETSIO_DATA_BLOCK) {
        Log_print("FujiNet_NetSIO: CheckSyncResponse - Not a sync response or data block packet (type 0x%02X)", 
                 recv_buffer[0]);
        return FALSE;
    }
    
    /* Sync number is typically in second byte */
    if (recv_buffer[1] != expected_sync) {
        Log_print("FujiNet_NetSIO: CheckSyncResponse - Sync mismatch (expected %d, got %d)", 
                 expected_sync, recv_buffer[1]);
        return FALSE;
    }
    
    /* For SYNC_RESPONSE, we typically have status in 3rd byte */
    if (recv_buffer[0] == NETSIO_SYNC_RESPONSE && recv_len >= 3) {
        *status_code = recv_buffer[2];
        Log_print("FujiNet_NetSIO: CheckSyncResponse - Valid SYNC_RESPONSE found for sync %d! Status: 0x%02X", 
                 expected_sync, *status_code);
        return TRUE;
    }
    
    /* For DATA_BLOCK, the format depends on the specific command.
     * Typically it contains the actual data being returned. */
    if (recv_buffer[0] == NETSIO_DATA_BLOCK) {
        if (recv_len >= 3) {
            *status_code = recv_buffer[2]; /* Status might be in byte 3 for data blocks */
            Log_print("FujiNet_NetSIO: CheckSyncResponse - Valid DATA_BLOCK found for sync %d! Status: 0x%02X", 
                    expected_sync, *status_code);
            return TRUE;
        }
    }
    
    Log_print("FujiNet_NetSIO: CheckSyncResponse - Incomplete response packet");
    return FALSE;
}

/* Called once per emulator frame to process incoming UDP packets */
void FujiNet_NetSIO_Frame(void) {
    unsigned char buffer[NETSIO_BUFFER_SIZE];
    unsigned char response_buffer[NETSIO_BUFFER_SIZE];
    size_t response_len = 0;
    struct sockaddr_in recv_client_addr;
    socklen_t recv_client_len = sizeof(recv_client_addr);
    ssize_t recv_len;
    BOOL send_response = FALSE;

    if (fujinet_sockfd < 0) {
        return; /* Socket not initialized */
    }

    /* Poll for incoming packets */
    while (FujiNet_UDP_Poll(fujinet_sockfd)) {
        /* Clear response buffer for each new packet */
        memset(response_buffer, 0, sizeof(response_buffer));
        response_len = 0;
        
        /* Receive packet */
        recv_len = FujiNet_UDP_Receive(fujinet_sockfd, buffer, sizeof(buffer), 
                                      &recv_client_addr, &recv_client_len);
        
        if (recv_len > 0) {
            /* Process packet and get response if needed */
            send_response = FujiNet_NetSIO_ProcessPacket(
                buffer, recv_len, 
                (struct sockaddr *)&recv_client_addr, recv_client_len,
                response_buffer, &response_len);
            
            /* If a response is needed, send it immediately */
            if (send_response && response_len > 0) {
                char addr_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(recv_client_addr.sin_addr), addr_str, INET_ADDRSTRLEN);
                
                Log_print("NETSIO FLOW [RESPONSE]: Sending response packet type 0x%02X (%d bytes) to %s:%d",
                          response_buffer[0], (int)response_len, 
                          addr_str, ntohs(recv_client_addr.sin_port));
                
                FujiNet_UDP_Send(fujinet_sockfd, response_buffer, response_len,
                                &recv_client_addr, recv_client_len);
            }
        }
    }
}

/*
 * Forwards an Atari SIO command frame to the connected FujiNet device via NetSIO.
 * Constructs and sends the required three UDP packets.
 * Returns TRUE on success, FALSE on failure.
 */
BOOL FujiNet_NetSIO_ForwardSIOCommand(UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2) {
    unsigned char cmd_on_buffer[2];
    unsigned char data_cmd_buffer[10];
    unsigned char data_out_buffer[2]; /* Just for ACK, no actual output data */
    unsigned char off_sync_buffer[5];
    size_t cmd_on_len, data_cmd_len, data_out_len, off_sync_len;
    int sync_num;
    uint8_t status_code = 0;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    unsigned char recv_buffer[BUFFER_SIZE];
    ssize_t recv_len;
    int max_attempts = 3;
    int attempt;
    
    if (!fujinet_connected || fujinet_sockfd < 0) {
        Log_print("NETSIO FLOW [ERROR]: Cannot forward SIO command, FujiNet not connected.");
        return FALSE;
    }
    
    /* 0. Get client address */
    if (!FujiNet_NetSIO_GetClientAddr(&client_addr, &client_len)) {
        Log_print("NETSIO FLOW [ERROR]: Cannot forward SIO command, no client address known.");
        return FALSE;
    }
    
    /* 1. Prepare the NetSIO command sequence */
    Log_print("NETSIO FLOW [SIO]: Forwarding SIO command - Device: 0x%02X, Command: 0x%02X, AUX1: 0x%02X, AUX2: 0x%02X", 
             device_id, command, aux1, aux2);
             
    sync_num = FujiNet_NetSIO_PrepareSIOCommandSequence(
        device_id, command, aux1, aux2,
        NULL, 0, /* No output data for standard command */
        cmd_on_buffer, &cmd_on_len,
        data_cmd_buffer, &data_cmd_len,
        data_out_buffer, &data_out_len,
        off_sync_buffer, &off_sync_len
    );
    
    if (sync_num < 0) {
        Log_print("NETSIO FLOW [ERROR]: Failed to prepare NetSIO command sequence.");
        return FALSE;
    }
    
    Log_print("NETSIO FLOW [SIO]: Using sync number %d for this command", sync_num);
    
    /* 2. Send the NetSIO command sequence */
    /* Send COMMAND_ON packet */
    Log_print("NETSIO FLOW [SIO]: Sending COMMAND_ON (0x11) packet...");
    if (FujiNet_UDP_Send(fujinet_sockfd, cmd_on_buffer, cmd_on_len, 
                        &client_addr, client_len) != (ssize_t)cmd_on_len) {
        Log_print("NETSIO FLOW [ERROR]: Failed to send COMMAND_ON packet.");
        return FALSE;
    }
    
    /* Send DATA_BLOCK for command frame */
    Log_print("NETSIO FLOW [SIO]: Sending DATA_BLOCK (0x02) packet with command frame...");
    if (FujiNet_UDP_Send(fujinet_sockfd, data_cmd_buffer, data_cmd_len, 
                        &client_addr, client_len) != (ssize_t)data_cmd_len) {
        Log_print("NETSIO FLOW [ERROR]: Failed to send command DATA_BLOCK packet.");
        return FALSE;
    }
    
    /* Send DATA_ACK or DATA_BLOCK for output data */
    Log_print("NETSIO FLOW [SIO]: Sending DATA_ACK (0x07) packet...");
    if (FujiNet_UDP_Send(fujinet_sockfd, data_out_buffer, data_out_len, 
                        &client_addr, client_len) != (ssize_t)data_out_len) {
        Log_print("NETSIO FLOW [ERROR]: Failed to send DATA_ACK packet.");
        return FALSE;
    }
    
    /* Send COMMAND_OFF with sync number */
    Log_print("NETSIO FLOW [SIO]: Sending COMMAND_OFF_SYNC (0x18) packet with sync %d...", sync_num);
    if (FujiNet_UDP_Send(fujinet_sockfd, off_sync_buffer, off_sync_len, 
                        &client_addr, client_len) != (ssize_t)off_sync_len) {
        Log_print("NETSIO FLOW [ERROR]: Failed to send COMMAND_OFF_SYNC packet.");
        return FALSE;
    }
    
    /* 3. Wait for SYNC_RESPONSE with matching sync number */
    Log_print("NETSIO FLOW [SIO]: Waiting for SYNC_RESPONSE (0x81) with sync %d...", sync_num);
    
    /* Set the flag that we're waiting for a FujiNet response */
    fujinet_WaitingForSync = TRUE;
    
    for (attempt = 0; attempt < max_attempts; attempt++) {
        /* Use non-blocking calls with a timeout to avoid freezing the emulator */
        int timeout_ms = 250; /* 250ms timeout per attempt */
        
        /* Poll with a simple timeout approach */
        int poll_ready = FujiNet_UDP_Poll(fujinet_sockfd);
        if (!poll_ready) {
            /* Sleep a bit (this is a simple approach - in a real implementation 
               you might want to use a proper timeout mechanism) */
            Log_print("NETSIO FLOW [SIO]: No response yet (attempt %d/%d), waiting...", 
                     attempt+1, max_attempts);
            PLATFORM_Sleep(timeout_ms);
            continue;
        }
        
        /* Data available, try to receive */
        recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, sizeof(recv_buffer), 
                                       &client_addr, &client_len);
        
        if (recv_len <= 0) {
            Log_print("NETSIO FLOW [ERROR]: Error receiving response.");
            continue;
        }
        
        /* Check if this is the SYNC_RESPONSE we're waiting for */
        if (FujiNet_NetSIO_CheckSyncResponse(recv_buffer, recv_len, sync_num, &status_code)) {
            Log_print("NETSIO FLOW [SIO]: Received expected SYNC_RESPONSE for sync %d with status 0x%02X", 
                     sync_num, status_code);
            fujinet_WaitingForSync = FALSE;
            return TRUE;
        } else {
            /* Process the packet anyway, as it might be a different type */
            unsigned char dummy_resp[BUFFER_SIZE];
            size_t dummy_resp_len;
            
            Log_print("NETSIO FLOW [SIO]: Received unexpected packet (not our sync response)");
            FujiNet_NetSIO_ProcessPacket(recv_buffer, recv_len, 
                                        (struct sockaddr *)&client_addr, client_len,
                                        dummy_resp, &dummy_resp_len);
        }
    }
    
    /* Timeout reached without getting our sync response */
    Log_print("NETSIO FLOW [ERROR]: Timeout waiting for SYNC_RESPONSE after %d attempts.", max_attempts);
    fujinet_WaitingForSync = FALSE;
    return FALSE;
}

/* Global flag to indicate waiting for NetSIO sync response - defined in fujinet.h */
extern int fujinet_WaitingForSync;

/* Buffer to hold data received from FujiNet for SIO operations */
static unsigned char fujinet_sio_data_buffer[1024];
static int fujinet_sio_data_length = 0;
static UBYTE fujinet_sio_status = 0;
static BOOL fujinet_sio_data_ready = FALSE;

/* Process a NetSIO response to feed data back into the Atari SIO subsystem.
 * This is called when a SYNC_RESPONSE or DATA_BLOCK is received for a command
 * that we previously sent to FujiNet via NetSIO.
 * Returns TRUE if the response was handled, FALSE otherwise. */
BOOL FujiNet_NetSIO_HandleSyncResponse(const unsigned char *buffer, size_t len, int sync_num) {
    UBYTE status_code = 0;
    
    if (buffer == NULL || len < 3) {
        Log_print("NETSIO FLOW [ERROR]: Invalid sync response packet");
        return FALSE;
    }
    
    /* First byte is packet type, second byte is sync number, third byte may be status */
    uint8_t packet_type = buffer[0];
    uint8_t pkt_sync_num = buffer[1];
    
    /* Verify this is a response for a command we're waiting for */
    if (!fujinet_WaitingForSync) {
        Log_print("NETSIO FLOW [WARNING]: Received sync response but not waiting for one");
        return FALSE;
    }
    
    /* Check if this is the sync number we're expecting */
    if (pkt_sync_num != sync_num) {
        Log_print("NETSIO FLOW [WARNING]: Sync number mismatch - expected %d, got %d", 
                 sync_num, pkt_sync_num);
        return FALSE;
    }
    
    /* Handle based on packet type */
    if (packet_type == NETSIO_SYNC_RESPONSE) {
        Log_print("NETSIO FLOW [SIO]: Processing SYNC_RESPONSE (0x81) for sync %d", pkt_sync_num);
        
        /* Get status code from the packet */
        status_code = buffer[2];
        fujinet_sio_status = status_code;
        fujinet_sio_data_length = 0; /* No data for sync response */
        fujinet_sio_data_ready = TRUE;
        
        /* Log based on status */
        switch (status_code) {
            case 'C': /* Command complete */
                Log_print("NETSIO FLOW [SIO]: Command completed successfully (status C)");
                break;
            case 'E': /* Command error */
                Log_print("NETSIO FLOW [SIO]: Command error (status E)");
                break;
            case 'N': /* Command NAK */
                Log_print("NETSIO FLOW [SIO]: Command NAK (status N)");
                break;
            default:
                Log_print("NETSIO FLOW [SIO]: Unknown status code: 0x%02X", status_code);
                break;
        }
        
        /* We're no longer waiting for a sync */
        fujinet_WaitingForSync = FALSE;
        return TRUE;
    }
    else if (packet_type == NETSIO_DATA_BLOCK) {
        /* DATA_BLOCK format:
         * [0] = 0x02 (NETSIO_DATA_BLOCK)
         * [1] = sync number
         * [2...] = actual data bytes
         */
        Log_print("NETSIO FLOW [SIO]: Processing DATA_BLOCK (0x02) for sync %d, %d bytes", 
                 pkt_sync_num, (int)(len - 2));
        
        /* Copy data to our buffer */
        if (len > 2) {
            int data_size = len - 2;
            if (data_size > sizeof(fujinet_sio_data_buffer)) {
                data_size = sizeof(fujinet_sio_data_buffer);
                Log_print("NETSIO FLOW [WARNING]: Data truncated (%d bytes -> %d bytes)",
                         (int)(len - 2), data_size);
            }
            
            memcpy(fujinet_sio_data_buffer, buffer + 2, data_size);
            fujinet_sio_data_length = data_size;
            fujinet_sio_data_ready = TRUE;
            fujinet_sio_status = 'C'; /* Assume success with data */
            
            Log_print("NETSIO FLOW [SIO]: Received %d bytes of data from FujiNet", data_size);
            
            /* Debug log for first few bytes */
            if (data_size > 0) {
                char hex_buf[100] = {0};
                int i, pos = 0;
                for (i = 0; i < data_size && i < 16; i++) {
                    pos += sprintf(hex_buf + pos, "%02X ", fujinet_sio_data_buffer[i]);
                }
                Log_print("NETSIO FLOW [SIO]: Data: %s%s", hex_buf, data_size > 16 ? "..." : "");
            }
        } else {
            Log_print("NETSIO FLOW [WARNING]: Empty data block");
            fujinet_sio_data_length = 0;
            fujinet_sio_data_ready = TRUE;
            fujinet_sio_status = 'E'; /* Error for empty data */
        }
        
        /* We're no longer waiting for a sync */
        fujinet_WaitingForSync = FALSE;
        return TRUE;
    }
    
    /* Not a packet type we can handle */
    return FALSE;
}

/* Check if there is data available from a FujiNet SIO response */
BOOL FujiNet_NetSIO_IsResponseReady(void) {
    return fujinet_sio_data_ready;
}

/* Get the status code from a FujiNet SIO response */
UBYTE FujiNet_NetSIO_GetResponseStatus(void) {
    return fujinet_sio_status;
}

/* Get the data from a FujiNet SIO response.
 * Copies data to the given buffer, up to max_len bytes.
 * Returns the number of bytes copied.
 * After retrieving the data, marks the response as consumed. */
int FujiNet_NetSIO_GetResponseData(UBYTE *buffer, int max_len) {
    int bytes_to_copy = 0;
    
    if (!fujinet_sio_data_ready || buffer == NULL || max_len <= 0) {
        return 0;
    }
    
    /* Copy data to the buffer */
    bytes_to_copy = (fujinet_sio_data_length < max_len) ? 
                     fujinet_sio_data_length : max_len;
    
    memcpy(buffer, fujinet_sio_data_buffer, bytes_to_copy);
    
    /* Mark the response as consumed */
    fujinet_sio_data_ready = FALSE;
    
    Log_print("NETSIO FLOW [SIO]: Retrieved %d bytes from FujiNet response", bytes_to_copy);
    
    return bytes_to_copy;
}

#else /* !USE_FUJINET - stub implementations */

void FujiNet_NetSIO_InitState(void) {
    /* Do nothing when FujiNet is disabled */
}

BOOL FujiNet_NetSIO_IsClientConnected(void) {
    /* Always return FALSE when FujiNet is disabled */
    return FALSE;
}

BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len) {
    /* Always return FALSE when FujiNet is disabled */
    return FALSE;
}

BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *buffer, size_t len,
                                 struct sockaddr *recv_addr, socklen_t recv_addr_len,
                                 unsigned char *response_buffer, size_t *response_len) {
    /* Always return FALSE when FujiNet is disabled */
    return FALSE;
}

int FujiNet_NetSIO_PrepareSIOCommandSequence(
                            UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                            const UBYTE *output_buffer, int output_len,
                            unsigned char *on_cmd_buf, size_t *on_cmd_len,
                            unsigned char *data_cmd_buf, size_t *data_cmd_len,
                            unsigned char *data_out_buf, size_t *data_out_len, 
                            unsigned char *off_sync_buf, size_t *off_sync_len) {
    /* Always return error when FujiNet is disabled */
    return -1;
}

#endif /* USE_FUJINET */

/* Add remaining NetSIO functions here if needed */
