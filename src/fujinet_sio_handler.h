#ifndef FUJINET_SIO_HANDLER_H
#define FUJINET_SIO_HANDLER_H

#include "atari.h"  /* For UBYTE definition */

/* FujiNet SIO States */
typedef enum {
    FUJINET_SIO_IDLE,          /* No active command */
    FUJINET_SIO_COMMAND_FRAME, /* Receiving command frame */
    FUJINET_SIO_DATA_SEND,     /* Sending data to Atari (FujiNet → Atari) */
    FUJINET_SIO_DATA_RECEIVE,  /* Receiving data from Atari (Atari → FujiNet) */
    FUJINET_SIO_COMPLETION     /* Command completed, sending completion */
} FujiNet_SIO_State;

/* Device types that FujiNet can handle */
typedef enum {
    FUJINET_DEVICE_NONE = 0,
    FUJINET_DEVICE_DISK,      /* D1:-D8: (0x31-0x38) */
    FUJINET_DEVICE_FUJINET    /* FujiNet device (0x70) */
} FujiNet_Device_Type;

/* Initializes the FujiNet SIO handler */
int FujiNet_SIO_Handler_Init(void);

/* Shuts down the FujiNet SIO handler */
void FujiNet_SIO_Handler_Shutdown(void);

/* Checks if a device is handled by FujiNet 
 * Returns the device type if handled, or FUJINET_DEVICE_NONE if not */
FujiNet_Device_Type FujiNet_SIO_Is_Device_Handled(UBYTE device_id);

/* Processes a complete SIO command frame for a FujiNet device
 * Returns: ACK ('A') if command accepted, NAK ('N') if rejected, ERROR ('E') on error */
UBYTE FujiNet_SIO_Process_Command_Frame(UBYTE *command_frame);

/* Gets a byte from FujiNet device during data transfer
 * Sets is_last_byte to TRUE if this is the last byte of the transfer
 * Returns: The byte read, or -1 on error */
int FujiNet_SIO_Get_Byte(int *is_last_byte);

/* Puts a byte to FujiNet device during data transfer
 * Returns: 1 on success, 0 on failure */
int FujiNet_SIO_Put_Byte(UBYTE byte);

/* Signals command completion to FujiNet
 * Returns: 1 on success, 0 on failure */
int FujiNet_SIO_Complete(void);

/* Gets the current state of the FujiNet SIO handler */
FujiNet_SIO_State FujiNet_SIO_Get_State(void);

/* Sets the state of the FujiNet SIO handler */
void FujiNet_SIO_Set_State(FujiNet_SIO_State state);

/* Get expected bytes for current transfer */
int FujiNet_SIO_Get_Expected_Bytes(void);

#endif /* FUJINET_SIO_HANDLER_H */