#ifndef FUJINET_H_
#define FUJINET_H_

#include "config.h" /* For HAVE_SOCKET etc. */
#include <stdint.h> /* For uint8_t type */

/* Configuration */
#define FUJINET_BUFFER_SIZE 1024 /* Max size for internal buffers */

/* Define USE_FUJINET temporarily for development, will be controlled by build system later */
#ifndef USE_FUJINET
#define USE_FUJINET 1
#endif

#ifdef USE_FUJINET

/* Global variable indicating if FujiNet is initialized and connected */
extern int fujinet_enabled;

/* Initializes the FujiNet device emulation.
   host_port is a string like "host:port", or NULL for default.
   Returns 1 on success, 0 on failure. */
int FujiNet_Initialise(const char *host_port);

/* Tears down the FujiNet device emulation. */
void FujiNet_Shutdown(void);

/* Processes a 5-byte SIO command frame and returns the immediate SIO response byte (A, N, C, E). */
UBYTE FujiNet_ProcessCommand(
    const UBYTE *command_frame /* Pointer to the 5-byte SIO command frame */
);

/* Sends a single byte to the FujiNet device.
   Returns 1 on success, 0 on failure. */
int FujiNet_PutByte(uint8_t byte);

/* Receives a single byte from the FujiNet device.
   The received byte is stored in the memory pointed to by 'byte'.
   Returns 1 on success, 0 on failure. */
int FujiNet_GetByte(uint8_t *byte);

/* Updates the motor status sent to FujiNet-PC.
   on: 1 for motor on, 0 for motor off. */
void FujiNet_SetMotorState(int on);

#endif /* USE_FUJINET */

#endif /* FUJINET_H_ */
