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

#define NUM_CLIENTS 5

int client_in [NUM_CLIENTS][NUM_CLIENTS] = {{0, 1, 0, 1, 0}, {1, 0, 1, 1, 1}, {0, 0, 0, 1, 0}, {0, 1, 0, 0, 1}, {0, 0, 0, 1, 0}};//define valiid inputs with 1
int client_out [NUM_CLIENTS][NUM_CLIENTS] = {{0, 1, 0, 0, 0}, {1, 0, 0, 1, 0}, {0, 1, 0, 0, 0}, {1, 1, 1, 0, 1}, {0, 1, 0, 1, 0}};//define valid outputs with 1
