#include<stdio.h> // for prinf
#include<unistd.h> // for close
#include<sys/socket.h>  // for socket connet
#include <stdlib.h>  // exit
#include <arpa/inet.h> // sockaddr_in, htons, htonl, INADDR_LOOPBACK
#include <string.h> // strlen


static void die(const char *msg){
    perror(msg);
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


// helper : to write or send the n bytes to the server. 
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

    // step1 : socket same as server
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) die("socket()");

    // step 2 : describe the server's address which is localHost in this case
    struct sockaddr_in addr ={};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 localhost

    // step3 connect() -> dial the server
    // bind+listen+accept happnes in server side
    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if(rv <0)die("connect()");


    // step 4: send a message
    char wbuf[] = "hello";
    write(fd, wbuf, strlen(wbuf));

    // step 5 : read the server's reply

    char rbuf[64] = {};
    ssize_t n = read_full(fd, rbuf, 4);

    if(n < 0)die("read()");

    printf("Server sent : %s\n", rbuf);

    // step 6: hang up
    close(fd);


    return 0;
}