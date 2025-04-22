// src/fujinet_netsio.h
#ifndef FUJINET_NETSIO_H_
#define FUJINET_NETSIO_H_

#ifdef USE_FUJINET

#include "config.h" // BOOL, UBYTE etc.
#include <sys/socket.h> // For struct sockaddr_in, socklen_t
#include <stdbool.h>

// --- Constants ---
#define NETSIO_DEFAULT_CREDITS 3
#define NETSIO_MAX_PACKET_SIZE 1024 // Reflects UDP reality

// --- NetSIO State Management ---

// Initialize NetSIO state variables.
void FujiNet_NetSIO_InitState(void);

// Check if a client is known and the initial handshake is complete.
BOOL FujiNet_NetSIO_IsClientConnected(void);

// Get the current client address (if known).
// Returns TRUE if client is known, FALSE otherwise.
BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len);

// --- NetSIO Packet Processing ---

// Process a received packet buffer.
// Checks for new clients, handles handshake (PING/ALIVE), credit status, etc.
// Determines if a response packet needs to be sent.
// Returns TRUE if a response packet was generated, FALSE otherwise.
BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *recv_buffer, ssize_t recv_len,
                                  const struct sockaddr_in *recv_addr, socklen_t recv_addr_len,
                                  unsigned char *response_buffer, ssize_t *response_len);

// --- NetSIO Command Sending ---

// Prepare the NetSIO sequence for an SIO command.
// This function populates multiple buffers for the sequence (ON, DATA, OFF_SYNC).
// Returns the sync number used for this command.
// Returns -1 on error (e.g., busy, client not connected).
int FujiNet_NetSIO_PrepareSIOCommandSequence(
                            UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                            const UBYTE *output_buffer, int output_len,
                            unsigned char *on_cmd_buf, size_t *on_cmd_len,
                            unsigned char *data_cmd_buf, size_t *data_cmd_len,
                            unsigned char *data_out_buf, size_t *data_out_len, // Optional output data
                            unsigned char *off_sync_buf, size_t *off_sync_len);

// --- NetSIO Response Handling ---

// Check if the received packet matches the expected sync response.
// Returns TRUE if it's the expected SYNC_RESPONSE, FALSE otherwise.
BOOL FujiNet_NetSIO_CheckSyncResponse(const unsigned char *recv_buffer, ssize_t recv_len,
                                      int expected_sync, uint8_t *status_code);

// TODO: Add functions/state for handling received DATA packets

#endif /* USE_FUJINET */

#endif /* FUJINET_NETSIO_H_ */
