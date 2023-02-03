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
#include <time.h>
static socklen_t clilen;
static struct sockaddr_in serv_addr, cli_addr;
static int connect_sock, sockfd, client_no;
static int read_socket_fds[NUM_CLIENTS], send_socket_fds[NUM_CLIENTS]; // server = read, client = send
static int token = 0;
static float lose_chance = 0;
static int token_flag = 0;

snap my_state[NUM_CLIENTS];                    // in case everyone wants a snapshot
uint32_t markers_in[NUM_CLIENTS][NUM_CLIENTS]; // track who have I received a marker from on incoming channels
// 1D - iniator of snapshot; 2D - Incoming channels that have received a marker
uint32_t active_markers[NUM_CLIENTS];          // check who has initiated markers
rec_msg *saved_msgs[NUM_CLIENTS][NUM_CLIENTS]; // 1D for marker initiator, 2D for incoming channel. HEAD
rec_msg *tail_msg[NUM_CLIENTS][NUM_CLIENTS];   // TAIL OF SAVED MESSAGES

gsnap my_global_state;
uint32_t awaiting_snaps;
uint32_t next_token_loc = 0;

char ip_addrs[NUM_CLIENTS][16];
uint32_t cli_ports[NUM_CLIENTS];
char client_names[NUM_CLIENTS];

pthread_t *send_threads[NUM_CLIENTS];
pthread_t *read_threads[NUM_CLIENTS];
pthread_t accept_thread;
pthread_t terminal_thread;

pthread_mutex_t rw_lock;

void init_vars(){
    for(int i = 0; i < NUM_CLIENTS; i++){
        for(int j = 0; j < NUM_CLIENTS; j++){
            saved_msgs[i][j] = NULL;
            tail_msg[i][j] = NULL;
        }
        send_threads[i] = NULL;
        read_threads[i] = NULL;
        read_socket_fds[i] = 0;
        send_socket_fds[i] = 0;
    }
    return;
}

void cleanup()
{
    pthread_cancel(terminal_thread);
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
    pthread_mutex_destroy(&rw_lock);
    exit(0);
    return;
}

void add_msg(rec_msg *added_msg, uint32_t init_id, uint32_t channel)
{
    if (saved_msgs[init_id][channel] == NULL)
    {   
        // printf("FIRST MSG IN LIST\n");
        saved_msgs[init_id][channel] = added_msg; // make new head
        tail_msg[init_id][channel] = added_msg;   // make new tail
        // printf("ADDED MSG\n");
        return;
    }
    tail_msg[init_id][channel]->next_msg = added_msg; // link next msg
    tail_msg[init_id][channel] = added_msg;           // append to tail
    // printf("ADDED MSG\n");
    return;
}

