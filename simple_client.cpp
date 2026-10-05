#include <stdio.h>      // for prinf
#include <unistd.h>     // for close
#include <sys/socket.h> // for socket connet
#include <stdlib.h>     // exit
#include <arpa/inet.h>  // sockaddr_in, htons, htonl, INADDR_LOOPBACK
#include <string.h>     // strlen
#include <string>
using namespace std;

static void die(const char *msg)
{
    perror(msg);
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

// helper : to write or send the n bytes to the server.
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

int main()
{

    // step1 : socket same as server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket()");

    // step 2 : describe the server's address which is localHost in this case
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 localhost

    // step3 connect() -> dial the server
    // bind+listen+accept happnes in server side
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv < 0)
        die("connect()");

    // // step 4: send a message
    // const char *msgs[] = {"hello1", "hello2", "hello3"};
    // for (int i = 0; i < 3; i++)
    // {
    //     const char *msg = msgs[i];

    //     uint32_t msg_len = (uint32_t)strlen(msg);

    //     // create a buffer to send
    //     char wbuf[4 + 64];              // 4 byte for message length and 5 is hard typed hello size
    //     memcpy(wbuf, &msg_len, 4);      // length prefix
    //     memcpy(&wbuf[4], msg, msg_len); // body

    //     int32_t err = write_all(fd, wbuf, 4 + msg_len);
    //     if (err)
    //         die("write()");
    //     printf("sent: %s\n", msg);
    // }

    // now read 3 responses
    // for (int i = 0; i < 3; i++)
    // {
    //     char rbuf[4 + 64];
    //     // step5 read server's reply
    //     // read the full length
    //     int32_t err = read_full(fd, rbuf, 4);
    //     if (err)
    //         die("read_full(length)");

    //     uint32_t reply_len = 0;
    //     memcpy(&reply_len, rbuf, 4);

    //     // read the reply body
    //     err = read_full(fd, &rbuf[4], reply_len);
    //     if (err)
    //         die("read_full(body)");

    //     printf("Server sent : %.*s\n", (int)reply_len, &rbuf[4]);
    // }

    // let's send a larget file to the server
    const size_t k_big = 1024 * 1024; // 1 MB
    string big(k_big, 'z');

    uint32_t msg_len = (uint32_t)big.size();

    char wbuf[4 + 64]; // header only body will be send seperately
    memcpy(wbuf, &msg_len, 4);

    // send length prefix
    int32_t err = write_all(fd, wbuf, 4);
    if (err)
        die("write_all(length)");

    // send body
    err = write_all(fd, big.data(), msg_len);

    printf("send %u bytes\n", msg_len);

    // read the response
    char rbuf[4 + 64];
    err = read_full(fd, rbuf, 4); // reading only head 4bytes
    if (err)
        die("read_full (lenth)");

    uint32_t reply_len = 0;
    memcpy(&reply_len, rbuf, 4); // reading the length of the message

    err = read_full(fd, &rbuf[4], reply_len);
    if (err)
        die("read_full (body)");

    printf("server sent: %.*s\n", (int)reply_len, &rbuf[4]);

    close(fd);
    return 0;
}