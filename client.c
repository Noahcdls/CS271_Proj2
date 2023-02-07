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
#include <poll.h>
static socklen_t clilen;
static struct sockaddr_in serv_addr, cli_addr;
static int connect_sock, sockfd, client_no;
static int read_socket_fds[NUM_CLIENTS], send_socket_fds[NUM_CLIENTS]; // server = read, client = send
static struct pollfd poll_socks[NUM_CLIENTS];
static int token = 0;
static float lose_chance = 0;
static int token_flag = 0;
static uint32_t t_clock = 0;

snap my_state[NUM_CLIENTS];                    // in case everyone wants a snapshot
uint32_t markers_in[NUM_CLIENTS][NUM_CLIENTS]; // track who have I received a marker from on incoming channels
// 1D - iniator of snapshot; 2D - Incoming channels that have received a marker
uint32_t active_markers[NUM_CLIENTS], snap_started; // check who has initiated markers
rec_msg *saved_msgs[NUM_CLIENTS][NUM_CLIENTS];      // 1D for marker initiator, 2D for incoming channel. HEAD
rec_msg *tail_msg[NUM_CLIENTS][NUM_CLIENTS];        // TAIL OF SAVED MESSAGES

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
pthread_cond_t token_added;

void init_vars()
{
    pthread_cond_init(&token_added, NULL);
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        for (int j = 0; j < NUM_CLIENTS; j++)
        {
            saved_msgs[i][j] = NULL;
            tail_msg[i][j] = NULL;
        }
        send_threads[i] = NULL;
        read_threads[i] = NULL;
        read_socket_fds[i] = 0;
        send_socket_fds[i] = 0;
        active_markers[i] = 0;
        snap_started = 0;
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
            // printf("CLOSING SEND %i\n", i);
            pthread_cancel(*send_threads[i]);
            free(send_threads[i]);
            // printf("FREED\n");
        }
        if (read_threads[i] != NULL)
        {
            // printf("CLOSING READ %i\n", i);
            pthread_cancel(*read_threads[i]);
            free(read_threads[i]);
            // printf("FREED\n");
        }
    }

    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        shutdown(read_socket_fds[i], SHUT_RDWR); // close sockets leftover
        shutdown(send_socket_fds[i], SHUT_RDWR);
        // printf("SHUTDOWN SOCKETS AT %d\n", i);
        close(read_socket_fds[i]);
        close(send_socket_fds[i]);
        for (int j = 0; j < NUM_CLIENTS; j++)
        {
            rec_msg *curr_ptr = saved_msgs[i][j];
            while (curr_ptr != NULL)
            {
                rec_msg *tmp_ptr = curr_ptr->next_msg;
                free(curr_ptr);
                // printf("FREED MSG\n");
                curr_ptr = tmp_ptr;
            }
        }
    }
    shutdown(sockfd, SHUT_RDWR);
    close(sockfd);
    // printf("SHUTDOWN AND CLOSED MY OWN SOCKET\n");
    pthread_cancel(accept_thread);
    pthread_mutex_destroy(&rw_lock);
    pthread_cond_destroy(&token_added);
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