void *server_read_thread(void *args)
{
    arg *my_args = args;
    uint32_t my_sock = my_args->socket;
    uint32_t conn_client = my_args->connected_client;
    int n;
    uint8_t buff[1024];
    // printf("MY SOCKET IS %u\n", my_sock);

    while (1)
    {
        printf("CLIENT %c WAITING FOR A MESSAGE\n\n", client_names[client_no]);
        n = recv(my_sock, buff, sizeof(buff), 0);
        printf("RECEIVED A MESSAGE FROM %c\n\n", client_names[conn_client]);

        if (n < 0)
        {
            read_socket_fds[conn_client] = 0;
            close(my_sock);
            printf("CLOSING READ SERVER ON %c READING MESSAGES FROM %c\n", client_names[client_no], client_names[conn_client]);
            break;
        }
        else if(n == 0){
            continue;
        }
        sleep(3); // 3 second delay as prescribed on handout
        printf("WAITING ON LOCK\n");
        pthread_mutex_lock(&rw_lock);         //lock for secure behavior
        switch (buff[0])
        {
        case TOKEN:
            float chance = (float)rand() / (float)RAND_MAX; // chance that we lose the token
            if (chance >= lose_chance)
            {            // beat the odds of losing the token
                printf("RECEIVED TOKEN\n\n");
                token+=1; // token now in our possession
                for (int i = 0; i < NUM_CLIENTS; i++)
                { // add messages for active markers
                    if (active_markers[i] == 1)
                    {
                        rec_msg *new_msg = malloc(sizeof(msg));
                        new_msg->saved_msg.msg_type = buff[0];
                        new_msg->saved_msg.sender = conn_client;
                        new_msg->next_msg = NULL;
                        add_msg(new_msg, i, conn_client);
                    }
                }
                uint32_t client_to_send_token;
                do
                {
                    next_token_loc = rand() % NUM_CLIENTS;
                } while (client_out[client_no][next_token_loc] != 1);
                printf("SENDING TOKEN TO %c NEXT\n\n", client_names[next_token_loc]);
                token_flag = 1;
            }
            else{
                printf("LOST TOKEN\n\n");
            }
            printf("NOW HOLD %d tokens\n\n", token);
            break;
        case MARKER:
            printf("GOT MARKER FROM CLIENT %c\n\n", client_names[conn_client]);
            for (int i = 0; i < NUM_CLIENTS; i++) // add messages to queues with active markers
            {
                if (active_markers[i] == 1)
                {
                    rec_msg *new_msg = malloc(sizeof(rec_msg));
                    new_msg->saved_msg.msg_type = buff[0];
                    new_msg->saved_msg.sender = conn_client;
                    new_msg->next_msg = NULL;
                    add_msg(new_msg, i, conn_client);
                }
            }
            marker *new_marker = malloc(sizeof(marker));
            memcpy(new_marker, buff+1, sizeof(marker));
            printf("TELL ME THE GUY WHO STARTED THIS!\n");
            printf("MARKER INITIATOR %c\n\n", client_names[new_marker->marker_id]);
            if (active_markers[new_marker->marker_id] == 0) // first time receiving marker from this initiator
            {                                               // first time receiving a marker
                printf("STARTING NEW SNAPSHOT");
                for (int i = 0; i < NUM_CLIENTS; i++)
                {
                    // markers_in[new_marker->marker_id][i] = client_in[client_no][i];//write in read channels
                    markers_in[new_marker->marker_id][i] = 0; // clear out markers received. Will eventually match incoming channels in conf.h
                    if (i == conn_client)
                    {
                        // markers_in[new_marker->marker_id][i] = 0;//clear channel the marker came from
                        markers_in[new_marker->marker_id][i] = 1; // already have received 1 by default from sender
                    }
                    if (client_out[client_no][i] == 1 && send_socket_fds[i] != 0) // send out marker to outgoing channels that are active
                    {                                                             // send out
                        buff[0] = MARKER;
                        marker send_mark = {new_marker->marker_id, client_no};
                        memcpy(buff + 1, &send_mark, sizeof(marker));    // copy marker to buffer
                        int x = 0;
                        do{
                        x = send(send_socket_fds[i], buff, sizeof(buff), 0); // send to channel
                        }while(x== 0);
                        printf("SENT MARKER TO CLIENT %c\n\n", client_names[i]);
                    }
                }
                active_markers[new_marker->marker_id] = 1; // add active marker so incoming messages can be added
                rec_msg *new_msg = malloc(sizeof(msg));//add current message to queue
                new_msg->saved_msg.msg_type = buff[0];
                new_msg->saved_msg.sender = conn_client;
                new_msg->next_msg = NULL;
                add_msg(new_msg, new_marker->marker_id, conn_client);
            }

            //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
            else
            {                                                     // this marker is already active and we are waiting to receive markers on incoming channels
                markers_in[new_marker->marker_id][conn_client]++; // increment amount of markers collected
            }
                for (int j = 0; j < NUM_CLIENTS; j++)
                {
                    if (markers_in[new_marker->marker_id][j] < client_in[client_no][j])//if we dont have enough markers, break out
                        break;
                    if (j == NUM_CLIENTS - 1)//we have markers on all incoming channels
                    { // we have validated that we have received markers on all incoming channels
                        printf("MARKERS RECEIVED ON ALL INCOMING CHANNELS\n\n");
                        bzero(&(my_state[new_marker->marker_id]), sizeof(snap));//clear out state. Select state for requested snapshot
                        my_state[new_marker->marker_id].my_id = client_no; // my_ID
                        my_state[new_marker->marker_id].tokens = token;    // Tokens if I have any
                        uint32_t msgs_added = 0;//msg counter

                        //add all recorded messages as part of state
                        for (int k = 0; k < NUM_CLIENTS; k++)
                        { // go through all saved msgs and add them to snapshot
                            // add msgs from linked list
                            uint32_t marker_counter = markers_in[new_marker->marker_id][k];//amount of markers we captured on channel k
                            rec_msg *starting_msg = saved_msgs[new_marker->marker_id][k];//saved messages since marker id first sent snapshot request, messages from incoming channel k
                            while (starting_msg != NULL)
                            {
                                if(starting_msg->saved_msg.msg_type == MARKER)//remove one marker we have seen
                                    marker_counter--;
                                if (msgs_added < 64 && marker_counter)
                                {                                                                                    // add messages with 64 max in queue
                                    my_state[new_marker->marker_id].msglist[msgs_added] = (starting_msg)->saved_msg; // copy message
                                    msgs_added++;
                                }
                                rec_msg *tmp_msg = starting_msg->next_msg;
                                free(starting_msg);     // clear memory
                                starting_msg = tmp_msg; // next message
                            }
                            saved_msgs[new_marker->marker_id][k] = NULL;//clear out head and tail
                            tail_msg[new_marker->marker_id][k] = NULL;
                        }

                        //return or add snapshot for global snapshot
                        if (new_marker->marker_id != client_no) // send back
                        {                                       // dont
                            printf("SENDING SNAPSHOT BACK TO INITIATOR %c\n\n", client_names[new_marker->marker_id]);
                            buff[0] = SNAP_BACK;
                            memcpy(buff + 1, my_state + new_marker->marker_id, sizeof(snap));
                            int x = 0;
                            do{
                            x = send(send_socket_fds[new_marker->marker_id], buff, sizeof(buff), 0); // send back snapshot
                            }while(x==0);
                        }
                        else
                        { // or if we initiated then add to our list of states
                            my_global_state.snapshots[client_no] = my_state[new_marker->marker_id];
                            awaiting_snaps--;
                            printf("MY SNAPSHOT HAS FINISHED. AWAITING %u SNAPS\n\n", awaiting_snaps);
                        }
                        active_markers[new_marker->marker_id] = 0;//no longer waiting for markers
                    }
                }
            free(new_marker);
            break;
        case SNAP_BACK:
            memcpy(&(my_global_state.snapshots[conn_client]), buff + 1, sizeof(snap));
            awaiting_snaps--;
            printf("RECEIVED SNAPSHOT BACK FROM %c. AWAITING %u SNAPS\n\n", client_names[conn_client], awaiting_snaps);
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&rw_lock);
        printf("UNLOCKED\n\n");
    }
    printf("Server read ending for %c\n", client_names[conn_client]);

    return NULL;
}


