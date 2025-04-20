#ifndef SIO_HANDLER_H
#define SIO_HANDLER_H

#include "atari.h"
#include "sio_state.h"
#include "sio.h"

/* Command frame structure */
typedef struct {
    UBYTE device_id;    /* Device ID (e.g., 0x31 for D1:, 0x70 for FujiNet) */
    UBYTE command;      /* Command code (e.g., 0x53 for Status) */
    UBYTE aux1;         /* Auxiliary byte 1 */
    UBYTE aux2;         /* Auxiliary byte 2 */
    UBYTE checksum;     /* Command frame checksum */
} SIO_Command_Frame;

/* Initialize the SIO handler */
int SIO_Handler_Init(void);

/* Shutdown the SIO handler */
void SIO_Handler_Shutdown(void);

/* Process a command frame (improved version of the original SIO_Handler) */
void SIO_Handler_Process_Command(SIO_Command_Frame *cmd_frame, UBYTE *data, int length);

/* Put a byte from the Atari to a device (refactored SIO_PutByte) */
void SIO_Handler_Put_Byte(UBYTE byte);

/* Get a byte from a device to the Atari (refactored SIO_GetByte) */
int SIO_Handler_Get_Byte(void);

/* Set CPU registers based on SIO result */
void SIO_Handler_Set_CPU_Registers(UBYTE result);

#endif /* SIO_HANDLER_H */