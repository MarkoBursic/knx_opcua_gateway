#include "knxnetip.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define KNXIP_HEADER_LEN 6
#define KNXIP_PROTOCOL_VERSION 0x10
#define SVC_ROUTING_INDICATION 0x0530

#define CEMI_MC_L_DATA_IND 0x29

/* ---------- address helpers ---------- */

uint16_t knx_parse_group_address(const char *addr_str) {
    unsigned int main_g = 0, mid_g = 0, sub_g = 0;
    if (sscanf(addr_str, "%u/%u/%u", &main_g, &mid_g, &sub_g) != 3) {
        // Some installs use 2-level addressing (main/sub) - fall back.
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

uint16_t knx_parse_individual_address(const char *addr_str) {
    unsigned int area = 0, line = 0, device = 0;
    if (sscanf(addr_str, "%u.%u.%u", &area, &line, &device) != 3) return 0;
    uint16_t addr = 0;
    addr |= (uint16_t)((area & 0x0F) << 12);
    addr |= (uint16_t)((line & 0x0F) << 8);
    addr |= (uint16_t)(device & 0xFF);
    return addr;
}

/* ---------- socket setup ---------- */

int knx_routing_open(const char *local_iface_ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("[KNXIP] socket()");
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(KNX_ROUTING_PORT);

    if (bind(fd, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("[KNXIP] bind()");
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr(KNX_ROUTING_MULTICAST_ADDR);
    if (local_iface_ip && local_iface_ip[0] != '\0') {
        mreq.imr_interface.s_addr = inet_addr(local_iface_ip);
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }

    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("[KNXIP] IP_ADD_MEMBERSHIP (join multicast group)");
        close(fd);
        return -1;
    }

    // We reflect our own state changes back into the OPC UA side via the
    // normal receive path's echo-suppression (raw-value comparison), so we
    // don't need the kernel to loop our own multicast packets back to us -
    // avoids a redundant parse of every frame we just sent.
    unsigned char loop = 0;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    // Non-blocking: main.c drains several frames per select() wake-up, and
    // must not stall in recvfrom() waiting for a frame that isn't there yet.
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (local_iface_ip && local_iface_ip[0] != '\0') {
        struct in_addr iface_addr;
        iface_addr.s_addr = inet_addr(local_iface_ip);
        setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr));
    }

    printf("[KNXIP] Joined KNXnet/IP routing multicast group %s:%d\n",
           KNX_ROUTING_MULTICAST_ADDR, KNX_ROUTING_PORT);
    return fd;
}

void knx_routing_close(int fd) {
    if (fd >= 0) close(fd);
}

/* ---------- frame encode ---------- */

// Builds the KNXnet/IP header + cEMI L_Data.ind frame for a GroupValue
// service and sends it to the routing multicast group. Returns 0 on
// success, -1 on error.
static int send_cemi_group_frame(int fd, uint16_t src, uint16_t dst,
                                  KnxApci apci, const uint8_t *data, uint8_t data_len,
                                  uint8_t short_value) {
    uint8_t buf[KNXIP_HEADER_LEN + 8 /*cemi fixed part*/ + sizeof(((KnxTelegram*)0)->data)];
    size_t off = 0;

    // NPDU length: TPCI byte + APCI byte (+ optional data bytes) - 1
    uint8_t npdu_len;
    if (data_len == 0) {
        npdu_len = 2 - 1; // short form: TPCI + APCI/value byte
    } else {
        npdu_len = (uint8_t)(2 + data_len - 1); // long form: TPCI + APCI + data bytes
    }

    uint8_t cemi_len = 2 /*ctrl*/ + 2 /*src*/ + 2 /*dst*/ + 1 /*len*/ + 2 /*tpci+apci*/ + data_len;
    cemi_len += 2; // message code + add-info-length byte

    // -- KNXnet/IP header --
    buf[off++] = KNXIP_HEADER_LEN;
    buf[off++] = KNXIP_PROTOCOL_VERSION;
    buf[off++] = (SVC_ROUTING_INDICATION >> 8) & 0xFF;
    buf[off++] = SVC_ROUTING_INDICATION & 0xFF;
    uint16_t total_len = KNXIP_HEADER_LEN + cemi_len;
    buf[off++] = (total_len >> 8) & 0xFF;
    buf[off++] = total_len & 0xFF;

    // -- cEMI --
    buf[off++] = CEMI_MC_L_DATA_IND;
    buf[off++] = 0x00; // additional info length = 0
    buf[off++] = 0xBC; // control field 1: standard frame, no repeat, low priority... (see header note)
    buf[off++] = 0xE0; // control field 2: group address, hop count 6
    buf[off++] = (src >> 8) & 0xFF;
    buf[off++] = src & 0xFF;
    buf[off++] = (dst >> 8) & 0xFF;
    buf[off++] = dst & 0xFF;
    buf[off++] = npdu_len;
    buf[off++] = 0x00; // TPCI = unnumbered data (group comms)

    uint8_t apci_low2 = (uint8_t)apci & 0x03;
    if (data_len == 0) {
        buf[off++] = (uint8_t)((apci_low2 << 6) | (short_value & 0x3F));
    } else {
        buf[off++] = (uint8_t)(apci_low2 << 6);
        memcpy(&buf[off], data, data_len);
        off += data_len;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(KNX_ROUTING_PORT);
    dest_addr.sin_addr.s_addr = inet_addr(KNX_ROUTING_MULTICAST_ADDR);

    ssize_t sent = sendto(fd, buf, off, 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    if (sent < 0) {
        perror("[KNXIP] sendto()");
        return -1;
    }
    return 0;
}

int knx_send_group_write_small(int fd, uint16_t src, uint16_t dst, uint8_t value6bit) {
    return send_cemi_group_frame(fd, src, dst, KNX_APCI_GROUPVALUE_WRITE, NULL, 0, value6bit);
}

int knx_send_group_write(int fd, uint16_t src, uint16_t dst, const uint8_t *data, uint8_t data_len) {
    if (data_len > sizeof(((KnxTelegram*)0)->data)) return -1;
    return send_cemi_group_frame(fd, src, dst, KNX_APCI_GROUPVALUE_WRITE, data, data_len, 0);
}

int knx_send_group_read(int fd, uint16_t src, uint16_t dst) {
    return send_cemi_group_frame(fd, src, dst, KNX_APCI_GROUPVALUE_READ, NULL, 0, 0);
}

/* ---------- frame decode ---------- */

int knx_routing_recv(int fd, KnxTelegram *out) {
    uint8_t buf[256];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr*)&from_addr, &from_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; // nothing waiting right now - not an error
        return -3; // real socket failure (interface down, etc.)
    }
    if (n < KNXIP_HEADER_LEN + 10) return -1; // received something, but too short to be a real frame

    if (buf[0] != KNXIP_HEADER_LEN) return -1;
    uint16_t service = (uint16_t)((buf[2] << 8) | buf[3]);
    if (service != SVC_ROUTING_INDICATION) return -1; // ignore search/connect traffic etc.

    const uint8_t *cemi = &buf[KNXIP_HEADER_LEN];
    size_t cemi_len = (size_t)n - KNXIP_HEADER_LEN;
    if (cemi_len < 10) return -1;

    if (cemi[0] != CEMI_MC_L_DATA_IND) return -1; // not a bus indication frame

    uint8_t add_info_len = cemi[1];
    const uint8_t *p = &cemi[2 + add_info_len]; // skip ctrl1/ctrl2 start
    if ((size_t)(p - cemi) + 8 > cemi_len) return -1;

    // p[0]=ctrl1, p[1]=ctrl2, p[2..3]=src, p[4..5]=dst, p[6]=len, p[7..]=tpci/apci/data
    uint16_t src = (uint16_t)((p[2] << 8) | p[3]);
    uint16_t dst = (uint16_t)((p[4] << 8) | p[5]);
    uint8_t npdu_len = p[6]; // = (TPCI+APCI[+data]) - 1

    const uint8_t *tpci_apci = &p[7];
    size_t remaining = cemi_len - (size_t)(tpci_apci - cemi);
    size_t npdu_bytes = (size_t)npdu_len + 1;
    if (npdu_bytes < 2 || npdu_bytes > remaining) return -1;

    uint8_t apci_byte = tpci_apci[1];
    uint8_t apci_low2 = (apci_byte >> 6) & 0x03;
    // Combined with the (always-zero, for GroupValue services) high bits from
    // tpci_apci[0], apci_low2 alone is enough to distinguish read/response/write.
    KnxApci apci = (KnxApci)apci_low2;

    out->src_addr = src;
    out->dst_addr = dst;
    out->apci = apci;

    if (npdu_bytes == 2) {
        // short form: 6-bit value packed into the APCI byte
        out->data_len = 0;
        out->short_value = apci_byte & 0x3F;
    } else {
        size_t data_len = npdu_bytes - 2;
        if (data_len > sizeof(out->data)) data_len = sizeof(out->data);
        memcpy(out->data, &tpci_apci[2], data_len);
        out->data_len = (uint8_t)data_len;
        out->short_value = 0;
    }

    return 0;
}