/*
@brief Send thread to other clients. Basic behavior that sends the token to other clients
@params args Arguments should contain socket and the client you are connected to.
*/
void *client_send_thread(void *args)
{
    arg *my_args = args;
    uint32_t my_sock = my_args->socket;
    uint32_t conn_client = my_args->connected_client;
    int n;
    uint8_t buff[1024];
    printf("ME A SEND CLIENT. CONNECTED TO CLIENT %c\n\n", client_names[conn_client]);
    while (1)
    {
        // pthread_mutex_lock(&rw_lock);
        if (token_flag > 0 && conn_client == next_token_loc)
        {
            printf("%c TOKEN WAITING FOR LOCK\n", client_names[conn_client]);
            pthread_mutex_lock(&rw_lock);
            sleep(1);
            
            buff[0] = TOKEN;
            printf("SENDING TOKEN\n\n");
            n = send(send_socket_fds[next_token_loc], buff, sizeof(buff), 0);
            if (n < 0)
            {
                send_socket_fds[conn_client] = 0;
                close(my_sock);
                pthread_mutex_unlock(&rw_lock);
                printf("CLOSING SEND SOCKET ON SERVER %c TO CLIENT %c\n", client_names[client_no], client_names[conn_client]);
                break;
            }
            printf("TOKEN SENT TO CLIENT %c\n", client_names[next_token_loc]);
            token = token > 0 ? token - 1 : 0;
            token_flag = 0;
            pthread_mutex_unlock(&rw_lock);
            printf("TOKEN HAS UNLOCKED\n\n");
        }
        // pthread_mutex_unlock(&rw_lock);
    }
    printf("Client send ending for %c\n", client_names[conn_client]);

    return NULL;
}


