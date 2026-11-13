#include "gateway.h"
#include "knx_eibd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <open62541/server.h>

// Watchdog bookkeeping (KNX thread only) - see main.c, section 3b.
time_t g_last_response_time = 0;
unsigned int g_reads_since_response = 0;
bool g_any_answer_seen = false;
__thread int g_internal_opc_write = 0;

// 1 = izpisuj tuja vprasanja (EXT-QRY), 0 = ne
#define LOG_EXT_QUERIES 1
time_t g_last_rx_time = 0;

// Watchdog: stejemo samo vprasanja, na katera BI morala priti odgovor. Naprave, ki se nikoli niso
// javile (odklopljene, brez Read zastavice), bi sicer same sprozile alarm. Dokler od zagona
// (ali zadnjega popolnega pregleda) ni odgovorila nobena naprava, stejemo vsa - tako ujamemo
// tudi ruter, ki je mrtev ze ob zagonu.
void watchdog_note_unanswered_read(const DataPoint *dp) {
    if (dp->answers_reads || !g_any_answer_seen) g_reads_since_response++;
}

int init_knx_connection(void) {
    const char *url = global_config.knx_connection.eibd_url;
    printf("[KNX] Povezovanje na knxd preko %s...\n", url[0] ? url : "local:/run/knx");

    int fd = knx_eibd_open(url[0] ? url : "local:/run/knx");
    if (fd < 0) {
        printf("[ERROR] Ne morem vzpostaviti povezave na knxd!\n");
        return -1;
    }
    printf("[KNX] Povezava uspešna.\n");
    return fd;
}

void send_knx_read_request(DataPoint *dp) {
    uint16_t dst = knx_parse_group_address(dp->knx_address);
    if (knx_eibd_send_group_read(dst) < 0) {
        printf("[WARN] Napaka pri pošiljanju GroupValueRead za %s (%s)\n", dp->opc_path, dp->knx_address);
        return;
    }
}

// Encodes dp's current logical value into raw KNX bytes for the wire,
// based on its DPT main type, and sends a GroupValueWrite.
void send_knx_write_request(DataPoint *dp) {
    uint16_t dst = knx_parse_group_address(dp->knx_address);
    int rc = -1;

    if (strncmp(dp->dpt, "1.", 2) == 0) {
        // 1 bit - switches, alarms, up/down, etc.
        rc = knx_eibd_send_group_write_small(dst, dp->knx_raw_value & 0x01);
        printf("[KNX-SND] Izveden vklop (DPT1) za %s (%s) -> %d\n", dp->opc_path, dp->knx_address, dp->knx_raw_value & 0x01);
    }
    else if (strncmp(dp->dpt, "5.", 2) == 0) {
        // 1 byte unsigned - percentage/scaling
        uint8_t b = (uint8_t)(dp->knx_raw_value & 0xFF);
        rc = knx_eibd_send_group_write(dst, &b, 1);
        printf("[KNX-SND] Izveden zapis (DPT5) za %s -> Surovo: %u\n", dp->opc_path, b);
    }
    else if (strncmp(dp->dpt, "4.", 2) == 0) {
        // 1 byte character (DPT4.001 ASCII)
        uint8_t b = (uint8_t)(dp->knx_raw_value & 0xFF);
        rc = knx_eibd_send_group_write(dst, &b, 1);
        printf("[KNX-SND] Izveden zapis (DPT4) za %s -> Znak: 0x%02X\n", dp->opc_path, b);
    }
    else if (strcmp(dp->dpt, "8") == 0 || strncmp(dp->dpt, "8.", 2) == 0) {
        // 2 byte signed
        uint8_t b[2] = { (uint8_t)((dp->knx_raw_value >> 8) & 0xFF), (uint8_t)(dp->knx_raw_value & 0xFF) };
        rc = knx_eibd_send_group_write(dst, b, 2);
        printf("[KNX-SND] Izveden zapis (DPT8) za %s -> Surovo: 0x%04X\n", dp->opc_path, dp->knx_raw_value);
    }
    else if (strncmp(dp->dpt, "9.", 2) == 0) {
        // 2 byte float16 (DPT9.xxx) - dp->knx_raw_value already holds the
        // encoded 16-bit float form, computed by the OPC UA write callback.
        uint8_t b[2] = { (uint8_t)((dp->knx_raw_value >> 8) & 0xFF), (uint8_t)(dp->knx_raw_value & 0xFF) };
        rc = knx_eibd_send_group_write(dst, b, 2);
        printf("[KNX-SND] Izveden zapis (DPT9) za %s -> Vrednost: %.2f (Surovo: 0x%04X)\n",
               dp->opc_path, dp->value.f_val, dp->knx_raw_value);
    }
    else {
        printf("[WARN] DPT '%s' za %s nima definiranega enkoderja za pisanje - preskočeno.\n", dp->dpt, dp->opc_path);
        return;
    }

    if (rc == 0) {
        dp->last_update_time = time(NULL);
    } else {
        printf("[WARN] Napaka pri pošiljanju GroupValueWrite za %s (%s)\n", dp->opc_path, dp->knx_address);
    }
}

