#include <stdio.h>      // for prinf
#include <unistd.h>     // for close
#include <sys/socket.h> // for socket connet
#include <stdlib.h>     // exit
#include <arpa/inet.h>  // sockaddr_in, htons, htonl, INADDR_LOOPBACK
#include <string.h>     // strlen
#include <string>
#include <vector>
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

// send a request : a list of strings , wrapped in length prefixed protocol
static int32_t send_req(int fd, const vector<string> &cmd)
{
    // build the body first (we don't know its size unitl it's done)
    vector<uint8_t> buf;

    // how many strings are in this command?
    uint32_t nstr = (uint32_t)cmd.size();
    buf.insert(buf.end(), (uint8_t *)&nstr, (uint8_t *)&nstr + 4);

    // append each string as [4-byte lenth][bytes]
    for (const string &s : cmd)
    {
        uint32_t len = (uint32_t)s.size();
        buf.insert(buf.end(), (uint8_t *)&len, (uint8_t *)&len + 4);
        buf.insert(buf.end(), (const uint8_t *)s.data(), (const uint8_t *)s.data() + len);
    }

    // now that we know the body size , send the outer 4-byte length first
    uint32_t total = (uint32_t)buf.size();
    int32_t err = write_all(fd, (char *)&total, 4); // first we send the total response size in first 4bytes
    if (err)
        return err;

    // the we send the body itself
    return write_all(fd, (char *)buf.data(), buf.size());
}

// read a response
// read one response from the server and prints it
// response format : [outer_len][status(4B)][data]
static int32_t read_res(int fd)
{
    char rbuf[4];                         // buffer for outer length
    int32_t err = read_full(fd, rbuf, 4); // read outer length

    if (err)
    {
        printf("read_full(length) failed \n"); // report error and bail
        return err;
    }

    uint32_t len = 0;      // will hold the message body size
    memcpy(&len, rbuf, 4); // copy bytes into the number

    vector<uint8_t> body(len);                     // allocate space for the body
    err = read_full(fd, (char *)body.data(), len); // read the full body

    if (err)
    {
        printf("read_full(body) failed\n"); // report and bail
        return err;
    }
    uint32_t status = 0;
    memcpy(&status, body.data(), 4);        // first 4 bytes of body = status
    printf("status = %u, data = ", status); // print status prefix

    if (len > 4)
    {                                                    // if there's more that just status
        printf("%.*s", (int)(len - 4), body.data() + 4); // print data bytes as string
    }
    printf("\n");
    return 0; // sucsss
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

    // send some commannds and read responses
    send_req(fd, {"set", "name", "bipin"}); // stores name = bipin
    read_res(fd);                           // expect : OK, no data

    send_req(fd, {"get", "name"}); // look up name
    read_res(fd);                  // expect: OK, data=Rishi

    send_req(fd, {"del", "name"}); // delete name
    read_res(fd);                  // expect: OK, data=<binary 1>

    send_req(fd, {"get", "name"}); // look up deleted key
    read_res(fd);                  // expect: not found

    close(fd); // hung up
    return 0;
}