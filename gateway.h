#ifndef GATEWAY_H
#define GATEWAY_H

#include <open62541/server.h>
#include <open62541/plugin/accesscontrol_default.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

#define MAX_DATAPOINTS 2000

// Sledljivost vprasanj, ki jih poslemo mi
#define READ_KIND_INIT 1          // vprasanje ob zagonu / po izpadu (KNX-INIT)
#define READ_KIND_POLL 2          // osvezevanje molceče naprave (KNX-POLL)
#define READ_ANSWER_WINDOW_SEC 10 // toliko casa po vprasanju stejemo Response za odgovor nanj (KNX-RESP)
#define READ_ECHO_WINDOW_SEC 2    // toliko casa po vprasanju stejemo Read na istem naslovu za odmev nasega vprasanja
#define MAX_USERS 10

typedef union {
    bool b_val;
    uint8_t u8_val;
    int32_t i32_val;
    float f_val;
} DP_Value;

typedef struct {
    char knx_address[20];
    char dpt[10];
    char opc_path[100];
    uint16_t knx_numeric_addr;
    DP_Value value;
    uint16_t knx_raw_value; // Raw KNX-side value (bytes packed LSB-aligned) used for echo suppression
    bool is_initialized;
    bool has_value;         // v OPC UA vozliscu je ze pravi podatek (ne le zacetna vrednost 0)
    uint32_t last_read_sent;
    time_t last_update_time;
    bool read_unanswered;   // zacetna vrsta je obupala po 3 branjih; ponovno poskusimo cez RETRY_UNANSWERED_SEC
    time_t read_sent_time;  // kdaj smo poslali NASE vprasanje (INIT/POLL), na katero se caka odgovor (0 = nic)
    uint8_t read_kind;      // READ_KIND_INIT ali READ_KIND_POLL
} DataPoint;

typedef struct {
    char username[50];
    char password[50];
} UserConfig;

// KNX connection settings, loaded from the optional "knx_connection"
// section of config.json. We connect to knxd (via libeibclient) rather
// than opening our own KNXnet/IP multicast socket - knxd owns the actual
// link to the KNX installation and gives us protocol-level ACK/retry that
// bare Routing-mode multicast lacks.
typedef struct {
    char eibd_url[128]; // e.g. "local:/run/knx" (matches knxd.socket's Listen: /run/knx)
                        // or "ip:127.0.0.1:6720" (matches knxd.socket's Listen: [::]:6720)
} KnxConnectionConfig;

typedef struct {
    bool allow_anonymous;
    bool require_encryption;
    UserConfig users[MAX_USERS];
    size_t user_count;
    DataPoint datapoints[MAX_DATAPOINTS];
    size_t dp_count;
    KnxConnectionConfig knx_connection;
} Config;

extern Config global_config;

// Nastavljeno med zapisom v OPC UA iz KNX niti: onWriteKNX takrat ne sme poslati vrednosti nazaj na bus.
extern __thread int g_internal_opc_write;

// Watchdog bookkeeping - touched only by the KNX thread, defined in knx_handler.c.
extern time_t g_last_response_time;        // last GroupValueResponse for a mapped datapoint
extern unsigned int g_reads_since_response; // our reads sent since that response
extern time_t g_last_rx_time;               // last telegram of any kind received from the bus

int load_config(const char *filename);
void init_opcua_server(UA_Server *server, UA_ServerConfig *config);
void create_opcua_nodes(UA_Server *server);
int init_knx_connection(void);
void send_knx_write_request(DataPoint *dp);
void send_knx_read_request(DataPoint *dp);
int handle_knx_traffic(int knx_fd, UA_Server *server);

#endif // GATEWAY_H