// Surova vrednost telegrama za DPT naprave (ista predstavitev kot dp->knx_raw_value).
// Vrne false, ce telegram nima dovolj podatkov za ta DPT.
static bool telegram_raw(const DataPoint *dp, const KnxTelegram *tel, uint16_t *raw) {
    if (strncmp(dp->dpt, "1.", 2) == 0) { *raw = tel->short_value & 0x01; return true; }
    if (strncmp(dp->dpt, "5.", 2) == 0 || strncmp(dp->dpt, "4.", 2) == 0) {
        if (tel->data_len < 1) return false;
        *raw = tel->data[0];
        return true;
    }
    if (strcmp(dp->dpt, "8") == 0 || strncmp(dp->dpt, "8.", 2) == 0 || strncmp(dp->dpt, "9.", 2) == 0) {
        if (tel->data_len < 2) return false;
        *raw = (uint16_t)((tel->data[0] << 8) | tel->data[1]);
        return true;
    }
    return false;
}

// Cloveku berljiva vrednost iz surove (enaka dekodiranja kot v handle_knx_traffic).
static void format_raw(const DataPoint *dp, uint16_t raw, char *out, size_t n) {
    if (strncmp(dp->dpt, "1.", 2) == 0) {
        snprintf(out, n, "%s", raw ? "ON" : "OFF");
    } else if (strncmp(dp->dpt, "5.", 2) == 0) {
        snprintf(out, n, "%.1f%%", (double)raw * 100.0 / 255.0);
    } else if (strncmp(dp->dpt, "4.", 2) == 0) {
        snprintf(out, n, "0x%02X", raw);
    } else if (strcmp(dp->dpt, "8") == 0 || strncmp(dp->dpt, "8.", 2) == 0) {
        snprintf(out, n, "%d", (int)(int16_t)raw);
    } else if (strncmp(dp->dpt, "9.", 2) == 0) {
        int sign = (raw & 0x8000) >> 15;
        int exponent = (raw & 0x7800) >> 11;
        int mantissa = raw & 0x07FF;
        if (sign) mantissa -= 2048;
        snprintf(out, n, "%.2f", 0.01 * mantissa * (1 << exponent));
    } else {
        snprintf(out, n, "0x%04X", raw);
    }
}

// Surovi podatki telegrama za izpis (kratka vrednost: stevilka, sicer hex bajti).
static void format_bytes(const KnxTelegram *tel, char *out, size_t n) {
    if (tel->data_len == 0) {
        snprintf(out, n, "%u", tel->short_value);
        return;
    }
    size_t off = 0;
    out[0] = '\0';
    for (uint8_t i = 0; i < tel->data_len && off + 4 < n; i++) {
        off += (size_t)snprintf(out + off, n - off, "%s%02X", i ? " " : "", tel->data[i]);
    }
}

static void src_to_str(uint16_t a, char *out, size_t n) {
    snprintf(out, n, "%u.%u.%u", (a >> 12) & 0x0F, (a >> 8) & 0x0F, a & 0xFF);
}

