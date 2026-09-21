#include <stdio.h>   // printf, fprintf
#include <stdlib.h>  // abort
#include <string.h>  // strlen
#include <errno.h>   // errno
#include <unistd.h>  // read, write, close
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

// die() and msg() must be defined BEFORE anything calls them
static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    abort();
}

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = read(connfd, rbuf, sizeof(rbuf) - 1);

    if (n < 0) {
        msg("read() error");
        return;
    }
    printf("Client says: %s\n", rbuf);

    char wbuf[] = "world";
    write(connfd, wbuf, strlen(wbuf));
}

int main() {
    // step 1: create a socket handle (IPv4 TCP)
    int fd = socket(AF_INET, SOCK_STREAM, 0); // AF_INET = IPv4, SOCK_STREAM = TCP
    if (fd < 0) {
        die("socket()");
    }

    // step 2: allow reusing the same port immediately after restart
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // step 3: bind the socket to an address and port
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0); // wildcard 0.0.0.0 — listen on all interfaces

    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // step 4: start listening for incoming connections
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    // step 5: accept connections
    while (true) {
        struct sockaddr_in client_addr = {};       // renamed to match usage below
        socklen_t addrlen = sizeof(client_addr);    // renamed to match usage below
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);

        if (connfd < 0) {
            continue;
        }
        do_something(connfd);
        close(connfd);
    }

    return 0;
}