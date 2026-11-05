# KNX ↔ OPC UA Gateway — knxd/libeibclient rewrite

## What changed (second rewrite)

The gateway went through two transport rewrites:

1. `knxtool`/`popen()` subprocess calls → hand-rolled KNXnet/IP Routing
   (raw multicast UDP, no `knxd` dependency)
2. Hand-rolled multicast Routing → **`knxd`, via `libeibclient`** (this version)

Step 2 happened because Routing mode is bare fire-and-forget UDP multicast
with zero delivery guarantees, and real-world testing on this installation
showed the KNX IP router (a Siemens n146) dropping frames under load
(confirmed by its own `ROUTING_LOST_MESSAGE` telemetry). `knxd`'s Tunneling
connection to the same router has protocol-level ACK + retry built in,
which is what was actually needed.

New/changed files:
- `knx_eibd.h`, `knx_eibd.c` — connects to `knxd` via `libeibclient`
  (`EIBSocketURL`, `EIBOpen_GroupSocket`, `EIBSendGroup`, `EIBGetGroup_Src`).
  Replaces `knxnetip.h`/`knxnetip.c` entirely — no more hand-built
  KNXnet/IP+cEMI framing, no multicast socket, no port-3671 conflicts.
- `knx_handler.c` — same DPT1/4/5/8/9 encode/decode logic as before, now
  calling into `knx_eibd_*` instead of `knxnetip_*`. No more
  `gateway_individual_address` handling — `knxd` assigns addresses from
  its own configured pool (`client-addrs` in `knxd.ini`).
- `config_loader.c` — `knx_connection` section now just needs `eibd_url`
  (was `multicast_interface`/`gateway_individual_address`).
- `gateway.h` — `KnxConnectionConfig` simplified to a single URL string.
- `main.c` — the receive path no longer needs the earlier 32-iteration
  "drain" loop. `EIBGetGroup_Src()` only blocks for a message already in
  flight over the local socket (not an arbitrary future network event
  like raw multicast `recvfrom()` was), so one call per `select()`
  wake-up is correct and safe.
- `Makefile` — links `vendor/eibclient/lib/libeibclient.a` statically, adds
  `-Ivendor/eibclient/include`. No more raw-socket-only build.

## Required one-time setup

**1. Vendor the eibclient library into the project** (so this builds on
the target platform without needing `knxd` built from source there too):

```
mkdir -p vendor/eibclient/include vendor/eibclient/lib
cp /path/to/knxd/src/include/eibclient.h vendor/eibclient/include/
cp /path/to/knxd/src/include/eibtypes.h vendor/eibclient/include/
cp /path/to/knxd/debian/knxd-dev/usr/include/eibloadresult.h vendor/eibclient/include/
cp /path/to/knxd/src/client/c/.libs/libeibclient.a vendor/eibclient/lib/
```

(On this machine those paths were under `/home/user/knxd/...` — a source
build of `knxd`. The `debian/knxd-dev/usr/include/` directory is the
easiest single source for all three headers already resolved together.)

**2. `knxd` must be running and correctly configured.** It needs:
- Its `[B.ipt]` backend pointing at the KNX IP router (`ip-address = ...`
  in `knxd.ini`) — this is what actually gets Tunneling's ACK/retry.
- `knxd.socket` (Unix socket `/run/knx`) active — this is what
  `eibd_url = "local:/run/knx"` connects to.

If you ran into the port-3671 conflict during the earlier multicast
version and masked `knxd`, **unmask and re-enable it now** — this version
depends on it being always running:

```
sudo systemctl unmask knxd.service knxd.socket knxd-net.socket
sudo systemctl enable --now knxd.socket knxd-net.socket
```

Since this gateway no longer opens any multicast socket of its own,
there's nothing left to conflict with `knxd` on port 3671 regardless of
whether `knxd` keeps its `server = ets_router` mode for other clients
(ETS, monitoring tools, etc.) — that decision is now independent of this
gateway's operation.

## config.json — knx_connection section

```json
{
  "knx_connection": {
    "eibd_url": "local:/run/knx"
  }
}
```

