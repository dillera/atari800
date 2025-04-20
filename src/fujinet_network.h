/*
 * fujinet_network.h - FujiNet network communication functions
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

#ifndef FUJINET_NETWORK_H_
#define FUJINET_NETWORK_H_

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Network Configuration defaults */
#define FUJINET_DEFAULT_HOST "localhost"
#define FUJINET_DEFAULT_PORT 9996

/* Timeout for network operations (ms) */
#define FUJINET_TIMEOUT_MS 500

/* Altirra NetSIO Protocol constants */
#define NETSIO_DATA_BYTE    0x01
#define NETSIO_STATUS_BYTE  0x02
#define NETSIO_COMMAND_OFF  0x10

#define EVENT_SCRIPT_POST   0x01
#define EVENT_CONNECTED     0x02
#define EVENT_DISCONNECTED  0x03
#define EVENT_SHUTDOWN      0x04

/* Public Function Declarations */
int Network_Initialize(const char *host_port);
void Network_Shutdown(void);
int Network_IsConnected(void);

/* NetSIO Protocol Functions */
int Network_SendAltirraMessage(uint8_t event, uint8_t arg, const uint8_t *data, int data_len);
int Network_ProcessAltirraMessage(void);
int Network_GetByte(uint8_t *byte);
int Network_PutByte(uint8_t byte);

/* Helper Functions - may be made private if not needed externally */
int Network_SendData(const uint8_t *data, int data_len);
int Network_ReadExactBytes(uint8_t *buffer, int buffer_size, int *received_len);

#ifdef __cplusplus
}
#endif

#endif /* FUJINET_NETWORK_H_ */
