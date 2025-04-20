#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "sio_handler.h"
#include "sio_state.h"
#include "sio.h"
#include "fujinet_sio_handler.h"
#include "log.h"
#include "memory.h"
#include "cpu.h"
#include "pokey.h"
#include "cassette.h"

/* Internal state */
static SIO_State current_state = SIO_STATE_IDLE;
static int expected_bytes = 0;
static int current_byte_idx = 0;
static UBYTE *data_buffer = NULL;
static int data_buffer_size = 0;
static SIO_Command_Frame current_command;
static SIO_Device_Type current_device_type = SIO_DEVICE_NONE;

/* Debug log macro */
#ifdef DEBUG_FUJINET
#define SIO_HANDLER_DEBUG_LOG(...) Log_print(__VA_ARGS__)
#else
#define SIO_HANDLER_DEBUG_LOG(...)
#endif

/* Initialize the SIO handler */
int SIO_Handler_Init(void) {
    /* Initialize state machine */
    SIO_State_Init();
    current_state = SIO_STATE_IDLE;
    expected_bytes = 0;
    current_byte_idx = 0;
    
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Initialized");
    return 1;
}

/* Shutdown the SIO handler */
void SIO_Handler_Shutdown(void) {
    /* Reset internal state */
    current_state = SIO_STATE_IDLE;
    expected_bytes = 0;
    current_byte_idx = 0;
    
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Shutdown");
}

/* Process a command frame */
void SIO_Handler_Process_Command(SIO_Command_Frame *cmd_frame, UBYTE *data, int length) {
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Processing command for device 0x%02X, command 0x%02X",
                         cmd_frame->device_id, cmd_frame->command);
    
    /* Store command frame and data for later use */
    memcpy(&current_command, cmd_frame, sizeof(SIO_Command_Frame));
    data_buffer = data;
    data_buffer_size = length;
    
    /* Check what type of device this is */
    current_device_type = SIO_State_Is_Device_Handled(cmd_frame->device_id);
    
    UBYTE result = 'N'; /* Default to NAK */
    
    switch (current_device_type) {
        case SIO_DEVICE_FUJINET:
            /* Handle FujiNet commands via the FujiNet SIO handler */
            {
                UBYTE fuji_command_frame[5];
                
                /* Assemble command frame for FujiNet */
                fuji_command_frame[0] = cmd_frame->device_id;
                fuji_command_frame[1] = cmd_frame->command;
                fuji_command_frame[2] = cmd_frame->aux1;
                fuji_command_frame[3] = cmd_frame->aux2;
                fuji_command_frame[4] = cmd_frame->checksum;
                
                SIO_HANDLER_DEBUG_LOG("SIO Handler: Calling FujiNet handler with command frame: %02X %02X %02X %02X %02X",
                                     fuji_command_frame[0], fuji_command_frame[1], fuji_command_frame[2],
                                     fuji_command_frame[3], fuji_command_frame[4]);
                
                result = FujiNet_SIO_Process_Command_Frame(fuji_command_frame);
                
                if (result == 'A') { /* Command ACKed, expect data */
                    /* Set up for data transfer */
                    current_state = SIO_STATE_DATA_TO_ATARI;
                    expected_bytes = FujiNet_SIO_Get_Expected_Bytes();
                    current_byte_idx = 0;
                    
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: FujiNet command ACKed, expecting %d bytes", expected_bytes);
                    
                    /* Schedule IRQ to signal first byte is ready */
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                } else {
                    /* Reset to idle state */
                    current_state = SIO_STATE_IDLE;
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: FujiNet command NOT ACKed, result=%c", result);
                }
            }
            break;
            
        case SIO_DEVICE_DISK:
            /* Handle standard disk drive operations using original SIO code */
            {
                int unit = cmd_frame->device_id - 0x31;
                UBYTE sio_result = 'N'; /* Default to NAK */
                
                SIO_HANDLER_DEBUG_LOG("SIO Handler: Handling disk command for unit %d", unit);
                
                /* Call appropriate disk functions based on command */
                switch (cmd_frame->command) {
                    case 0x53: /* Status */
                        if (data_buffer_size == 4) {
                            sio_result = SIO_DriveStatus(unit, data_buffer);
                            if (sio_result == 'C') {
                                result = 'A'; /* ACK */
                                current_state = SIO_STATE_DATA_TO_ATARI;
                                expected_bytes = 4;
                                current_byte_idx = 0;
                            }
                        }
                        break;
                        
                    case 0x52: /* Read Sector */
                        {
                            int realsize;
                            int sector = (cmd_frame->aux2 << 8) + cmd_frame->aux1;
                            
                            SIO_SizeOfSector((UBYTE)unit, sector, &realsize, NULL);
                            if (realsize == data_buffer_size) {
                                sio_result = SIO_ReadSector(unit, sector, data_buffer);
                                if (sio_result == 'C') {
                                    result = 'A'; /* ACK */
                                    current_state = SIO_STATE_DATA_TO_ATARI;
                                    expected_bytes = realsize;
                                    current_byte_idx = 0;
                                }
                            }
                        }
                        break;
                        
                    /* Handle other commands like Write, Format, etc. */
                    default:
                        SIO_HANDLER_DEBUG_LOG("SIO Handler: Unhandled disk command 0x%02X", cmd_frame->command);
                        break;
                }
            }
            break;
            
        case SIO_DEVICE_CASSETTE:
            /* Handle cassette operations */
            SIO_HANDLER_DEBUG_LOG("SIO Handler: Handling cassette command");
            /* ... cassette operations ... */
            break;
            
        case SIO_DEVICE_NONE:
        default:
            SIO_HANDLER_DEBUG_LOG("SIO Handler: Device 0x%02X not handled", cmd_frame->device_id);
            break;
    }
    
    /* Set CPU registers based on result */
    SIO_Handler_Set_CPU_Registers(result);
}

