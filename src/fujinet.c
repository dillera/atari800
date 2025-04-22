// src/fujinet.c
#ifdef USE_FUJINET

#include "config.h" // Standard Atari800 types
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h> // For isprint (might be needed for logging helpers)
#include <stdbool.h> // Use bool, true, false

#include "log.h"      // For Log_print()
#include "fujinet.h"
#include "fujinet_udp.h"
#include "fujinet_netsio.h"

#define BUFFER_SIZE NETSIO_MAX_PACKET_SIZE // Use constant from netsio.h
#define FUJINET_RESPONSE_TIMEOUT_MS 2000 // Timeout for blocking wait

// --- Module State --- 
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
    if (fujinet_sockfd < 0 || !FujiNet_NetSIO_IsClientConnected()) {
        Log_print("FujiNet: Cannot send SIO command - not initialised or client not connected.");
        return 'E';
    }

    struct sockaddr_in client_addr;
    socklen_t client_len;
    if (!FujiNet_NetSIO_GetClientAddr(&client_addr, &client_len)) {
        Log_print("FujiNet: Cannot send SIO command - failed to get client address.");
        return 'E';
    }

    unsigned char on_cmd_buf[10]; size_t on_cmd_len = 0;
    unsigned char data_cmd_buf[10]; size_t data_cmd_len = 0;
    unsigned char data_out_buf[BUFFER_SIZE]; size_t data_out_len = 0;
    unsigned char off_sync_buf[10]; size_t off_sync_len = 0;
    int sync_to_use;

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

    int timeout_counter = FUJINET_RESPONSE_TIMEOUT_MS;
    bool response_received = false;
    uint8_t status_code = 0xFF;

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
                    response_received = true;
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
