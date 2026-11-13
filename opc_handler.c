#include <open62541/server.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/accesscontrol_default.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "gateway.h"

extern Config global_config;
extern void send_knx_write_request(DataPoint *dp);

static uint16_t float_to_dpt9(float value) {
    float v = value * 100.0f;
    int exponent = 0;
    int mantissa = (int)roundf(v);

    while (mantissa < -2048 || mantissa > 2047) {
        mantissa /= 2;
        exponent++;
        if (exponent > 15) break;
    }

    uint16_t raw = 0;
    if (mantissa < 0) {
        raw |= 0x8000;
        mantissa += 2048;
    }
    raw |= (exponent & 0x0F) << 11;
    raw |= (mantissa & 0x07FF);
    return raw;
}

static UA_NodeId get_or_create_folder(UA_Server *server, const UA_NodeId *parent, const char *full_path, const char *display_name) {
    UA_NodeId targetNodeId = UA_NODEID_STRING(1, (char*)full_path);
    UA_NodeId outNodeId;
    UA_StatusCode rs = UA_Server_readDataType(server, targetNodeId, &outNodeId);
    if (rs == UA_STATUSCODE_GOOD || rs != UA_STATUSCODE_BADNODEIDUNKNOWN) return targetNodeId;

    UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
    oAttr.displayName = UA_LOCALIZEDTEXT("sl-SI", (char*)display_name);
    UA_Server_addObjectNode(server, targetNodeId, *parent, UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, (char*)display_name), UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE), oAttr, NULL, NULL);
    return targetNodeId;
}

static void onWriteKNX(UA_Server *server, const UA_NodeId *sessionId, void *sessionContext,
                       const UA_NodeId *nodeId, void *nodeContext, const UA_NumericRange *range, const UA_DataValue *data) {
    if (g_internal_opc_write) return; // zapis iz KNX niti (KNX-RSP/KNX-SPY) - nikoli ne vracaj na bus

    DataPoint *dp = (DataPoint*)nodeContext;
    if (!dp || !data->hasValue) return;

    UA_Boolean changed = false;

    if (strncmp(dp->dpt, "1.", 2) == 0) {
        UA_Boolean new_val = *(UA_Boolean*)data->value.data;
        uint16_t raw_target = new_val ? 1 : 0;
        if (!dp->is_initialized || !dp->has_value || dp->knx_raw_value != raw_target) {
            dp->knx_raw_value = raw_target;
            dp->value.b_val = new_val;
            dp->is_initialized = true;
            dp->has_value = true;
            changed = true;
            printf("[OPCUA->KNX] Stikalo %s -> Surovo: %d\n", dp->opc_path, dp->knx_raw_value);
        }
    }
    else if (strncmp(dp->dpt, "5.", 2) == 0) {
        float percent_val = 0.0f;
        if (data->value.type == &UA_TYPES[UA_TYPES_FLOAT]) percent_val = *(UA_Float*)data->value.data;
        else if (data->value.type == &UA_TYPES[UA_TYPES_INT32]) percent_val = (float)(*(UA_Int32*)data->value.data);

        if (percent_val < 0.0f) percent_val = 0.0f;
        if (percent_val > 100.0f) percent_val = 100.0f;
        uint16_t raw_target = (uint16_t)((percent_val * 255.0f) / 100.0f);

        if (!dp->is_initialized || !dp->has_value || dp->knx_raw_value != raw_target) {
            dp->knx_raw_value = raw_target;
            dp->is_initialized = true;
            dp->has_value = true;
            changed = true;
            printf("[OPCUA->KNX] Zatemnitev %s -> Surovo: %d\n", dp->opc_path, dp->knx_raw_value);
        }
    }
    else if (strncmp(dp->dpt, "4.", 2) == 0) {
        // DPT4.001 - single ASCII character, sent from OPC UA as Int32
        int32_t char_val = 0;
        if (data->value.type == &UA_TYPES[UA_TYPES_INT32]) char_val = *(UA_Int32*)data->value.data;
        uint16_t raw_target = (uint16_t)(char_val & 0xFF);

        if (!dp->is_initialized || !dp->has_value || dp->knx_raw_value != raw_target) {
            dp->knx_raw_value = raw_target;
            dp->value.i32_val = char_val;
            dp->is_initialized = true;
            dp->has_value = true;
            changed = true;
            printf("[OPCUA->KNX] Znak %s -> Surovo: 0x%02X\n", dp->opc_path, dp->knx_raw_value);
        }
    }
    else if (strcmp(dp->dpt, "8") == 0 || strncmp(dp->dpt, "8.", 2) == 0) {
        // DPT8.xxx - 2 byte signed
        int32_t int_val = 0;
        if (data->value.type == &UA_TYPES[UA_TYPES_INT32]) int_val = *(UA_Int32*)data->value.data;
        if (int_val < -32768) int_val = -32768;
        if (int_val > 32767) int_val = 32767;
        uint16_t raw_target = (uint16_t)((int16_t)int_val);

        if (!dp->is_initialized || !dp->has_value || dp->knx_raw_value != raw_target) {
            dp->knx_raw_value = raw_target;
            dp->value.i32_val = int_val;
            dp->is_initialized = true;
            dp->has_value = true;
            changed = true;
            printf("[OPCUA->KNX] Vrednost (DPT8) %s -> Surovo: 0x%04X\n", dp->opc_path, dp->knx_raw_value);
        }
    }
    else if (strncmp(dp->dpt, "9.", 2) == 0) {
        UA_Float target_temp = *(UA_Float*)data->value.data;
        uint16_t raw_target = float_to_dpt9(target_temp);

        if (!dp->is_initialized || !dp->has_value || dp->knx_raw_value != raw_target) {
            dp->knx_raw_value = raw_target;
            int sign = (raw_target & 0x8000) >> 15;
            int exponent = (raw_target & 0x7800) >> 11;
            int mantissa = raw_target & 0x07FF;
            if (sign) mantissa = mantissa - 2048;
            dp->value.f_val = 0.01f * mantissa * (1 << exponent);

            dp->is_initialized = true;
            dp->has_value = true;
            changed = true;
            printf("[OPCUA->KNX] Temperatura %s -> Nastavljeno surovo: 0x%04X (%.2f °C)\n", dp->opc_path, dp->knx_raw_value, dp->value.f_val);
        }
    }

    if (changed) {
        send_knx_write_request(dp);
    }
}

