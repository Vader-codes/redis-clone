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
            if (conn->want->write)
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
            close(connfd);
        }
    }

    printf("setup done! \n");

    return 0;
}