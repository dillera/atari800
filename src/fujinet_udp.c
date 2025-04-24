/* src/fujinet_udp.c */
#include "config.h"
#include "atari.h"  /* For BOOL type */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* Ensure BOOL type is defined */
#ifndef BOOL
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#endif

#ifdef USE_FUJINET

#include <unistd.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>

#include "log.h"
#include "fujinet_udp.h"

/* Initializes the UDP socket on the specified port.
 * Returns the socket file descriptor on success, -1 on failure. */
int FujiNet_UDP_Init(int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    int reuse = 1;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        Log_print("FujiNet_UDP: socket creation failed: %s", strerror(errno));
        return -1;
    }

    /* Allow address reuse */
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
         Log_print("FujiNet_UDP: setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        /* Non-fatal, continue */
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; /* IPv4 */
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    /* Bind the socket with the server address */
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        Log_print("FujiNet_UDP: bind failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    Log_print("FujiNet_UDP: Socket %d bound to port %d", sockfd, port);
    return sockfd;
}

/* Shuts down the UDP socket. */
void FujiNet_UDP_Shutdown(int sockfd) {
    if (sockfd >= 0) {
        close(sockfd);
        Log_print("FujiNet_UDP: Socket %d closed.", sockfd);
    }
}

/* Checks if data is available to read on the socket (non-blocking).
 * Returns TRUE if data is ready, FALSE otherwise or on error. */
BOOL FujiNet_UDP_Poll(int sockfd) {
    struct pollfd fds;
    int ret;

    if (sockfd < 0) {
        return FALSE;
    }

    fds.fd = sockfd;
    fds.events = POLLIN;
    fds.revents = 0;

    ret = poll(&fds, 1, 0); /* 0 timeout = non-blocking */

    if (ret < 0) {
        Log_print("FujiNet_UDP: poll error: %s", strerror(errno));
        return FALSE;
    }

    return (ret > 0 && (fds.revents & POLLIN)) ? TRUE : FALSE;
}

/* Receives a UDP packet.
 * Returns the number of bytes received, or -1 on error.
 * Fills in client_addr and client_len. */
ssize_t FujiNet_UDP_Receive(int sockfd, unsigned char *buffer, size_t buffer_size,
                           struct sockaddr_in *client_addr, socklen_t *client_len) {
    ssize_t recv_len;

    if (sockfd < 0 || buffer == NULL || client_addr == NULL || client_len == NULL) {
        return -1;
    }

    recv_len = recvfrom(sockfd, buffer, buffer_size, 0,
                        (struct sockaddr*)client_addr, client_len);

    if (recv_len < 0) {
        Log_print("FujiNet_UDP: recvfrom error: %s", strerror(errno));
        return -1;
    }

    /* Enhanced logging of received packet */
    if (recv_len > 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
        
        /* Skip detailed logging for frequent packet types */
        uint8_t packet_type = buffer[0];
        
        if (packet_type == 0xC4) { /* ALIVE_REQUEST */
            /* Only log the first ALIVE request or every 10th one */
            static int alive_count = 0;
            if (alive_count++ % 10 == 0) {
                Log_print("<<< FROM FUJINET: Received ALIVE_REQUEST (%d)", alive_count);
            }
        } else {
            /* For all other packet types, log with direction indicator */
            Log_print("<<< FROM FUJINET [%s:%d]: Received %d bytes, packet type 0x%02X", 
                     ip_str, ntohs(client_addr->sin_port), (int)recv_len, buffer[0]);
            
            /* Print hex dump for detailed debugging */
            if (recv_len <= 32) {  /* Only dump short packets to avoid log spam */
                char hexbuf[128] = {0};
                char asciibuf[33] = {0};
                int i, hexpos = 0, asciipos = 0;
                
                for (i = 0; i < recv_len; i++) {
                    hexpos += sprintf(hexbuf + hexpos, "%02X ", buffer[i]);
                    asciipos += sprintf(asciibuf + asciipos, "%c", 
                                       (buffer[i] >= 32 && buffer[i] <= 126) ? buffer[i] : '.');
                }
                
                Log_print("    Data: %s | %s", hexbuf, asciibuf);
            } else {
                Log_print("    Packet too large to display (%d bytes)", (int)recv_len);
            }
        }
    }

    return recv_len;
}

/* Sends a UDP packet to the specified client address.
 * Returns the number of bytes sent, or -1 on error. */
ssize_t FujiNet_UDP_Send(int sockfd, const unsigned char *buffer, size_t len,
                        struct sockaddr_in *dest_addr, socklen_t dest_len) {
    ssize_t send_len;
    int i;
    char hexbuf[128] = {0};
    int hexpos = 0;
    char ip_str[INET_ADDRSTRLEN];
    
    if (sockfd < 0 || buffer == NULL || dest_addr == NULL) {
        return -1;
    }

    /* Convert IP to string for logging */
    inet_ntop(AF_INET, &(dest_addr->sin_addr), ip_str, INET_ADDRSTRLEN);
    
    /* Print hex representation of packet for key packet types */
    if (len > 0) {
        uint8_t packet_type = buffer[0];
        
        /* Skip detailed logging for frequent packet types like ALIVE responses */
        if (packet_type == 0xC5) { /* ALIVE_RESPONSE */
            /* Just log a minimal message for ALIVE responses */
            Log_print(">>> TO FUJINET: Sent ALIVE_RESPONSE");
        } else {
            /* For other packets, log in detail with direction indicator */
            if (len <= 32) { /* Only dump short packets to avoid log spam */
                for (i = 0; i < len && hexpos < 120; i++) {
                    hexpos += sprintf(hexbuf + hexpos, "%02X ", buffer[i]);
                }
                
                Log_print(">>> TO FUJINET [%s:%d]: Packet type 0x%02X, %d bytes: %s", 
                         ip_str, ntohs(dest_addr->sin_port), buffer[0], (int)len, hexbuf);
            } else {
                Log_print(">>> TO FUJINET [%s:%d]: Packet type 0x%02X, %d bytes (too large to display)", 
                         ip_str, ntohs(dest_addr->sin_port), buffer[0], (int)len);
            }
        }
    }

    send_len = sendto(sockfd, buffer, len, 0, (struct sockaddr *)dest_addr, dest_len);
    if (send_len < 0) {
        Log_print("FujiNet_UDP: sendto error: %s", strerror(errno));
    }

    return send_len;
}

#else /* !USE_FUJINET - stub implementations */

/* Stubs for when FujiNet is disabled */
int FujiNet_UDP_Init(int port) {
    /* Do nothing when FujiNet is disabled */
    return -1;
}

void FujiNet_UDP_Shutdown(int sockfd) {
    /* Do nothing when FujiNet is disabled */
}

BOOL FujiNet_UDP_Poll(int sockfd) {
    /* Always report FALSE when FujiNet is disabled */
    return FALSE;
}

ssize_t FujiNet_UDP_Receive(int sockfd, unsigned char *buffer, size_t buffer_size,
                           struct sockaddr_in *client_addr, socklen_t *client_len) {
    /* Always report error when FujiNet is disabled */
    return -1;
}

ssize_t FujiNet_UDP_Send(int sockfd, const unsigned char *buffer, size_t len,
                        struct sockaddr_in *client_addr, socklen_t client_len) {
    /* Always report error when FujiNet is disabled */
    return -1;
}

#endif /* USE_FUJINET */
