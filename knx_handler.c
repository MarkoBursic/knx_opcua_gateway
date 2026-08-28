#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <open62541/server.h>

static FILE *knx_monitor_stream = NULL;

int init_knx_connection(void) {
    printf("[KNX] Inicializacija asinsidega sprejemnika preko groupsocketlisten...\n");
    knx_monitor_stream = popen("knxtool groupsocketlisten ip:127.0.0.0 2>/dev/null", "r");
    if (!knx_monitor_stream) {
        printf("[ERROR] Ne morem zagnati knxtool monitorja za branje stanj!\n");
        return -1;
    }
    printf("[KNX] Poslušanje avtobusa uspešno aktivirano.\n");
    return fileno(knx_monitor_stream);
}

void send_knx_read_request(DataPoint *dp) {
    char cmd[256]; 
    snprintf(cmd, sizeof(cmd), "knxtool groupread ip:127.0.0.0 %s > /dev/null 2>&1", dp->knx_address);
    system(cmd);
}

void send_knx_write_request(DataPoint *dp) {
    char cmd[256]; 

    if (strncmp(dp->dpt, "1.", 2) == 0) {
        snprintf(cmd, sizeof(cmd), "knxtool groupswrite ip:127.0.0.0 %s %d > /dev/null 2>&1", 
                 dp->knx_address, dp->value.b_val);
        system(cmd);
        dp->last_update_time = time(NULL);
        printf("[KNX-SISTEM] Izveden vklop prek knxtool za %s -> %d\n", dp->opc_path, dp->value.b_val);
    } 
    else if (strncmp(dp->dpt, "5.", 2) == 0) {
        snprintf(cmd, sizeof(cmd), "knxtool groupswrite ip:127.0.0.0 %s %d > /dev/null 2>&1", 
                 dp->knx_address, dp->knx_raw_value); // Pošljemo surov celoštevilski bajt
        system(cmd);
        dp->last_update_time = time(NULL);
        printf("[KNX-SISTEM] Izveden vklop prek knxtool za %s -> Surova vrednost: %d\n", dp->opc_path, dp->knx_raw_value);
    }
    else if (strncmp(dp->dpt, "9.", 2) == 0) {
        // Za DPT9 iz surove 16-bitne vrednosti rekonstruiramo fizikalni float za knxtool
        int sign = (dp->knx_raw_value & 0x8000) >> 15;
        int exponent = (dp->knx_raw_value & 0x7800) >> 11;
        int mantissa = dp->knx_raw_value & 0x07FF;
        if (sign) mantissa = mantissa - 2048;
        float temp_to_send = 0.01f * mantissa * (1 << exponent);

        snprintf(cmd, sizeof(cmd), "knxtool groupswrite ip:127.0.0.0 %s %.2f > /dev/null 2>&1", 
                 dp->knx_address, temp_to_send);
        system(cmd);
        dp->last_update_time = time(NULL);
        printf("[KNX-SISTEM] Nastavljena temperatura za %s -> Vrednost: %.2f °C (Surovo: 0x%04X)\n", dp->opc_path, temp_to_send, dp->knx_raw_value);
    }
}

