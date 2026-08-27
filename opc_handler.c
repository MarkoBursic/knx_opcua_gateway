#include <open62541/server.h>
#include <open62541/plugin/log_stdout.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/accesscontrol_default.h>
#include <string.h>
#include <stdio.h>

#include "gateway.h"

extern Config global_config;
extern void send_knx_write_request(DataPoint *dp);

static UA_NodeId get_or_create_folder(UA_Server *server, const UA_NodeId *parent, const char *full_path, const char *display_name) {
    UA_NodeId targetNodeId = UA_NODEID_STRING(1, (char*)full_path);
    
    UA_NodeId outNodeId;
    UA_StatusCode rs = UA_Server_readDataType(server, targetNodeId, &outNodeId);
    
    // Če mapa (npr. Nad1) kjerkoli v sistemu že obstaja, jo takoj uporabimo in ne javljamo napak
    if (rs == UA_STATUSCODE_GOOD || rs != UA_STATUSCODE_BADNODEIDUNKNOWN) {
        return targetNodeId; 
    }

    UA_ObjectAttributes oAttr = UA_ObjectAttributes_default;
    oAttr.displayName = UA_LOCALIZEDTEXT("sl-SI", (char*)display_name);
    
    UA_Server_addObjectNode(server, targetNodeId, *parent,
                            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                            UA_QUALIFIEDNAME(1, (char*)display_name),
                            UA_NODEID_NUMERIC(0, UA_NS0ID_FOLDERTYPE),
                            oAttr, NULL, NULL);
    return targetNodeId;
}

static void onWriteKNX(UA_Server *server, const UA_NodeId *sessionId,
                       void *sessionContext, const UA_NodeId *nodeId,
                       void *nodeContext, const UA_NumericRange *range,
                       const UA_DataValue *data) {
    
    DataPoint *dp = (DataPoint*)nodeContext;
    if (!dp || !data->hasValue) return;

    UA_Boolean changed = false;

    if (strncmp(dp->dpt, "1.", 2) == 0) {
        UA_Boolean new_val = *(UA_Boolean*)data->value.data;
        if (!dp->is_initialized || dp->value.b_val != new_val) {
            dp->value.b_val = new_val;
            dp->is_initialized = true;
            changed = true;
            printf("[OPCUA->KNX] Sprememba stikala %s na: %d\n", dp->opc_path, dp->value.b_val);
        }
    } 
    else if (strncmp(dp->dpt, "5.", 2) == 0) {
        double percent_val = 0;
        if (data->value.type == &UA_TYPES[UA_TYPES_INT32]) {
            percent_val = (double)(*(UA_Int32*)data->value.data);
        } else if (data->value.type == &UA_TYPES[UA_TYPES_FLOAT]) {
            percent_val = (double)(*(UA_Float*)data->value.data) * 100.0;
        } else if (data->value.type == &UA_TYPES[UA_TYPES_BYTE]) {
            percent_val = (double)(*(UA_Byte*)data->value.data);
        }

        uint8_t knx_raw = (uint8_t)((percent_val * 255.0) / 100.0);
        if (percent_val > 100.0) knx_raw = (uint8_t)percent_val;

        if (!dp->is_initialized || dp->value.b_val != knx_raw) {
            dp->value.b_val = knx_raw;
            dp->is_initialized = true;
            changed = true;
            printf("[OPCUA->KNX] Sprememba zatemnitve %s -> Vrednost: %d\n", dp->opc_path, knx_raw);
        }
    } 
    else if (strncmp(dp->dpt, "9.", 2) == 0) {
        UA_Float new_val = *(UA_Float*)data->value.data;
        if (!dp->is_initialized || dp->value.f_val != new_val) {
            dp->value.f_val = new_val;
            dp->is_initialized = true;
            changed = true;
            printf("[OPCUA->KNX] Nastavljanje temperature %s na: %.2f °C\n", dp->opc_path, dp->value.f_val);
        }
    }

    if (changed) {
        send_knx_write_request(dp);
    }
}

