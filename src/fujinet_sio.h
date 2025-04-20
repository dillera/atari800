/*
 * fujinet_sio.h - FujiNet SIO command handling functions
 *
 * Copyright (C) 2020-2023 AtariGeekyJames
 * Copyright (C) 2023 DigiDude
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

#ifndef FUJINET_SIO_H_
#define FUJINET_SIO_H_

#include <stdint.h>
#include "atari.h" /* For UBYTE type */

#ifdef __cplusplus
extern "C" {
#endif

/* FujiNet device ID for SIO commands */
#define FUJINET_DEVICE_ID 0x70

/* SIO command response codes */
#define SIO_COMMAND_ACCEPTED 0x01  /* Command was accepted and sent to NetSIO hub */
#define FUJINET_SIO_ERROR    0xFF  /* Error sending command to NetSIO hub */

/* Legacy FujiNet SIO response codes - kept for reference */
#define FUJINET_SIO_COMPLETE      0x01  /* Command completed, no error */
#define FUJINET_SIO_ERROR_FRAME   0x8F  /* Command frame error */
#define FUJINET_SIO_ERROR_DEVICE  0x90  /* Device error */
#define FUJINET_SIO_ERROR_NAK     0xFE  /* No acknowledgement (NAK) response */
#define FUJINET_SIO_ERROR_GENERAL 0xFF  /* General error */

/* Public Function Declarations */
void FujiNet_SIO_Initialize(void);
void FujiNet_SIO_Shutdown(void);

UBYTE FujiNet_SIO_ProcessCommand(const UBYTE *command_frame);
int FujiNet_SIO_GetByte(uint8_t *byte);
int FujiNet_SIO_PutByte(uint8_t byte);

/* Device emulation */
void FujiNet_SIO_SetMotorState(int on);
int FujiNet_SIO_IsDeviceEnabled(void);

#ifdef __cplusplus
}
#endif

#endif /* FUJINET_SIO_H_ */
