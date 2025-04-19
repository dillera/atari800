#include "config.h"

#ifdef USE_FUJINET

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef HAVE_SOCKET
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
#else /* Assume Windows */
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif /* HAVE_SOCKET */

#include "log.h"
#include "fujinet.h"
#include "util.h" // For Util_strdup

#define FUJINET_DEFAULT_HOST "127.0.0.1"
#define FUJINET_DEFAULT_PORT 16384
#define FUJINET_BUFFER_SIZE 1024
#define FUJINET_TIMEOUT_SEC 1 // Timeout for UDP receive

static int fujinet_enabled = 0;
static SOCKET udp_socket = INVALID_SOCKET;
static struct sockaddr_in server_addr;
static char *fujinet_address_str = NULL; // Store the "host:port" string

#ifdef _WIN32
static int wsa_initialized = 0;
#endif

// Internal helper to send a command string
static int send_command_str(const char *cmd) {
    if (sendto(udp_socket, cmd, (int)strlen(cmd), 0, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        Log_print("FujiNet: sendto failed: %s", strerror(errno));
        return 0;
    }
    // Log_print("FujiNet: Sent: %s", cmd); // Debug
    return 1;
}

int FujiNet_Initialise(const char *host_port) {
    char host[256] = FUJINET_DEFAULT_HOST;
    int port = FUJINET_DEFAULT_PORT;
    struct hostent *he;
    char *colon;

#ifdef _WIN32
    if (!wsa_initialized) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            Log_print("FujiNet: WSAStartup failed.");
            return 0;
        }
        wsa_initialized = 1;
    }
#endif

    if (host_port && *host_port) {
        fujinet_address_str = Util_strdup(host_port); // Store for potential later use/display
        colon = strrchr(fujinet_address_str, ':');
        if (colon) {
            *colon = '\0'; // Null-terminate host part
            strncpy(host, fujinet_address_str, sizeof(host) - 1);
            host[sizeof(host) - 1] = '\0';
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535) {
                Log_print("FujiNet: Invalid port number %d, using default %d.", port, FUJINET_DEFAULT_PORT);
                port = FUJINET_DEFAULT_PORT;
            }
        } else {
             strncpy(host, fujinet_address_str, sizeof(host) - 1);
             host[sizeof(host) - 1] = '\0';
             // Keep default port if only host is specified
        }
    } else {
        fujinet_address_str = Util_strdup(FUJINET_DEFAULT_HOST ":" FUJINET_STRINGIFY(FUJINET_DEFAULT_PORT));
    }

    Log_print("FujiNet: Initializing connection to %s:%d", host, port);

    if ((udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) {
        Log_print("FujiNet: socket creation failed: %s", strerror(errno));
        goto error_cleanup;
    }

    // Resolve hostname
    if ((he = gethostbyname(host)) == NULL) {
        Log_print("FujiNet: gethostbyname failed for %s", host);
        goto error_cleanup;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr = *((struct in_addr *)he->h_addr);

    // Set receive timeout
#ifdef HAVE_SOCKET
    struct timeval tv;
    tv.tv_sec = FUJINET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        Log_print("FujiNet: setsockopt SO_RCVTIMEO failed: %s", strerror(errno));
         // Non-fatal, but responses might block indefinitely without it
    }
#else // Windows
    DWORD timeout = FUJINET_TIMEOUT_SEC * 1000;
    if (setsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout)) < 0) {
         Log_print("FujiNet: setsockopt SO_RCVTIMEO failed: %d", WSAGetLastError());
         // Non-fatal
    }
#endif

    fujinet_enabled = 1;
    Log_print("FujiNet: Initialized successfully.");
    return 1;

error_cleanup:
    if (udp_socket != INVALID_SOCKET) {
        closesocket(udp_socket);
        udp_socket = INVALID_SOCKET;
    }
    if (fujinet_address_str) {
        free(fujinet_address_str);
        fujinet_address_str = NULL;
    }
#ifdef _WIN32
    // Don't WSACleanup here, might be needed by other parts or subsequent init attempts
#endif
    return 0;
}

void FujiNet_Shutdown(void) {
    if (!fujinet_enabled) return;

    Log_print("FujiNet: Shutting down.");
    if (udp_socket != INVALID_SOCKET) {
        // Optionally send a shutdown signal? Protocol doesn't specify one.
        closesocket(udp_socket);
        udp_socket = INVALID_SOCKET;
    }
    fujinet_enabled = 0;
    if (fujinet_address_str) {
        free(fujinet_address_str);
        fujinet_address_str = NULL;
    }

#ifdef _WIN32
    if (wsa_initialized) {
        WSACleanup();
        wsa_initialized = 0;
    }
#endif
}

int FujiNet_ProcessCommand(const unsigned char *command_frame, unsigned char *response_frame) {
    unsigned char tx_buffer[FUJINET_BUFFER_SIZE];
    unsigned char rx_buffer[FUJINET_BUFFER_SIZE];
    int tx_len;
    int rx_len;
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    if (!fujinet_enabled || udp_socket == INVALID_SOCKET) {
        return 0; // Not initialized or error during init
    }

    // Format: Binary SIO frame (5 bytes)
    memcpy(tx_buffer, command_frame, 5);
    tx_len = 5;

    // Log_print("FujiNet: Sending SIO Frame: %02X %02X %02X %02X %02X",
    //           command_frame[0], command_frame[1], command_frame[2], command_frame[3], command_frame[4]); // Debug

    if (sendto(udp_socket, (const char *)tx_buffer, tx_len, 0, (struct sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        Log_print("FujiNet: sendto failed: %s", strerror(errno));
        return 0;
    }

    // Wait for response
    rx_len = recvfrom(udp_socket, (char *)rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&from_addr, &from_len);

    if (rx_len == SOCKET_ERROR) {
#ifdef HAVE_SOCKET
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
             Log_print("FujiNet: recvfrom timeout.");
        } else {
             Log_print("FujiNet: recvfrom failed: %s", strerror(errno));
        }
#else // Windows
        int wsa_error = WSAGetLastError();
        if (wsa_error == WSAETIMEDOUT) {
            Log_print("FujiNet: recvfrom timeout.");
        } else {
            Log_print("FujiNet: recvfrom failed: WSAError %d", wsa_error);
        }
#endif
        return 0; // Timeout or other error
    }

    // Basic validation: Check source address? Maybe not necessary for localhost. Check length.
    if (rx_len != 4) {
        Log_print("FujiNet: Received unexpected response length %d (expected 4).", rx_len);
        // Dump received data for debugging?
        return 0;
    }

    // Copy response
    memcpy(response_frame, rx_buffer, 4);

    // Log_print("FujiNet: Received SIO Response: %02X %02X %02X %02X",
    //           response_frame[0], response_frame[1], response_frame[2], response_frame[3]); // Debug

    return 1; // Success
}

void FujiNet_SetMotor(int on) {
    if (!fujinet_enabled || udp_socket == INVALID_SOCKET) {
        return;
    }
    const char *cmd = on ? "MOTOR 1\n" : "MOTOR 0\n";
    send_command_str(cmd);
}

int FujiNet_IsEnabled(void) {
    return fujinet_enabled;
}

#endif /* USE_FUJINET */
