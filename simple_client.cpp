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

// read one request from the server, parse the TLV tag, print it
// read one response from the server, parse the TLV tag, print it
static int32_t read_res(int fd)
{
    // read the outer length
    char hdr[4];
    int32_t err = read_full(fd, hdr, 4);
    if (err)
    {
        printf("read_full(length) failed\n");
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, hdr, 4);

    // read the whole response body
    vector<uint8_t> body(len);
    err = read_full(fd, (char *)body.data(), len);
    if (err)
    {
        printf("read_full(body) failed\n");
        return err;
    }

    // first byte is the tag
    uint8_t tag = body[0];

    if (tag == 0)
    { // TAG_NIL
        printf("(nil)\n");
    }
    else if (tag == 2)
    { // TAG_STR
        uint32_t slen = 0;
        memcpy(&slen, body.data() + 1, 4); // next 4 bytes = string length
        printf("%.*s\n", (int)slen, body.data() + 5);
    }
    else if (tag == 3)
    { // TAG_INT
        int64_t val = 0;
        memcpy(&val, body.data() + 1, 8); // next 8 bytes = int64
        printf("(int) %lld\n", (long long)val);
    }
    else if (tag == 1)
    { // TAG_ERR
        uint32_t code = 0;
        memcpy(&code, body.data() + 1, 4); // 4 bytes: error code
        uint32_t msglen = 0;
        memcpy(&msglen, body.data() + 5, 4); // 4 bytes: message length
        printf("(err) %u: %.*s\n", code, (int)msglen, body.data() + 9);
    }
    else
    {
        printf("(unknown tag %d)\n", tag);
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