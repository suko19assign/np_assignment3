#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define PROTO_VERSION    "1.0"
#define MAX_NICK_LEN     12
#define MAX_MSG_BODY     255
#define MAX_LINE         (6 + MAX_MSG_BODY) /* "MSG " + body + '\n' */

typedef struct client {
    int  fd;
    bool registered;
    char nick[MAX_NICK_LEN + 1];
    char buf[MAX_LINE + 1];
    size_t buflen;
    struct client *next;
} client_t;

static client_t *clients = NULL;

static void fatal(const char *msg) { perror(msg); exit(EXIT_FAILURE); }

static void broadcast(const client_t *from, const char *line)
{
    for (client_t *c = clients; c; c = c->next) {
        if (send(c->fd, line, strlen(line), 0) < 0)
            perror("send"); /* non-fatal */
    }
}

static void remove_client(client_t *c)
{
    close(c->fd);
    /* unlink */
    client_t **pp = &clients;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;
    printf("Client %s disconnected\n",
           c->registered ? c->nick : "(unregistered)");
    fflush(stdout);
    free(c);
}

static bool valid_nick(const char *nick)
{
    static const char *rx = "^[A-Za-z0-9_]{1,12}$";
    regex_t r;
    regcomp(&r, rx, REG_EXTENDED);
    int ok = regexec(&r, nick, 0, NULL, 0) == 0;
    regfree(&r);
    return ok;
}

/* Accept a complete line into c->buf (non-blocking recv) */
static bool getline_nb(client_t *c, char *out, size_t cap)
{
    while (c->buflen < cap - 1) {
        ssize_t n = recv(c->fd, c->buf + c->buflen, 1, 0);
        if (n == 0)
            return false;                 /* closed */
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;             /* no full line yet */
            return false;
        }
        if (c->buf[c->buflen++] == '\n') {
            memcpy(out, c->buf, c->buflen);
            out[c->buflen] = '\0';
            c->buflen = 0;
            return true;
        }
    }
    /* overflow – protocol violation */
    send(c->fd, "ERROR line too long\n", 20, 0);
    return false;
}

static void handle_msg(client_t *c, const char *line)
{
    if (strncmp(line, "MSG ", 4) == 0) {
        const char *msg = line + 4;
        size_t len = strlen(msg);
        if (len > MAX_MSG_BODY + 1) { /* include \n */
            send(c->fd, "ERROR message too long\n", 23, 0);
            return;
        }
        char out[MAX_LINE + MAX_NICK_LEN + 6];
        snprintf(out, sizeof out, "MSG %s %s", c->nick, msg);
        broadcast(c, out);
    } else if (strncmp(line, "NICK ", 5) == 0) {
        char candidate[MAX_NICK_LEN + 1] = {0};
        sscanf(line + 5, "%12s", candidate);
        if (!valid_nick(candidate)) {
            send(c->fd, "ERR bad nick\n", 13, 0);
            return;
        }
        /* ensure uniqueness */
        for (client_t *p = clients; p; p = p->next) {
            if (p != c && p->registered && strcmp(p->nick, candidate) == 0) {
                send(c->fd, "ERR nick in use\n", 16, 0);
                return;
            }
        }
        strcpy(c->nick, candidate);
        c->registered = true;
        send(c->fd, "OK\n", 3, 0);
        printf("Client registered: %s\n", c->nick);
        fflush(stdout);
    } else {
        send(c->fd, "ERROR unknown command\n", 22, 0);
    }
}

/* Create, bind, listen socket */
static int create_listener(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_flags = AI_PASSIVE,
                             .ai_family = AF_UNSPEC,
                             .ai_socktype = SOCK_STREAM};
    struct addrinfo *ai, *p;
    int rc = getaddrinfo(host[0] ? host : NULL, port, &hints, &ai);
    if (rc != 0)
        fatal(gai_strerror(rc));

    int s = -1, opt = 1;
    for (p = ai; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s < 0)
            continue;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
        if (bind(s, p->ai_addr, p->ai_addrlen) == 0 &&
            listen(s, SOMAXCONN) == 0)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(ai);
    if (s < 0)
        fatal("bind/listen");
    return s;
}

/*  Main  */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s IP:PORT\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *host, *port;
    char *arg = strdup(argv[1]);
    char *colon = strchr(arg, ':');
    if (!colon) {
        fprintf(stderr, "IP:PORT required\n");
        return EXIT_FAILURE;
    }
    *colon = '\0';
    host = arg;
    port = colon + 1;

    int lsock = create_listener(host, port);
    printf("Listening on %s:%s …\n", *host ? host : "0.0.0.0", port);
    fflush(stdout);

    fd_set master, rfds;
    FD_ZERO(&master);
    FD_SET(lsock, &master);
    int maxfd = lsock;

    for (;;) {
        rfds = master;
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0)
            fatal("select");

        /* new connections  */
        if (FD_ISSET(lsock, &rfds)) {
            struct sockaddr_storage addr;
            socklen_t alen = sizeof addr;
            int fd = accept(lsock, (struct sockaddr *)&addr, &alen);
            if (fd < 0) {
                perror("accept");
                continue;
            }
            /* make non-blocking */
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            client_t *c = calloc(1, sizeof *c);
            c->fd = fd;
            c->next = clients;
            clients = c;

            FD_SET(fd, &master);
            if (fd > maxfd) maxfd = fd;

            send(fd, "Hello " PROTO_VERSION "\n", 8 + strlen(PROTO_VERSION), 0);
        }

        /* client data */
        for (client_t *c = clients, *next; c; c = next) {
            next = c->next; /* because we may free() c */
            if (!FD_ISSET(c->fd, &rfds))
                continue;

            char line[MAX_LINE + 1];
            while (getline_nb(c, line, sizeof line)) {
                if (!c->registered &&
                    strncmp(line, "NICK ", 5) != 0) {
                    send(c->fd, "ERR register first\n", 19, 0);
                    continue;
                }
                handle_msg(c, line);
            }
            if (errno == 0 && c->buflen == 0) { /* closed */
                FD_CLR(c->fd, &master);
                remove_client(c);
            }
        }
    }
    return EXIT_SUCCESS;
}