int init_opcua_server(UA_Server *server, UA_ServerConfig *config) {
    if(!server || !config) return -1;

    // Streznik se v celoti nastavi v configure_opcua_security(): certifikat,
    // endpointi, zaupani odjemalci in prijava. Brez uspesne nastavitve se ne zazene.
    if(configure_opcua_security(config) != 0) return -1;

    create_opcua_nodes(server);
    return 0;
}

void create_opcua_nodes(UA_Server *server) {
    for (size_t i = 0; i < global_config.dp_count; i++) {
        DataPoint *dp = &global_config.datapoints[i];
        if (dp->is_initialized) continue;
        UA_NodeId current_parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);

        char path_copy[256];
        strncpy(path_copy, dp->opc_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';
        char current_path[256];
        memset(current_path, 0, sizeof(current_path));

        char *saveptr;
        char *token = strtok_r(path_copy, ".", &saveptr);

        while (token != NULL) {
            char *next_token = strtok_r(NULL, ".", &saveptr);
            if (current_path[0] != '\0') strcat(current_path, ".");
            strcat(current_path, token);

            if (next_token != NULL) {
                current_parent = get_or_create_folder(server, &current_parent, current_path, token);
            } else {
                UA_VariableAttributes attr = UA_VariableAttributes_default;

                // DisplayName iz polne poti, kjer piko zamenjamo s podčrtajem
                char display_name_under[256];
                strncpy(display_name_under, dp->opc_path, sizeof(display_name_under) - 1);
                display_name_under[sizeof(display_name_under) - 1] = '\0';

                for (size_t j = 0; display_name_under[j] != '\0'; j++) {
                    if (display_name_under[j] == '.') {
                        display_name_under[j] = '_';
                    }
                }

                attr.displayName = UA_LOCALIZEDTEXT("sl-SI", display_name_under);
                attr.description = UA_LOCALIZEDTEXT("sl-SI", dp->knx_address);

                if (strncmp(dp->dpt, "1.", 2) == 0) {
                    UA_Boolean init_val = false;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_BOOLEAN]);
                    attr.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
                } else if (strncmp(dp->dpt, "9.", 2) == 0 || strncmp(dp->dpt, "5.", 2) == 0) {
                    UA_Float init_val = 0.0f;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_FLOAT]);
                    attr.dataType = UA_TYPES[UA_TYPES_FLOAT].typeId;
                } else {
                    // DPT4 (char), DPT8 (2-byte signed), and anything else we
                    // don't have a specific float/bool mapping for yet.
                    UA_Int32 init_val = 0;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_INT32]);
                    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
                }

                attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

                UA_NodeId myNodeId = UA_NODEID_STRING(1, dp->opc_path);
                UA_QualifiedName browseName = UA_QUALIFIEDNAME(1, dp->opc_path);

                UA_StatusCode retval = UA_Server_addVariableNode(server, myNodeId, current_parent,
                                          UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                          browseName,
                                          UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE), attr, dp, NULL);

                if (retval == UA_STATUSCODE_GOOD) {
                    UA_DataValue dv; UA_DataValue_init(&dv);
                    dv.hasStatus = true; dv.status = UA_STATUSCODE_BADWAITINGFORINITIALDATA;
                    UA_Server_writeDataValue(server, myNodeId, dv);
                }

                UA_ValueCallback callback; callback.onRead = NULL; callback.onWrite = onWriteKNX;
                UA_Server_setVariableNode_valueCallback(server, myNodeId, callback);
                dp->is_initialized = true;
            }
            token = next_token;
        }
    }
    printf("[OPCUA] Imenik zgrajen. DisplayName uporablja podčrtaje namesto pik.\n");
}
