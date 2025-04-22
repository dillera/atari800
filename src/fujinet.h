/* src/fujinet.h */
#ifndef FUJINET_H_
#define FUJINET_H_

#include "config.h" /* For Atari800 types like UBYTE */
#include "atari.h" /* For Atari800 types */
#include <time.h>

#ifdef USE_FUJINET

#include <stdbool.h> /* For bool type */

/* Define BOOL type if not already defined */
#ifndef BOOL
typedef int BOOL;
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

/* Constants */
#define BUFFER_SIZE 1024                 /* Size of UDP receive buffer */
#define FUJINET_RESPONSE_TIMEOUT_MS 5000 /* Timeout in milliseconds for SIO command response */

/* --- NetSIO Definitions (subset needed for interface) --- */
#define NETSIO_HUB_PORT 9997
#define DEFAULT_CREDITS 3
#define NETSIO_MAX_PACKET_SIZE 1024 

/* --- Public Function Prototypes --- */

/* Flag to indicate if the emulator is waiting for a FujiNet SIO response */
extern int fujinet_WaitingForSync;

/* Initializes the FujiNet UDP socket and communication state.
 * Returns TRUE on success, FALSE on failure. */
BOOL FujiNet_Initialise(void);

/* Shuts down the FujiNet UDP socket. */
void FujiNet_Shutdown(void);

/* Checks for and processes incoming NetSIO packets.
 * Should be called periodically. */
void FujiNet_Update(void);

/* Checks if a FujiNet device is currently considered connected. */
BOOL FujiNet_IsConnected(void);

/* Sends an SIO command (like Status, Read, Write) via NetSIO.
 * device_id: The SIO device ID (e.g., 0x31 for D1:, 0x70 for FujiNet itself)
 * command: The SIO command byte (e.g., 0x53 for Status)
 * aux1, aux2: SIO auxiliary bytes
 * output_buffer: Pointer to buffer for data *sent* to the device (e.g., sector data for write)
 * output_len: Length of data in output_buffer
 * input_buffer: Pointer to buffer to receive data *from* the device (e.g., sector data for read)
 * input_len_ptr: Pointer to store the length of data received in input_buffer
 * Returns: SIO completion code (e.g., 'A', 'C', 'E') - TBD how to map NetSIO status */
char FujiNet_SendSIOCommand(UBYTE device_id, UBYTE command, UBYTE aux1, UBYTE aux2,
                             const UBYTE *output_buffer, int output_len,
                             UBYTE *input_buffer, int *input_len_ptr);

#endif /* USE_FUJINET */

#endif /* FUJINET_H_ */
