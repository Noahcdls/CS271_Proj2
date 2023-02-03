#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <arpa/inet.h>
#include "config.h"

enum msg_types{
    TOKEN = 1,
    MARKER,
    SNAP_BACK
};

struct message{
    uint32_t msg_type;
    uint32_t sender;
};

typedef struct message msg;


struct snapshot{
    uint8_t my_id;
    uint32_t tokens;
    msg msglist[64];
};
typedef struct snapshot snap;

struct global_snapshot{
    snap snapshots[NUM_CLIENTS];//collect snap shots received
};
typedef struct global_snapshot gsnap;



struct snapshot_marker{
    uint32_t marker_id;//initiator
    uint32_t sender_id;//marker id for who initiated, sender for who to send back to
};
typedef struct snapshot_marker marker;

struct arguments {
    uint32_t connected_client;
    uint32_t socket;
};

typedef struct arguments arg;

struct recorded_msgs{
    msg saved_msg;
    struct recorded_msgs * next_msg;
};
typedef struct recorded_msgs rec_msg;