/* src/fujinet_netsio.h */
#ifndef FUJINET_NETSIO_H_
#define FUJINET_NETSIO_H_

#include "config.h"
#include "atari.h" /* For BOOL, UBYTE */
#include <stdint.h> /* For uint8_t */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <stddef.h>     /* For size_t -> ssize_t */

/* Ensure BOOL type is defined */
#ifndef BOOL
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#endif

#ifdef USE_FUJINET

/* --- Constants --- */
#define NETSIO_DEFAULT_CREDITS 3
#define NETSIO_MAX_PACKET_SIZE 1024 /* Reflects UDP reality */

/* NetSIO Packet Types */
#define NETSIO_DEVICE_DISCONNECT    0xC0
#define NETSIO_DEVICE_CONNECT       0xC1
#define NETSIO_PING_REQUEST         0xC2
#define NETSIO_PING_RESPONSE        0xC3
#define NETSIO_ALIVE_REQUEST        0xC4
#define NETSIO_ALIVE_RESPONSE       0xC5
#define NETSIO_CREDIT_STATUS        0xC6
#define NETSIO_CREDIT_UPDATE        0xC7

#define NETSIO_SPEED_CHANGE         0x80
#define NETSIO_ACKNOWLEDGE          0x83
#define NETSIO_SYNC_RESPONSE        0x81 /* SIO Status byte follows */

/* SIO Command encapsulation */
#define NETSIO_COMMAND_ON           0x11
#define NETSIO_COMMAND_OFF          0x12 /* No response expected */
#define NETSIO_COMMAND_OFF_SYNC     0x18 /* Sync response expected */
#define NETSIO_DATA_BLOCK           0x02 /* Data payload */
#define NETSIO_DATA_ACK             0x06 /* Acknowledge data */

#define NETSIO_BUFFER_SIZE 1024

/* --- NetSIO State Management --- */

/* Initialize NetSIO state variables. */
void FujiNet_NetSIO_InitState(void);

/* Check if a client is known and the initial handshake is complete. */
BOOL FujiNet_NetSIO_IsClientConnected(void);

/* Get the current client address (if known).
 * Returns TRUE if client is known, FALSE otherwise. */
BOOL FujiNet_NetSIO_GetClientAddr(struct sockaddr_in *client_addr, socklen_t *client_len);

/* --- NetSIO Packet Processing --- */

/* Process an incoming NetSIO packet and prepare a response if needed.
 * Checks for new clients, handles handshake (PING/ALIVE), credit status, etc.
 * Returns TRUE if a response packet was generated, FALSE otherwise. */
BOOL FujiNet_NetSIO_ProcessPacket(const unsigned char *buffer, size_t len,
                                  struct sockaddr *recv_addr, socklen_t recv_addr_len,
                                  unsigned char *response_buffer, size_t *response_len);

/* --- NetSIO Command Sending --- */

/* Prepare the NetSIO sequence for an SIO command.
 * Returns the sync number used for this command, or -1 on error. */
int FujiNet_NetSIO_PrepareSIOCommandSequence(
                            UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                            const UBYTE *output_buffer, int output_len,
                            unsigned char *on_cmd_buf, size_t *on_cmd_len,
                            unsigned char *data_cmd_buf, size_t *data_cmd_len,
                            unsigned char *data_out_buf, size_t *data_out_len, 
                            unsigned char *off_sync_buf, size_t *off_sync_len);

/* Forwards an SIO command frame via NetSIO */
void FujiNet_NetSIO_ForwardSIOCommand(const UBYTE sio_frame[4], UBYTE checksum);

/* --- NetSIO Response Handling --- */

/* Check if a packet is a sync response for the expected sync.
 * Returns TRUE if it's a sync response with the expected sync, FALSE otherwise.
 * If TRUE, status_code is set to the status from the response. */
BOOL FujiNet_NetSIO_CheckSyncResponse(const unsigned char *recv_buffer, ssize_t recv_len,
                                      int expected_sync, uint8_t *status_code);

/* Process a NetSIO response to feed data back into the Atari SIO subsystem.
 * This is called when a SYNC_RESPONSE or DATA_BLOCK is received for a command
 * that we previously sent to FujiNet via NetSIO.
 * Returns TRUE if the response was handled, FALSE otherwise. */
BOOL FujiNet_NetSIO_HandleSyncResponse(const unsigned char *buffer, size_t len, int sync_num);

/* Check if there is data available from a FujiNet SIO response */
BOOL FujiNet_NetSIO_IsResponseReady(void);

/* Get the status code from a FujiNet SIO response */
UBYTE FujiNet_NetSIO_GetResponseStatus(void);

/* Get the data from a FujiNet SIO response.
 * Copies data to the given buffer, up to max_len bytes.
 * Returns the number of bytes copied.
 * After retrieving the data, marks the response as consumed. */
int FujiNet_NetSIO_GetResponseData(UBYTE *buffer, int max_len);

/* TODO: Add functions/state for handling received DATA packets */

#endif /* USE_FUJINET */

#endif /* FUJINET_NETSIO_H_ */
