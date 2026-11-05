#include "knx_eibd.h"
#include <eibclient.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>

// Two separate knxd connections - see the header comment on knx_eibd_open()
// for why. g_send_con is shared between threads and MUST only be touched
// while holding g_send_mutex; g_recv_con has a single owner (the KNX
// thread's receive loop) and needs no locking.
static EIBConnection *g_recv_con = NULL;
static EIBConnection *g_send_con = NULL;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

// Set by a failed send (from any thread); checked/cleared by main.c's loop
// so a send-only failure also triggers reconnection, not just recv failures.
static atomic_bool g_connection_broken = false;

/* ---------- address helpers (unchanged from the multicast implementation) ---------- */

uint16_t knx_parse_group_address(const char *addr_str) {
    unsigned int main_g = 0, mid_g = 0, sub_g = 0;
    if (sscanf(addr_str, "%u/%u/%u", &main_g, &mid_g, &sub_g) != 3) {
        if (sscanf(addr_str, "%u/%u", &main_g, &sub_g) == 2) {
            mid_g = 0;
        } else {
            return 0;
        }
    }
    uint16_t addr = 0;
    addr |= (uint16_t)((main_g & 0x1F) << 11);
    addr |= (uint16_t)((mid_g & 0x07) << 8);
    addr |= (uint16_t)(sub_g & 0xFF);
    return addr;
}

void knx_format_group_address(uint16_t addr, char *out, size_t out_len) {
    unsigned int main_g = (addr >> 11) & 0x1F;
    unsigned int mid_g = (addr >> 8) & 0x07;
    unsigned int sub_g = addr & 0xFF;
    snprintf(out, out_len, "%u/%u/%u", main_g, mid_g, sub_g);
}

/* ---------- connection management ---------- */

// Timeout za branje/pisanje po socketu proti knxd. Brez njega se
// EIBOpen_GroupSocket() (rokovanje) ali EIBSendGroup() zatakne za vedno, ce
// knxd ravno ne odgovarja (npr. se ponovno zaganja) - in z njim cela KNX nit
// (ali OPC UA nit, ker drzi mutex). Ob timeoutu knjiznica vrne napako, mi pa
// povezavo zapremo in ponovno vzpostavimo.
#define KNXD_IO_TIMEOUT_SEC 3

