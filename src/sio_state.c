#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atari.h"
#include "sio_state.h"
#include "sio.h"
#include "fujinet_sio_handler.h"
#include "log.h"

/* Internal state */
static SIO_State current_state = SIO_STATE_IDLE;
static int is_initialized = 0;

/* Debug log macro */
#ifdef DEBUG_FUJINET
#define SIO_STATE_DEBUG_LOG(...) Log_print(__VA_ARGS__)
#else
#define SIO_STATE_DEBUG_LOG(...)
#endif

/* Initialize the SIO state machine */
void SIO_State_Init(void) {
    if (is_initialized) {
        return; // Already initialized
    }
    
    current_state = SIO_STATE_IDLE;
    SIO_STATE_DEBUG_LOG("SIO State Machine: Initialized");
    is_initialized = 1;
}

/* Reset the SIO state machine to idle */
void SIO_State_Reset(void) {
    SIO_State old_state = current_state;
    current_state = SIO_STATE_IDLE;
    SIO_STATE_DEBUG_LOG("SIO State Machine: Reset from %d to IDLE", old_state);
}

/* Set the current SIO state */
void SIO_State_Set(SIO_State state) {
    if (current_state == state) {
        return; // No change
    }
    
    SIO_STATE_DEBUG_LOG("SIO State Machine: State change %d -> %d", current_state, state);
    current_state = state;
}

/* Get the current SIO state */
SIO_State SIO_State_Get(void) {
    return current_state;
}

/* Check if a device is handled by the SIO subsystem */
SIO_Device_Type SIO_State_Is_Device_Handled(UBYTE device_id) {
    // Check for FujiNet devices first
    extern int fujinet_enabled; // Defined in sio.c
    
    if (fujinet_enabled) {
        FujiNet_Device_Type fuji_type = FujiNet_SIO_Is_Device_Handled(device_id);
        
        if (fuji_type == FUJINET_DEVICE_FUJINET) {
            SIO_STATE_DEBUG_LOG("SIO State Machine: Device 0x%02X handled by FujiNet", device_id);
            return SIO_DEVICE_FUJINET;
        }
        else if (fuji_type == FUJINET_DEVICE_DISK) {
            SIO_STATE_DEBUG_LOG("SIO State Machine: Disk device 0x%02X handled by FujiNet", device_id);
            return SIO_DEVICE_DISK;
        }
    }
    
    // Check for standard disk devices
    if (device_id >= 0x31 && device_id <= 0x38) {
        int unit = device_id - 0x31;
        
        if (SIO_drive_status[unit] != SIO_OFF) {
            SIO_STATE_DEBUG_LOG("SIO State Machine: Disk device 0x%02X handled by emulator", device_id);
            return SIO_DEVICE_DISK;
        }
    }
    
    // Check for cassette device
    if (device_id == 0x60) {
        SIO_STATE_DEBUG_LOG("SIO State Machine: Cassette device 0x60 handled by emulator");
        return SIO_DEVICE_CASSETTE;
    }
    
    SIO_STATE_DEBUG_LOG("SIO State Machine: Device 0x%02X not handled", device_id);
    return SIO_DEVICE_NONE;
}