- `"local:/run/knx"` — Unix socket (matches `knxd.socket`'s `Listen: /run/knx`).
  Preferred: lower overhead, and this is what we've tested against.
- `"ip:127.0.0.1:6720"` — TCP, if you ever need to reach `knxd` from a
  different host (matches `knxd.socket`'s `Listen: [::]:6720`).

If the section or field is missing, `"local:/run/knx"` is used by default.

## Build

```
make clean && make
```

Requires `vendor/eibclient/` to be populated first (see above), plus the
existing `-lopen62541 -ljson-c` dependencies unchanged from before.

## Running unattended (standalone)

The gateway is meant to run as a service. It needs `knxd` running on the same
machine (it talks to it over `/run/knx`), so on the target platform install and
enable both.

1. Never let the machine sleep. A suspended PC cannot keep the KNX tunnel
   alive, the router drops it, and `knxd` terminates with "Link down":

   ```
   sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
   ```

2. Let `knxd` keep retrying forever (`sudo systemctl edit knxd.service`):

   ```
   [Unit]
   StartLimitIntervalSec=0
   [Service]
   Restart=always
   RestartSec=10
   ```

3. Install the gateway as a service:

   ```
   sudo mkdir -p /opt/knx_opcua_gateway
   sudo cp knx_opcua_gateway config.json /opt/knx_opcua_gateway/
   sudo cp knx-opcua-gateway.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now knx-opcua-gateway
   journalctl -u knx-opcua-gateway -f
   ```

4. Optional: cap the journal size (`SystemMaxUse=500M` in `/etc/systemd/journald.conf`).

Behaviour worth knowing:
- After a reconnect to `knxd` that followed an outage of 60+ seconds (`RESCAN_MIN_OUTAGE_SEC` in
  `main.c`), the gateway re-reads all datapoints. Shorter blips just continue where they left off,
  so a flapping link does not trigger a full scan every few seconds.
- All socket I/O to `knxd` has a 3 s timeout, so a restarting `knxd` can never freeze the gateway.
- `[WATCHDOG]` is printed when 30+ reads in a row go unanswered for 5+ minutes
  (thresholds at the top of `main.c`). It also shows when the last telegram of
  any kind came from the bus. If telegrams keep arriving but no read is ever
  answered, the KNX IP router usually needs a restart.
- Load on the bus is tunable at the top of `main.c`: the startup queue reads one
  datapoint every 500 ms, smart-poll every 2 s, and a silent datapoint is only
  re-read after 30 minutes. Datapoints that never answered during the startup
  queue (3/3 reads) are retried after 2 hours.
## Console log legend

Every question the gateway sends is logged, and every answer is tied back to it:

| Prefix | Meaning |
|---|---|
| `[KNX-INIT]` | our question at startup / after a long outage (`n/3` attempts) |
| `[KNX-POLL]` | our question to a device that sent nothing for 30 min (2 h if it never answered) |
| `[KNX-RESP]` | answer to OUR question (matched by group address within 10 s): decoded value, raw bytes, `[spremenjeno]` / `[nespremenjeno]` |
| `[KNX-SNIFF]` | telegram from a device in `config.json` that we did not ask for (it sent a value itself, or answered someone else's question) |
| `[EXT-QRY]` | a question sent by ANOTHER device on the bus (e.g. SCADA); set `LOG_EXT_QUERIES` to 0 in `knx_handler.c` to silence it |
| `[KNX-DEBUG]` | telegram for a group address that is not in `config.json`: raw values only, never written to OPC UA |

For `KNX-RESP` and `KNX-SNIFF` the OPC UA node gets the value and a fresh source timestamp even
when the value is unchanged, and the refresh timer of that datapoint is reset.
If no telegram arrives for the poll interval, a `KNX-POLL` question is sent and the normal
outcome is a `KNX-RESP` line right after it (or `[KNX-POLL] Brez odgovora` after 10 s).
- `SIGTERM`/`SIGINT` stop the gateway cleanly; `SIGPIPE` is ignored.
