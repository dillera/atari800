#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "fujinet_sio_handler.h"
#include "fujinet_sio.h"
#include "fujinet_network.h"
#include "log.h"
#include "sio.h"
#include "memory.h"
#include "cpu.h"

/* Internal state */
static FujiNet_SIO_State current_state = FUJINET_SIO_IDLE;
static int expected_bytes = 0;
static int current_byte_idx = 0;
static int is_fujinet_initialized = 0;

/* Debug log macro */
#ifdef DEBUG_FUJINET
#define FUJINET_SIO_DEBUG_LOG(...) Log_print(__VA_ARGS__)
#else
#define FUJINET_SIO_DEBUG_LOG(...)
#endif

/* Initializes the FujiNet SIO handler */
int FujiNet_SIO_Handler_Init(void) {
    if (is_fujinet_initialized) {
        return 1; // Already initialized
    }
    
    // Reset internal state
    current_state = FUJINET_SIO_IDLE;
    expected_bytes = 0;
    current_byte_idx = 0;
    
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Initialized");
    is_fujinet_initialized = 1;
    return 1;
}

/* Shuts down the FujiNet SIO handler */
void FujiNet_SIO_Handler_Shutdown(void) {
    if (!is_fujinet_initialized) {
        return; // Not initialized
    }
    
    // Reset internal state
    current_state = FUJINET_SIO_IDLE;
    expected_bytes = 0;
    current_byte_idx = 0;
    
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Shutdown");
    is_fujinet_initialized = 0;
}

/* Checks if a device is handled by FujiNet */
FujiNet_Device_Type FujiNet_SIO_Is_Device_Handled(UBYTE device_id) {
    if (!is_fujinet_initialized) {
        return FUJINET_DEVICE_NONE;
    }
    
    // Check if it's a FujiNet device
    if (device_id == 0x70) {
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Device 0x70 (FujiNet) is handled");
        return FUJINET_DEVICE_FUJINET;
    }
    
    // Check if it's a disk device (D1:-D8:)
    if (device_id >= 0x31 && device_id <= 0x38) {
        // Only handle disk devices if they're disabled locally in the emulator
        // or if FujiNet explicitly wants to handle them
        int unit = device_id - 0x31;
        if (SIO_drive_status[unit] == SIO_OFF) {
            FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Device 0x%02X (Disk) is handled (local disk OFF)", device_id);
            return FUJINET_DEVICE_DISK;
        }
    }
    
    return FUJINET_DEVICE_NONE;
}

/* Processes a complete SIO command frame for a FujiNet device */
UBYTE FujiNet_SIO_Process_Command_Frame(UBYTE *command_frame) {
    if (!is_fujinet_initialized) {
        return 'E'; // Error
    }
    
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Processing command frame: %02X %02X %02X %02X %02X",
                         command_frame[0], command_frame[1], command_frame[2], 
                         command_frame[3], command_frame[4]);
    
    // Call the FujiNet SIO module to process the command
    UBYTE result = FujiNet_SIO_ProcessCommand(command_frame);
    
    // Map the result code to SIO protocol response
    UBYTE sio_response;
    if (result == 1) { // Success/ACK
        sio_response = 'A'; // ACK
        
        // Set up for data transfer
        current_state = FUJINET_SIO_DATA_SEND;
        current_byte_idx = 0;
        expected_bytes = FujiNet_SIO_GetResponseBufferSize();
        
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Command ACKed, expecting %d bytes", expected_bytes);
    }
    else if (result == 0) { // NAK
        sio_response = 'N'; // NAK
        current_state = FUJINET_SIO_IDLE;
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Command NAKed");
    }
    else { // Error
        sio_response = 'E'; // ERROR
        current_state = FUJINET_SIO_IDLE;
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Command Error");
    }
    
    return sio_response;
}

/* Gets a byte from FujiNet device during data transfer */
int FujiNet_SIO_Get_Byte(int *is_last_byte) {
    if (!is_fujinet_initialized || current_state != FUJINET_SIO_DATA_SEND) {
        return -1; // Error
    }
    
    // Default to not last byte
    if (is_last_byte) {
        *is_last_byte = 0;
    }
    
    uint8_t byte;
    int result = FujiNet_SIO_GetByte(&byte);
    
    if (result == 1) { // Success
        current_byte_idx++;
        
        // Check if this is the last byte
        if (current_byte_idx >= expected_bytes) {
            if (is_last_byte) {
                *is_last_byte = 1;
            }
            current_state = FUJINET_SIO_COMPLETE;
            FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Last byte sent (0x%02X), transitioning to COMPLETE", byte);
        } else {
            FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Byte sent: 0x%02X (%d/%d)", 
                                 byte, current_byte_idx, expected_bytes);
        }
        
        return byte;
    }
    else if (result == 0) { // No more data
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: No more data available");
        current_state = FUJINET_SIO_IDLE;
        return -1;
    }
    else { // Error
        FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Error getting byte");
        current_state = FUJINET_SIO_IDLE;
        return -1;
    }
}

/* Puts a byte to FujiNet device during data transfer */
int FujiNet_SIO_Put_Byte(UBYTE byte) {
    if (!is_fujinet_initialized || current_state != FUJINET_SIO_DATA_RECEIVE) {
        return 0; // Failure
    }
    
    // TODO: Implement when needed for write operations
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Put byte not yet implemented");
    
    return 0; // Failure for now
}

/* Signals command completion to FujiNet */
int FujiNet_SIO_Complete(void) {
    if (!is_fujinet_initialized || current_state != FUJINET_SIO_COMPLETE) {
        return 0; // Failure
    }
    
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: Command completed");
    
    // Reset internal state
    current_state = FUJINET_SIO_IDLE;
    current_byte_idx = 0;
    expected_bytes = 0;
    
    return 1; // Success
}

/* Gets the current state of the FujiNet SIO handler */
FujiNet_SIO_State FujiNet_SIO_Get_State(void) {
    return current_state;
}

/* Sets the state of the FujiNet SIO handler */
void FujiNet_SIO_Set_State(FujiNet_SIO_State state) {
    FUJINET_SIO_DEBUG_LOG("FujiNet SIO Handler: State change %d -> %d", current_state, state);
    current_state = state;
}

/* Get expected bytes for current transfer */
int FujiNet_SIO_Get_Expected_Bytes(void) {
    return expected_bytes;
}