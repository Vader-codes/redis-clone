#include <stdio.h>      // for prinf
#include <unistd.h>     // for close
#include <sys/socket.h> // for socket, bind, listen, accept
#include <stdlib.h>     // for exit
#include <arpa/inet.h>  // sockaddr_in, htons, htonl
#include <string.h>     // memcpy, strlen
#include <errno.h>      //errno
#include <fcntl.h>      // fnctl, O_NONBlock
#include <vector>       // vector in c++
#include <poll.h>
using namespace std;

// per Client state , remembered across event loop iterations
struct Conn
{
    int fd = -1;              // socket for this client
    bool want_read = false;   //"tell me when this client has data to read"
    bool want_write = false;  //"tell me when this client has data to write"
    bool want_close = false;  // "close this client at the end of the iteration"
    vector<uint8_t> incoming; // bytes received, not yet processed one byte per element
    vector<uint8_t> outgoing; // bytes to send back
};
// print error and exit
static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

// make an fd non-blocking : read/write/accept/ return immediately
static void make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0); // get current flags
    flags |= O_NONBLOCK;               // add the non-blocking flag
    fcntl(fd, F_SETFL, flags);         // set the flags
}

// try to parse one request form conn->incoming
//  returns true if a request was parsed , false if we need more data
static bool try_one_request(Conn *conn)
{
    // need at least 4bytes for the length prefix
    if (conn->incoming.size() < 4)
    {
        return false; // not enough data yet
    }
    // read the 4 byte length prefix
    uint32_t len = 0;
    memcpy(&len, conn->incoming.data(), 4);

    if (len > 64)
    {
        printf("message too long\n");
        conn->want_close = true;
        return false;
    }
    // need the full body too
    if (4 + len > conn->incoming.size())
    {
        return false; // body not fully arrived yet
    }

    // we have a complete message
    const uint8_t *request = &conn->incoming[4];
    printf("client says %.*s\n", (int)len, request);

    // build the response : [4-byte length ][body]
    const char reply[] = "world";
    uint32_t reply_len = (uint32_t)strlen(reply);

    // append length prefix to outgoing
    conn->outgoing.insert(conn->outgoing.end(), (uint8_t *)&reply_len, (uint8_t *)&reply_len + 4);

    // append body to outgoing
    conn->outgoing.insert(conn->outgoing.end(), (uint8_t *)reply, (uint8_t *)reply + reply_len);

    // remove the consumed request from incoming
    conn->incoming.erase(conn->incoming.begin(), conn->incoming.begin() + 4 + len);
    return true;
}
int main()
{
    // skeleton of our event loop

    // step1 socket()
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket()");

    printf("Got the Ticket from os fd = %d\n", fd);

    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    // step2: bind();
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(0);

    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv < 0)
        die("bind()");

    printf("Bound to port 1234!\n");

    // step3 listen
    rv = listen(fd, SOMAXCONN);
    if (rv < 0)
        die("listen()");

    printf("listening on port 1234!\n");

    // now make the listening fd non-blocking
    // so accept() never blocks; it returns -1 + EAGAIN if no client is listening

    make_nonblocking(fd);
    // map from fd to Conn pointer, indexed by fd
    // fd2conn[4] = the Conn for fd 4
    // fd2conn[7] = NULL means fd 7 has no Conn (unused)
    vector<Conn *> fd2conn; // which sockets ready

    // -- event loop --
    while (true)
    {
        // build the lists of fds for poll() to watch
        vector<struct pollfd> poll_args;

        // the first entry is always listening fd
        // we want to know when the new client connects
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        // add every client's fd to the poll list
        for (Conn *conn : fd2conn)
        {
            if (!conn)
                continue; // empty slot

            struct pollfd cpfd = {conn->fd, POLLERR, 0};
            // pollerr always asks for error notifications

            if (conn->want_read)
            {
                cpfd.events |= POLLIN;
            }
            if (conn->want_write)
            {
                cpfd.events |= POLLOUT;
            }

            poll_args.push_back(cpfd);
        }

        // wait for readiness - this blocks until at least one fd is ready
        // -1 timeout blocksout forever until something is ready
        int nready = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (nready < 0)
            die("poll()");

        // handle the listening fd-
        // poll_args[0] is the listening fd
        //  if revents has POLLIN, a new client is waiting to be accepted
        if (poll_args[0].revents & POLLIN)
        { // handling the listening fd
            struct sockaddr_in client_addr = {};
            socklen_t addrlen = sizeof(client_addr);
            int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
            if (connfd < 0)
                continue;

            // make the new client fd non-blocking too--
            make_nonblocking(connfd);

            // create a Conn for this client
            Conn *conn = new Conn();
            conn->fd = connfd;
            conn->want_read = true; // we want to read the first request

            // store it in fd2Conn, indexed by fd
            // grow the array if needed
            if (fd2conn.size() <= (size_t)connfd)
            {
                fd2conn.resize(connfd + 1);
            }
            fd2conn[connfd] = conn; // it will point to the memory addres of client's conn state
            printf("New client connected ! connfd : %d\n", connfd);
        }

        // handle the client fds
        // poll_args[0] is the lisnening fd, so clients starts at index1
        for (size_t i = 1; i < poll_args.size(); i++)
        {
            uint32_t ready = poll_args[i].revents;
            Conn *conn = fd2conn[poll_args[i].fd];

            if (ready & POLLIN)
            {
                // read some bytes into temporary buffer
                uint8_t buf[64 * 1024];
                ssize_t rv = read(conn->fd, buf, sizeof(buf));

                if (rv <= 0)
                {
                    // client closed (rv ==0)or error(rv < 0);
                    printf("client %d is disconnected\n", conn->fd);
                    conn->want_close = true;
                    continue;
                }
                // append the bytes into the client's buffer
                conn->incoming.insert(conn->incoming.end(), buf, buf + rv);
                printf("client %d sent %zd bytes (total: %zu)\n", conn->fd, rv, conn->incoming.size());

                // try to parse as many complete requests as possible
                while (try_one_request(conn))
                {
                    // keep going while there are complete requests
                }
                // if we produced a response , switch to want -write
                if (conn->outgoing.size() > 0)
                {
                    conn->want_read = false;
                    conn->want_write = true;
                }
            }
            if (ready & POLLOUT)
            {
                // write some bytes from outgoing to the socket
                ssize_t rv = write(conn->fd, conn->outgoing.data(), conn->outgoing.size());

                if (rv <= 0)
                {
                    printf("client %d write error\n", conn->fd);
                    conn->want_close = true;
                    continue;
                }
                // remove written bytes from outgoing cause we have sent them to client no need to store taht
                conn->outgoing.erase(conn->outgoing.begin(), conn->outgoing.begin() + rv);

                printf("client %d wrote %zd bytes (remaining : %zu)\n", conn->fd, rv, conn->outgoing.size());

                // all sent go back to reading
                if (conn->outgoing.size() == 0)
                {
                    conn->want_read = true;
                    conn->want_write = false;
                }
            }
            if (ready & POLLERR)
            {
                printf("client %d has an error!\n", conn->fd);
            }
        }

        // cleanup clients marked up for close --
        for (size_t i = 1; i < poll_args.size(); i++)
        {
            Conn *conn = fd2conn[poll_args[i].fd]; // get the clients state
            if (!conn)
                continue; // if null continue
            if (conn->want_close)
            {
                close(conn->fd);
                fd2conn[conn->fd] = NULL;
                delete conn;
                printf("client cleaned up \n");
            }
        }
    }

    printf("setup done! \n");

    return 0;
}