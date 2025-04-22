/* src/fujinet.c */
#include "config.h"
#include "atari.h"  /* For BOOL, TRUE, FALSE */

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

#include "log.h"      /* For Log_print() */
#include "fujinet.h"
#include "fujinet_udp.h"
#include "fujinet_netsio.h"

/* Global flag to pause CPU while waiting for FujiNet SIO response */
int fujinet_WaitingForSync = FALSE;

/* --- Module State --- */
static int fujinet_sockfd = -1;

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

    fujinet_sockfd = FujiNet_UDP_Init(NETSIO_HUB_PORT);
    if (fujinet_sockfd < 0) {
        return FALSE;
    }

    FujiNet_NetSIO_InitState(); // Reset NetSIO protocol state

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

void FujiNet_Update(void) {
    if (fujinet_sockfd < 0) return;

    while (FujiNet_UDP_Poll(fujinet_sockfd)) {
        unsigned char recv_buffer[BUFFER_SIZE];
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t recv_len;

        recv_len = FujiNet_UDP_Receive(fujinet_sockfd, recv_buffer, BUFFER_SIZE,
                                       &client_addr, &client_len);

        if (recv_len > 0) {
            unsigned char response_buffer[BUFFER_SIZE];
            ssize_t response_len = 0;

            if (FujiNet_NetSIO_ProcessPacket(recv_buffer, recv_len, &client_addr, client_len,
                                             response_buffer, &response_len)) 
            {
                if (response_len > 0) {
                    FujiNet_UDP_Send(fujinet_sockfd, response_buffer, response_len,
                                      &client_addr, client_len);
                }
            }
        } else if (recv_len < 0) {
            break;
        }
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

    if (FujiNet_UDP_Send(fujinet_sockfd, on_cmd_buf, on_cmd_len, &client_addr, client_len) < 0) return 'E';
    if (FujiNet_UDP_Send(fujinet_sockfd, data_cmd_buf, data_cmd_len, &client_addr, client_len) < 0) return 'E';
    if (data_out_len > 0) {
        if (FujiNet_UDP_Send(fujinet_sockfd, data_out_buf, data_out_len, &client_addr, client_len) < 0) return 'E';
    }
    if (FujiNet_UDP_Send(fujinet_sockfd, off_sync_buf, off_sync_len, &client_addr, client_len) < 0) return 'E';

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
                unsigned char dummy_resp_buf[1];
                ssize_t dummy_resp_len = 0;
                if (FujiNet_NetSIO_ProcessPacket(recv_buffer, recv_len, &resp_client_addr, resp_client_len,
                                                 dummy_resp_buf, &dummy_resp_len)){
                    if(dummy_resp_len > 0) FujiNet_UDP_Send(fujinet_sockfd, dummy_resp_buf, dummy_resp_len, &resp_client_addr, resp_client_len);
                }

                if (FujiNet_NetSIO_CheckSyncResponse(recv_buffer, recv_len, sync_to_use, &status_code)) {
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
