#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>

// die() must be defined BEFORE it's used
static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    abort();
}

int main() {
    // step 1: create a socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    // step 2: set up the server's address
    struct sockaddr_in addr{};
    // note: addr = the address we want to connect to (the server), not our own address
    // INADDR_LOOPBACK = localhost, 127.0.0.1, since both server and client are on the same machine
    addr.sin_family = AF_INET;              // ipv4 connection
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(INADDR_LOOPBACK); // 127.0.0.1

    // step 3: connect
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {   // 0 = success, non-zero = failure
        die("connect");
    }

    // step 4: send a message
    char msg[] = "hello";
    write(fd, msg, strlen(msg));

    // step 5: read the reply
    char rbuf[64] = {};
    ssize_t n = read(fd, rbuf, sizeof(rbuf) - 1); // ssize_t so it can hold -1 on failure
    if (n < 0) {
        die("read");
    }
    printf("server says: %s\n", rbuf);

    // step 6: close the connection
    close(fd);
    return 0;
}