/* src/fujinet.c */
#include "config.h"
#include "atari.h"  /* For BOOL, TRUE, FALSE */
#include "esc.h"      /* For ESC_enable_sio_patch */
#include "devices.h"  /* For Devices_enable_*_patch */
#include "sio.h"      /* For SIO constants and types */
#include "fujinet_netsio.h" /* For NetSIO functions and constants */
#include "fujinet_udp.h"

/* Helper function prototype */
long long get_time_ms(void);

/* Forward declarations */
static void print_hex_fuji(const unsigned char *buf, size_t len);
static void print_packet(const char *prefix, const unsigned char *data, int len, const struct sockaddr_in *addr);

/* Include socket headers globally to avoid incomplete struct issues */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h> /* Required for gettimeofday */

#ifdef USE_FUJINET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h> /* For isprint (might be needed for logging helpers) */
#include <stdint.h> /* For uint8_t */
#include <sys/types.h>
#include <sys/select.h>
#include <time.h>

#include "log.h"      /* For Log_print() */
#include "fujinet.h"

/* Global flag to pause CPU while waiting for FujiNet SIO response */
int fujinet_WaitingForSync = FALSE;

/* --- Module State --- */
int fujinet_sockfd = -1;
int fujinet_connected = FALSE;
int fujinet_enabled = FALSE; /* Flag indicating if FujiNet support is enabled */
struct sockaddr_in fujinet_client_addr;
socklen_t fujinet_client_len = sizeof(struct sockaddr_in);

#define BUFFER_SIZE 1024

#define FUJINET_SIO_TIMEOUT_MS 2000 /* 2 seconds timeout for SIO operations */

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

static void print_packet(const char *prefix, const unsigned char *data, int len, const struct sockaddr_in *addr)
{ 
    /* C89: Declarations at top */
    char ip_str[INET_ADDRSTRLEN]; /* Moved */

    /* Use Log_print for consistency with emulator logging */
    inet_ntop(addr->sin_family, &addr->sin_addr, ip_str, sizeof(ip_str));
    Log_print("%s %s:%d, %d bytes:", prefix, ip_str, ntohs(addr->sin_port), len);
    print_hex_fuji(data, len);
}

