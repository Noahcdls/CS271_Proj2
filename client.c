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
int token = 0;

snap my_state[NUM_CLIENTS];//in case everyone wants a snapshot
uint32_t markers_in [NUM_CLIENTS][NUM_CLIENTS];//track who have I received a marker from on incoming channels
//1D - iniator of snapshot; 2D - Incoming channels that have received a marker
uint32_t active_markers[NUM_CLIENTS];//check who has initiated markers
rec_msg * saved_msgs [NUM_CLIENTS][NUM_CLIENTS];//1D for marker initiator, 2D for incoming channel

gsnap my_global_state;
uint32_t awaiting_snaps;


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
        if (n <= 0)
        {
            read_socket_fds[conn_client] = 0;
            close(my_sock);
            break;
        }
        sleep(3); // 3 second delay as prescribed on handout
        switch (buff[0])
        {
        case TOKEN:
            float chance = (float)rand() / (float)RAND_MAX;
            if (chance > lose_chance)
            { // beat the odds of losing
                uint32_t client_to_send_token;
                do
                {
                    client_to_send_token = rand() % NUM_CLIENTS;
                    if (client_out[client_no][client_to_send_token] == 1)
                    {
                        next_token_loc = client_to_send_token;
                    }
                } while (client_out[client_no][client_to_send_token] != 1);
                token++;
            }
        case MARKER:
            marker * new_marker;
            memcpy(new_marker, buff[1], sizeof(marker));

            if(active_markers[new_marker->marker_id] == 0){//first time receiving a marker
                for(int i = 0; i < NUM_CLIENTS; i++){
                    // markers_in[new_marker->marker_id][i] = client_in[client_no][i];//write in read channels
                    markers_in[new_marker->marker_id][i] = 0;//clear out markers received. Will eventually match incoming channels in conf.h
                    if(i == conn_client){
                        // markers_in[new_marker->marker_id][i] = 0;//clear channel the marker came from
                        markers_in[new_marker->marker_id][i] = 1;//already have received 1 by default from sender
                    }
                    if(client_out[client_no][i] == 1 && send_socket_fds[i] != 0){//send out 
                        buff[0] = MARKER;
                        marker send_mark = {new_marker->marker_id, client_no};
                        memcpy(buff+1, &send_mark, sizeof(marker));
                        send(send_socket_fds[i] ,buff, sizeof(buff), 0);
                    }
                } 
                active_markers[new_marker->marker_id] = 1;//add active marker so incoming can add messages
            }
            else{//I now know I have an active marker for one client
                markers_in[new_marker->marker_id][conn_client]++;//increment
                if(markers_in[new_marker->marker_id][conn_client] > 1){//not first time receiving marker on channel
                    //add messages we have left out
                }

                for(int j =0; j<NUM_CLIENTS; j++){
                    if(markers_in[new_marker->marker_id][j] < client_in[client_no][j])
                        break;
                    if(j == NUM_CLIENTS-1){//we have validated that we have received markers on all incoming channels
                        active_markers[new_marker->marker_id] = 0;
                        for(int k = 0; k < NUM_CLIENTS; k++){//go through all saved msgs and add them to snapshot
                            if(client_in[client_no][k] == 0)
                                continue;
                            //add msgs from linked list

                        }
                    }
                }



            }
        case SNAP_BACK:
            memcpy(&my_global_state.snapshots[conn_client], buff+1, sizeof(snap));
            awaiting_snaps--;
            if(awaiting_snaps == 0){
                printf("FINISHED GLOBAL SNAPSHOT. NOW PRINTING\n");
                for(int i = 0; i < NUM_CLIENTS; i++){
                    printf("CLIENT %c SNAPSHOT:\n", client_names[i]);
                    printf("TOKENS: %d\n", my_global_state.snapshots[i].tokens);//print tokens
                    for(int j = 0; j < 64; j++){//go through messages saved
                        uint32_t sender = my_global_state.snapshots[i].msglist[j].sender;
                        uint32_t messg = my_global_state.snapshots[i].msglist[j].msg_type;
                        if(sender == 0)
                            break;
                        else{
                            switch(messg){
                                case TOKEN:
                                    printf("TOKEN MSG FROM CLIENT %c\n", client_names[sender]);
                                case MARKER:
                                    printf("MARKER MSG FROM CLIENT %c\n", client_names[sender]);
                                case SNAP_BACK:
                                    ("SNAPSHOT RETURNED FROM CLIENT %c\n", client_names[sender]);
                            }
                            printf("\n");
                        }
                    }
                }
            }


        }
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

        if (token && conn_client == next_token_loc)
        {
            sleep(1);
            buff[0] = TOKEN;
            n = send(my_sock, buff, sizeof(buff), 0);
            if (n <= 0)
            {
                send_socket_fds[conn_client] = 0;
                close(my_sock);
                break;
            }
        }
    }
    printf("Client send ending for %c\n", client_names[conn_client]);

    return NULL;
}

void *server_accept_thread(void *args)
{ // runs forever
    arg serv_args;
    while (1)
    {
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
    while (1)
        ;
    printf("LEAVING\n");
    return 0;
}