/*
@brief print global state of client
*/
void print_global_state()
{
    printf("FINISHED GLOBAL SNAPSHOT. NOW PRINTING\n");
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        printf("CLIENT %c SNAPSHOT:\n", client_names[i]);
        printf("TOKENS: %d\n", my_global_state.snapshots[i].tokens); // print tokens
        for (int j = 0; j < 64; j++)
        { // go through messages saved
            uint32_t sender = my_global_state.snapshots[i].msglist[j].sender;
            uint32_t messg = my_global_state.snapshots[i].msglist[j].msg_type;
            if (sender == 0)
                break;
            else
            {
                switch (messg)
                {
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

void *server_accept_thread(void *args)
{ // runs forever
    arg serv_args;
    printf("STARTING SERVER ACCEPT\n");
    while (1)
    {
        do{
        connect_sock = accept(sockfd,
                              (struct sockaddr *)&cli_addr,
                              &clilen); // accept a connection
        }while(connect_sock <= 0);
        // printf("A CLIENT IS TRYING TO CONNECT\n");
        uint8_t buffer[32];
        read(connect_sock, buffer, 32);
        for (int i = 0; i < NUM_CLIENTS; i++)
        {
            // printf("%u %u\n", buffer[0], (uint8_t)client_names[i]);
            if ((char)buffer[0] == client_names[i] && i != client_no)
            {
                printf("READ SOCKET %d CONNECTING WITH CLIENT %c\n\n", connect_sock, client_names[i]);
                read_socket_fds[i] = connect_sock;
                read_threads[i] = malloc(sizeof(pthread_t));
                serv_args.connected_client = i;
                serv_args.socket = connect_sock;
                printf("ACCEPTED CLIENT %c\n", client_names[i]);
                pthread_create(read_threads[i], 0, &server_read_thread, &serv_args);
                sleep(1);
                break;
            }
        }
    }
}

void *terminal_handler(void *args)
{
    uint8_t buff[1024];
    while (1)
    {
        printf("WELCOME CLIENT %c. PLEASE SELECT AN OPTION\n1. ADD TOKEN\n2. START SNAPSHOT\n3. CHANGE TOKEN LOSS PROBABILITY\n", client_names[client_no]);
        fgets(buff, 128, stdin);
        printf("\n");
        uint8_t option = (uint8_t)atoi(buff);
        switch (option)
        {
        case 1:
            pthread_mutex_lock(&rw_lock);
            do{
                next_token_loc = rand() % NUM_CLIENTS;
            }while(client_out[client_no][next_token_loc] != 1);
            printf("SENDING NEXT TO %u\n", next_token_loc);
            token += 1;
            pthread_mutex_unlock(&rw_lock);
            token_flag = 1;
            printf("ADDED TOKEN. NOW HAVE %d TOKENS\n", token);
            break;
        case 2:
            printf("STARTING SNAPSHOT\n");
            awaiting_snaps = NUM_CLIENTS;
            bzero(&my_global_state, sizeof(my_global_state));
            active_markers[client_no] = 1;//Say that I am initiating a marker and have understood it
            buff[0] = MARKER;
            marker snap_marker = {client_no, client_no};
            memcpy(buff + 1, &snap_marker, sizeof(marker));
            for (int i = 0; i < NUM_CLIENTS; i++)
                if (client_out[client_no][i] == 1 && send_socket_fds[i] != 0){//send to all outgoing channels
                    int x;
                    do{
                    x = send(send_socket_fds[i], buff, sizeof(buff), 0);
                    }while(x == 0);
                }
            while (awaiting_snaps)
                ;
            pthread_mutex_lock(&rw_lock);
            print_global_state();
            pthread_mutex_unlock(&rw_lock);
            break;
        case 3:
            printf("TYPE IN NEW PROBABILITY\n");
            fgets(buff, 128, stdin);
            lose_chance = atof(buff);
            printf("\n");
            break;
        default:
            break;
        }
    printf("\n");
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    init_vars();
    lose_chance = 0;
    srand(time(NULL));
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

    // int yes = 1;
    // if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
    // {
    //     printf("setsockopt error");
    //     exit(1);
    // }
    if (bind(sockfd, (struct sockaddr *)&serv_addr,
             sizeof(serv_addr)) < 0)
    {
        printf("ERROR on binding");
        return 0;
    }
    if(listen(sockfd, NUM_CLIENTS) < 0){// listen for clients
        printf("ERROR ON LISTEN");
        return 0;
    }
    clilen = sizeof(cli_addr);
    int pid;
    pthread_create(&accept_thread, 0, &server_accept_thread, NULL);
    arg cli_args[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (i == client_no)
            continue;
        do{
            send_socket_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        }while (send_socket_fds[i] < 0);

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
        while (connect(send_socket_fds[i], (struct sockaddr *)&conn_addr, sizeof(conn_addr)) < 0);
        int x = 0;
        do{
        x = send(send_socket_fds[i], client_names + client_no, 1, 0);
        }while(x == 0);
        // printf("CONNECTED TO CLIENT %c!\n", client_names[i]);
        cli_args[i].connected_client = i;
        cli_args[i].socket = send_socket_fds[i];
        send_threads[i] = malloc(sizeof(pthread_t));
        pthread_create(send_threads[i], 0, &client_send_thread, &cli_args[i]);
    }
    for(int i = 0; i < NUM_CLIENTS; i++){
        if(i != client_no && send_socket_fds[i] <= 0){
            printf("FAILED TO CONNECT TO CLIENT %c\n", client_names[i]);
        }
        printf("SEND SOCKET %d TO CLIENT %c\n", send_socket_fds[i], client_names[i]);
        printf("READ SOCKET %d TO CLIENT %c\n", read_socket_fds[i], client_names[i]);
    }
    // printf("Finished connecting to all clients\n");
    signal(SIGINT, cleanup);
    pthread_create(&terminal_thread, 0, &terminal_handler, NULL);
    while (1)
        ;
    printf("LEAVING\n");
    return 0;
}