int FujiNet_Initialise(void) {
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
    fujinet_enabled = TRUE;  /* Set the global enabled flag */
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

int FujiNet_IsConnected(void) {
    return FujiNet_NetSIO_IsClientConnected();
}

void FujiNet_Update(void)
{
    Log_print("FujiNet_Update: Entered function. Enabled=%d, Socket=%d", fujinet_enabled, fujinet_sockfd);
    if (fujinet_sockfd < 0) return;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    ssize_t len;

    unsigned char recv_buffer[BUFFER_SIZE];

    Log_print("FujiNet_Update: Checking for UDP packets on socket %d", fujinet_sockfd);
    while (FujiNet_UDP_Poll(fujinet_sockfd)) {
        Log_print("FujiNet_Update: Poll detected incoming packet.");
        len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, BUFFER_SIZE,
                                     &client_addr, &client_len);

        if (len > 0) {
            // Log received packet type before processing
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(client_addr.sin_addr), ip_str, INET_ADDRSTRLEN);
            Log_print("[RAW UDP RX %02d:%02d:%02d] %zd bytes from %s:%d | HEX: %02X | ASCII: %c",
                      0, 0, 0, /* Time placeholder */
                      len, ip_str, ntohs(client_addr.sin_port), 
                      recv_buffer[0], isprint(recv_buffer[0]) ? recv_buffer[0] : '.');
            Log_print("<<< FROM FUJINET [%s:%d]: Received %zd bytes, packet type 0x%02X",
                      ip_str, ntohs(client_addr.sin_port), len, recv_buffer[0]);
            Log_print("    Data: %02X  | %c", 
                      recv_buffer[0], isprint(recv_buffer[0]) ? recv_buffer[0] : '.');

            unsigned char response_buffer[BUFFER_SIZE];
            size_t response_len = 0;

            // Process the packet and check if a response is needed
            BOOL should_respond = FujiNet_NetSIO_ProcessPacket(recv_buffer, len, (struct sockaddr *)&client_addr, client_len,
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
        } else if (len < 0) {
            Log_print("FujiNet_Update: FujiNet_UDP_Receive error occurred, breaking loop.");
            break; // Exit loop on receive error
        } else { // len == 0
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
    extern int available_credits; // Use the global available_credits from fujinet_netsio.c
    char expected_ip[INET_ADDRSTRLEN]; /* For address comparison/logging */
    char received_ip[INET_ADDRSTRLEN]; /* For address comparison/logging */
    char ip_str[INET_ADDRSTRLEN];      /* For general IP string handling */

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
    uint8_t status_code = 0xFF; /* Moved declaration here */
    while (!response_received && timeout_counter > 0) {
        if (FujiNet_UDP_Poll(fujinet_sockfd)) {
            unsigned char recv_buffer[BUFFER_SIZE];
            struct sockaddr_in resp_client_addr;
            socklen_t resp_client_len = sizeof(resp_client_addr);
            ssize_t recv_len;

            recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, BUFFER_SIZE, &resp_client_addr, &resp_client_len);

            if (recv_len > 0) {
                /* AFTER receiving packet, now do the address comparison */
                inet_ntop(AF_INET, &fujinet_client_addr.sin_addr, expected_ip, sizeof(expected_ip));
                inet_ntop(AF_INET, &resp_client_addr.sin_addr, received_ip, sizeof(received_ip));

                /* Compare only IP addresses, not ports (ports may change on some systems) */
                int ip_match = (memcmp(&resp_client_addr.sin_addr, &fujinet_client_addr.sin_addr, sizeof(struct in_addr)) == 0);
                
                if (ip_match) /* Only check if IP addresses match - ignore port number */
                {
                    /* Convert addresses to strings for comparison and logging */
                    inet_ntop(AF_INET, &fujinet_client_addr.sin_addr, ip_str, sizeof(ip_str));
                    inet_ntop(AF_INET, &resp_client_addr.sin_addr, received_ip, sizeof(received_ip));

                    /* For debugging: Print detailed address comparison */
                    int expected_port = ntohs(fujinet_client_addr.sin_port);
                    int received_port = ntohs(resp_client_addr.sin_port);
                    Log_print("FUJINET DEBUG: Comparing addresses - Expected %s:%d, Received %s:%d", 
                             expected_ip, expected_port, received_ip, received_port);
                    
                    /* Compare IP addresses (sin_addr) and ports (sin_port) */
                    int ip_match = (memcmp(&resp_client_addr.sin_addr, &fujinet_client_addr.sin_addr, sizeof(struct in_addr)) == 0);
                    Log_print("FUJINET DEBUG: Address comparison result - IP match: %d", ip_match);

                    /* Log each packet received during response wait */
                    Log_print("FujiNet: Received packet from %s:%d (%d bytes, type 0x%02X)", 
                             received_ip, received_port, (int)recv_len, recv_buffer[0]);
                    
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
    /* --- Variable Declarations (C89) --- */
    int is_write_cmd;
    int output_len = 0;
    UBYTE output_buffer[NETSIO_BUFFER_SIZE]; /* Buffer for data *to* device (populated from SIO) */

    UBYTE sync_num;
    unsigned char on_cmd_buf[NETSIO_BUFFER_SIZE];
    size_t on_cmd_len;
    unsigned char data_cmd_buf[NETSIO_BUFFER_SIZE];
    size_t data_cmd_len;
    unsigned char data_out_buf[NETSIO_BUFFER_SIZE]; /* Dummy buffer for received data (handled by SIO.DataBuffer) */
    size_t data_out_len;                           /* Dummy length */
    unsigned char off_cmd_buf[NETSIO_BUFFER_SIZE]; /* For COMMAND_OFF or COMMAND_OFF_SYNC */
    size_t off_cmd_len;

    int success = FALSE;

    long long start_time_ms = 0;
    long long current_time_ms = 0;

    struct sockaddr_in recv_client_addr; /* Address of the received packet source */
    socklen_t recv_client_len = sizeof(struct sockaddr_in); /* Initialize size for recvfrom */
    ssize_t recv_len;
    unsigned char recv_buffer[NETSIO_BUFFER_SIZE];
    UBYTE packet_type;
    UBYTE recv_sync_num;
    UBYTE received_status_code;
    int data_len;

    /* IP address string buffers for comparison and logging */
    char expected_ip[INET_ADDRSTRLEN];
    char received_ip[INET_ADDRSTRLEN];

    unsigned char alive_resp[] = { NETSIO_ALIVE_RESPONSE }; /* Moved declaration here */

    /* --- Logic --- */
    Log_print("FUJINET: FujiNet_ProcessSIO D:%02X C:%02X A1:%02X A2:%02X", device_id, command, aux1, aux2);

    if (!fujinet_enabled || fujinet_sockfd < 0) {
        Log_print("FUJINET: FujiNet not enabled or socket not initialised.");
        return SIO_ERR;
    }

    is_write_cmd = (command == 0x50 || command == 'W');
    if (is_write_cmd) {
        if (TransferStatus == SIO_SendFrame) {
            if (SIO.DataLen > NETSIO_BUFFER_SIZE) {
                Log_print("FUJINET: Warning - SIO output data (%d bytes) exceeds NetSIO buffer size (%d), truncating.", 
                           SIO.DataLen, NETSIO_BUFFER_SIZE);
                output_len = NETSIO_BUFFER_SIZE;
            } else {
                output_len = SIO.DataLen;
            }
            memcpy(output_buffer, SIO.DataBuffer, output_len);
            Log_print("FUJINET: Preparing %d bytes for SIO WRITE", output_len);
        } else {
            Log_print("FUJINET: WARNING - SIO WRITE command but no data in SIO buffer (TransferStatus=%d)", TransferStatus);
            output_len = 0;
        }
    }

    sync_num = FujiNet_NetSIO_PrepareSIOCommandSequence(
        device_id, command, aux1, aux2,
        output_buffer, output_len,       /* Data *to* device */
        on_cmd_buf, &on_cmd_len,         /* Buffer for COMMAND_ON */
        data_cmd_buf, &data_cmd_len,     /* Buffer for SIO command */
        data_out_buf, &data_out_len,     /* Buffer for SIO data *from* device (unused here) */
        off_cmd_buf, &off_cmd_len        /* Buffer for COMMAND_OFF_SYNC */
    );

    if (sync_num == 0xFF) { /* Check for error return (-1 might conflict with valid sync numbers) */
        Log_print("FUJINET: Error preparing NetSIO command sequence");
        return SIO_ERR;
    }

    int send_ret; /* Restore for error checking */
    send_ret = FujiNet_UDP_Send(fujinet_sockfd, on_cmd_buf, on_cmd_len, &fujinet_client_addr, fujinet_client_len);
    if (send_ret != on_cmd_len) {
        Log_print("FUJINET: Error sending COMMAND_ON packet (%d)", send_ret);
        return SIO_ERR;
    }
    Log_print(">>> TO FUJINET [CLIENT]: Sent NetSIO Command On (Sync %d, len %zu)", sync_num, on_cmd_len);

    send_ret = FujiNet_UDP_Send(fujinet_sockfd, data_cmd_buf, data_cmd_len, &fujinet_client_addr, fujinet_client_len);
    if (send_ret != data_cmd_len) {
          Log_print("FUJINET: Error sending DATA_BLOCK packet (%d)", send_ret);
          return SIO_ERR;
    }
    Log_print(">>> TO FUJINET [CLIENT]: Sent NetSIO Data Block (Sync %d, len %zu)", sync_num, data_cmd_len);

    if (off_cmd_len > 0) { 
        send_ret = FujiNet_UDP_Send(fujinet_sockfd, off_cmd_buf, off_cmd_len, &fujinet_client_addr, fujinet_client_len);
        if (send_ret != off_cmd_len) {
            Log_print("FUJINET: Error sending COMMAND_OFF_SYNC packet (%d)", send_ret);
            return SIO_ERR;
        }
        Log_print(">>> TO FUJINET [CLIENT]: Sent NetSIO Command Off Sync (Sync %d, len %zu)", sync_num, off_cmd_len);
    } else {
        Log_print("FUJINET: Warning - OFF_SYNC command was not prepared (len %zu). Did prepare sequence fail partially?", off_cmd_len);
    }

    start_time_ms = get_time_ms();
    fujinet_WaitingForSync = TRUE; /* Indicate we are waiting for a response */

    uint8_t status_code = 0xFF; /* Moved declaration here */
    while (!success) {
        current_time_ms = get_time_ms();
        if (current_time_ms - start_time_ms > FUJINET_SIO_TIMEOUT_MS) {
            Log_print("FUJINET: Timeout waiting for NetSIO response after %d ms for sync %d",
                      FUJINET_SIO_TIMEOUT_MS, sync_num);
            return SIO_TIMEOUT;
        }

        if (FujiNet_UDP_Poll(fujinet_sockfd)) {
            recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, sizeof(recv_buffer), &recv_client_addr, &recv_client_len);

            if (recv_len > 0) {
                inet_ntop(AF_INET, &fujinet_client_addr.sin_addr, expected_ip, sizeof(expected_ip));
                inet_ntop(AF_INET, &recv_client_addr.sin_addr, received_ip, sizeof(received_ip));

                int ip_match = (memcmp(&recv_client_addr.sin_addr, &fujinet_client_addr.sin_addr, sizeof(struct in_addr)) == 0);
                
                if (ip_match)  /* Only check IP match - port numbers may change */
                {
                    packet_type = recv_buffer[0];
                    recv_sync_num = (recv_len > 1) ? recv_buffer[1] : 0xFF; /* Basic sync check */

                    Log_print("FUJINET: Received packet from expected client (sync %d).", sync_num);

                    if (packet_type == NETSIO_ALIVE_REQUEST) {
                        print_packet(">>> TO FUJINET [ALIVE_RESP]", alive_resp, sizeof(alive_resp), &recv_client_addr);
                        FujiNet_UDP_Send(fujinet_sockfd, alive_resp, sizeof(alive_resp), &recv_client_addr, recv_client_len);
                    } else if (packet_type == NETSIO_CREDIT_STATUS) {
                        /* Handle Credit Status packet (0xC6) - update credit info if needed */
                        /* Just acknowledge we received it - no response needed */
                        Log_print("FUJINET: Received CREDIT_STATUS (0xC6) packet");
                    } else if (packet_type == NETSIO_CREDIT_UPDATE) {
                        /* Handle Credit Update packet (0xC7) - update credit info if needed */
                        /* Just acknowledge we received it - no response needed */
                        Log_print("FUJINET: Received CREDIT_UPDATE (0xC7) packet");
                    } else if (packet_type == NETSIO_PING_REQUEST) {
                        /* Handle ping request even during sync wait */
                        unsigned char ping_resp[] = { NETSIO_PING_RESPONSE };
                        Log_print("FUJINET: Received PING request, sending PING response");
                        FujiNet_UDP_Send(fujinet_sockfd, ping_resp, sizeof(ping_resp), &recv_client_addr, recv_client_len);
                    } else if (packet_type == NETSIO_SYNC_RESPONSE && recv_sync_num == sync_num) {
                        if (recv_len >= 3) { /* Type + Sync + Status */
                            received_status_code = recv_buffer[2];
                            Log_print("FUJINET: Valid response received for sync %d (Type: 0x%02X, Status: 0x%02X)",
                                      sync_num, packet_type, received_status_code);
                            success = TRUE;

                            if (packet_type == NETSIO_DATA_BLOCK && command == 0x52) {
                                data_len = (int)(recv_len - 2); /* Cast ssize_t to int */
                                if (data_len > 0) {
                                    if (data_len > SIO_BUFFER_SIZE) data_len = SIO_BUFFER_SIZE;
                                    memcpy(SIO.DataBuffer, recv_buffer + 2, data_len);
                                    SIO.DataLen = data_len;
                                    Log_print("FUJINET: Copied %d bytes from DATA_BLOCK to SIO buffer", data_len);
                                } else {
                                    SIO.DataLen = 0;
                                }
                            } else {
                                SIO.DataLen = 0; // No data expected or not a READ
                            }
                        } else {
                            return SIO_ERR; /* Invalid sync response */
                        }
                    } else {
                        Log_print("FUJINET: Ignoring non-sync packet from %s (type 0x%02X) while waiting for sync %d",
                                 received_ip, packet_type, sync_num);
                    }
                } else {
                    Log_print("FUJINET: Ignoring packet from %s - Wrong source IP (expected %s) while waiting for sync %d",
                             received_ip, expected_ip, sync_num);
                }
            } else if (recv_len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                Log_print("FUJINET: Error receiving packet: %s (errno %d)", strerror(errno), errno);
            }
        } else {
            usleep(100); /* Sleep for 100 microseconds to prevent busy-waiting */
        }
    }

    if (success) {
        unsigned char off_cmd_buf[1] = { NETSIO_COMMAND_OFF };
        Log_print("FUJINET: Sending COMMAND_OFF (0x12) after successful sync %d", sync_num);
        if (FujiNet_UDP_Send(fujinet_sockfd, off_cmd_buf, sizeof(off_cmd_buf),
                         &fujinet_client_addr, fujinet_client_len) == sizeof(off_cmd_buf)) {
             available_credits--; /* Consume credit ONLY if send was successful */
        } else {
             Log_print("FUJINET: Error sending COMMAND_OFF packet");
        }

        switch (received_status_code) {
            case SIO_COMPLETE:  return SIO_COMPLETE; break;
            case SIO_ACK:       return SIO_ACK; break;
            case SIO_ERR:       return SIO_ERR; break;
            case SIO_NAK:       return SIO_NAK; break;
            case SIO_TIMEOUT:   return SIO_TIMEOUT; break; /* Device reported timeout */
            default:            return SIO_ERR; /* Treat unknown status as error */
                                Log_print("FUJINET: Received unknown status code 0x%02X ('%c')", received_status_code, isprint(received_status_code) ? received_status_code : '?');
                                break;
        }

        if (command == 0x52 && received_status_code == SIO_COMPLETE) {
             if (SIO.DataLen > 0) {
                  TransferStatus = SIO_ReceiveFrame; /* Indicate data is ready */
                  Log_print("FUJINET: SIO READ successful, %d bytes ready.", SIO.DataLen);
             } else {
                  TransferStatus = SIO_NoFrame; /* Complete, but no data? */
                  Log_print("FUJINET: SIO READ successful, but no data received.");
             }
        } else if (command == 0x50 && received_status_code == SIO_COMPLETE) {
             TransferStatus = SIO_NoFrame; /* Write finished, no data transfer active */
             Log_print("FUJINET: SIO WRITE successful.");
        } else if (received_status_code != SIO_COMPLETE && received_status_code != SIO_ACK) { 
             TransferStatus = SIO_NoFrame; /* No frame pending after error */
             Log_print("FUJINET: SIO command resulted in status %c (0x%02X).", received_status_code, received_status_code);
        } else {
             TransferStatus = SIO_NoFrame; /* Treat as no data transfer needed */
             Log_print("FUJINET: SIO command ACK'd (Result: %c)", received_status_code);
        }

    } else { // Loop exited due to timeout
        Log_print("FUJINET: SIO command timed out (Sync: %d)", sync_num);
        return SIO_TIMEOUT;
    }

    fujinet_WaitingForSync = FALSE; /* No longer waiting */
    return SIO_ERR;
}

long long get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000) + (tv.tv_usec / 1000);
}

#endif /* USE_FUJINET */

/* Fallback implementations for when FujiNet is disabled */
#ifndef USE_FUJINET

#include "atari.h"
#include "sio.h"   /* For UBYTE */

int FujiNet_Initialise(void) {
    // Do nothing when FujiNet is disabled
    return FALSE;
}

void FujiNet_Shutdown(void) {
    // Do nothing when FujiNet is disabled
}

void FujiNet_Update(void) {
    // Do nothing when FujiNet is disabled
}

int FujiNet_IsConnected(void) {
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
