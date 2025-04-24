/* src/fujinet.c */
#include "config.h"
#include "atari.h"  /* For BOOL, TRUE, FALSE */
#include "esc.h"      /* For ESC_enable_sio_patch */
#include "devices.h"  /* For Devices_enable_*_patch */
#include "sio.h"      /* For SIO constants and types */
#include "fujinet_netsio.h" /* For NetSIO functions and constants */

/* Include socket headers globally to avoid incomplete struct issues */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef USE_FUJINET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h> /* For isprint (might be needed for logging helpers) */
#include <stdbool.h> /* Use bool, true, false */
#include <stdint.h> /* For uint8_t */
#include <sys/types.h>
#include <sys/select.h>
#include <time.h>

#include "log.h"      /* For Log_print() */
#include "fujinet.h"
#include "fujinet_udp.h"

/* Global flag to pause CPU while waiting for FujiNet SIO response */
int fujinet_WaitingForSync = FALSE;

/* --- Module State --- */
int fujinet_sockfd = -1;
BOOL fujinet_connected = FALSE;
struct sockaddr_in fujinet_client_addr;
socklen_t fujinet_client_len = sizeof(struct sockaddr_in);

#define BUFFER_SIZE 1024

static void print_hex_fuji(const unsigned char *buf, size_t len) {
    size_t i;
    // Use Log_print for consistency with emulator logging
    // Need to handle potential long lines if Log_print buffers
    // For now, keep it simple using stderr for direct debug output
    fprintf(stderr, "      HEX:");
    for (i = 0; i < len; ++i) {
        fprintf(stderr, " %02X", buf[i]);
    }
    fprintf(stderr, " | ");
    for (i = 0; i < len; ++i) {
        fprintf(stderr, "%c", isprint(buf[i]) ? buf[i] : '.');
    }
    fprintf(stderr, "\n");
}

BOOL FujiNet_Initialise(void) {
    Log_print("FujiNet: Initialising...");
    if (fujinet_sockfd >= 0) {
        Log_print("FujiNet: Already initialised.");
        return TRUE;
    }

    /* Force SIO and Device patches off, similar to -nopatchall */
    Log_print("FujiNet: Forcing SIO and device patches OFF.");
    // ESC_enable_sio_patch = FALSE; /* ABLE ARCHER: Allow SIO patch for handler trigger */
    Devices_enable_h_patch = FALSE;
    Devices_enable_p_patch = FALSE;
    Devices_enable_r_patch = FALSE;

    fujinet_sockfd = FujiNet_UDP_Init(NETSIO_HUB_PORT);
    if (fujinet_sockfd < 0) {
        return FALSE;
    }

    FujiNet_NetSIO_InitState(); // Reset NetSIO protocol state

#ifdef FUJINET_SIO_PATCH
    {
        /* Read original bytes for restoration and logging */
        UBYTE sio_original_byte1 = PEEK(0xE459);
        UBYTE sio_original_byte2 = PEEK(0xE45A);
        UBYTE sio_original_byte3 = PEEK(0xE45B);
        const int patch_location = 0xE459; /* SIOV vector location */
        const int patch_target = FUJINET_SIO_HANDLER_ADDRESS;
        Log_print("FujiNet_Initialise: Attempting to patch SIO vector at $%04X to jump to $%04X", patch_location, patch_target);
        POKE(patch_location    , 0x4C); /* JMP instruction */
        POKE(patch_location + 1, patch_target & 0xFF);        /* Low byte of target address */
        POKE(patch_location + 2, (patch_target >> 8) & 0xFF); /* High byte of target address */
        Log_print("FujiNet_Initialise: SIO vector patched. Original bytes at $%04X: %02X %02X %02X", patch_location, sio_original_byte1, sio_original_byte2, sio_original_byte3);
        Log_print("FujiNet_Initialise: Patched SIO vector at $%04X now points to $%04X", patch_location, PEEK(patch_location+1) + (PEEK(patch_location+2)<<8));
    }
#endif /* FUJINET_SIO_PATCH */

    Log_print("FujiNet: Initialised successfully.");
    return TRUE;
}

void FujiNet_Shutdown(void) {
    Log_print("FujiNet: Shutting down...");
    if (fujinet_sockfd >= 0) {
        FujiNet_UDP_Shutdown(fujinet_sockfd);
        fujinet_sockfd = -1;
        FujiNet_NetSIO_InitState(); // Clear NetSIO state too
    }
}

