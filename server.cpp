#include <stdio.h> // printf , fprintf
#include <stdlib.h> // abort
#include < string.h>//strlen
#include <errno.h> //errno
#include < unistd.h> // read write close
#include< arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>


int main(){
    // step 1 Create a socket handle (IPv4 TCP)
    int fd = socket(AF_INET, SOCK_STREAM, 0);   // AF_INET -> ipv4 , Sock_Stream  = TCP, different for UDP, IPV6

    if(fd < 0){
        die("socekt()");  // socket() return -1 on failure
    }
    // step 2 : allow reusing the same port immediately after restart
    int val =1;
    setsocketopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    struct sockaddr_in addr ={};
    return 0;
}