/* Put a byte from the Atari to a device */
void SIO_Handler_Put_Byte(int byte) {
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Put byte 0x%02X, current state=%d", byte, current_state);
    
    switch (current_state) {
        case SIO_STATE_COMMAND_FRAME:
            /* Building command frame */
            if (current_byte_idx < 5) {
                UBYTE *cmd_bytes = (UBYTE *)&current_command;
                cmd_bytes[current_byte_idx++] = byte;
                
                SIO_HANDLER_DEBUG_LOG("SIO Handler: Command frame byte %d: 0x%02X", current_byte_idx, byte);
                
                if (current_byte_idx == 5) {
                    /* Complete command frame received */
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: Full command frame received: %02X %02X %02X %02X %02X",
                                         current_command.device_id, current_command.command,
                                         current_command.aux1, current_command.aux2,
                                         current_command.checksum);
                    
                    /* Check device type */
                    current_device_type = SIO_State_Is_Device_Handled(current_command.device_id);
                    
                    if (current_device_type != SIO_DEVICE_NONE) {
                        /* Valid device, transition to wait for ACK */
                        current_state = SIO_STATE_WAIT_ACK;
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL + SIO_ACK_INTERVAL;
                    } else {
                        /* Invalid device, reset state */
                        SIO_HANDLER_DEBUG_LOG("SIO Handler: Invalid device ID 0x%02X", current_command.device_id);
                        current_state = SIO_STATE_IDLE;
                    }
                }
            } else {
                SIO_HANDLER_DEBUG_LOG("SIO Handler: ERROR - Too many command frame bytes");
                current_state = SIO_STATE_IDLE;
            }
            break;
            
        case SIO_STATE_DATA_FROM_ATARI:
            /* Handle data being sent from Atari to device */
            if (current_device_type == SIO_DEVICE_FUJINET) {
                /* Pass the byte to FujiNet */
                if (FujiNet_SIO_Put_Byte(byte)) {
                    current_byte_idx++;
                    
                    if (current_byte_idx >= expected_bytes) {
                        /* All data received, transition to completion */
                        current_state = SIO_STATE_COMPLETION;
                        FujiNet_SIO_Complete();
                    }
                } else {
                    /* Error handling byte */
                    current_state = SIO_STATE_ERROR;
                }
            } else if (current_device_type == SIO_DEVICE_DISK) {
                /* Store byte in data buffer */
                if (current_byte_idx < data_buffer_size) {
                    data_buffer[current_byte_idx++] = byte;
                    
                    if (current_byte_idx >= expected_bytes) {
                        /* All data received, process it */
                        current_state = SIO_STATE_COMPLETION;
                        
                        /* Original SIO disk operations would go here */
                    }
                } else {
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: ERROR - Data buffer overflow");
                    current_state = SIO_STATE_ERROR;
                }
            }
            break;
            
        default:
            SIO_HANDLER_DEBUG_LOG("SIO Handler: WARNING - Byte 0x%02X received in unexpected state %d",
                                 byte, current_state);
            break;
    }
    
    /* Always pass byte to cassette */
    CASSETTE_PutByte(byte);
}