void *poll_read_thread(void *args)
{
    // arg *my_args = args;
    uint32_t my_sock;     // = my_args->socket;
    uint32_t conn_client; // = my_args->connected_client;
    int n;
    uint8_t buff[1024];
    // printf("STARTING POLL THREAD\n\n\n\n");
    while (1)
    {
        // printf("CLIENT %c WAITING FOR A MESSAGE\n\n", client_names[client_no]);
        n = poll(poll_socks, NUM_CLIENTS, -1); // infinitely poll
        // printf("RECEIVED A MESSAGE FROM %c\n\n", client_names[conn_client]);

        if (n < 0)
        {
            // read_socket_fds[conn_client] = 0;
            // close(my_sock);
            printf("CLOSING READ SERVER ON %c READING MESSAGES FROM %c\n", client_names[client_no], client_names[conn_client]);
            break;
        }
        else if (n == 0)
        {
            continue;
        }
        for (int i = 0; i < NUM_CLIENTS; i++)
        {
            if (poll_socks[i].revents & POLL_IN)
            {
                conn_client = i;
                my_sock = poll_socks[i].fd;
                read(my_sock, buff, sizeof(buff));
                break;
            }
        }
        if (n == 1)
        {             // if we have more messages waiting, we need to only worry about 1 delay
        sleep(3); // 3 second delay as prescribed on handout
        }
        // printf("WAITING ON LOCK\n");
        pthread_mutex_lock(&rw_lock); // lock for secure behavior
        t_clock++;
        switch (buff[0])
        {
        case TOKEN:
            float chance = (float)rand() / (float)RAND_MAX; // chance that we lose the token
            if (chance >= lose_chance)
            { // beat the odds of losing the token
                t_clock = *(uint32_t *)(buff + 1) > t_clock ? *(uint32_t *)(buff + 1) + 1 : t_clock + 1;
                for (int i = 0; i < NUM_CLIENTS; i++) // add messages to queues with active markers
                {
                    if (active_markers[i] == 1)
                    {
                        rec_msg *new_msg = malloc(sizeof(rec_msg));
                        new_msg->saved_msg.msg_type = buff[0];
                        new_msg->saved_msg.sender = conn_client;
                        new_msg->next_msg = NULL;
                        new_msg->saved_msg.clock_time = t_clock;
                        add_msg(new_msg, i, conn_client);
                    }
                }
                // printf("RECEIVED TOKEN\n\n");
                token += 1; // token now in our possession
                uint32_t client_to_send_token;
                do
                {
                    next_token_loc = rand() % NUM_CLIENTS;
                } while (client_out[client_no][next_token_loc] != 1);
                // printf("SENDING TOKEN TO %c NEXT\n\n", client_names[next_token_loc]);
                token_flag = 1;
                pthread_cond_signal(&token_added);
            }
            else
            {
                // printf("LOST TOKEN\n\n");
            }
            // printf("NOW HOLD %d tokens\n\n", token);
            break;
        case MARKER:
            t_clock = *(uint32_t *)(buff + 1) > t_clock ? *(uint32_t *)(buff + 1) + 1 : t_clock + 1;
            marker *new_marker = malloc(sizeof(marker));
            memcpy(new_marker, buff + 5, sizeof(marker));
            printf("GOT MARKER FROM CLIENT %c FOR CLIENT %c\n\n", client_names[conn_client], client_names[new_marker->marker_id]);
            // printf("TELL ME THE GUY WHO STARTED THIS!\n");
            // printf("MARKER INITIATOR %c\n\n", client_names[new_marker->marker_id]);
            for (int i = 0; i < NUM_CLIENTS; i++) // add messages to queues with active markers
            {
                if (active_markers[i] == 1)
                {
                    rec_msg *new_msg = malloc(sizeof(rec_msg));
                    new_msg->saved_msg.msg_type = buff[0];
                    new_msg->saved_msg.sender = conn_client;
                    new_msg->next_msg = NULL;
                    new_msg->saved_msg.mark = *new_marker;
                    new_msg->saved_msg.clock_time = t_clock;
                    add_msg(new_msg, i, conn_client);
                }
            }
            if (new_marker->marker_id == client_no && snap_started == 0)
            {
                printf("THIS IS MY MARKER\n");
                break;
            }
            if (active_markers[new_marker->marker_id] == 0) // first time receiving marker from this initiator
            {                                               // first time receiving a marker
                // printf("STARTING NEW SNAPSHOT\n\n");
                bzero(&(my_state[new_marker->marker_id]), sizeof(snap)); // clear out state. Select state for requested snapshot
                my_state[new_marker->marker_id].my_id = client_no;       // my_ID
                my_state[new_marker->marker_id].tokens = token;          // Tokens if I have any
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
                        memcpy(buff + 1, &t_clock, sizeof(uint32_t));
                        memcpy(buff + 5, &send_mark, sizeof(marker)); // copy marker to buffer
                        int x = 0;
                        do
                        {
                            x = send(send_socket_fds[i], buff, sizeof(buff), 0); // send to channel
                        } while (x == 0);
                        printf("SENT MARKER TO CLIENT %c\n\n", client_names[i]);
                    }
                }
                active_markers[new_marker->marker_id] = 1;     // add active marker so incoming messages can be added
                rec_msg *newest_msg = malloc(sizeof(rec_msg)); // add current message to queue
                newest_msg->saved_msg.msg_type = buff[0];
                newest_msg->saved_msg.sender = conn_client;
                newest_msg->next_msg = NULL;
                newest_msg->saved_msg.clock_time = t_clock;
                add_msg(newest_msg, new_marker->marker_id, conn_client);
            }

            //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
            else
            {                                                     // this marker is already active and we are waiting to receive markers on incoming channels
                markers_in[new_marker->marker_id][conn_client]++; // increment amount of markers collected
            }
            for (int j = 0; j < NUM_CLIENTS; j++)
            {
                if (markers_in[new_marker->marker_id][j] < client_in[client_no][j]) // if we dont have enough markers, break out
                    break;
                if (j == NUM_CLIENTS - 1) // we have markers on all incoming channels
                {                         // we have validated that we have received markers on all incoming channels
                    printf("MARKERS RECEIVED ON ALL INCOMING CHANNELS\n\n");
                    uint32_t msgs_added = 0; // msg counter

                    // add all recorded messages as part of state
                    for (int k = 0; k < NUM_CLIENTS; k++)
                    { // go through all saved msgs and add them to snapshot
                        // add msgs from linked list
                        uint32_t marker_counter = markers_in[new_marker->marker_id][k]; // amount of markers we captured on channel k
                        rec_msg *starting_msg = saved_msgs[new_marker->marker_id][k];   // saved messages since marker id first sent snapshot request, messages from incoming channel k
                        while (starting_msg != NULL)
                        {
                            if (starting_msg->saved_msg.msg_type == MARKER) // remove one marker we have seen
                                marker_counter--;
                            if (msgs_added < MAX_MSGS && marker_counter)
                            {                                                                                                              // add messages with MAX_MSGS as max in queue
                                memcpy(&(my_state[new_marker->marker_id].msglist[msgs_added]), &((starting_msg)->saved_msg), sizeof(msg)); // copy message
                                // printf("MSG SENT: %u FROM CLIENT %c\n\n", (starting_msg)->saved_msg.msg_type, client_names[(starting_msg)->saved_msg.sender]);
                                msgs_added++;
                            }
                            rec_msg *tmp_msg = starting_msg->next_msg;
                            free(starting_msg);     // clear memory
                            starting_msg = tmp_msg; // next message
                        }
                        saved_msgs[new_marker->marker_id][k] = NULL; // clear out head and tail
                        tail_msg[new_marker->marker_id][k] = NULL;
                    }

                    // return or add snapshot for global snapshot
                    if (new_marker->marker_id != client_no) // send back
                    {                                       // dont
                        printf("SENDING SNAPSHOT BACK TO INITIATOR %c\n", client_names[new_marker->marker_id]);
                        buff[0] = SNAP_BACK;
                        memcpy((uint32_t *)(buff + 1), &t_clock, sizeof(uint32_t));
                        memcpy(buff + 5, &my_state[new_marker->marker_id], sizeof(snap));
                        int x = 0;
                        do
                        {
                            x = send(send_socket_fds[new_marker->marker_id], buff, sizeof(buff), 0); // send back snapshot
                        } while (x == 0);
                    }
                    else
                    { // or if we initiated then add to our list of states
                        memcpy(&(my_global_state.snapshots[client_no]), &(my_state[new_marker->marker_id]), sizeof(snap));
                        awaiting_snaps--;
                        printf("MY SNAPSHOT HAS FINISHED. AWAITING %u SNAPS\n", awaiting_snaps);
                    }
                    active_markers[new_marker->marker_id] = 0; // no longer waiting for markers
                    printf("FINSHED SNAPSHOT FOR CLIENT %c\n\n", client_names[new_marker->marker_id]);
                }
            }
            free(new_marker);
            break;
        case SNAP_BACK:
            t_clock = *(uint32_t *)(buff + 1) > t_clock ? *(uint32_t *)(buff + 1) + 1 : t_clock + 1;
            if (snap_started == 0)
                break;
            memcpy(&(my_global_state.snapshots[conn_client]), buff + 5, sizeof(snap));
            awaiting_snaps--;
            printf("RECEIVED SNAPSHOT BACK FROM %c. AWAITING %u SNAPS\n\n", client_names[conn_client], awaiting_snaps);
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&rw_lock);
        // printf("UNLOCKED\n\n");
    }
    printf("Server read ending for %c\n", client_names[conn_client]);

    return NULL;
}

