#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "config.h"
#include "cpu.h"
#include "memory.h"
#include "pokey.h"
#include "sio.h"
#include "sio_state.h"
#include "sio_handler.h"
#include "fujinet_sio_handler.h"
#include "fujinet.h"
#include "fujinet_sio.h"
#include "log.h"
#include "cassette.h"

/* Debug logging macros */
#ifdef DEBUG_FUJINET
#define SIO_HANDLER_DEBUG_LOG(msg, ...) \
    do { \
        Log_print("FujiNet DEBUG (sio_handler.c): " msg, ##__VA_ARGS__); \
    } while(0)
#else
#define SIO_HANDLER_DEBUG_LOG(msg, ...) do {} while(0)
#endif

/* Debug logging macro for FUJINET if not defined */
#ifndef FUJINET_DEBUG_LOG
#ifdef DEBUG_FUJINET
#define FUJINET_DEBUG_LOG(msg, ...) \
    do { \
        Log_print("FujiNet DEBUG: " msg, ##__VA_ARGS__); \
    } while(0)
#else
#define FUJINET_DEBUG_LOG(msg, ...) do {} while(0)
#endif
#endif

/* Internal state */
static SIO_State current_state = SIO_STATE_IDLE;
static int expected_bytes = 0;
static int current_byte_idx = 0;
static UBYTE *data_buffer = NULL;
static int data_buffer_size = 0;
static SIO_Command_Frame current_command;
static SIO_Device_Type current_device_type = SIO_DEVICE_NONE;

/* Initialize the SIO handler */
int SIO_Handler_Init(void) {
    /* Initialize state machine to expect command frames */
    current_state = SIO_STATE_COMMAND_FRAME;
    current_byte_idx = 0;
    expected_bytes = 5; /* Standard command frame is 5 bytes */
    
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Initialized (state=%d)", current_state);
    return 1;
}

/* Shutdown the SIO handler */
void SIO_Handler_Shutdown(void) {
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Shutdown");
}

/* Process a command frame */
void SIO_Handler_Process_Command(SIO_Command_Frame *cmd_frame, UBYTE *data, int length) {
    /* Save data buffer for later use */
    data_buffer = data;
    data_buffer_size = length;
    
    /* Create command array for FujiNet SIO processing */
    UBYTE command_array[5];
    command_array[0] = cmd_frame->device_id;
    command_array[1] = cmd_frame->command;
    command_array[2] = cmd_frame->aux1;
    command_array[3] = cmd_frame->aux2;
    command_array[4] = cmd_frame->checksum;
    
    /* Debug log the command being sent to FujiNet */
    SIO_HANDLER_DEBUG_LOG("SIO_Handler: Routing command to FujiNet: %02X %02X %02X %02X %02X",
                        command_array[0], command_array[1], command_array[2], 
                        command_array[3], command_array[4]);
    
    /* Process command through FujiNet */
    UBYTE fujinet_response_byte = FujiNet_SIO_ProcessCommand(command_array);
    
    SIO_HANDLER_DEBUG_LOG("SIO_Handler: FujiNet_SIO_ProcessCommand returned %c (0x%02X)", 
                       fujinet_response_byte, fujinet_response_byte);
    
    /* Map FujiNet result to SIO protocol response */
    UBYTE sio_response;
    switch (fujinet_response_byte) {
        case 'A': /* ACK */
            sio_response = 'A';
            break;
        case 'N': /* NAK */
            sio_response = 'N';
            break;
        case 'C': /* COMPLETE */
            sio_response = 'C';
            break;
        case 'E': /* ERROR */
            sio_response = 'E';
            break;
        default: /* Unknown response */
            SIO_HANDLER_DEBUG_LOG("SIO_Handler: Unknown response byte: %c (0x%02X)", 
                               fujinet_response_byte, fujinet_response_byte);
            sio_response = 'E';
            break;
    }

    /* If the command was acknowledged (device will send data),
     * set state for data transfer. Otherwise, reset state machine
     * to expect another command frame */
    if (sio_response == 'A') {
        SIO_HANDLER_DEBUG_LOG("SIO_Handler: FujiNet command ACKed, proceeding to data phase");
        current_state = SIO_STATE_DATA_TO_ATARI;
        expected_bytes = FujiNet_SIO_Get_Expected_Bytes();
        current_byte_idx = 0;
        
        SIO_HANDLER_DEBUG_LOG("SIO_Handler: FujiNet command ACKed, expecting %d bytes", expected_bytes);
        
        /* Schedule IRQ to signal first byte is ready */
        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
    } else {
        SIO_HANDLER_DEBUG_LOG("SIO_Handler: FujiNet command NOT ACKed, resetting for next command");
        /* Reset for next command frame */
        current_state = SIO_STATE_COMMAND_FRAME;
        current_byte_idx = 0;
        expected_bytes = 5;
    }
    
    /* Set CPU registers based on result */
    SIO_Handler_Set_CPU_Registers(sio_response);
}

/* Put a byte from the Atari to a device */
void SIO_Handler_Put_Byte(UBYTE byte) {
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Put byte 0x%02X, current state=%d", byte, current_state);
    
    switch (current_state) {
        case SIO_STATE_COMMAND_FRAME:
            /* Collect bytes for the command frame */
            if (current_byte_idx < expected_bytes) {
                /* Store byte in command frame */
                UBYTE *frame_bytes = (UBYTE *)&current_command;
                frame_bytes[current_byte_idx++] = byte;
                
                SIO_HANDLER_DEBUG_LOG("SIO Handler: Command frame byte %d = 0x%02X", 
                                     current_byte_idx - 1, byte);
                
                if (current_byte_idx >= expected_bytes) {
                    /* We have a complete command frame, process it */
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: Full command frame received: %02X %02X %02X %02X %02X",
                                         current_command.device_id, current_command.command,
                                         current_command.aux1, current_command.aux2,
                                         current_command.checksum);
                    
                    /* Process the command */
                    SIO_Handler_Process_Command(&current_command, NULL, 0);
                    
                    /* Reset for next command frame */
                    current_byte_idx = 0;
                }
            } else {
                SIO_HANDLER_DEBUG_LOG("SIO Handler: ERROR - Too many command frame bytes");
                /* Reset to start of command frame */
                current_byte_idx = 0;
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
    SIO_HANDLER_DEBUG_LOG("SIO Handler: Get byte, current state=%d", current_state);
    
    int result_byte = 0;
    
    switch (current_state) {
        case SIO_STATE_DATA_TO_ATARI:
            /* Check if we have a data buffer */
            if (data_buffer != NULL && current_byte_idx < data_buffer_size) {
                /* Return byte from data buffer */
                result_byte = data_buffer[current_byte_idx++];
                
                /* Schedule next byte */
                if (current_byte_idx < data_buffer_size) {
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                } else {
                    /* Last byte sent, next we'll send completion */
                    current_state = SIO_STATE_COMPLETION;
                    POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                }
                return result_byte;
            } else {
                /* Try getting data from FujiNet */
                uint8_t fuji_byte;
                int fuji_result = FujiNet_SIO_GetByte(&fuji_byte);
                
                if (fuji_result == 1) {
                    int current_pos = FujiNet_SIO_GetResponseBufferPos();
                    int buffer_size = FujiNet_SIO_GetResponseBufferSize();
                    
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: Got byte 0x%02X from FujiNet (%d/%d)", 
                                         fuji_byte, current_pos, buffer_size);
                    
                    /* If this was the last byte, transition to completion */
                    if (current_pos >= buffer_size) {
                        current_state = SIO_STATE_COMPLETION;
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    } else {
                        /* Schedule next byte */
                        POKEY_DELAYED_SERIN_IRQ = SIO_SERIN_INTERVAL;
                    }
                    
                    return fuji_byte;
                } else {
                    SIO_HANDLER_DEBUG_LOG("SIO Handler: Error getting byte from FujiNet");
                    /* Reset to command frame state on error */
                    current_state = SIO_STATE_COMMAND_FRAME;
                    current_byte_idx = 0;
                    expected_bytes = 5;
                    return SIO_ERROR_FRAME;
                }
            }
            break;
            
        case SIO_STATE_DATA_FROM_ATARI:
            if (data_buffer != NULL && current_byte_idx < data_buffer_size) {
                SIO_HANDLER_DEBUG_LOG("SIO Handler: Data from Atari (index=%d)", current_byte_idx);
                return data_buffer[current_byte_idx++];
            } else {
                SIO_HANDLER_DEBUG_LOG("SIO Handler: ERROR - Trying to read beyond data buffer");
                /* Reset to command frame state on error */
                current_state = SIO_STATE_COMMAND_FRAME;
                current_byte_idx = 0;
                expected_bytes = 5;
                return SIO_ERROR_FRAME;
            }
            break;
            
        case SIO_STATE_COMPLETION:
            /* Send completion byte and reset for next command */
            SIO_HANDLER_DEBUG_LOG("SIO Handler: Sending completion byte");
            current_state = SIO_STATE_COMMAND_FRAME;
            current_byte_idx = 0;
            expected_bytes = 5;
            return SIO_COMPLETE_FRAME;
            
        default:
            /* In any other state, just return error and reset to command frame state */
            SIO_HANDLER_DEBUG_LOG("SIO Handler: Get byte called in unexpected state %d", current_state);
            current_state = SIO_STATE_COMMAND_FRAME;
            current_byte_idx = 0;
            expected_bytes = 5;
            return SIO_ERROR_FRAME;
    }
    
    /* Should not reach here, but just in case */
    current_state = SIO_STATE_COMMAND_FRAME;
    current_byte_idx = 0;
    expected_bytes = 5;
    return SIO_ERROR_FRAME;
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