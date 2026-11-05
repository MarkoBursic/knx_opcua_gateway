#ifndef KNX_EIBD_H
#define KNX_EIBD_H

/*
 * KNX transport layer backed by knxd, via libeibclient.
 *
 * This replaces the earlier hand-rolled KNXnet/IP Routing (multicast) code.
 * We connect to knxd's local socket and let it own the actual link to the
 * KNX installation (Tunneling to the n146, in this setup) - knxd already
 * implements the protocol-level ACK/retry that bare multicast Routing
 * lacks, which is what we were missing frames on.
 *
 * knxd must be running for this to work: see /etc/knxd.ini - the [B.ipt]
 * section pointing at the KNX IP router's address is what actually reaches
 * the bus; this module just talks to knxd itself over its local socket.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    KNX_APCI_GROUPVALUE_READ = 0x00,
    KNX_APCI_GROUPVALUE_RESPONSE = 0x01,
    KNX_APCI_GROUPVALUE_WRITE = 0x02
} KnxApci;

typedef struct {
    uint16_t src_addr;      // KNX individual address of sender (assigned by knxd)
    uint16_t dst_addr;      // KNX group address
    KnxApci   apci;
    uint8_t   data[14];     // payload bytes (long form) - unused for short form
    uint8_t   data_len;     // number of valid bytes in `data` (0 for short/6-bit form)
    uint8_t   short_value;  // 6-bit packed value, valid when data_len == 0
} KnxTelegram;

// Address helpers - unchanged from the multicast implementation, still
// 3-level "main/middle/sub" group address notation.
uint16_t knx_parse_group_address(const char *addr_str);
void knx_format_group_address(uint16_t addr, char *out, size_t out_len);

// Connects to knxd for BOTH directions this process needs:
//  - a dedicated receive connection, used only by the KNX thread's
//    continuous EIBGetGroup_Src() loop (no locking needed - single owner)
//  - a dedicated send connection, used by BOTH the KNX thread (queue/poll
//    reads) and the OPC UA server thread (writes from onWriteKNX) - this
//    one is protected by a mutex, since libeibclient's EIBConnection is
//    not documented as thread-safe and sharing one handle across threads
//    without this caused writes to silently fail.
// Returns a pollable fd for the RECEIVE connection (for select()), or -1
// if either connection failed to open.
int knx_eibd_open(const char *url);
void knx_eibd_close(void);

// Sends a GroupValueWrite. Use the *_small variant for DPTs that fit in
// 6 bits (DPT1/2/3). Use the general variant with a data buffer for
// everything else (DPT4/5/6/7/8/9/12/13/14 ...).
int knx_eibd_send_group_write_small(uint16_t dst, uint8_t value6bit);
int knx_eibd_send_group_write(uint16_t dst, const uint8_t *data, uint8_t data_len);
int knx_eibd_send_group_read(uint16_t dst);

// Call after select() indicates the fd from knx_eibd_open() is readable.
// EIBGetGroup_Src() is a blocking library call, but only blocks for the
// remainder of the message already in flight (guaranteed to complete
// quickly over the local socket) - it is NOT the same hazard as blocking
// on an arbitrary future network event, so call this exactly once per
// select() wake-up, not in a speculative drain loop.
// Returns 0 and fills *out on a valid GroupValue telegram.
// Returns -1 if a frame was received but isn't one we track (too short,
// non-GroupValue APCI).
// Returns -3 on a real connection failure - reconnect needed.
int knx_eibd_recv(KnxTelegram *out);

// Call from the main thread's loop, once per iteration, in addition to
// normal receive-side failure detection. A send failure (from a different
// thread) doesn't otherwise get noticed until the receive side happens to
// also fail - this lets a send-only failure trigger the same reconnect
// path instead of leaving a stale connection indefinitely. Returns true
// (and clears the flag) if a send failure was flagged since the last check.
bool knx_eibd_check_and_clear_broken(void);

#endif // KNX_EIBD_H
