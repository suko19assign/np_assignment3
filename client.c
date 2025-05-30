#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <regex.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROTO_VERSION    "1.0"
#define MAX_NICK_LEN     12
#define MAX_MSG_BODY     255
#define MAX_LINE         (6 + MAX_MSG_BODY)
#define ARRAY_COUNT(a)   (sizeof(a) / sizeof(*(a)))
#define NB_AGAIN (-2)

static int connected_ok = 0;

static void fatal(const char *msg)
{
    fprintf(stderr, "ERROR ");
    perror(msg);
    fflush(stderr);
    exit(connected_ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

//nick validation
static void validate_nick(const char *nick)
{
    static const char *pattern = "^[A-Za-z0-9_]{1,12}$";
    regex_t rx;
    if (regcomp(&rx, pattern, REG_EXTENDED) != 0)
        fatal("regcomp");

    int rc = regexec(&rx, nick, 0, NULL, 0);
    regfree(&rx);

    if (rc != 0) {
        fprintf(stderr, "ERROR Nickname must match %s and be ≤ 12 chars\n", pattern);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
}

/* Parse "host:port"  */
static void split_host_port(char *arg, char **host, char **port)
{
    char *colon = strchr(arg, ':');
    if (!colon) {
        fprintf(stderr, "ERROR HOST:PORT expected\n");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    *colon = '\0';
    *host   = arg;
    *port   = colon + 1;
}

/*  Connect using getaddrinfo  */
static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM,
                             .ai_flags = AI_ADDRCONFIG};
    struct addrinfo *ai, *p;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc != 0)
        fatal(gai_strerror(rc));

    int sock = -1;
    for (p = ai; p; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0)
            continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
            break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(ai);

    if (sock < 0)
        fatal("connect");
    return sock;
}

/* set FD to non-blocking */
static void make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        fatal("fcntl");
}

/* Read one line ( \n terminated ) from a non-blocking fd  */
static ssize_t readline_nonblock(int fd, char *buf, size_t cap)
{
    size_t len = 0;
    while (len + 1 < cap) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0)
            return 0;
        if (n < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN ||
                errno == EINTR      || errno == EBADF) {
                if (len == 0)
                    return NB_AGAIN;
                break;
            }
            return -1;
        }
        buf[len++] = c;
        if (c == '\n')
            break;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

/*  Main loop  */
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc != 3) {
        fprintf(stderr, "ERROR Usage: %s HOST:PORT NICK\n", argv[0]);
        fflush(stderr);
        return EXIT_FAILURE;
    }

    char *host, *port;
    split_host_port(argv[1], &host, &port);
    validate_nick(argv[2]);
    const char *nick = argv[2];

    int sock = connect_to_server(host, port);
    make_nonblocking(sock);
    make_nonblocking(STDIN_FILENO);

    char line[MAX_LINE + 32];
    ssize_t r;
    while ((r = readline_nonblock(sock, line, sizeof line)) == NB_AGAIN)
        ;
    if (r <= 0 || strncasecmp(line, "HELLO ", 6) != 0) {
        fprintf(stderr, "ERROR Protocol mismatch: %s", line);
        fflush(stderr);
        return EXIT_FAILURE;
    }
    char version[16] = {0};
    sscanf(line + 6, "%15s", version);
    if (strcmp(version, "1") != 0 && strcmp(version, "1.0") != 0) {
        fprintf(stderr, "ERROR Unsupported version: %s\n", version);
        fflush(stderr);
        return EXIT_FAILURE;
    }

    dprintf(sock, "NICK %s\n", nick);

    while ((r = readline_nonblock(sock, line, sizeof line)) == NB_AGAIN)
        ;
    if (r <= 0 || strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "%s", line);
        fflush(stderr);
        return EXIT_FAILURE;
    }

    printf("Connected as %s.\n", nick);
    fflush(stdout);
    connected_ok = 1;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0) {
            if (errno == EINTR || errno == EBADF)
                continue;
            fatal("select");
        }

        if (FD_ISSET(sock, &rfds)) {
            while ((r = readline_nonblock(sock, line, sizeof line)) > 0) {
                if (strncmp(line, "MSG ", 4) == 0) {
                    const char *msg_nick = line + 4;
                    const char *space    = strchr(msg_nick, ' ');
                    if (!space)
                        continue;
                    size_t nlen = (size_t)(space - msg_nick);
                    if (nlen == strlen(nick) &&
                        strncmp(msg_nick, nick, nlen) == 0)
                        continue;                /* skip own echo */
                    printf("%.*s: %s", (int)nlen, msg_nick, space + 1);
                    fflush(stdout);
                } else {
                    fputs(line, stderr);
                    fflush(stderr);
                }
            }
            if (r == NB_AGAIN)
                ;
            else if (r == 0) {
                fprintf(stderr, "Server closed connection.\n");
                fflush(stderr);
                break;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char userbuf[MAX_MSG_BODY + 2];
            ssize_t n = read(STDIN_FILENO, userbuf, sizeof userbuf - 1);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;
                fatal("read stdin");
            }
            if (n <= 0)
                continue;
            userbuf[n] = '\0';
            if (userbuf[n - 1] == '\n')
                userbuf[n - 1] = '\0';

            if (strcmp(userbuf, "/quit") == 0)
                break;

            if ((int)strlen(userbuf) > MAX_MSG_BODY) {
                fprintf(stderr, "Message too long (255 max)\n");
                fflush(stderr);
                continue;
            }

            printf("me: %s\n", userbuf);
            fflush(stdout);
            dprintf(sock, "MSG %s\n", userbuf);
        }
    }
    close(sock);
    puts("Bye.");
    return EXIT_SUCCESS;
}