/*
@brief Send thread to other clients. Basic behavior that sends the token to other clients
@params args Arguments should contain socket and the client you are connected to.
*/
void *poll_send_thread(void *args)
{
    // arg *my_args = args;
    // uint32_t my_sock = my_args->socket;
    // uint32_t conn_client = my_args->connected_client;
    int n;
    uint8_t buff[1024];
    // printf("ME A SEND CLIENT. CONNECTED TO CLIENT %c\n\n", client_names[conn_client]);

    while (1)
    {
        pthread_mutex_lock(&rw_lock);
        pthread_cond_wait(&token_added, &rw_lock);
        pthread_mutex_unlock(&rw_lock); // unlock so other reads can occur
        if (token_flag > 0)
        {
            // printf("%c TOKEN WAITING FOR LOCK\n", client_names[conn_client]);
            sleep(1);
            // t_clock++;
            buff[0] = TOKEN;
            memcpy((uint32_t *)(buff + 1), &t_clock, sizeof(uint32_t));
            // printf("SENDING TOKEN\n\n");
            // uint8_t ongoing_snap = 0;
            // do
            // {
            //     ongoing_snap = 0;
            //     for (int i = 0; i < NUM_CLIENTS; i++)
            //         ongoing_snap |= active_markers[i];
            // } while (ongoing_snap);
            n = send(send_socket_fds[next_token_loc], buff, sizeof(buff), 0);
            if (n < 0)
            {
                send_socket_fds[next_token_loc] = 0;
                // close(my_sock);
                pthread_mutex_unlock(&rw_lock);
                printf("CLOSING SEND SOCKET ON SERVER %c\n", client_names[client_no]);
                break;
            }
            printf("TOKEN SENT TO CLIENT %c\n\n", client_names[next_token_loc]);
            token = token > 0 ? token - 1 : 0;
            token_flag = 0;
            // printf("TOKEN HAS UNLOCKED\n\n");
        }
    }
    printf("Client send ending for %c\n", client_names[client_no]);

    return NULL;
}

