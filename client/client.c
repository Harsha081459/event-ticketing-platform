/*
 * ============================================================================
 * Event Ticketing Platform — CLI Client
 * ============================================================================
 * Interactive TCP client that connects to the server and sends commands.
 * Uses select() for multiplexing between user input and server responses.
 *
 * Usage: ./etp_client [-h host] [-p port]
 * ============================================================================
 */

#include "../common/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <strings.h>    /* strncasecmp */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ================================================================
 * Connect to Server
 * ================================================================ */
static int connect_to_server(const char *host, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", host);
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to %s:%d — %s\n",
                host, port, strerror(errno));
        close(sockfd);
        return -1;
    }

    return sockfd;
}

/* ================================================================
 * Main Client Loop
 *
 * Uses select() to multiplex between:
 *   - stdin (user typing commands)
 *   - server socket (responses arriving)
 *
 * This avoids blocking on read() and allows responsive I/O.
 * ================================================================ */
int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = SERVER_PORT;

    /* Ignore SIGPIPE so write() to closed socket returns EPIPE instead of terminating */
    signal(SIGPIPE, SIG_IGN);

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-h host] [-p port]\n", argv[0]);
            printf("  -h host  Server host (default: 127.0.0.1)\n");
            printf("  -p port  Server port (default: %d)\n", SERVER_PORT);
            return 0;
        }
    }

    printf("\n  Event Ticketing Platform — Client\n");
    printf("  Connecting to %s:%d...\n", host, port);

    int sockfd = connect_to_server(host, port);
    if (sockfd < 0) return 1;

    printf("  Connected! Type commands or HELP for usage.\n\n");

    /*
     * select()-based I/O multiplexing loop.
     * Monitors both stdin (fd 0) and the server socket simultaneously.
     */
    fd_set read_fds;
    char buf[MAX_RESPONSE_SIZE];
    int running = 1;

    while (running) {
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);
        FD_SET(sockfd, &read_fds);

        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        int ready = select(maxfd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        /* ── Server sent data ── */
        if (FD_ISSET(sockfd, &read_fds)) {
            ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
            if (n <= 0) {
                printf("\n  Server disconnected.\n");
                break;
            }
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        }

        /* ── User typed a command ── */
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (!fgets(buf, sizeof(buf), stdin)) {
                break;  /* EOF (Ctrl+D) */
            }

            /* Check for local quit */
            if (strncasecmp(buf, "quit", 4) == 0 ||
                strncasecmp(buf, "exit", 4) == 0) {
                /* Send QUIT to server first */
                write(sockfd, "QUIT\n", 5);
                /* Read the goodbye response */
                ssize_t n = read(sockfd, buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    printf("%s", buf);
                }
                running = 0;
                continue;
            }

            /* Send command to server */
            size_t len = strlen(buf);
            if (write(sockfd, buf, len) < 0) {
                printf("  Error sending command: %s\n", strerror(errno));
                break;
            }
        }
    }

    close(sockfd);
    printf("  Disconnected.\n\n");
    return 0;
}