BOOL FujiNet_IsConnected(void) {
    return FujiNet_NetSIO_IsClientConnected();
}

void FujiNet_Update(void)
{
    Log_print("FujiNet_Update: Entered function."); // Added Log at entry
    if (fujinet_sockfd < 0) return;

    while (FujiNet_UDP_Poll(fujinet_sockfd)) {
        Log_print("FujiNet_Update: Poll detected incoming packet.");
        unsigned char recv_buffer[BUFFER_SIZE];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t recv_len;

        recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, BUFFER_SIZE,
                                     &client_addr, &client_len);

        if (recv_len > 0) {
            // Log received packet type before processing
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            Log_print("FujiNet_Update: Received %zd bytes from %s:%d, Packet Type: 0x%02X", 
                      recv_len, ip_str, ntohs(client_addr.sin_port), recv_buffer[0]);

            unsigned char response_buffer[BUFFER_SIZE];
            size_t response_len = 0;

            // Process the packet and check if a response is needed
            BOOL should_respond = FujiNet_NetSIO_ProcessPacket(recv_buffer, recv_len, (struct sockaddr *)&client_addr, client_len,
                                                             response_buffer, &response_len);
            Log_print("FujiNet_Update: ProcessPacket returned should_respond=%d, response_len=%zu", should_respond, response_len);

            if (should_respond)
            {
                if (response_len > 0) {
                    Log_print("FujiNet_Update: Sending response (type 0x%02X, len %zu) to %s:%d", 
                              response_buffer[0], response_len, ip_str, ntohs(client_addr.sin_port));
                    FujiNet_UDP_Send(fujinet_sockfd, response_buffer, response_len,
                                    &client_addr, client_len);
                } else {
                    Log_print("FujiNet_Update: ProcessPacket indicated response needed, but response_len is 0.");
                }
            } else {
                Log_print("FujiNet_Update: No response needed for packet type 0x%02X.", recv_buffer[0]);
            }
        } else if (recv_len < 0) {
            Log_print("FujiNet_Update: FujiNet_UDP_Receive error occurred, breaking loop.");
            break; // Exit loop on receive error
        } else { // recv_len == 0
             Log_print("FujiNet_Update: Received 0 bytes (possible connection close indicator?), ignoring.");
        }
    }
    Log_print("FujiNet_Update: Poll returned false, no more packets for now."); 
}

void FujiNet_TestSIOStatus(void)
{
    UBYTE status_buffer[4]; /* Status command returns 4 bytes */
    int result_len = 0;
    char result;

    Log_print("FujiNet: Sending test STATUS command to D1: (device 0x31)");
    
    /* Send STATUS command (0x53) to device 0x31 (D1:) */
    /* For a STATUS command, AUX1 and AUX2 are typically 0 */
    result = FujiNet_SendSIOCommand(0x31, 0x53, 0, 0, NULL, 0, status_buffer, &result_len);
    
    if (result == 'C') {
        Log_print("FujiNet: STATUS command successful! Response %d bytes:", result_len);
        if (result_len >= 4) {
            Log_print("  Status: %02X %02X %02X %02X", 
                status_buffer[0], status_buffer[1], 
                status_buffer[2], status_buffer[3]);
            
            /* Interpret the status bytes */
            Log_print("  Drive Status: %s, %s, %s",
                (status_buffer[0] & 0x01) ? "Motor on" : "Motor off",
                (status_buffer[0] & 0x04) ? "Write-protected" : "Writeable",
                (status_buffer[0] & 0x02) ? "Double density" : "Single density");
        } else {
            Log_print("  Unexpected response length: %d", result_len);
        }
    } else {
        Log_print("FujiNet: STATUS command failed with result: '%c'", result);
    }
}

