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
#include <pthread.h>
#include <fcntl.h>
#include "client.h"
socklen_t clilen;
struct sockaddr_in serv_addr, cli_addr;
int connect_sock, sockfd, client_no;
int read_socket_fds[NUM_CLIENTS], send_socket_fds[NUM_CLIENTS]; // server = read, client = send
int token;

char ip_addrs[NUM_CLIENTS][16];
uint32_t cli_ports[NUM_CLIENTS];
char client_names[NUM_CLIENTS];

pthread_t *send_threads[NUM_CLIENTS];
pthread_t *read_threads[NUM_CLIENTS];
pthread_t accept_thread;
pthread_mutex_t rw_lock;

void cleanup()
{


    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (send_threads[i] != NULL)
        { // shutdown and clean threads
            pthread_cancel(*send_threads[i]);
            free(send_threads[i]);
        }
        if (read_threads[i] != NULL)
        {
            pthread_cancel(*read_threads[i]);
            free(read_threads[i]);
        }
        shutdown(read_socket_fds[i], SHUT_RDWR); // close sockets leftover
        shutdown(send_socket_fds[i], SHUT_RDWR);
        printf("SHUTDOWN SOCKETS AT %d\n", i);
        close(read_socket_fds[i]);
        close(send_socket_fds[i]);
    }
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    printf("SHUTDOWN AND CLOSED MY OWN SOCKET\n");
    pthread_cancel(accept_thread);
    exit(0);
    return;
}


void *server_read_thread(void *args)
{
    arg *my_args = args;
    uint32_t my_sock = my_args->socket;
    uint32_t conn_client = my_args->connected_client;
    int n;
    uint8_t buff[128];
    while (1)
    {
        n = read(my_sock, buff, sizeof(buff));
        if(n <= 0){
            if(send_threads[conn_client] != NULL)
            pthread_cancel(*send_threads[conn_client]);
            close(my_sock);
            break;
            pthread_cancel(*send_threads[conn_client]);
            printf("Server read ending for %c\n", client_names[conn_client]);
            return NULL;
        }
        printf("%s", (char *)buff);
        sleep(3);
    }
            printf("Server read ending for %c\n", client_names[conn_client]);

    return NULL;
}
void *client_send_thread(void *args)
{
    arg *my_args = args;
    uint32_t my_sock = my_args->socket;
    uint32_t conn_client = my_args->connected_client;
    int n;
    uint8_t buff[128];
    while (1)
    {
        sprintf(buff, "MESSAGE FROM %d\n", client_no);
        n = send(my_sock, buff, sizeof(buff), MSG_NOSIGNAL);
        if(n <= 0){
            close(my_sock);
            break;
            pthread_cancel(*read_threads[conn_client]);
            printf("Client send ending for %c\n", client_names[conn_client]);
            return NULL;
        }
        printf("WROTE MESSAGE TO %c\n", client_names[conn_client]);
        sleep(10);
    }
                printf("Client send ending for %c\n", client_names[conn_client]);

    return NULL;
}

void *server_accept_thread(void *args)
{ // runs forever
    arg serv_args;
    while(1){
    connect_sock = accept(sockfd,
                          (struct sockaddr *)&cli_addr,
                          &clilen); // accept a connection
    uint8_t buffer[32];
    read(connect_sock, buffer, 32);
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (buffer[0] == client_names[i] && i != client_no)
        {
            read_socket_fds[i] = connect_sock;
            read_threads[i] = malloc(sizeof(pthread_t));
            serv_args.connected_client = i;
            serv_args.socket = connect_sock;
            pthread_create(read_threads[i], 0, &server_read_thread, &serv_args);
            break;
        }
    }
    }
}

int main(int argc, char *argv[])
{
    pthread_mutex_init(&rw_lock, 0); // init rw lock
    FILE *fp = fopen("config.txt", "r");
    if (fp == NULL)
        return 0;
    char *config_line = NULL;
    size_t line_len;
    ssize_t read_length;
    int client_counter = 0;
    while (read_length = getline(&config_line, &line_len, fp) != -1)
    {
        if (client_counter >= NUM_CLIENTS)
            break;
        char ip_port[24];
        char *tok;
        tok = strtok(config_line, " \n");
        if (tok != NULL)
        {
            strcpy(ip_addrs[client_counter], tok);
            printf("%s ", ip_addrs[client_counter]);
        }
        tok = strtok(NULL, " \n");
        if (tok != NULL)
        {
            int tmp_port = atoi(tok);
            cli_ports[client_counter] = tmp_port;
            printf("%d ", cli_ports[client_counter]);
        }
        tok = strtok(NULL, " \n");
        if (tok != NULL)
        {
            client_names[client_counter] = *tok;
            printf("%c\n", client_names[client_counter]);
        }

        client_counter++;
    }
    fclose(fp);

    int newsockfd, portno;
    int n;
    if (argc < 2)
    {
        fprintf(stderr, "ERROR, no port provided\n");
        exit(1);
    }
    char *client_id = argv[1];
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (client_names[i] == *client_id)
        {
            client_no = i;
            break;
        }
        if (i == NUM_CLIENTS - 1)
        {
            printf("Client name not found\n");
            return 0;
        }
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        printf("ERROR opening socket");
        return 0;
    }
    bzero((char *)&serv_addr, sizeof(serv_addr));
    // portno = atoi(argv[1]);
    portno = cli_ports[client_no];
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    int yes = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
    {
        printf("setsockopt error");
        exit(1);
    }
    if (bind(sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr)) < 0)
    {
        printf("ERROR on binding");
        return 0;
    }
    listen(sockfd, NUM_CLIENTS); // listen for clients
    clilen = sizeof(cli_addr);
    int pid;
    pthread_create(&accept_thread, 0, &server_accept_thread, NULL);
    arg cli_args;
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (i == client_no)
            continue;
        do
            send_socket_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        while (send_socket_fds[i] < 0);

        struct hostent *tmp_server = gethostbyname(ip_addrs[i]);
        if (tmp_server == NULL)
        {
            printf("Bad IP address. Please fix your config\n");
            cleanup();
        }
        struct sockaddr_in conn_addr;
        bzero((char *)&conn_addr, sizeof(conn_addr));
        conn_addr.sin_family = AF_INET;
        bcopy((char *)tmp_server->h_addr,
              (char *)&conn_addr.sin_addr.s_addr,
              tmp_server->h_length);
        conn_addr.sin_port = htons(cli_ports[i]);
        while (connect(send_socket_fds[i], (struct sockaddr *)&conn_addr, sizeof(conn_addr)) < 0)
            ;
        send(send_socket_fds[i], client_names + i, 1, MSG_NOSIGNAL);
        printf("CONNECTED TO CLIENT %c!\n", client_names[i]);
        cli_args.connected_client = i;
        cli_args.socket = send_socket_fds[i];
        send_threads[i] = malloc(sizeof(pthread_t));
        pthread_create(send_threads[i], 0, &client_send_thread, &cli_args);
    }
    printf("Finished connecting to all clients\n");
    signal(SIGINT, cleanup);
    while(1);
    printf("LEAVING\n");
    return 0;
}