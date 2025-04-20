#ifndef FUJINET_H_
#define FUJINET_H_

#include <stdint.h>
#include "atari.h" /* For UBYTE type */

#ifdef __cplusplus
extern "C" {
#endif

/* Buffer size definition needed by various modules */
#define FUJINET_BUFFER_SIZE 1024

/* Conditional Logging Macros */
#ifdef DEBUG_FUJINET
#define FUJINET_LOG_DEBUG(msg, ...) do { Log_print("FujiNet DEBUG: " msg, ##__VA_ARGS__); } while(0)
#else
#define FUJINET_LOG_DEBUG(msg, ...) ((void)0)
#endif

#ifdef USE_FUJINET

/* Include the module-specific headers */
#include "fujinet_network.h"
#include "fujinet_sio.h"

/* FujiNet Main Interface Functions */
int FujiNet_Initialise(const char *host_port);
void FujiNet_Shutdown(void);
UBYTE FujiNet_ProcessCommand(const UBYTE *command_frame);
int FujiNet_GetByte(uint8_t *byte);
int FujiNet_PutByte(uint8_t byte);
void FujiNet_SetMotorState(int on);

/* Global variable declarations */
extern int fujinet_enabled;

#endif /* USE_FUJINET */

#ifdef __cplusplus
}
#endif

#endif /* FUJINET_H_ */