static void set_io_timeout(EIBConnection *con) {
    int fd = EIB_Poll_FD(con);
    if (fd < 0) return;
    struct timeval tv;
    tv.tv_sec = KNXD_IO_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static EIBConnection *open_one(const char *url, int write_only, const char *label) {
    EIBConnection *con = EIBSocketURL(url);
    if (!con) {
        printf("[EIBD] Ne morem vzpostaviti %s povezave na knxd (%s)\n", label, url);
        return NULL;
    }
    set_io_timeout(con);
    if (EIBOpen_GroupSocket(con, write_only) == -1) {
        printf("[EIBD] Napaka pri odpiranju %s group socketa\n", label);
        EIBClose(con);
        return NULL;
    }
    return con;
}

int knx_eibd_open(const char *url) {
    atomic_store(&g_connection_broken, false);

    // Send connection first (write_only=1 - we never read from this one,
    // avoids it buffering bus traffic nobody drains).
    g_send_con = open_one(url, 1, "posiljalno");
    if (!g_send_con) return -1;

    // Receive connection (write_only=0 - this is the one that gets bus
    // traffic delivered; its fd is what main.c's select() watches).
    g_recv_con = open_one(url, 0, "sprejemno");
    if (!g_recv_con) {
        EIBClose(g_send_con);
        g_send_con = NULL;
        return -1;
    }

    int fd = EIB_Poll_FD(g_recv_con);
    if (fd < 0) {
        printf("[EIBD] Ne morem pridobiti file descriptorja za select()\n");
        EIBClose(g_send_con); g_send_con = NULL;
        EIBClose(g_recv_con); g_recv_con = NULL;
        return -1;
    }

    printf("[EIBD] Povezan na knxd (%s) - loceni posiljalni in sprejemni kanal.\n", url);
    return fd;
}

void knx_eibd_close(void) {
    pthread_mutex_lock(&g_send_mutex);
    if (g_send_con) { EIBClose(g_send_con); g_send_con = NULL; }
    pthread_mutex_unlock(&g_send_mutex);

    if (g_recv_con) { EIBClose(g_recv_con); g_recv_con = NULL; }
}

/* ---------- send ---------- */

// Builds the raw APDU (TPCI + APCI[+data]) - same short/long form encoding
// our multicast implementation used, but without the KNXnet/IP+cEMI
// wrapper: knxd handles that transport detail for us.
static int send_apdu(uint16_t dst, KnxApci apci, const uint8_t *data, uint8_t data_len, uint8_t short_value) {
    uint8_t apdu[2 + sizeof(((KnxTelegram*)0)->data)];
    uint8_t apci_low2 = (uint8_t)apci & 0x03;
    apdu[0] = 0x00; // TPCI = unnumbered data (group comms)

    int len;
    if (data_len == 0) {
        apdu[1] = (uint8_t)((apci_low2 << 6) | (short_value & 0x3F));
        len = 2;
    } else {
        apdu[1] = (uint8_t)(apci_low2 << 6);
        memcpy(&apdu[2], data, data_len);
        len = 2 + data_len;
    }

    // g_send_con is shared between the KNX thread (queue/poll reads) and
    // the OPC UA server thread (writes) - always locked, never touched
    // without the mutex held.
    pthread_mutex_lock(&g_send_mutex);
    if (!g_send_con) {
        pthread_mutex_unlock(&g_send_mutex);
        return -1;
    }
    int rc = EIBSendGroup(g_send_con, dst, len, apdu);
    pthread_mutex_unlock(&g_send_mutex);

    if (rc < 0) {
        printf("[EIBD] EIBSendGroup napaka za naslov 0x%04X\n", dst);
        atomic_store(&g_connection_broken, true);
        return -1;
    }
    return 0;
}

bool knx_eibd_check_and_clear_broken(void) {
    return atomic_exchange(&g_connection_broken, false);
}

int knx_eibd_send_group_write_small(uint16_t dst, uint8_t value6bit) {
    return send_apdu(dst, KNX_APCI_GROUPVALUE_WRITE, NULL, 0, value6bit);
}

int knx_eibd_send_group_write(uint16_t dst, const uint8_t *data, uint8_t data_len) {
    if (data_len > sizeof(((KnxTelegram*)0)->data)) return -1;
    return send_apdu(dst, KNX_APCI_GROUPVALUE_WRITE, data, data_len, 0);
}

int knx_eibd_send_group_read(uint16_t dst) {
    return send_apdu(dst, KNX_APCI_GROUPVALUE_READ, NULL, 0, 0);
}

/* ---------- receive ---------- */

int knx_eibd_recv(KnxTelegram *out) {
    if (!g_recv_con) return -3;

    uint8_t buf[32];
    eibaddr_t src = 0, dst = 0;

    int len = EIBGetGroup_Src(g_recv_con, sizeof(buf), buf, &src, &dst);
    if (len < 0) {
        printf("[EIBD] Napaka pri branju iz knxd - povezava je verjetno izgubljena.\n");
        return -3;
    }
    if (len < 2) return -1; // premalo bajtov za veljaven group APDU

    uint8_t apci_byte = buf[1];
    uint8_t apci_low2 = (apci_byte >> 6) & 0x03;

    out->src_addr = (uint16_t)src;
    out->dst_addr = (uint16_t)dst;
    out->apci = (KnxApci)apci_low2;

    if (len == 2) {
        out->data_len = 0;
        out->short_value = apci_byte & 0x3F;
    } else {
        size_t dlen = (size_t)len - 2;
        if (dlen > sizeof(out->data)) dlen = sizeof(out->data);
        memcpy(out->data, &buf[2], dlen);
        out->data_len = (uint8_t)dlen;
        out->short_value = 0;
    }

    return 0;
}
