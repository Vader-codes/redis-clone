#include<stdio.h> // for prinf
#include<unistd.h> // for close
#include<sys/socket.h>  // for socket
#include <stdlib.h> 
#include <arpa/inet.h>
#include <string.h>

// helper function to close the socket
static void die(const char *msg){
    perror(msg);  // print error message
    exit(1);
}



//helper : read exactly n bytes
static int32_t read_full(int fd, char* buf, size_t n){
    while( n > 0){
        ssize_t rv = read(fd, buf, n);
         if(rv <= 0)return -1;
         

         n-=(size_t)rv;
         buf+=rv;
    }

    return 0;
}

// helper : to write or send the n bytes to the client 
static int32_t write_all(int fd, const char* buf, size_t n){
    while( n > 0){
        ssize_t rv = write(fd, buf, n);

        if(rv <=0 )return -1;

        n-= (size_t)rv;
        buf+=rv;
    }
    return 0;
}

int main(){
     // step 1
    // ask  OS for a socket (a "ticket" for networking)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) die("socket()"); // socket failed print and quit
    printf("Got  a socket! fd = %d:\n", fd);
   
  // allow rebinding same port after restart
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));


    // prepare the address we want to claim : 0.0.0.0: 1234
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;  // this is for ipv4 address
    addr.sin_port = htons(1234); // port converted to network format
    addr.sin_addr.s_addr = htonl(0); // 0.0.0.0 wildcard (converted too)
    
    
    // step 2 : claim it
    int rv = bind(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if(rv < 0) die("listen()");
     // we got the ticket
      printf("Bound to port 1234!\n");
   


    //step 3 : start accepting connections , queue up to SOMAXCONN waiting clients
    rv = listen(fd, SOMAXCONN);
    if(rv < 0) die("bind()");
     printf("listening to port 1234!\n");
    


    // step 4: accept() pull one clien from the queue
    // connfd = new socket for our  side of the client's connection
    // client_addr = filled with client's address not a new socket
    // addrlen = size of client_addr(in /out parameter);
    while(true){
        struct sockaddr_in client_addr = {};
        socklen_t addrlen = sizeof(client_addr);

        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if(connfd < 0)
            continue; // retry on failure

        printf("New client connected! connfd = %d :\n", connfd);

        // step 5 read what the client sent (up to 63 bytes + null operator)
        char rbuf[4+64] = {};  // 4 bytes for length 64 for message body
        int32_t err = read_full(connfd, rbuf, 4); 
        if(err){
            printf("read() error (length)\n");
            close(connfd);
            continue;
        }

        // interept the message as a uint32_t 
        uint32_t len =0;
        memcpy(&len, rbuf, 4); // assume little endian
        printf("client says : %s \n", rbuf);s
        // send a reply
        char wbuf[] = "world";
        write(connfd, wbuf, sizeof(wbuf));
        close(connfd); // for now 
    }
    close(fd);

    return 0;
}