void init_opcua_server(UA_Server *server, UA_ServerConfig *config) {
    if(!server || !config) return;
    UA_ServerConfig_setBasics(config);

    UA_Boolean allowAnonymous = true;
    size_t usernamePasswordLoginSize = 0;
    UA_UsernamePasswordLogin *usernamePasswordLogin = NULL;
    UA_String securityPolicyNone = UA_STRING("http://opcfoundation.org");

    UA_AccessControl_default(config, allowAnonymous, &securityPolicyNone, 
                             usernamePasswordLoginSize, usernamePasswordLogin);
    
    create_opcua_nodes(server);
}

// POPRAVLJENA LOGIKA: Popolnoma varno in čisto rekurzivno grajenje pomešanih JSON poti
void create_opcua_nodes(UA_Server *server) {
    for (size_t i = 0; i < global_config.dp_count; i++) {
        DataPoint *dp = &global_config.datapoints[i];
        
        if (dp->is_initialized) continue; 

        UA_NodeId current_parent = UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER);
        
        char path_copy[256];
        strncpy(path_copy, dp->opc_path, sizeof(path_copy) - 1);
        path_copy[sizeof(path_copy) - 1] = '\0';
        
        // REŠITEV: Medpomnilnik za pot ob vsakem ciklu popolnoma spraznimo in očistimo spomin
        char current_path[256];
        memset(current_path, 0, sizeof(current_path));
        
        char *saveptr;
        char *token = strtok_r(path_copy, ".", &saveptr);

        while (token != NULL) {
            char *next_token = strtok_r(NULL, ".", &saveptr);
            
            if (current_path[0] != '\0') {
                strcat(current_path, ".");
            }
            strcat(current_path, token);

            if (next_token != NULL) {
                // Varno prečkamo obstoječo mapo (ali Nad1 ali Prit), ne glede na prejšnje vnose
                current_parent = get_or_create_folder(server, &current_parent, current_path, token);
            } else {
                // Ustvarimo končno spremenljivko naprave na pravilnem mestu
                UA_VariableAttributes attr = UA_VariableAttributes_default;
                attr.displayName = UA_LOCALIZEDTEXT("sl-SI", token);
                attr.description = UA_LOCALIZEDTEXT("sl-SI", dp->knx_address);

                if (strncmp(dp->dpt, "1.", 2) == 0) {
                    UA_Boolean init_val = false;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_BOOLEAN]);
                    attr.dataType = UA_TYPES[UA_TYPES_BOOLEAN].typeId;
                } else if (strncmp(dp->dpt, "9.", 2) == 0) {
                    UA_Float init_val = 0.0f;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_FLOAT]);
                    attr.dataType = UA_TYPES[UA_TYPES_FLOAT].typeId;
                } else {
                    UA_Int32 init_val = 0;
                    UA_Variant_setScalar(&attr.value, &init_val, &UA_TYPES[UA_TYPES_INT32]);
                    attr.dataType = UA_TYPES[UA_TYPES_INT32].typeId;
                }

                attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;
                UA_NodeId myNodeId = UA_NODEID_STRING(1, dp->opc_path);
                
                UA_Server_addVariableNode(server, myNodeId, current_parent,
                                          UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
                                          UA_QUALIFIEDNAME(1, token),
                                          UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
                                          attr, dp, NULL);

                UA_ValueCallback callback;
                callback.onRead = NULL;
                callback.onWrite = onWriteKNX;
                UA_Server_setVariableNode_valueCallback(server, myNodeId, callback);

                dp->is_initialized = true;
            }
            token = next_token;
        }
    }
    printf("[OPCUA] Imenik uspešno zgrajen. Vse pomešane veje (Prit/Nad1) so sedaj v celoti naložene.\n");
}

