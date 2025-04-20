#ifndef SIO_STATE_H
#define SIO_STATE_H

#include "atari.h"

/* Improved SIO state machine states */
typedef enum {
    SIO_STATE_IDLE,            /* No active SIO operation */
    SIO_STATE_COMMAND_FRAME,   /* Receiving command frame */
    SIO_STATE_WAIT_ACK,        /* Waiting for device to ACK command */
    SIO_STATE_DATA_TO_ATARI,   /* Transferring data from device to Atari */
    SIO_STATE_DATA_FROM_ATARI, /* Transferring data from Atari to device */
    SIO_STATE_COMPLETION,      /* Sending completion byte */
    SIO_STATE_ERROR            /* Error state */
} SIO_State;

/* Device type identifiers */
typedef enum {
    SIO_DEVICE_NONE = 0,
    SIO_DEVICE_DISK,           /* D1:-D8: (0x31-0x38) */
    SIO_DEVICE_CASSETTE,       /* Cassette (0x60) */
    SIO_DEVICE_FUJINET         /* FujiNet device (0x70) */
} SIO_Device_Type;

/* SIO result codes */
#define SIO_RESULT_SUCCESS 'C'  /* Command completed successfully */
#define SIO_RESULT_ACK     'A'  /* Command acknowledged */
#define SIO_RESULT_NAK     'N'  /* Command not acknowledged */
#define SIO_RESULT_ERROR   'E'  /* Error during command processing */

/* Function prototypes */

/* Initialize the SIO state machine */
void SIO_State_Init(void);

/* Reset the SIO state machine to idle */
void SIO_State_Reset(void);

/* Set the current SIO state */
void SIO_State_Set(SIO_State state);

/* Get the current SIO state */
SIO_State SIO_State_Get(void);

/* Check if a device is handled by the SIO subsystem 
 * Returns the device type if handled, or SIO_DEVICE_NONE if not */
SIO_Device_Type SIO_State_Is_Device_Handled(UBYTE device_id);

#endif /* SIO_STATE_H */