/*
@brief print global state of client
*/
void print_global_state()
{
    printf("FINISHED GLOBAL SNAPSHOT. NOW PRINTING\nOLDEST EVENTS ON TOP WITH MOST RECENT AT BOTTOM\n\n");
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        printf("CLIENT %c SNAPSHOT:\n", client_names[i]);
        printf("TOKENS: %d\n", my_global_state.snapshots[i].tokens); // print tokens
        for (int j = 0; j < 64; j++)
        { // go through messages saved
            uint32_t sender = my_global_state.snapshots[i].msglist[j].sender;
            uint32_t messg = my_global_state.snapshots[i].msglist[j].msg_type;
            uint32_t event_time = my_global_state.snapshots[i].msglist[j].clock_time;
            if (messg == 0)
                break;
            else
            {
                printf("TIME: %u | ", event_time);
                switch (messg)
                {
                case TOKEN:
                    printf("TOKEN MSG FROM CLIENT %c\n", client_names[sender]);
                    break;
                case MARKER:
                    printf("MARKER MSG FROM CLIENT %c\n", client_names[sender]);
                    printf("THE SNAPSHOT WAS INITIATED BY CLIENT %c\n", client_names[my_global_state.snapshots[i].msglist[j].mark.marker_id]);
                    break;
                case SNAP_BACK:
                    printf("SNAPSHOT RETURNED FROM CLIENT %c\n", client_names[sender]);
                    break;
                }
                // printf("\n");
            }
        }
        printf("\n");
    }
}