int handle_knx_traffic(int knx_fd, UA_Server *server) {
    char line[512];
    if (fgets(line, sizeof(line), knx_monitor_stream) == NULL) return -1; 

    char *to_ptr = strstr(line, " to ");
    if (!to_ptr) return 0;

    char *addr_start = to_ptr + 4; 
    char *colon_ptr = strchr(addr_start, ':');
    if (!colon_ptr) return 0;

    char extracted_addr[64];
    size_t addr_len = colon_ptr - addr_start;
    if (addr_len >= sizeof(extracted_addr)) return 0;
    
    strncpy(extracted_addr, addr_start, addr_len);
    extracted_addr[addr_len] = '\0';

    char *trimmed_addr = strtok(extracted_addr, " \t\r\n");
    if (!trimmed_addr) return 0;
    char *val_ptr = colon_ptr + 1; 

    for (size_t i = 0; i < global_config.dp_count; i++) {
        DataPoint *dp = &global_config.datapoints[i];
        if (strcmp(dp->knx_address, trimmed_addr) == 0) {
            UA_NodeId node = UA_NODEID_STRING(1, dp->opc_path);
            dp->last_update_time = time(NULL);

            if (strncmp(dp->dpt, "1.", 2) == 0) {
                int raw_val = 0;
                if (sscanf(val_ptr, "%x", &raw_val) == 1 || sscanf(val_ptr, "%d", &raw_val) == 1) {
                    uint16_t actual_raw = (raw_val & 0x01);
                    if (dp->is_initialized && dp->knx_raw_value == actual_raw) break; // Echo suppression

                    dp->knx_raw_value = actual_raw;
                    dp->value.b_val = (bool)actual_raw;
                    dp->is_initialized = true;

                    UA_DataValue dv; UA_DataValue_init(&dv);
                    UA_Variant_setScalar(&dv.value, &dp->value.b_val, &UA_TYPES[UA_TYPES_BOOLEAN]);
                    dv.hasValue = true; dv.status = UA_STATUSCODE_GOOD; dv.hasStatus = true;
                    UA_Server_writeDataValue(server, node, dv);
                }
            }
            else if (strncmp(dp->dpt, "5.", 2) == 0) {
                int raw_val = 0;
                if (sscanf(val_ptr, "%x", &raw_val) == 1 || sscanf(val_ptr, "%d", &raw_val) == 1) {
                    uint16_t actual_raw = (uint16_t)(raw_val & 0xFF);
                    if (dp->is_initialized && dp->knx_raw_value == actual_raw) break; // Echo suppression

                    dp->knx_raw_value = actual_raw;
                    dp->is_initialized = true;

                    UA_Float percent_display = ((float)actual_raw * 100.0f) / 255.0f;
                    UA_DataValue dv; UA_DataValue_init(&dv);
                    UA_Variant_setScalar(&dv.value, &percent_display, &UA_TYPES[UA_TYPES_FLOAT]);
                    dv.hasValue = true; dv.status = UA_STATUSCODE_GOOD; dv.hasStatus = true;
                    UA_Server_writeDataValue(server, node, dv);
                }
            }
            else if (strncmp(dp->dpt, "9.", 2) == 0) {
                unsigned int b1 = 0, b2 = 0;
                if (sscanf(val_ptr, "%x %x", &b1, &b2) == 2) {
                    uint16_t actual_raw = (uint16_t)(((b1 & 0xFF) << 8) | (b2 & 0xFF));
                    
                    // 100% zanesljiva celoštevilska primerjava bajtov - odmev takoj zavržemo!
                    if (dp->is_initialized && dp->knx_raw_value == actual_raw) break;

                    dp->knx_raw_value = actual_raw;
                    dp->is_initialized = true;

                    int sign = (b1 & 0x80) >> 7;
                    int exponent = (b1 & 0x78) >> 3;
                    int mantissa = ((b1 & 0x07) << 8) | b2;
                    if (sign) mantissa = mantissa - 2048;
                    dp->value.f_val = 0.01f * mantissa * (1 << exponent);

                    UA_DataValue dv; UA_DataValue_init(&dv);
                    UA_Variant_setScalar(&dv.value, &dp->value.f_val, &UA_TYPES[UA_TYPES_FLOAT]);
                    dv.hasValue = true; dv.status = UA_STATUSCODE_GOOD; dv.hasStatus = true;
                    UA_Server_writeDataValue(server, node, dv);
                    printf("[KNX-REALNO] Posodobljena temperatura %s -> %.2f °C\n", dp->opc_path, dp->value.f_val);
                }
            }
            break; 
        }
    }
    return 0;
}

