#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include <open62541/server.h>
#include <open62541/server_config_default.h>

Config global_config;
UA_Server *server = NULL;
volatile UA_Boolean running = true;

void* knx_thread_func(void *arg) {
    int knx_fd = init_knx_connection();
    bool knx_connected = (knx_fd >= 0);
    time_t last_reconnect_attempt = 0;

    if (knx_connected) {
        printf("[KNX] Začenjam začetno branje stanj (Startup Scan)...\n");
        for (size_t i = 0; i < global_config.dp_count; i++) {
            send_knx_read_request(&global_config.datapoints[i]);
            usleep(50000); 
        }
    }

    printf("[SYSTEM] KNX nit uspešno zagnana. Čakam na dogodke...\n");

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int max_fd = 0;

        if (knx_connected) {
            FD_SET(knx_fd, &rfds);
            max_fd = knx_fd;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        int retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        if (retval < 0) {
            perror("[SYSTEM] Napaka v select() zanki KNX");
            break;
        }

        if (knx_connected && FD_ISSET(knx_fd, &rfds)) {
            if (handle_knx_traffic(knx_fd, server) < 0) {
                printf("[WARN] KNX povezava izgubljena! Ponovno povezovanje...\n");
                close(knx_fd);
                knx_connected = false;
                last_reconnect_attempt = now;
            }
        }

        if (!knx_connected && (now - last_reconnect_attempt >= 5)) {
            last_reconnect_attempt = now;
            printf("[KNX] Poskus ponovnega povezovanja na knxd...\n");
            knx_fd = init_knx_connection();
            if (knx_fd >= 0) {
                printf("[KNX] Povezava vzpostavljena!\n");
                knx_connected = true;
            }
        }
    }

    if (knx_connected) close(knx_fd);
    return NULL;
}

int main(void) {
    printf("[SYSTEM] Zaganjam KNX <-> OPC UA Prehod...\n");

    if (load_config("config.json") != 0) {
        fprintf(stderr, "[ERROR] Napaka pri nalaganju konfiguracije.\n");
        return 1;
    }

    server = UA_Server_new();
    UA_ServerConfig *opc_config = UA_Server_getConfig(server);
    
    UA_ServerConfig_setDefault(opc_config);
    
    // Nujno: Pokličemo samo init_opcua_server, ki bo sam zgradil vse mape brez podvajanja!
    init_opcua_server(server, opc_config);

    pthread_t knx_thread;
    if (pthread_create(&knx_thread, NULL, knx_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Napaka pri ustvarjanju KNX niti!\n");
        return 1;
    }

    printf("[SYSTEM] Prehod uspešno nameščen in teče. Začenjam OPC UA mrežni sloj...\n");
    UA_StatusCode retval = UA_Server_run(server, &running);

    running = false;
    pthread_join(knx_thread, NULL);
    UA_Server_delete(server);

    return retval == UA_STATUSCODE_GOOD ? 0 : 1;
}

