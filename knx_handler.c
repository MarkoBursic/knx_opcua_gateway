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
        printf("[KNX-SISTEM] Izveden vklop prek knxtool za %s -> %d\n", dp->opc_path, dp->value.b_val);
    } 
    else if (strncmp(dp->dpt, "5.", 2) == 0) {
        snprintf(cmd, sizeof(cmd), "knxtool groupswrite ip:127.0.0.0 %s %d > /dev/null 2>&1", 
                 dp->knx_address, dp->value.b_val);
        system(cmd);
        printf("[KNX-SISTEM] Izveden vklop prek knxtool za %s -> Surova vrednost: %d\n", dp->opc_path, dp->value.b_val);
    }
    else if (strncmp(dp->dpt, "9.", 2) == 0) {
        snprintf(cmd, sizeof(cmd), "knxtool groupswrite ip:127.0.0.0 %s %.2f > /dev/null 2>&1", 
                 dp->knx_address, dp->value.f_val);
        system(cmd);
        printf("[KNX-SISTEM] Uspešno nastavljena temperatura za %s -> Vrednost: %.2f °C\n", dp->opc_path, dp->value.f_val);
    }
}

int handle_knx_traffic(int knx_fd, UA_Server *server) {
    char line[512];
    
    if (fgets(line, sizeof(line), knx_monitor_stream) == NULL) {
        return -1; 
    }

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
            UA_Variant val;

            if (strncmp(dp->dpt, "1.", 2) == 0) {
                int raw_val = 0;
                if (sscanf(val_ptr, "%x", &raw_val) == 1 || sscanf(val_ptr, "%d", &raw_val) == 1) {
                    UA_Boolean actual_val = (raw_val & 0x01);
                    
                    if (!dp->is_initialized || dp->value.b_val != actual_val) {
                        dp->value.b_val = actual_val;
                        dp->is_initialized = true;

                        UA_Variant_setScalar(&val, &actual_val, &UA_TYPES[UA_TYPES_BOOLEAN]);
                        UA_Server_writeValue(server, node, val);
                        printf("[KNX-REALNO] Posodobljeno stikalo %s -> Vrednost: %d\n", dp->opc_path, actual_val);
                    }
                }
            }
            else if (strncmp(dp->dpt, "5.", 2) == 0) {
                int raw_val = 0;
                if (sscanf(val_ptr, "%x", &raw_val) == 1 || sscanf(val_ptr, "%d", &raw_val) == 1) {
                    UA_Byte actual_val = (UA_Byte)raw_val;

                    if (!dp->is_initialized || dp->value.b_val != actual_val) {
                        dp->value.b_val = actual_val;
                        dp->is_initialized = true;

                        // Skaliranje nazaj v procente za lep prikaz v UaExpertu
                        UA_Int32 percent_display = (UA_Int32)((actual_val * 100) / 255);
                        UA_Variant_setScalar(&val, &percent_display, &UA_TYPES[UA_TYPES_INT32]);
                        UA_Server_writeValue(server, node, val);
                        printf("[KNX-REALNO] Posodobljena zatemnitev %s -> Vrednost: %d%%\n", dp->opc_path, percent_display);
                    }
                }
            }
            else if (strncmp(dp->dpt, "9.", 2) == 0) {
                unsigned int b1 = 0, b2 = 0;
                if (sscanf(val_ptr, "%x %x", &b1, &b2) == 2) {
                    int sign = (b1 & 0x80) >> 7;
                    int exponent = (b1 & 0x78) >> 3;
                    int mantissa = ((b1 & 0x07) << 8) | b2;
                    
                    if (sign) mantissa = mantissa - 2048;
                    float temp = 0.01f * mantissa * (1 << exponent);

                    if (temp > -30.0f && temp < 60.0f) {
                        // REŠITEV: Če vrednost že obstaja v strežniku in smo jo ravnokar poslali,
                        // ignoriramo odmev monitorja, ki javi 0.00 ali staro vrednost
                        if (temp == 0.00f && dp->value.f_val != 0.00f) break; 

                        if (!dp->is_initialized || dp->value.f_val != temp) {
                            dp->value.f_val = temp;
                            dp->is_initialized = true;

                            UA_Float my_float = temp;
                            UA_Variant_setScalar(&val, &my_float, &UA_TYPES[UA_TYPES_FLOAT]);
                            UA_Server_writeValue(server, node, val);
                            printf("[KNX-REALNO] Posodobljena temperatura %s -> %.2f °C\n", dp->opc_path, temp);
                        }
                    }
                }
            }
            break; 
        }
    }
    return 0;
}

