#include "gateway.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>

int load_config(const char *filename) {
    printf("[CONFIG] Naložiti poskušam konfiguracijsko datoteko: %s...\n", filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "[ERROR] Konfiguracijska datoteka %s ne obstaja!\n", filename);
        return -1;
    }

    // Preberemo celotno datoteko v medpomnilnik
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buffer = (char*)malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return -1;
    }

    size_t read_bytes = fread(buffer, 1, size, fp);
    buffer[read_bytes] = '\0';
    fclose(fp);

    // Parsanje JSON podatkov prek json-c
    struct json_object *parsed_json = json_tokener_parse(buffer);
    free(buffer);

    if (!parsed_json) {
        fprintf(stderr, "[ERROR] Napačna struktura JSON znotraj datoteke %s!\n", filename);
        return -1;
    }

    // Iskanje krovnih nastavitev (varnost)
    struct json_object *allow_anon_obj, *req_enc_obj, *datapoints_obj, *knx_conn_obj;

    if (json_object_object_get_ex(parsed_json, "allow_anonymous", &allow_anon_obj)) {
        global_config.allow_anonymous = json_object_get_boolean(allow_anon_obj);
    } else {
        global_config.allow_anonymous = true;
    }

    if (json_object_object_get_ex(parsed_json, "require_encryption", &req_enc_obj)) {
        global_config.require_encryption = json_object_get_boolean(req_enc_obj);
    } else {
        global_config.require_encryption = false;
    }

    // KNXnet/IP Routing je zamenjan s povezavo preko knxd (libeibclient).
    // Če sekcija (ali polje) manjka, uporabimo varno privzeto vrednost.
    memset(&global_config.knx_connection, 0, sizeof(global_config.knx_connection));
    strncpy(global_config.knx_connection.eibd_url, "local:/run/knx",
            sizeof(global_config.knx_connection.eibd_url) - 1);

    if (json_object_object_get_ex(parsed_json, "knx_connection", &knx_conn_obj)) {
        struct json_object *url_obj;

        if (json_object_object_get_ex(knx_conn_obj, "eibd_url", &url_obj)) {
            strncpy(global_config.knx_connection.eibd_url, json_object_get_string(url_obj),
                    sizeof(global_config.knx_connection.eibd_url) - 1);
        }
    }

    global_config.dp_count = 0;

    // OPC UA zascita: uporabniki (geslo = crypt(3) hash) in poti do PKI
    global_config.user_count = 0;
    memset(global_config.users, 0, sizeof(global_config.users));
    snprintf(global_config.pki_certificate, sizeof(global_config.pki_certificate), "pki/server_cert.der");
    snprintf(global_config.pki_private_key, sizeof(global_config.pki_private_key), "pki/server_key.der");
    snprintf(global_config.pki_trusted_dir, sizeof(global_config.pki_trusted_dir), "pki/trusted");
    snprintf(global_config.pki_application_uri, sizeof(global_config.pki_application_uri), "urn:knx-opcua-gateway");

    struct json_object *users_obj, *pki_obj;
    if (json_object_object_get_ex(parsed_json, "users", &users_obj) &&
        json_object_is_type(users_obj, json_type_array)) {
        size_t n_users = json_object_array_length(users_obj);
        for (size_t i = 0; i < n_users && global_config.user_count < MAX_USERS; i++) {
            struct json_object *uobj = json_object_array_get_idx(users_obj, i);
            struct json_object *uname, *uhash;
            if (uobj && json_object_object_get_ex(uobj, "username", &uname) &&
                json_object_object_get_ex(uobj, "password_hash", &uhash)) {
                UserConfig *u = &global_config.users[global_config.user_count++];
                strncpy(u->username, json_object_get_string(uname), sizeof(u->username) - 1);
                strncpy(u->password_hash, json_object_get_string(uhash), sizeof(u->password_hash) - 1);
            }
        }
    }
    if (json_object_object_get_ex(parsed_json, "pki", &pki_obj)) {
        struct json_object *v;
        if (json_object_object_get_ex(pki_obj, "certificate", &v))
            snprintf(global_config.pki_certificate, sizeof(global_config.pki_certificate), "%s", json_object_get_string(v));
        if (json_object_object_get_ex(pki_obj, "private_key", &v))
            snprintf(global_config.pki_private_key, sizeof(global_config.pki_private_key), "%s", json_object_get_string(v));
        if (json_object_object_get_ex(pki_obj, "trusted_dir", &v))
            snprintf(global_config.pki_trusted_dir, sizeof(global_config.pki_trusted_dir), "%s", json_object_get_string(v));
        if (json_object_object_get_ex(pki_obj, "application_uri", &v))
            snprintf(global_config.pki_application_uri, sizeof(global_config.pki_application_uri), "%s", json_object_get_string(v));
    }

    // Iskanje polja naprav (datapoints)
    if (json_object_object_get_ex(parsed_json, "datapoints", &datapoints_obj)) {
        size_t array_len = json_object_array_length(datapoints_obj);

        // Omejitev polja na maksimalno definirano velikost v gateway.h
        if (array_len > MAX_DATAPOINTS) array_len = MAX_DATAPOINTS;

        for (size_t i = 0; i < array_len; i++) {
            struct json_object *obj = json_object_array_get_idx(datapoints_obj, i);
            struct json_object *knx_addr, *dpt, *opc_path;

            if (json_object_object_get_ex(obj, "knx_address", &knx_addr) &&
                json_object_object_get_ex(obj, "dpt", &dpt) &&
                json_object_object_get_ex(obj, "opc_path", &opc_path)) {

                DataPoint *dp = &global_config.datapoints[global_config.dp_count];

                strncpy(dp->knx_address, json_object_get_string(knx_addr), sizeof(dp->knx_address) - 1);
                strncpy(dp->dpt, json_object_get_string(dpt), sizeof(dp->dpt) - 1);
                strncpy(dp->opc_path, json_object_get_string(opc_path), sizeof(dp->opc_path) - 1);
                dp->is_initialized = false;

                global_config.dp_count++;
            }
        }
    }

    json_object_put(parsed_json); // Sprostitev JSON spomina
    printf("[CONFIG] Uspešno prebranih %zu realnih naprav iz JSON konfiguracije.\n", global_config.dp_count);
    printf("[CONFIG] Povezava preko knxd: %s\n", global_config.knx_connection.eibd_url);
    printf("[CONFIG] OPC UA uporabnikov: %zu\n", global_config.user_count);
    return 0;
}