// Vsak prestrezen telegram naprave iz config.json (tudi z enako vrednostjo) zapisemo v
// OPC UA z novim izvornim casovnim zigom. Zapis je oznacen kot "notranji", da
// onWriteKNX v opc_handler.c te vrednosti nikoli ne poslje nazaj na bus.
static void publish_to_opcua(UA_Server *server, DataPoint *dp, uint16_t raw) {
    UA_Boolean b = false;
    UA_Float f = 0.0f;
    UA_Int32 i32 = 0;
    UA_DataValue dv;
    UA_DataValue_init(&dv);

    if (strncmp(dp->dpt, "1.", 2) == 0) {
        b = (raw & 0x01) ? true : false;
        dp->value.b_val = b;
        UA_Variant_setScalar(&dv.value, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
    } else if (strncmp(dp->dpt, "5.", 2) == 0) {
        f = ((float)raw * 100.0f) / 255.0f;
        UA_Variant_setScalar(&dv.value, &f, &UA_TYPES[UA_TYPES_FLOAT]);
    } else if (strncmp(dp->dpt, "4.", 2) == 0) {
        i32 = (UA_Int32)raw;
        dp->value.i32_val = i32;
        UA_Variant_setScalar(&dv.value, &i32, &UA_TYPES[UA_TYPES_INT32]);
    } else if (strcmp(dp->dpt, "8") == 0 || strncmp(dp->dpt, "8.", 2) == 0) {
        i32 = (UA_Int32)(int16_t)raw;
        dp->value.i32_val = i32;
        UA_Variant_setScalar(&dv.value, &i32, &UA_TYPES[UA_TYPES_INT32]);
    } else if (strncmp(dp->dpt, "9.", 2) == 0) {
        int sign = (raw & 0x8000) >> 15;
        int exponent = (raw & 0x7800) >> 11;
        int mantissa = raw & 0x07FF;
        if (sign) mantissa -= 2048;
        f = 0.01f * mantissa * (1 << exponent);
        dp->value.f_val = f;
        UA_Variant_setScalar(&dv.value, &f, &UA_TYPES[UA_TYPES_FLOAT]);
    } else {
        return;
    }

    dv.hasValue = true;
    dv.status = UA_STATUSCODE_GOOD;
    dv.hasStatus = true;
    dv.hasSourceTimestamp = true;
    dv.sourceTimestamp = UA_DateTime_now();

    UA_NodeId node = UA_NODEID_STRING(1, dp->opc_path);
    g_internal_opc_write = 1;
    UA_Server_writeDataValue(server, node, dv);
    g_internal_opc_write = 0;
}

// Telegram z vrednostjo (Write ali Response) za datapoint iz config.json.
//   KNX-RSP  = odgovor na NASE vprasanje (KNX-INI / KNX-POL), ujema se po naslovu v oknu READ_ANSWER_WINDOW_SEC
//   KNX-SPY = vse ostalo: naprava je sama poslala vrednost ali pa je odgovorila nekomu drugemu
static void handle_dp_value(UA_Server *server, DataPoint *dp, const KnxTelegram *tel, const char *src) {
    time_t now = time(NULL);
    dp->last_update_time = now; // ura osvezevanja: ob VSAKEM telegramu naprave, tudi ob enaki vrednosti

    const bool is_response = (tel->apci == KNX_APCI_GROUPVALUE_RESPONSE);
    if (is_response) {
        dp->read_unanswered = false; // naprava ocitno odgovarja na branja
        dp->answers_reads = true;
    }

    const bool is_answer = is_response && dp->read_sent_time &&
                           (now - dp->read_sent_time) <= READ_ANSWER_WINDOW_SEC;
    if (is_answer) {
        g_last_response_time = now; // watchdog: odgovor na NASE vprasanje
        g_reads_since_response = 0;
        g_any_answer_seen = true;
        dp->read_sent_time = 0;
    }

    uint16_t raw = 0;
    if (!telegram_raw(dp, tel, &raw)) {
        printf("[KNX-%s] %s (%s) od %s -> telegram brez uporabnih podatkov ali DPT %s ni podprt\n",
               is_answer ? "RSP" : "SPY", dp->opc_path, dp->knx_address, src, dp->dpt);
        return;
    }

    const bool same = dp->has_value && dp->knx_raw_value == raw;
    dp->knx_raw_value = raw;
    dp->is_initialized = true;
    dp->has_value = true;

    publish_to_opcua(server, dp, raw);

    char shown[32], rawtxt[64];
    format_raw(dp, raw, shown, sizeof(shown));
    format_bytes(tel, rawtxt, sizeof(rawtxt));
    const char *state = same ? "nespremenjeno" : "spremenjeno";

    if (is_answer) {
        printf("[KNX-RSP] %s (%s) od %s -> %s (surovo: %s) [%s]\n",
               dp->opc_path, dp->knx_address, src, shown, rawtxt, state);
    } else {
        printf("[KNX-SPY] %s (%s) od %s -> %s (surovo: %s) [%s%s]\n",
               dp->opc_path, dp->knx_address, src, shown, rawtxt, state,
               is_response ? ", odgovor drugemu" : "");
    }
}

// Called from main's select() loop when knxd's fd has data. EIBGetGroup_Src
// (inside knx_eibd_recv) blocks only for the message already in flight - safe to
// call exactly once per select() wake-up.
// Returns 0 if handled (or a frame we don't track), -1 on real connection
// failure - caller should reconnect.
int handle_knx_traffic(int knx_fd, UA_Server *server) {
    (void)knx_fd; // kept for API compatibility with main.c's select() usage

    KnxTelegram tel;
    int rc = knx_eibd_recv(&tel);
    if (rc == -3) return -1;  // real connection failure - reconnect
    if (rc != 0) return 0;    // received something, but not a GroupValue telegram

    time_t now = time(NULL);
    g_last_rx_time = now; // vodilo zivi (tudi ce nihce ne odgovarja na nasa vprasanja)

    char ga_str[16], src[16];
    knx_format_group_address(tel.dst_addr, ga_str, sizeof(ga_str));
    src_to_str(tel.src_addr, src, sizeof(src));

    // 1) Vprasanje (Read). Nase vprasanje knxd odmeva tudi na nasi sprejemni povezavi:
    //    Read na naslovu, kamor smo ravnokar poslali vprasanje, je odmev nas; vse ostalo je tuje.
    if (tel.apci == KNX_APCI_GROUPVALUE_READ) {
        bool ours = false;
        const DataPoint *first = NULL;
        for (size_t i = 0; i < global_config.dp_count; i++) {
            const DataPoint *dp = &global_config.datapoints[i];
            if (strcmp(dp->knx_address, ga_str) != 0) continue;
            if (!first) first = dp;
            if (dp->read_sent_time && (now - dp->read_sent_time) <= READ_ECHO_WINDOW_SEC) ours = true;
        }
        if (LOG_EXT_QUERIES && !ours) {
            if (first) printf("[EXT-QRY] %s (%s) od %s\n", first->opc_path, ga_str, src);
            else       printf("[EXT-QRY] (%s) od %s - ni v config.json\n", ga_str, src);
        }
        return 0;
    }

    if (tel.apci != KNX_APCI_GROUPVALUE_WRITE && tel.apci != KNX_APCI_GROUPVALUE_RESPONSE) return 0;

    // 2) Telegram z vrednostjo. Isti naslov je lahko v config.json vec krat - posodobimo vse.
    bool matched = false;
    for (size_t i = 0; i < global_config.dp_count; i++) {
        DataPoint *dp = &global_config.datapoints[i];
        if (strcmp(dp->knx_address, ga_str) != 0) continue;
        matched = true;
        handle_dp_value(server, dp, &tel, src);
    }

    // 3) Naprava ni v config.json: samo izpis surovih vrednosti, v OPC UA se ne zapisuje.
    if (!matched) {
        if (tel.data_len == 0) {
            printf("[KNX-DBG] Neznan naslov %s (apci=%d, kratka vrednost=%u) - ni v config.json\n",
                   ga_str, (int)tel.apci, tel.short_value);
        } else {
            char hex_str[3 * sizeof(tel.data) + 1] = {0};
            size_t hex_off = 0;
            for (uint8_t b = 0; b < tel.data_len && hex_off + 3 < sizeof(hex_str); b++) {
                hex_off += (size_t)snprintf(&hex_str[hex_off], sizeof(hex_str) - hex_off, "%02X ", tel.data[b]);
            }
            printf("[KNX-DBG] Neznan naslov %s (apci=%d, podatki=%s) - ni v config.json\n",
                   ga_str, (int)tel.apci, hex_str);
        }
    }
    return 0;
}
