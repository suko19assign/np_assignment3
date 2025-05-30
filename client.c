#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
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
#define MAX_LINE         (6 + MAX_MSG_BODY)    
#define ARRAY_COUNT(a)   (sizeof(a) / sizeof(*(a)))


int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s HOST:PORT NICK\n", argv[0]);
        return EXIT_FAILURE;
    }
