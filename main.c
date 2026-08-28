#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h> // Nujno za delovanje funkcije gettimeofday()

#include <open62541/server.h>
#include <open62541/server_config_default.h>

Config global_config;
UA_Server *server = NULL;
volatile UA_Boolean running = true;

void* knx_thread_func(void *arg) {
    int knx_fd = init_knx_connection();
    bool knx_connected = (knx_fd >= 0);
    time_t last_reconnect_attempt = 0;
    
    // Časovniki za upravljanje z vrstami prometa
    struct timeval last_queue_process_time;
    gettimeofday(&last_queue_process_time, NULL);
    
    time_t last_smart_poll_check = time(NULL);
    size_t current_queue_index = 0;
    bool queue_completely_finished = false; // Zastavica za konec začetnega scana

    // Počakamo sekundo, da se OPC UA strežnik v glavni niti popolnoma zažene
    sleep(1);

    if (knx_connected) {
        printf("[KNX] Zagon uspešen. Vse naprave dodane v začetno čakalno vrsto.\n");
        time_t startup_time = time(NULL);
        for (size_t i = 0; i < global_config.dp_count; i++) {
            global_config.datapoints[i].is_initialized = false; 
            global_config.datapoints[i].last_update_time = startup_time;
            global_config.datapoints[i].last_read_sent = 0; // Števec poskusov branja ob zagonu
        }
    }

    printf("[SYSTEM] KNX nit uspešno zagnana. Začenjam optimizirano vrsto...\n");

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
        tv.tv_usec = 50000; // 50ms odzivnost zanke

        int retval = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        time_t now = time(NULL);

        if (retval < 0) {
            perror("[SYSTEM] Napaka v select() zanki KNX");
            break;
        }

        // 1. Prestrezanje prometa (Spremembe ali odgovori iz vodila)
        if (knx_connected && FD_ISSET(knx_fd, &rfds)) {
            if (handle_knx_traffic(knx_fd, server) < 0) {
                printf("[WARN] KNX povezava izgubljena! Ponovno povezovanje...\n");
                close(knx_fd);
                knx_connected = false;
                last_reconnect_attempt = now;
            }
        }

        // 2. ASINHRONA ČAKALNA VRSTA: Branje začetnih stanj (vsakih 200ms ena naprava)
        if (knx_connected && !queue_completely_finished) {
            struct timeval tv_now;
            gettimeofday(&tv_now, NULL);
            
            long elapsed_ms = (tv_now.tv_sec - last_queue_process_time.tv_sec) * 1000 + 
                               (tv_now.tv_usec - last_queue_process_time.tv_usec) / 1000;

            if (elapsed_ms >= 200) { // Takt 200ms za začetni prehod
                last_queue_process_time = tv_now;
                
                size_t checked_loops = 0;
                bool command_sent = false;
                bool any_left_in_queue = false;

                while (checked_loops < global_config.dp_count && !command_sent) {
                    DataPoint *dp = &global_config.datapoints[current_queue_index];
                    
                    if (!dp->is_initialized) {
                        any_left_in_queue = true;

                        if (dp->last_read_sent < 3) {
                            printf("[QUEUE] Začetno branje (%u/3) za: %s (%s)\n", 
                                   dp->last_read_sent + 1, dp->opc_path, dp->knx_address);
                            
                            send_knx_read_request(dp);
                            dp->last_read_sent++;
                            dp->last_update_time = now;
                            command_sent = true;
                        } else {
                            printf("[WARN] Naprava %s (%s) ne odgovarja po 3 poskusih. Preskakujem.\n", 
                                   dp->opc_path, dp->knx_address);
                            dp->is_initialized = true; 
                            dp->last_update_time = now;
                        }
                    }
                    
                    current_queue_index = (current_queue_index + 1) % global_config.dp_count;
                    checked_loops++;
                }

                if (!any_left_in_queue) {
                    printf("[QUEUE] Začetna čakalna vrsta je popolnoma IZPRAZNJENA. Prehodi v normalen način.\n");
                    queue_completely_finished = true;
                    last_smart_poll_check = now; 
                }
            }
        }

        // 3. PAMETNO OSVEŽEVANJE: Zažene se šele, ko je začetni scan popolnoma zaključen
        if (knx_connected && queue_completely_finished && (now - last_smart_poll_check >= 10)) {
            last_smart_poll_check = now;
            size_t polled_count = 0;

            for (size_t i = 0; i < global_config.dp_count; i++) {
                DataPoint *dp = &global_config.datapoints[i];

                // Osvežujemo le tiste, ki so uspešno inicializirani in so molčale 10 minut
                if (dp->is_initialized && (now - dp->last_update_time >= 600)) {
                    send_knx_read_request(dp);
                    dp->last_update_time = now;
                    polled_count++;
                    
                    // DODANO: 200ms varna pavza med poizvedbami v Smart Pollu!
                    usleep(200000); 
                }
            }
            if (polled_count > 0) {
                printf("[SMART POLL] Osveženo %zu naprav, ki so molčale več kot 10 minut.\n", polled_count);
            }
        }

        // 4. Ponovno povezovanje na knxd
        if (!knx_connected && (now - last_reconnect_attempt >= 5)) {
            last_reconnect_attempt = now;
            printf("[KNX] Poskus ponovnega povezovanja na knxd...\n");
            knx_fd = init_knx_connection();
            if (knx_fd >= 0) {
                printf("[KNX] Povezava vzpostavljena!\n");
                knx_connected = true;
                last_smart_poll_check = time(NULL);
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
    init_opcua_server(server, opc_config);

    pthread_t knx_thread;
    if (pthread_create(&knx_thread, NULL, knx_thread_func, NULL) != 0) {
        fprintf(stderr, "[ERROR] Napaka pri ustvarjanju KNX niti!\n");
        return 1;
    }

    printf("[SYSTEM] Prehod uspešno nameščen. Začenjam uradno OPC UA zanko...\n");
    UA_StatusCode retval = UA_Server_run(server, &running);

    running = false;
    pthread_join(knx_thread, NULL);
    UA_Server_delete(server);

    return retval == UA_STATUSCODE_GOOD ? 0 : 1;
}

