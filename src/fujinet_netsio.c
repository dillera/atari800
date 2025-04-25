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
#include <time.h>    /* For time() */

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

/* --- Static Variables --- */
int available_credits = NETSIO_DEFAULT_CREDITS; /* Our available send credits - global */
static struct sockaddr_in netsio_client_addr;
static socklen_t netsio_client_addr_len = 0;
static int netsio_client_connected = FALSE;
static int current_sync_num = 0;         /* Sync number counter */
static time_t last_packet_time = 0;      /* Time of last packet received */
static int client_credits = 0;           /* Credits reported by the client */

/* State for handling SIO responses */
static BOOL response_ready = FALSE;
static unsigned char response_data_buffer[1024];
static int response_data_len = 0;

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

/* Add includes if not already present */
#include "fujinet.h"
#include "fujinet_udp.h"
#include "sio.h" /* For SIO_ChkSum - needed for checksum calc if done here */

/* Static variable to track the NetSIO sync number */
static UBYTE netsio_sync_number = 0;

/* Assume these exist in fujinet.c/h and are properly linked/visible */
extern int fujinet_sockfd;
extern struct sockaddr_in fujinet_client_addr;
extern socklen_t fujinet_client_len;
extern BOOL fujinet_connected;

/* --- Function Implementations --- */

/* Initialize the NetSIO protocol state. */
void FujiNet_NetSIO_InitState(void) {
    netsio_client_connected = FALSE;
    available_credits = NETSIO_DEFAULT_CREDITS;
    current_sync_num = 0;
    
    /* Log the initialization */
    Log_print("FujiNet_NetSIO: Protocol state initialized. Ready for client connection.");
}

/* Check if a client is known and the initial handshake is complete. */
BOOL FujiNet_NetSIO_IsClientConnected(void) {
    if (!netsio_client_connected) {
        return FALSE;
    }
    if (available_credits <= 0) {
        return FALSE;
    }
    return TRUE;
}

/* Get the current client address (if known).
 * Returns TRUE if client is known, FALSE otherwise. */
BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len) {
    if (!netsio_client_connected || client_addr == NULL || client_len == NULL) {
        return FALSE;
    }
    
    memcpy(client_addr, &netsio_client_addr, sizeof(netsio_client_addr));
    *client_len = netsio_client_addr_len;
    return TRUE;
}

/* Process a received packet buffer.
 * Checks for new clients, handles handshake (PING/ALIVE), credit status, etc.
 * Determines if a response packet needs to be sent.
 * Returns TRUE if a response packet was generated, FALSE otherwise. */
BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *buffer, size_t len,
                                  struct sockaddr *recv_addr, socklen_t recv_addr_len,
                                  unsigned char *response_buffer, size_t *response_len)
{
    BOOL response_generated = FALSE;
    uint8_t packet_type = buffer[0];
    char addr_str[INET_ADDRSTRLEN];

    /* Clear response buffer initially */
    *response_len = 0;

    inet_ntop(AF_INET, &(((struct sockaddr_in *)recv_addr)->sin_addr), addr_str, sizeof(addr_str));

    /* Log incoming packet */
    if (packet_type != NETSIO_CREDIT_STATUS && packet_type != NETSIO_CREDIT_UPDATE) {
        /* Avoid excessive logging for common credit management packets */
        Log_print("NetSIO: Received packet type 0x%02X (%d bytes) from %s:%d",
                packet_type, (int)len, addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        print_hex(buffer, len);
    }
    
    /* ======== CONNECTION FLOW - PING & CONNECT ======== */
    
    /* Handle PING request - critical for initial handshake */
    if (packet_type == NETSIO_PING_REQUEST) {
        Log_print("NETSIO FLOW [CONNECTION]: Received PING_REQUEST (0xC2) from %s:%d", 
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));

        /* Store client address for future communication - USE GLOBAL VARS */
        memcpy(&netsio_client_addr, recv_addr, recv_addr_len); // Use global netsio_client_addr
        netsio_client_addr_len = recv_addr_len;                   // Use global netsio_client_addr_len

        /* Update client status */
        if (!netsio_client_connected) {
            netsio_client_connected = 1;
            extern int fujinet_connected; /* Declare extern to access main module variable */
            fujinet_connected = TRUE;    /* Update main module connection status */
            
            /* Copy client address to the main FujiNet module's global variables */
            extern struct sockaddr_in fujinet_client_addr;
            extern socklen_t fujinet_client_len;
            memcpy(&fujinet_client_addr, recv_addr, recv_addr_len);
            fujinet_client_len = recv_addr_len;
            Log_print("NETSIO FLOW [CONNECTION]: Copied client address to main FujiNet module");
            
            Log_print("NETSIO FLOW [CONNECTION]: New client connected from %s:%d", 
                     addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        } else {
            /* Even for existing connections, update the address in case it changed */
            extern struct sockaddr_in fujinet_client_addr;
            extern socklen_t fujinet_client_len;
            memcpy(&fujinet_client_addr, recv_addr, recv_addr_len);
            fujinet_client_len = recv_addr_len;
            Log_print("NETSIO FLOW [CONNECTION]: Updated client address in main FujiNet module");
            
            Log_print("NETSIO FLOW [CONNECTION]: Existing client ping from %s:%d", 
                     addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        }

        /* Prepare PING response + initial CREDIT_UPDATE (200 credits) */
        response_buffer[0] = NETSIO_PING_RESPONSE;
        response_buffer[1] = NETSIO_CREDIT_UPDATE;
        response_buffer[2] = 0xC8; // Low byte (200)
        response_buffer[3] = 0x00; // High byte
        *response_len = 4;

        /* Set credits immediately */
        available_credits = 200;
        Log_print("NETSIO FLOW [CONNECTION]: Sending PING_RESPONSE (0xC3) with 200 initial credits");
        
        /* Update timestamp for client activity */
        last_packet_time = time(NULL);

        response_generated = TRUE;  /* Signal that we have a response to send */
        return TRUE; /* Explicitly return TRUE to ensure response is sent */
    }
    
    /* Handle PING response */
    if (packet_type == NETSIO_PING_RESPONSE) {
        Log_print("NETSIO FLOW [CONNECTION]: Received PING_RESPONSE (0xC3) from %s:%d",
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        /* No response needed for a response packet */
        return FALSE;
    }
    
    /* Handle device connect notification - completes connection handshake */
    if (packet_type == NETSIO_DEVICE_CONNECT) {
        Log_print("NETSIO FLOW [CONNECTION]: Received DEVICE_CONNECT (0xC1) from %s:%d - Handshake complete!", 
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        
        /* Ensure we have this client stored */
        if (!netsio_client_connected) {
            memcpy(&netsio_client_addr, recv_addr, recv_addr_len); // Use global netsio_client_addr
            netsio_client_addr_len = recv_addr_len;                   // Use global netsio_client_addr_len
            netsio_client_connected = 1;
            extern int fujinet_connected; /* Declare extern to access main module variable */
            fujinet_connected = TRUE;    /* Update main module connection status */
            
            /* Copy client address to the main FujiNet module's global variables */
            extern struct sockaddr_in fujinet_client_addr;
            extern socklen_t fujinet_client_len;
            memcpy(&fujinet_client_addr, recv_addr, recv_addr_len);
            fujinet_client_len = recv_addr_len;
            Log_print("NETSIO FLOW [CONNECTION]: Copied client address to main FujiNet module");
            
            Log_print("NETSIO FLOW [CONNECTION]: Client information stored for future communication");
        }
        
        /* No response needed for DEVICE_CONNECT */
        return FALSE;
    }
    
    /* ======== CREDIT FLOW ======== */
    
    /* Handle credit status - device is asking how many commands it can send */
    if (packet_type == NETSIO_CREDIT_STATUS) {
        Log_print("NETSIO FLOW [CREDIT]: Received CREDIT_STATUS (0xC6) from %s:%d - Client needs more credits", 
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));

        /* Prepare CREDIT_UPDATE response */
        response_buffer[0] = NETSIO_CREDIT_UPDATE; // 0xC7
        response_buffer[1] = 0xC8; // Low byte (200)
        response_buffer[2] = 0x00; // High byte
        *response_len = 3;

        /* Optionally update internal credit state */
        available_credits += 200;
        Log_print("NETSIO FLOW [CREDIT]: Sent CREDIT_UPDATE (0xC7) granting 200 credits");
        response_generated = TRUE;
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
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        Log_print("NETSIO FLOW [ALIVE]: Setting ALIVE received flag (for debug)");
        /* Optionally set a static flag for ALIVE seen, for future handshake tracking */
        response_buffer[0] = NETSIO_ALIVE_RESPONSE;
        *response_len = 1;
        
        Log_print("NETSIO FLOW [ALIVE]: Sending ALIVE_RESPONSE (0xC5)");
        response_generated = TRUE;
    }
    
    /* Handle ALIVE response */
    if (packet_type == NETSIO_ALIVE_RESPONSE) {
        Log_print("NETSIO FLOW [ALIVE]: Received ALIVE_RESPONSE (0xC5) from %s:%d", 
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        return FALSE;
    }
    
    /* Handle disconnect from client */
    if (packet_type == NETSIO_DEVICE_DISCONNECT) {
        Log_print("NETSIO FLOW [CONNECTION]: Received DEVICE_DISCONNECT (0xC0) from %s:%d", 
                 addr_str, ntohs(((struct sockaddr_in *)recv_addr)->sin_port));
        /* Keep the client known status but reset credits, 
         * as this seems to be part of the normal handshake */
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
            response_generated = TRUE;
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
            FujiNet_NetSIO_HandleSyncResponse(buffer, len, current_sync_num);
        }
        
        /* No immediate response needed for the NetSIO protocol */
        return FALSE;
    }
    
    if (packet_type == NETSIO_SYNC_RESPONSE) {
        Log_print("NETSIO FLOW [SIO]: Received SYNC_RESPONSE (0x81) - %zu bytes", len);
        
        /* Process this as a sync response if we're waiting for one */
        if (fujinet_WaitingForSync) {
            FujiNet_NetSIO_HandleSyncResponse(buffer, len, current_sync_num);
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
                            unsigned char *off_sync_buf, size_t *off_sync_len)
{
    int sync_num = -1;

    /* Check if we have credits to send */
    if (available_credits <= 0) {
        Log_print("NetSIO: No credits available to send SIO command.");
        return -1;
    }

    /* Check if client is connected */
    if (!netsio_client_connected) {
        Log_print("NetSIO: Client not connected.");
        return -1;
    }

    /* Use next sync number in sequence */
    sync_num = current_sync_num++;
    Log_print("FujiNet_NetSIO: Preparing SIO command for device 0x%02X, command 0x%02X with sync %d",
             device_id, command, sync_num);
    
    // In the SIO command send path, decrement available_credits only if sendto/FujiNet_UDP_Send returns expected length
    // We'll patch this in FujiNet_SendSIOCommand and FujiNet_ProcessSIO, but here is the correct pattern:
    // ssize_t sent_len = FujiNet_UDP_Send(...);
    // if (sent_len == expected_len) {
    //     available_credits--;
    // } else {
    //     Log_print("ERROR: sendto() failed: %s (errno=%d)", strerror(errno), errno);
    // }
    // (Apply this in all SIO command send logic)
    
    /* Decrement available credits */
    available_credits--;
    Log_print("FujiNet_NetSIO: Decremented credits to %d", available_credits);
    
    /* 1. COMMAND_ON packet */
    on_cmd_buf[0] = NETSIO_COMMAND_ON;
    *on_cmd_len = 1;
    Log_print("FujiNet_NetSIO: Prepared COMMAND_ON packet");
    
    /* 2. DATA_BLOCK for command frame */
    data_cmd_buf[0] = NETSIO_DATA_BLOCK;
    data_cmd_buf[1] = 5; /* 5 bytes in command frame: device_id, command, aux1, aux2, 0xFF */
    data_cmd_buf[2] = device_id;
    data_cmd_buf[3] = command;
    data_cmd_buf[4] = aux1;
    data_cmd_buf[5] = aux2;
    data_cmd_buf[6] = 0xFF; /* Extra byte required by FujiNet implementation */
    *data_cmd_len = 7;
    Log_print("FujiNet_NetSIO: Prepared DATA_BLOCK packet with command frame: Device=0x%02X, Cmd=0x%02X, Aux1=0x%02X, Aux2=0x%02X",
             device_id, command, aux1, aux2);
    
    /* 3. Optional DATA_BLOCK for output data */
    if (output_buffer != NULL && output_len > 0) {
        size_t copy_len = output_len;
        if (copy_len > BUFFER_SIZE - 3) { /* -3 to leave room for header and the extra 0xFF byte */
            copy_len = BUFFER_SIZE - 3;
            Log_print("FujiNet_NetSIO: Warning - output data truncated from %d to %d bytes",
                     output_len, (int)copy_len);
        }
        
        data_out_buf[0] = NETSIO_DATA_BLOCK;
        data_out_buf[1] = (uint8_t)copy_len;
        memcpy(data_out_buf + 2, output_buffer, copy_len);
        data_out_buf[copy_len + 2] = 0xFF; /* Extra byte required by FujiNet implementation */
        *data_out_len = copy_len + 3;
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
        Log_print("NetSIO: CheckSyncResponse - Invalid parameters (len %d)", (int)recv_len);
        return FALSE;
    }
    
    /* Log the packet we're checking */
    /* Log_print("NetSIO: CheckSyncResponse - Analyzing packet type 0x%02X (len: %d) for sync %d", 
             recv_buffer[0], (int)recv_len, expected_sync); */
             
    /* First byte should be NETSIO_SYNC_RESPONSE or NETSIO_DATA_BLOCK */
    if (recv_buffer[0] != NETSIO_SYNC_RESPONSE && recv_buffer[0] != NETSIO_DATA_BLOCK) {
        /* Log_print("NetSIO: CheckSyncResponse - Not a sync response or data block packet (type 0x%02X)", 
                 recv_buffer[0]); */
        return FALSE;
    }
    
    /* Sync number is typically in second byte */
    if (recv_buffer[1] != expected_sync) {
        /* Log_print("NetSIO: CheckSyncResponse - Sync mismatch (expected %d, got %d)", 
                 expected_sync, recv_buffer[1]); */
        return FALSE;
    }
    
    /* Status code is typically in third byte */
    *status_code = recv_buffer[2];
    /* Log_print("NetSIO: CheckSyncResponse - Valid response found for sync %d! Type: 0x%02X Status: 0x%02X", 
             expected_sync, recv_buffer[0], *status_code); */
    return TRUE;

    /* Old logic split by type:
    if (recv_buffer[0] == NETSIO_SYNC_RESPONSE) {
        *status_code = recv_buffer[2];
        Log_print("NetSIO: CheckSyncResponse - Valid SYNC_RESPONSE found for sync %d! Status: 0x%02X", 
                 expected_sync, *status_code);
        return TRUE;
    }
    
    if (recv_buffer[0] == NETSIO_DATA_BLOCK) {
         *status_code = recv_buffer[2]; // Status might be in byte 3 for data blocks
         Log_print("NetSIO: CheckSyncResponse - Valid DATA_BLOCK found for sync %d! Status: 0x%02X", 
                 expected_sync, *status_code);
        return TRUE;
    }
    
    Log_print("NetSIO: CheckSyncResponse - Incomplete or unexpected response packet type 0x%02X", recv_buffer[0]);
    return FALSE;
    */
}

/* Handle a received NetSIO sync response (SYNC_RESPONSE or DATA_BLOCK)
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
        response_data_len = 0; /* No data for sync response */
        response_ready = TRUE;
        
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
            if (data_size > sizeof(response_data_buffer)) {
                data_size = sizeof(response_data_buffer);
                Log_print("NETSIO FLOW [WARNING]: Data truncated (%d bytes -> %d bytes)",
                         (int)(len - 2), data_size);
            }
            
            memcpy(response_data_buffer, buffer + 2, data_size);
            response_data_len = data_size;
            response_ready = TRUE;
            status_code = 'C'; /* Assume success with data */
            
            Log_print("NETSIO FLOW [SIO]: Received %d bytes of data from FujiNet", data_size);
            
            /* Debug log for first few bytes */
            if (data_size > 0) {
                char hex_buf[100] = {0};
                int i, pos = 0;
                for (i = 0; i < data_size && i < 16; i++) {
                    pos += sprintf(hex_buf + pos, "%02X ", response_data_buffer[i]);
                }
                Log_print("NETSIO FLOW [SIO]: Data: %s%s", hex_buf, data_size > 16 ? "..." : "");
            }
        } else {
            Log_print("NETSIO FLOW [WARNING]: Empty data block");
            response_data_len = 0;
            response_ready = TRUE;
            status_code = 'E'; /* Error for empty data */
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
    return response_ready;
}

/* Get the status code from a FujiNet SIO response */
UBYTE FujiNet_NetSIO_GetResponseStatus(void) {
    return 0; // TODO: Return actual status code
}

/* Get the data from a FujiNet SIO response.
 * Copies data to the given buffer, up to max_len bytes.
 * Returns the number of bytes copied.
 * After retrieving the data, marks the response as consumed. */
int FujiNet_NetSIO_GetResponseData(UBYTE *buffer, int max_len)
{
    int len_to_copy = 0;
    if (!response_ready || buffer == NULL) {
        return 0;
    }

    len_to_copy = response_data_len;
    if (len_to_copy > max_len) {
        len_to_copy = max_len;
    }

    if (len_to_copy > 0) {
        memcpy(buffer, response_data_buffer, len_to_copy);
    }

    /* Mark response as consumed */
    response_ready = FALSE;
    response_data_len = 0;

    return len_to_copy;
}

/* Called once per emulator frame to process incoming UDP packets */
void FujiNet_NetSIO_Frame(void) {
    unsigned char recv_buffer[NETSIO_BUFFER_SIZE];
    unsigned char response_buffer[NETSIO_BUFFER_SIZE];
    size_t response_len = 0;
    struct sockaddr_storage recv_addr;
    socklen_t recv_addr_len = sizeof(recv_addr);
    ssize_t len;

    if (fujinet_sockfd < 0) {
        return; /* Socket not initialized */
    }

    /* Poll for incoming packets */
    while (FujiNet_UDP_Poll(fujinet_sockfd)) {
        /* Clear response buffer for each new packet */
        memset(response_buffer, 0, sizeof(response_buffer));
        response_len = 0;
        
        /* Receive packet */
        len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, sizeof(recv_buffer), 
                                  (struct sockaddr *)&recv_addr, &recv_addr_len);
        
        if (len > 0) {
            /* Process packet and get response if needed */
            if (FujiNet_NetSIO_ProcessPacket(recv_buffer, len, 
                                             (struct sockaddr *)&recv_addr, recv_addr_len, 
                                             response_buffer, &response_len)) 
            {
                /* Send response if one was generated */
                if (response_len > 0) {
                    FujiNet_UDP_Send(fujinet_sockfd, response_buffer, response_len,
                                       (struct sockaddr *)&netsio_client_addr, netsio_client_addr_len);
                }
            }
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            /* Handle receive error */
            Log_print("NetSIO: Error receiving frame: %s", strerror(errno));
        }

        /* TODO: Add timeout logic to check last_packet_time and potentially disconnect */
    }
}

/* Global flag to indicate waiting for NetSIO sync response - defined in fujinet.h */
extern int fujinet_WaitingForSync;

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
