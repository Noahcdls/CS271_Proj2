#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <arpa/inet.h>
#include "config.h"

socklen_t clilen;
struct sockaddr_in serv_addr, cli_addr;
int connect_sock, sockfd, client_no;
int server_socket_fds [NUM_CLIENTS], client_socket_fds [NUM_CLIENTS];
int token;



void * server_accept_thread(void *args){
    connect_sock = accept(sockfd,
                       (struct sockaddr *)&cli_addr,
                       &clilen); // accept a connection
    uint8_t buffer[32];
    read(connect_sock, buffer, 32);
    for(int i = 0; i < NUM_CLIENTS; i++){
        if(buffer[0] == client_names[i]){
            socket_fds[i] = connect_sock;
            break;
        }
    }
    
}


int main(int argc, char *argv[])
{

    int newsockfd, portno;
    int n;
    if (argc < 2)
    {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }
    char * client_id = argv[1];
    for(int i = 0; i < NUM_CLIENTS; i++){
        if(client_names[i] == *client_id){
            client_no = i; break;}
        if(i == NUM_CLIENTS - 1){
            printf("Client name not found\n");
            return;
        }
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");
    bzero((char *)&serv_addr, sizeof(serv_addr));
    portno = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);
    if (bind(sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr)) < 0)
        error("ERROR on binding");
    listen(sockfd, NUM_CLIENTS); // listen for clients
    clilen = sizeof(cli_addr);
    int pid;

    for(int i = 0; i < NUM_CLIENTS; i++){
        
    }


}