/* Get a byte from a device to the Atari */
int SIO_Handler_Get_Byte(void) {
    int byte = 0;
    
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Get byte, current state=%d", current_state);
    
    switch (current_state) {
        case SIO_STATE_WAIT_ACK:
            /* Process command and get status */
            {
                UBYTE *data = NULL;
                int length = 0;
                
                /* Call original SIO_Handler equivalent */
                SIO_Handler_Process_Command(&current_command, data, length);
                
                /* Return ACK/NAK as appropriate (will be set by Process_Command) */
                byte = CPU_regA;
            }
            break;
            
        case SIO_STATE_DATA_TO_ATARI:
            /* Return data from device to Atari */
            if (current_device_type == SIO_DEVICE_FUJINET) {
                /* Get byte from FujiNet */
                int is_last_byte = 0;
                int fuji_byte = FujiNet_SIO_Get_Byte(&is_last_byte);
                
                if (fuji_byte >= 0) {
                    byte = fuji_byte;
                    current_byte_idx++;
                    
                    /* Schedule next byte IRQ */
                    if (is_last_byte) {
                        /* Last byte, transition to completion */
                        current_state = SIO_STATE_COMPLETION;
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    } else {
                        /* More bytes to come */
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    }
                } else {
                    /* Error getting byte */
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: Error getting byte from FujiNet");
                    current_state = SIO_STATE_ERROR;
                    byte = 'E';
                }
            } else if (current_device_type == SIO_DEVICE_DISK) {
                /* Get byte from data buffer */
                if (current_byte_idx < expected_bytes) {
                    byte = data_buffer[current_byte_idx++];
                    
                    if (current_byte_idx >= expected_bytes) {
                        /* All data sent, transition to idle */
                        current_state = SIO_STATE_IDLE;
                    } else {
                        /* Schedule next byte IRQ */
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    }
                } else {
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: ERROR - Trying to read beyond data buffer");
                    current_state = SIO_STATE_ERROR;
                    byte = 'E';
                }
            }
            break;
            
        case SIO_STATE_COMPLETION:
            /* Send completion byte */
            byte = 'C'; /* Command complete */
            current_state = SIO_STATE_IDLE;
            SIO_HANDLER_DEBUG_LOG("SIO Handler: Sending completion byte");
            break;
            
        default:
            /* Get byte from cassette if no other handler */
            byte = CASSETTE_GetByte();
            break;
    }
    
    return byte;
}

/* Set CPU registers based on SIO result */
void SIO_Handler_Set_CPU_Registers(UBYTE result) {
    /* Set CPU registers to return status to the OS */
    CPU_regA = result;
    
    if (result == 'A' || result == 'C') { /* ACK or COMPLETE */
        CPU_regY = 1; /* SUCCESS */
        CPU_ClrN;
        CPU_SetC;
    } else { /* NAK or ERROR */
        CPU_regY = 0; /* FAILURE */
        CPU_SetN;
        CPU_ClrC;
    }
    
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Set CPU registers A=0x%02X, Y=%d, N=%d, C=%d",
                         result, CPU_regY, (CPU_regP & CPU_N_FLAG) ? 1 : 0, (CPU_regP & CPU_C_FLAG) ? 1 : 0);
}