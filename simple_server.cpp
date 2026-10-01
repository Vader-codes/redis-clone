#include <stdio.h>      // for prinf
#include <unistd.h>     // for close
#include <sys/socket.h> // for socket
#include <stdlib.h>
#include <arpa/inet.h>
#include <string.h>
#include <errno.h>

// helper function to close the socket
static void die(const char *msg)
{
    perror(msg); // print error message
    exit(1);
}

// helper : read exactly n bytes
static int32_t read_full(int fd, char *buf, size_t n)
{
    while (n > 0)
    {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0)
            return -1;

        n -= (size_t)rv;
        buf += rv;
    }

    return 0;
}

// helper : to write or send the n bytes to the client
static int32_t write_all(int fd, const char *buf, size_t n)
{
    while (n > 0)
    {
        ssize_t rv = write(fd, buf, n);

        if (rv <= 0)
            return -1;

        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// handle one request on an open connection
// read a request , write a response, Return 0 on success -1 on failure
static int32_t one_request(int connfd)
{
    char rbuf[4 + 64] = {}; // 4 bytes for length 64 for message body
    errno = 0;
    int32_t err = read_full(connfd, rbuf, 4);
    if (err)
    {
        if (errno == 0)
        {
            printf("EOF\n");
        }
        else
        {
            printf("read()  errno\n");
        }
        return err;
    }

    // interept the 4 bytes as  uint32_t
    uint32_t len = 0;
    memcpy(&len, rbuf, 4); // assume little endian

    if (len > 64)
    {
        printf("Message is too long!\n");
        return -1;
    }
    // read the body (len bytes)
    err = read_full(connfd, &rbuf[4], len);
    if (err)
    {
        printf("read() error (body)\n");
        return err;
    }
    // print the message
    printf("client says: %.*s\n", (int)len, &rbuf[4]);

    // build the response
    const char reply[] = "world";
    uint32_t reply_len = (uint32_t)strlen(reply);
    char wbuf[4 + sizeof(reply)];
    memcpy(wbuf, &reply_len, 4);
    memcpy(&wbuf[4], reply, reply_len);

    // send it
    return write_all(connfd, wbuf, 4 + reply_len);
}

int main()
{
    // step 1
    // ask  OS for a socket (a "ticket" for networking)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket()"); // socket failed print and quit
    printf("Got  a socket! fd = %d:\n", fd);

    // allow rebinding same port after restart
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // prepare the address we want to claim : 0.0.0.0: 1234
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;       // this is for ipv4 address
    addr.sin_port = htons(1234);     // port converted to network format
    addr.sin_addr.s_addr = htonl(0); // 0.0.0.0 wildcard (converted too)

    // step 2 : claim it
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv < 0)
        die("bind()");
    // we got the ticket
    printf("Bound to port 1234!\n");

    // step 3 : start accepting connections , queue up to SOMAXCONN waiting clients
    rv = listen(fd, SOMAXCONN);
    if (rv < 0)
        die("listen()");
    printf("listening to port 1234!\n");

    // step 4: accept() pull one clien from the queue
    // connfd = new socket for our  side of the client's connection
    // client_addr = filled with client's address not a new socket
    // addrlen = size of client_addr(in /out parameter);
    while (true)
    {
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);

        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if (connfd < 0)
            continue; // retry on failure

        printf("New client connected! connfd = %d :\n", connfd);

        // serve this client until it disconnects or error
        while (true)
        {
            int32_t err = one_request(connfd);
            if (err)
                break;
        }
        close(connfd);
        printf("client disconnected\n");
    }
    return 0;
}