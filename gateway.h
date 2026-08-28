#ifndef GATEWAY_H
#define GATEWAY_H

#include <open62541/server.h>
#include <open62541/plugin/accesscontrol_default.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define MAX_DATAPOINTS 2000
#define MAX_USERS 10

typedef union {
    bool b_val;
    uint8_t u8_val;
    float f_val;
} DP_Value;

typedef struct {
    char knx_address[20];
    char dpt[10];
    char opc_path[100];
    uint16_t knx_numeric_addr;
    DP_Value value;
    uint16_t knx_raw_value; // POPRAVEK: Shramba surovega celoštevilskega stanja (bajtov) za Echo Suppression
    bool is_initialized;
    uint32_t last_read_sent;
    time_t last_update_time;
} DataPoint;

typedef struct {
    char username[50];
    char password[50];
} UserConfig;

typedef struct {
    bool allow_anonymous;
    bool require_encryption;
    UserConfig users[MAX_USERS];
    size_t user_count;
    DataPoint datapoints[MAX_DATAPOINTS];
    size_t dp_count;
} Config;

extern Config global_config;

int load_config(const char *filename);
void init_opcua_server(UA_Server *server, UA_ServerConfig *config);
void create_opcua_nodes(UA_Server *server);
int init_knx_connection(void);
void send_knx_write_request(DataPoint *dp);
void send_knx_read_request(DataPoint *dp);
int handle_knx_traffic(int knx_fd, UA_Server *server);

#endif // GATEWAY_H

