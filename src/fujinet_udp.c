// src/fujinet_udp.c
#ifdef USE_FUJINET

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <errno.h>
#include <stdbool.h>

#include "log.h"
#include "fujinet_udp.h"

// Initializes the UDP socket on the specified port.
// Returns the socket file descriptor on success, -1 on failure.
int FujiNet_UDP_Init(int port) {
    int sockfd;
    struct sockaddr_in server_addr;
    int reuse = 1;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        Log_print("FujiNet_UDP: socket creation failed: %s", strerror(errno));
        return -1;
    }

    // Allow address reuse
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
         Log_print("FujiNet_UDP: setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
        // Non-fatal, continue
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET; // IPv4
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind the socket with the server address
    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        Log_print("FujiNet_UDP: bind failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    Log_print("FujiNet_UDP: Socket %d bound to port %d", sockfd, port);
    return sockfd;
}

// Shuts down the UDP socket.
void FujiNet_UDP_Shutdown(int sockfd) {
    if (sockfd >= 0) {
        Log_print("FujiNet_UDP: Closing socket %d", sockfd);
        close(sockfd);
    }
}

// Checks if data is available to read on the socket (non-blocking).
// Returns TRUE if data is ready, FALSE otherwise or on error.
BOOL FujiNet_UDP_Poll(int sockfd) {
    struct pollfd pfd;
    int ret;

    if (sockfd < 0) return FALSE;

    pfd.fd = sockfd;
    pfd.events = POLLIN; // Check for data to read
    pfd.revents = 0;

    // Poll with zero timeout for non-blocking check
    ret = poll(&pfd, 1, 0);

    if (ret < 0) {
        Log_print("FujiNet_UDP: poll failed: %s", strerror(errno));
        return FALSE;
    }

    // ret > 0 means data is available (pfd.revents & POLLIN should be set)
    // ret == 0 means timeout (no data)
    return (ret > 0 && (pfd.revents & POLLIN));
}

// Receives a UDP packet.
// Returns the number of bytes received, or -1 on error.
// Fills in client_addr and client_len.
ssize_t FujiNet_UDP_Receive(int sockfd, unsigned char *buffer, size_t buffer_size,
                           struct sockaddr_in *client_addr, socklen_t *client_len) {
    ssize_t n;

    if (sockfd < 0) return -1;

    n = recvfrom(sockfd, buffer, buffer_size, 0, // Flags set to 0
                 (struct sockaddr *)client_addr, client_len);

    if (n < 0) {
        // EAGAIN or EWOULDBLOCK are not true errors in non-blocking mode,
        // but we poll first, so receiving this likely means an issue.
        Log_print("FujiNet_UDP: recvfrom failed: %s", strerror(errno));
        return -1;
    }

    // Log received packet details (optional, can be verbose)
    // char client_ip[INET_ADDRSTRLEN];
    // inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    // Log_print("FujiNet_UDP: Received %zd bytes from %s:%d", n, client_ip, ntohs(client_addr->sin_port));

    return n;
}

// Sends a UDP packet to the specified client address.
// Returns the number of bytes sent, or -1 on error.
ssize_t FujiNet_UDP_Send(int sockfd, const unsigned char *buffer, size_t len,
                        const struct sockaddr_in *client_addr, socklen_t client_len) {
    ssize_t n;

    if (sockfd < 0) return -1;

    // Log sent packet details (optional)
    // char client_ip[INET_ADDRSTRLEN];
    // inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    // Log_print("FujiNet_UDP: Sending %zu bytes to %s:%d", len, client_ip, ntohs(client_addr->sin_port));

    n = sendto(sockfd, buffer, len, 0, // Flags set to 0
               (const struct sockaddr *)client_addr, client_len);

    if (n < 0) {
        Log_print("FujiNet_UDP: sendto failed: %s", strerror(errno));
        return -1;
    } else if (n != len) {
        Log_print("FujiNet_UDP: sendto sent %zd bytes, expected %zu", n, len);
        // Partial send is unusual for UDP but possible? Treat as error for simplicity.
        return -1;
    }

    return n;
}

#endif /* USE_FUJINET */