char FujiNet_SendSIOCommand(UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                             const UBYTE *output_buffer, int output_len,
                             UBYTE *input_buffer, int *input_len_ptr)
{
    char device_type = 'U'; /* Unknown by default */
    char cmd_desc[32] = "Unknown";
    int sync_to_use;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    unsigned char on_cmd_buf[10]; 
    size_t on_cmd_len = 0;
    unsigned char data_cmd_buf[10]; 
    size_t data_cmd_len = 0;
    unsigned char data_out_buf[BUFFER_SIZE]; 
    size_t data_out_len = 0;
    unsigned char off_sync_buf[10]; 
    size_t off_sync_len = 0;
    int timeout_counter;
    int response_received = 0; /* Use int instead of bool for C89 */
    uint8_t status_code = 0xFF;
    extern int available_credits; // Use the global available_credits from fujinet_netsio.c

    /* Determine device type for better logging */
    if (device_id >= 0x31 && device_id <= 0x38) {
        device_type = 'D'; /* Disk */
    } else if (device_id == 0x60) {
        device_type = 'P'; /* Printer */
    } else if (device_id == 0x50) {
        device_type = 'T'; /* Tape */
    } else if (device_id == 0x40) {
        device_type = 'C'; /* Cassette */
    }
    
    /* Describe common commands for better logs */
    switch (command) {
        case 0x53: strcpy(cmd_desc, "STATUS"); break;
        case 0x52: strcpy(cmd_desc, "READ"); break;
        case 0x57: strcpy(cmd_desc, "WRITE"); break;
        case 0x50: strcpy(cmd_desc, "PUT"); break;
        case 0x4E: strcpy(cmd_desc, "READ STATUS"); break;
        case 0x21: strcpy(cmd_desc, "FORMAT DISK"); break;
        case 0x2A: strcpy(cmd_desc, "FORMAT MEDIUM"); break;
    }

    Log_print("FujiNet: SIO Command: %s (0x%02X) for Device: %c%d (0x%02X), AUX1: 0x%02X, AUX2: 0x%02X",
              cmd_desc, command, device_type, device_id & 0x0F, device_id, aux1, aux2);

    if (fujinet_sockfd < 0 || !FujiNet_NetSIO_IsClientConnected()) {
        Log_print("FujiNet: Cannot send SIO command - not initialised or client not connected.");
        return 'E';
    }

    if (!FujiNet_NetSIO_GetClientAddr(&client_addr, &client_len)) {
        Log_print("FujiNet: Cannot send SIO command - failed to get client address.");
        return 'E';
    }

    /* Log client address */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
    Log_print("FujiNet: Sending to client at %s:%d", ip_str, ntohs(client_addr.sin_port));

    sync_to_use = FujiNet_NetSIO_PrepareSIOCommandSequence(
                        device_id, command, aux1, aux2, output_buffer, output_len,
                        on_cmd_buf, &on_cmd_len,
                        data_cmd_buf, &data_cmd_len,
                        data_out_buf, &data_out_len,
                        off_sync_buf, &off_sync_len);

    if (sync_to_use < 0) {
        Log_print("FujiNet: Failed to prepare SIO command sequence (error code %d)", sync_to_use);
        return 'E';
    }

    Log_print("FujiNet: Sending SIO Cmd (Sync %d) via NetSIO...", sync_to_use);

    /* Log detailed packet contents */
    if (on_cmd_len > 0) {
        char hexbuf[128] = {0};
        int i, hexpos = 0;
        
        for (i = 0; i < on_cmd_len && hexpos < 120; i++) {
            hexpos += sprintf(hexbuf + hexpos, "%02X ", on_cmd_buf[i]);
        }
        
        Log_print("FujiNet: ON_CMD packet [%d bytes]: %s", (int)on_cmd_len, hexbuf);
    }

    /* Send the entire command sequence with minimal delays between packets */
    ssize_t sent_len;
    int send_error = 0;
    
    // ON_CMD
    sent_len = FujiNet_UDP_Send(fujinet_sockfd, on_cmd_buf, on_cmd_len, &client_addr, client_len);
    if (sent_len == (ssize_t)on_cmd_len) {
        available_credits--;
        Log_print("FujiNet: ON_CMD sent, credits now %d", available_credits);
    } else {
        Log_print("FujiNet: Failed to send ON_CMD packet (errno=%d: %s)", errno, strerror(errno));
        return 'E';
    }
    Log_print("FujiNet: ON_CMD sent, delaying 1ms...");
    usleep(1000); /* Small 1ms delay to ensure proper packet separation */
    
    // DATA_CMD
    if (data_cmd_len > 0) {
        sent_len = FujiNet_UDP_Send(fujinet_sockfd, data_cmd_buf, data_cmd_len, &client_addr, client_len);
        if (sent_len == (ssize_t)data_cmd_len) {
            available_credits--;
            Log_print("FujiNet: DATA_CMD sent, credits now %d", available_credits);
        } else {
            Log_print("FujiNet: Failed to send DATA_CMD packet (errno=%d: %s)", errno, strerror(errno));
            return 'E';
        }
    }
    
    // DATA
    if (data_out_len > 0) {
        Log_print("FujiNet: Sending %d bytes of data", (int)data_out_len);
        usleep(1000); /* Small 1ms delay */
        sent_len = FujiNet_UDP_Send(fujinet_sockfd, data_out_buf, data_out_len, &client_addr, client_len);
        if (sent_len == (ssize_t)data_out_len) {
            available_credits--;
            Log_print("FujiNet: DATA sent, credits now %d", available_credits);
        } else {
            Log_print("FujiNet: Failed to send DATA packet (errno=%d: %s)", errno, strerror(errno));
            return 'E';
        }
    }
    
    // OFF/SYNC
    if (off_sync_len > 0) {
        sent_len = FujiNet_UDP_Send(fujinet_sockfd, off_sync_buf, off_sync_len, &client_addr, client_len);
        if (sent_len == (ssize_t)off_sync_len) {
            available_credits--;
            Log_print("FujiNet: OFF/SYNC sent, credits now %d", available_credits);
        } else {
            Log_print("FujiNet: Failed to send OFF/SYNC packet (errno=%d: %s)", errno, strerror(errno));
            return 'E';
        }
    }

    Log_print("FujiNet: SIO sequence sent. Waiting for response (sync %d)...", sync_to_use);

    timeout_counter = FUJINET_RESPONSE_TIMEOUT_MS;

    fujinet_WaitingForSync = TRUE; /* Pause CPU emulation */
    while (!response_received && timeout_counter > 0) {
        if (FujiNet_UDP_Poll(fujinet_sockfd)) {
            unsigned char recv_buffer[BUFFER_SIZE];
            struct sockaddr_in resp_client_addr;
            socklen_t resp_client_len = sizeof(resp_client_addr);
            ssize_t recv_len;

            recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, BUFFER_SIZE, &resp_client_addr, &resp_client_len);

            if (recv_len > 0) {
                /* Log each packet received during response wait */
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(resp_client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
                Log_print("FujiNet: Received packet from %s:%d (%d bytes, type 0x%02X)", 
                         ip_str, ntohs(resp_client_addr.sin_port), (int)recv_len, recv_buffer[0]);
                
                /* Print first 16 bytes as hex for debugging */
                if (recv_len > 0) {
                    char hexbuf[128] = {0};
                    int i, hexpos = 0;
                    int max_bytes = recv_len > 16 ? 16 : recv_len;
                    
                    for (i = 0; i < max_bytes; i++) {
                        hexpos += sprintf(hexbuf + hexpos, "%02X ", recv_buffer[i]);
                    }
                    
                    Log_print("FujiNet: Packet data: %s%s", hexbuf, recv_len > 16 ? "..." : "");
                }
                
                unsigned char dummy_resp_buf[1];
                size_t dummy_resp_len = 0;
                if (FujiNet_NetSIO_ProcessPacket(recv_buffer, recv_len, (struct sockaddr *)&resp_client_addr, resp_client_len,
                                               dummy_resp_buf, &dummy_resp_len)){
                    if(dummy_resp_len > 0) {
                        Log_print("FujiNet: Sending %d byte response to packet", (int)dummy_resp_len);
                        FujiNet_UDP_Send(fujinet_sockfd, dummy_resp_buf, dummy_resp_len, &resp_client_addr, resp_client_len);
                    }
                }

                if (FujiNet_NetSIO_CheckSyncResponse(recv_buffer, recv_len, sync_to_use, &status_code)) {
                    Log_print("FujiNet: Received sync response matching sync %d! Status code: 0x%02X", 
                             sync_to_use, status_code);
                    fujinet_WaitingForSync = FALSE; /* Resume CPU emulation */
                    response_received = 1;
                    break;
                }
            }
        }

        if (!response_received) {
            usleep(1000);
            timeout_counter--;
        }

        if (!FujiNet_NetSIO_IsClientConnected()) {
            Log_print("FujiNet: Client disconnected while waiting for sync %d", sync_to_use);
            return 'E';
        }
    }

    if (!response_received) {
        Log_print("FujiNet: Timeout waiting for response for sync %d", sync_to_use);
        return 'E';
    }

    Log_print("FujiNet: Received response for sync %d with status 0x%02X", sync_to_use, status_code);

    if (status_code == 0x00) {
        return 'C';
    } else if (status_code == 0x01) {
        return 'N';
    } else {
        return 'E';
    }
}

