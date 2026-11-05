#ifndef KNXNETIP_H
#define KNXNETIP_H

/*
 * Minimal KNXnet/IP Routing client.
 *
 * This talks the *Routing* variant of KNXnet/IP (multicast UDP,
 * 224.0.23.12:3671), not Tunneling. In Routing mode there is no
 * connect/heartbeat/ack state machine: every participant (real KNX IP
 * routers, and now this gateway) simply sends cEMI L_Data.ind frames onto
 * the multicast group and everyone else treats it as bus traffic.
 *
 * IMPORTANT before using this against a live installation:
 *  - Your network must actually pass multicast traffic between this host
 *    and your KNX IP router (some managed switches/routers block it by
 *    default - if you never see any incoming traffic, that's the first
 *    thing to check).
 *  - `gateway_individual_address` in config.json must be a KNX individual
 *    address that is NOT assigned to any real device on your installation
 *    (reserve one via ETS, e.g. in the 15.15.x "tool" range).
 *  - Test with a bus/group monitor (ETS group monitor, or a KNXnet/IP
 *    capable Wireshark dissector) before trusting writes against real
 *    equipment - this gateway can be pointed at HVAC valves and blinds.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define KNX_ROUTING_MULTICAST_ADDR "224.0.23.12"
#define KNX_ROUTING_PORT 3671

typedef enum {
    KNX_APCI_GROUPVALUE_READ = 0x00,
    KNX_APCI_GROUPVALUE_RESPONSE = 0x01,
    KNX_APCI_GROUPVALUE_WRITE = 0x02
} KnxApci;

typedef struct {
    uint16_t src_addr;      // KNX individual address of sender
    uint16_t dst_addr;      // KNX group address
    KnxApci   apci;
    uint8_t   data[14];     // payload bytes (long form) - unused for short form
    uint8_t   data_len;     // number of valid bytes in `data` (0 for short/6-bit form)
    uint8_t   short_value;  // 6-bit packed value, valid when data_len == 0
} KnxTelegram;

// Opens a UDP socket bound to the KNXnet/IP routing port and joins the
// standard KNX multicast group. `local_iface_ip` may be NULL/"" to let the
// OS pick the interface. Returns a pollable fd, or -1 on error.
int knx_routing_open(const char *local_iface_ip);
void knx_routing_close(int fd);

// Address helpers. Group addresses use 3-level "main/middle/sub" notation
// (e.g. "3/0/12"); individual addresses use "area.line.device" (e.g. "15.15.250").
uint16_t knx_parse_group_address(const char *addr_str);
void knx_format_group_address(uint16_t addr, char *out, size_t out_len);
uint16_t knx_parse_individual_address(const char *addr_str);

// Sends a GroupValueWrite. Use the *_small variant for DPTs that fit in
// 6 bits (DPT1/2/3 - switches, up/down, dimming steps, etc, value 0-63).
// Use the general variant with a data buffer for everything else
// (DPT4/5/6/7/8/9/12/13/14 ...).
int knx_send_group_write_small(int fd, uint16_t src, uint16_t dst, uint8_t value6bit);
int knx_send_group_write(int fd, uint16_t src, uint16_t dst, const uint8_t *data, uint8_t data_len);
int knx_send_group_read(int fd, uint16_t src, uint16_t dst);

// Blocking-safe (only call after select()/poll() says fd is readable).
// Returns 0 and fills *out on a valid GroupValue telegram.
// Returns -1 if a frame was received but isn't one we care about (parse
// error, wrong service type, non-GroupValue APCI) - there may still be more
// frames queued, keep draining.
// Returns -2 if nothing is waiting right now (EAGAIN) - stop draining, this
// is not an error.
// Returns -3 on a real socket failure (interface down, etc.) - treat as
// connection lost.
int knx_routing_recv(int fd, KnxTelegram *out);

#endif // KNXNETIP_H