void *server_accept_thread(void *args)
{ // runs forever
    arg serv_args;
    printf("STARTING SERVER ACCEPT\n");
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        poll_socks[i].fd = 0;
    }
    read_threads[0] = malloc(sizeof(pthread_t));
    int client_counter = 0;
    while (1)
    {
        do
        {
            connect_sock = accept(sockfd,
                                  (struct sockaddr *)&cli_addr,
                                  &clilen); // accept a connection
        } while (connect_sock <= 0);
        // printf("A CLIENT IS TRYING TO CONNECT\n");
        uint8_t buffer[32];
        read(connect_sock, buffer, 32);
        for (int i = 0; i < NUM_CLIENTS; i++)
        {
            // printf("%u %u\n", buffer[0], (uint8_t)client_names[i]);
            if ((char)buffer[0] == client_names[i] && i != client_no)
            {
                // printf("READ SOCKET %d CONNECTING WITH CLIENT %c\n\n", connect_sock, client_names[i]);
                read_socket_fds[i] = connect_sock;
                // read_threads[i] = malloc(sizeof(pthread_t));
                serv_args.connected_client = i;
                serv_args.socket = connect_sock;
                poll_socks[i].fd = connect_sock;
                poll_socks[i].events = POLL_IN;
                // printf("ACCEPTED CLIENT %c\n", client_names[i]);

                // pthread_create(read_threads[i], 0, &server_read_thread, &serv_args);
                // sleep(1);
                client_counter++;
                if (client_counter == NUM_CLIENTS - 1) // wait for other clients to connect before start reading
                    pthread_create(read_threads[0], 0, &poll_read_thread, NULL);
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
            do
            {
                next_token_loc = rand() % NUM_CLIENTS;
            } while (client_out[client_no][next_token_loc] != 1);
            printf("SENDING NEXT TO %u\n", next_token_loc);
            token += 1;
            token_flag = 1;
            printf("ADDED TOKEN. NOW HAVE %d TOKENS\n", token);
            pthread_cond_signal(&token_added);
            pthread_mutex_unlock(&rw_lock);
            break;
        case 2:
            pthread_mutex_lock(&rw_lock);
            printf("STARTING SNAPSHOT\n");
            awaiting_snaps = NUM_CLIENTS;
            bzero(&my_global_state, sizeof(my_global_state));
            bzero(&my_state[client_no], sizeof(snap));
            my_state[client_no].my_id = client_no;
            my_state[client_no].tokens = token;
            for (int i = 0; i < NUM_CLIENTS; i++)
                markers_in[client_no][i] = 0; // clear incoming markers
            active_markers[client_no] = 1;    // Say that I am initiating a marker and have understood it
            snap_started = 1;
            buff[0] = MARKER;
            marker snap_marker = {client_no, client_no};
            memcpy(buff+1, &t_clock, sizeof(uint32_t));
            memcpy(buff + 5, &snap_marker, sizeof(marker));
            for (int i = 0; i < NUM_CLIENTS; i++)
                if (client_out[client_no][i] == 1 && send_socket_fds[i] != 0)
                { // send to all outgoing channels
                    int x;
                    do
                    {
                        x = send(send_socket_fds[i], buff, sizeof(buff), 0);
                    } while (x == 0);
                }
            pthread_mutex_unlock(&rw_lock);
            while (awaiting_snaps)
                ;
            pthread_mutex_lock(&rw_lock);
            print_global_state();
            snap_started = 0;
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
            // printf("%s ", ip_addrs[client_counter]);
        }
        tok = strtok(NULL, " \n");
        if (tok != NULL)
        {
            int tmp_port = atoi(tok);
            cli_ports[client_counter] = tmp_port;
            // printf("%d ", cli_ports[client_counter]);
        }
        tok = strtok(NULL, " \n");
        if (tok != NULL)
        {
            client_names[client_counter] = *tok;
            // printf("%c\n", client_names[client_counter]);
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
    if (listen(sockfd, NUM_CLIENTS) < 0)
    { // listen for clients
        printf("ERROR ON LISTEN");
        return 0;
    }
    clilen = sizeof(cli_addr);
    int pid;
    pthread_create(&accept_thread, 0, &server_accept_thread, NULL);
    arg cli_args[NUM_CLIENTS];
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        read_threads[i] = NULL;
        send_threads[i] = NULL;
        if (i == client_no)
            continue;
        do
        {
            send_socket_fds[i] = socket(AF_INET, SOCK_STREAM, 0);
        } while (send_socket_fds[i] < 0);

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
        int x = 0;
        do
        {
            x = send(send_socket_fds[i], client_names + client_no, 1, 0);
        } while (x == 0);
        // printf("CONNECTED TO CLIENT %c!\n", client_names[i]);
        cli_args[i].connected_client = i;
        cli_args[i].socket = send_socket_fds[i];
        // send_threads[i] = malloc(sizeof(pthread_t));
        // pthread_create(send_threads[i], 0, &client_send_thread, &cli_args[i]);
    }
    for (int i = 0; i < NUM_CLIENTS; i++)
    {
        if (i != client_no && send_socket_fds[i] <= 0)
        {
            printf("FAILED TO CONNECT TO CLIENT %c\n", client_names[i]);
        }
        // printf("SEND SOCKET %d TO CLIENT %c\n", send_socket_fds[i], client_names[i]);
        // printf("READ SOCKET %d TO CLIENT %c\n", read_socket_fds[i], client_names[i]);
    }
    send_threads[0] = malloc(sizeof(pthread_t));
    pthread_create(send_threads[0], 0, &poll_send_thread, NULL);
    // printf("Finished connecting to all clients\n");
    signal(SIGINT, cleanup);
    pthread_create(&terminal_thread, 0, &terminal_handler, NULL);
    while (1)
        ;
    printf("LEAVING\n");
    return 0;
}