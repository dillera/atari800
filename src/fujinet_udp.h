// src/fujinet_udp.h
#ifndef FUJINET_UDP_H_
#define FUJINET_UDP_H_

#ifdef USE_FUJINET

#include "config.h" // BOOL, UBYTE etc.
#include <sys/socket.h> // For socklen_t, struct sockaddr_in
#include <stdbool.h>    // For bool

// Initializes the UDP socket on the specified port.
// Returns the socket file descriptor on success, -1 on failure.
int FujiNet_UDP_Init(int port);

// Shuts down the UDP socket.
void FujiNet_UDP_Shutdown(int sockfd);

// Checks if data is available to read on the socket (non-blocking).
// Returns TRUE if data is ready, FALSE otherwise or on error.
BOOL FujiNet_UDP_Poll(int sockfd);

// Receives a UDP packet.
// Returns the number of bytes received, or -1 on error.
// Fills in client_addr and client_len.
ssize_t FujiNet_UDP_Receive(int sockfd, unsigned char *buffer, size_t buffer_size,
                           struct sockaddr_in *client_addr, socklen_t *client_len);

// Sends a UDP packet to the specified client address.
// Returns the number of bytes sent, or -1 on error.
ssize_t FujiNet_UDP_Send(int sockfd, const unsigned char *buffer, size_t len,
                        const struct sockaddr_in *client_addr, socklen_t client_len);

#endif /* USE_FUJINET */

#endif /* FUJINET_UDP_H_ */
