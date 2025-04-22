/*
 * fujinet_netsio.h - FujiNet NetSIO integration
 *
 * Copyright (C) 2023-2024 Atari800 development team
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef FUJINET_NETSIO_H_
#define FUJINET_NETSIO_H_

#include <stdint.h>
#include "atari.h" /* For UBYTE type */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the FujiNet NetSIO integration */
int FujiNet_NetSIO_Initialize(const char *host, int port);

/* Shutdown the FujiNet NetSIO integration */
void FujiNet_NetSIO_Shutdown(void);

/* Process incoming NetSIO messages */
void FujiNet_NetSIO_ProcessMessages(void);

/* Send an SIO command to FujiNet via NetSIO */
int FujiNet_NetSIO_SendCommand(const UBYTE *command_frame);

/* Get a byte from the response buffer */
int FujiNet_NetSIO_GetByte(uint8_t *byte);

/* Send a byte to FujiNet via NetSIO */
int FujiNet_NetSIO_PutByte(uint8_t byte);

/* Set motor state via NetSIO */
void FujiNet_NetSIO_SetMotorState(int on);

/* Get the response buffer position */
int FujiNet_NetSIO_GetResponseBufferPos(void);

/* Get the response buffer size */
int FujiNet_NetSIO_GetResponseBufferSize(void);

/* Message handlers */
void FujiNet_NetSIO_HandleDataByte(uint8_t byte);
void FujiNet_NetSIO_HandleDataBlock(const uint8_t *data, uint16_t data_length);
void FujiNet_NetSIO_HandleSyncResponse(uint8_t sync_number, uint8_t ack_type, uint8_t ack_byte, uint16_t write_size);

/* Register message handlers with the NetSIO module */
void FujiNet_NetSIO_RegisterHandlers(void);

#ifdef __cplusplus
}
#endif

#endif /* FUJINET_NETSIO_H_ */