/* Processes an SIO command directly via NetSIO, handling the full transaction.
 * device_id, command, aux1, aux2: SIO command parameters.
 * Returns: SIO completion code ('A', 'C', 'E', 'N'). */
UBYTE FujiNet_ProcessSIO(UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2)
{
    UBYTE output_buffer[NETSIO_BUFFER_SIZE]; /* Buffer for data *to* device */
    UBYTE input_buffer[NETSIO_BUFFER_SIZE];  /* Buffer for data *from* device */
    int output_len = 0;
    int input_len = 0; /* We expect NetSIO layer to fill this */
    uint8_t status_code = 0;
    BOOL is_write_cmd = FALSE;
    UBYTE sio_result = SIO_TIMEOUT; /* Default SIO result code */
    extern SIO_State_t SIO; /* From sio.c */
    extern int TransferStatus; /* From sio.c */

    Log_print("FUJINET: FujiNet_ProcessSIO D:%02X C:%02X A1:%02X A2:%02X", device_id, command, aux1, aux2);

    /* Define common SIO command types */
    #define CMD_READ 0x52    /* 'R' */
    #define CMD_WRITE 0x50   /* 'P' (PUT) or 'W' */
    #define CMD_STATUS 0x53  /* 'S' */

    /* Prepare output data if necessary (e.g., for WRITE command) */
    is_write_cmd = (command == CMD_WRITE || command == 'W');
    if (is_write_cmd) {
        /* Copy data from SIO's global buffer (if it's ready to be sent) */
        if (TransferStatus == SIO_SendFrame) {
            memcpy(output_buffer, SIO.DataBuffer, SIO.DataLen);
            output_len = SIO.DataLen;
            Log_print("FUJINET: Preparing %d bytes for SIO WRITE", output_len);
        } else {
            Log_print("FUJINET: WARNING - SIO WRITE command but no data in SIO buffer (TransferStatus=%d)", TransferStatus);
            /* Proceed anyway, maybe it's a 0-byte write? NetSIO should handle it */
            output_len = 0;
        }
    }

    /* Create a temporary packet/buffer for sending the command */
    {
        unsigned char on_cmd_buf[NETSIO_BUFFER_SIZE];
        unsigned char data_cmd_buf[NETSIO_BUFFER_SIZE];
        unsigned char data_out_buf[NETSIO_BUFFER_SIZE];
        unsigned char off_sync_buf[NETSIO_BUFFER_SIZE];
        size_t on_cmd_len = 0;
        size_t data_cmd_len = 0;
        size_t data_out_len = 0;
        size_t off_sync_len = 0;
        int sync_num;

        /* Prepare the NetSIO command sequence */
        sync_num = FujiNet_NetSIO_PrepareSIOCommandSequence(
            device_id, command, aux1, aux2,
            output_buffer, output_len,
            on_cmd_buf, &on_cmd_len,
            data_cmd_buf, &data_cmd_len,
            data_out_buf, &data_out_len,
            off_sync_buf, &off_sync_len
        );

        if (sync_num < 0) {
            Log_print("FUJINET: Error preparing NetSIO command sequence");
            return SIO_NAK;
        }

        /* Send the ON command */
        if (on_cmd_len > 0) {
            if (sendto(fujinet_sockfd, on_cmd_buf, on_cmd_len, 0,
                       (struct sockaddr *)&fujinet_client_addr, fujinet_client_len) < 0) {
                Log_print("FUJINET: Error sending NetSIO ON command");
                return SIO_ERR;
            }
        }

        /* Send the DATA command if needed */
        if (data_cmd_len > 0) {
            if (sendto(fujinet_sockfd, data_cmd_buf, data_cmd_len, 0,
                       (struct sockaddr *)&fujinet_client_addr, fujinet_client_len) < 0) {
                Log_print("FUJINET: Error sending NetSIO DATA command");
                return SIO_ERR;
            }
        }

        /* Send the DATA if needed */
        if (data_out_len > 0) {
            if (sendto(fujinet_sockfd, data_out_buf, data_out_len, 0,
                       (struct sockaddr *)&fujinet_client_addr, fujinet_client_len) < 0) {
                Log_print("FUJINET: Error sending NetSIO DATA");
                return SIO_ERR;
            }
        }

        /* Wait for response with polling */
        {
            unsigned char recv_buffer[NETSIO_BUFFER_SIZE];
            ssize_t recv_len;
            struct sockaddr_in from_addr;
            socklen_t from_len = sizeof(from_addr);
            fd_set readfds;
            struct timeval tv;
            int max_polls = 50; /* 5 seconds max wait (50 * 100ms) */
            int poll_count = 0;
            BOOL success = FALSE; // Flag to indicate if we got the expected response

            Log_print("FUJINET: Waiting for NetSIO response for sync %d...", sync_num);

            while (poll_count < max_polls && !success) { // Loop until success or timeout
                FD_ZERO(&readfds);
                FD_SET(fujinet_sockfd, &readfds);
                tv.tv_sec = 0;
                tv.tv_usec = 100000; /* 100ms timeout */

                /* Check if data is available to read */
                if (select(fujinet_sockfd + 1, &readfds, NULL, NULL, &tv) > 0) {
                    recv_len = recvfrom(fujinet_sockfd, recv_buffer, sizeof(recv_buffer), 0,
                                        (struct sockaddr *)&from_addr, &from_len);
                    
                    if (recv_len > 0) {
                        // Compare source address with known FujiNet client address
                        struct sockaddr_in *source_addr_in = (struct sockaddr_in *)&from_addr;
                        struct sockaddr_in *client_addr_in = (struct sockaddr_in *)&fujinet_client_addr;

                        // Check IP and Port match
                        if (from_len == fujinet_client_len && // Check length first
                            source_addr_in->sin_family == client_addr_in->sin_family &&
                            source_addr_in->sin_addr.s_addr == client_addr_in->sin_addr.s_addr &&
                            source_addr_in->sin_port == client_addr_in->sin_port)
                        {
                            // Packet is from the expected FujiNet client
                            Log_print("FUJINET: Received packet from expected client (sync %d).", sync_num);
                            
                            /* Check if it's the sync response we're waiting for */
                            if (FujiNet_NetSIO_CheckSyncResponse(recv_buffer, recv_len, sync_num, &status_code)) {
                                success = TRUE;
                                Log_print("FUJINET: Got valid sync response! Status: 0x%02X", status_code);

                                // Handle received data if it was a READ command and we got a DATA_BLOCK
                                if (command == CMD_READ && recv_buffer[0] == NETSIO_DATA_BLOCK && recv_len > 3) {
                                    input_len = recv_len - 3; // Assuming header is 3 bytes (type, sync, status)
                                    if (input_len > NETSIO_BUFFER_SIZE) input_len = NETSIO_BUFFER_SIZE; // Prevent overflow
                                    memcpy(input_buffer, recv_buffer + 3, input_len);
                                    Log_print("FUJINET: Copied %d bytes from DATA_BLOCK response.", input_len);
                                } else {
                                    input_len = 0; // No data payload for non-read or non-data-block responses
                                }
                                // No break here, let loop condition handle exit
                            } else {
                                // Valid packet from client, but not the sync response we wanted.
                                // Could be ALIVE or other async message. Log and continue waiting.
                                Log_print("FUJINET: Ignored unexpected packet type 0x%02X from client while waiting for sync %d.",
                                          recv_buffer[0], sync_num);
                            }
                        } else {
                            // Packet from an unexpected source (e.g., ourselves!)
                            char unexpected_ip_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &(source_addr_in->sin_addr), unexpected_ip_str, INET_ADDRSTRLEN);
                            Log_print("FUJINET: Ignored packet from unexpected source %s:%d while waiting for client sync %d.",
                                      unexpected_ip_str, ntohs(source_addr_in->sin_port), sync_num);
                        }
                    } else if (recv_len < 0) {
                         if (errno != EAGAIN && errno != EWOULDBLOCK) { // Ignore non-blocking errors
                             perror("FUJINET: recvfrom failed during SIO wait");
                             sio_result = SIO_ERR; // Set error status
                             success = TRUE; // Exit loop on error
                         }
                    } else { // recv_len == 0 (Should not happen with UDP, but handle defensively)
                         Log_print("FUJINET: Received 0 bytes during SIO wait?");
                    }
                } else { // select() timed out or returned error < 0
                    // Timeout is handled by loop condition, select errors are rare here but could be logged
                    if (select(fujinet_sockfd + 1, NULL, NULL, NULL, &tv) < 0) { // Check for actual error
                        perror("FUJINET: select() error during SIO wait");
                        sio_result = SIO_ERR;
                        success = TRUE; // Exit loop on error
                    }
                }
                poll_count++;
            } // end while loop

            /* --- Process Result --- */
            if (!success && poll_count >= max_polls) { // Explicitly check for timeout
                Log_print("FUJINET: Timeout waiting for NetSIO response after %d polls for sync %d", max_polls, sync_num);
                sio_result = SIO_TIMEOUT; /* Set timeout status */
                /* NOTE: Do NOT send OFF_SYNC on timeout, device might still be processing */
            } else if (success) {
                /* Send OFF/SYNC command */
                if (off_sync_len > 0) {
                    if (sendto(fujinet_sockfd, off_sync_buf, off_sync_len, 0,
                           (struct sockaddr *)&fujinet_client_addr, fujinet_client_len) < 0) {
                        Log_print("FUJINET: Error sending NetSIO OFF/SYNC command");
                        /* Even if OFF fails, we received the response, so proceed with status */
                    } else {
                         Log_print("FUJINET: Sent OFF/SYNC for sync %d", sync_num);
                    }
                }

                /* Determine SIO result based on status_code */
                switch (status_code) {
                    case SIO_COMPLETE:  sio_result = SIO_COMPLETE; break;
                    case SIO_ACK:       sio_result = SIO_ACK; break;
                    case SIO_ERR:       sio_result = SIO_ERR; break;
                    case SIO_NAK:       sio_result = SIO_NAK; break;
                    case SIO_TIMEOUT:   sio_result = SIO_TIMEOUT; break; /* Device reported timeout */
                    default:            sio_result = SIO_ERR; /* Treat unknown status as error */
                                        Log_print("FUJINET: Received unknown status code 0x%02X ('%c')", status_code, isprint(status_code) ? status_code : '?');
                                        break;
                }

                /* Handle data received for READ commands */
                if (command == CMD_READ && sio_result == SIO_COMPLETE) { // Only copy data on success
                    if (input_len > 0) {
                        memcpy(SIO.DataBuffer, input_buffer, input_len);
                        SIO.DataLen = input_len;
                        TransferStatus = SIO_ReceiveFrame; /* Indicate data ready for OS */
                        Log_print("FUJINET: SIO READ successful, %d bytes received.", input_len);
                    } else {
                        Log_print("FUJINET: SIO READ successful, but 0 bytes received.");
                        SIO.DataLen = 0;
                        TransferStatus = SIO_ReceiveFrame; /* Still indicate completion */
                    }
                } else if (command == CMD_WRITE && sio_result == SIO_COMPLETE) {
                     TransferStatus = SIO_NoFrame; /* Write finished, no data transfer active */
                     Log_print("FUJINET: SIO WRITE successful.");
                } else if (sio_result != SIO_COMPLETE && sio_result != SIO_ACK) {
                    /* On error, timeout, NAK etc. */
                    TransferStatus = SIO_NoFrame;
                }

            } else { // Loop exited for reason other than success or timeout (e.g., select error)
                 Log_print("FUJINET: SIO processing loop exited unexpectedly.");
                 sio_result = SIO_ERR; 
            }
        }
    }
    return sio_result;
}

#endif /* USE_FUJINET */

/* Fallback implementations for when FujiNet is disabled */
#ifndef USE_FUJINET

#include "atari.h"
#include "sio.h"   /* For UBYTE */

BOOL FujiNet_Initialise(void) {
    // Do nothing when FujiNet is disabled
    return FALSE;
}

void FujiNet_Shutdown(void) {
    // Do nothing when FujiNet is disabled
}

void FujiNet_Update(void) {
    // Do nothing when FujiNet is disabled
}

BOOL FujiNet_IsConnected(void) {
    // Always report disconnected when FujiNet is disabled
    return FALSE;
}

char FujiNet_SendSIOCommand(UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                          const UBYTE *output_buffer, int output_len,
                          UBYTE *input_buffer, int *input_len_ptr) {
    // Always return error when FujiNet is disabled
    return 'E';
}
#endif
