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
- `[WATCHDOG]` is printed when 10+ questions in a row stay unanswered for 5+ minutes
  (thresholds at the top of `main.c`). Only devices that have answered a read before are
  counted, so disconnected devices that never answer cannot trigger it; until any device
  has answered since startup, every question counts (catches a router that is dead from the
  start). It also shows when the last telegram of any kind came from the bus. If telegrams
  keep arriving but no read is answered, the KNX IP router usually needs a restart.
- Load on the bus is tunable at the top of `main.c`: the startup queue reads one
  datapoint every 500 ms, smart-poll every 2 s, and a silent datapoint is only
  re-read after 30 minutes. Datapoints that never answered during the startup
  queue (3/3 reads) are retried after 2 hours.
## Console log legend

Every question the gateway sends is logged, and every answer is tied back to it:

| Prefix | Meaning |
|---|---|
| `[KNX-INI]` | our question at startup / after a long outage (`n/3` attempts) |
| `[KNX-POL]` | our question to a device that sent nothing for 30 min (2 h if it never answered) |
| `[KNX-RSP]` | answer to OUR question (matched by group address within 10 s): decoded value, raw bytes, `[spremenjeno]` / `[nespremenjeno]` |
| `[KNX-SPY]` | telegram from a device in `config.json` that we did not ask for (it sent a value itself, or answered someone else's question) |
| `[EXT-QRY]` | a question sent by ANOTHER device on the bus (e.g. SCADA); set `LOG_EXT_QUERIES` to 0 in `knx_handler.c` to silence it |
| `[KNX-DBG]` | telegram for a group address that is not in `config.json`: raw values only, never written to OPC UA |
| `[KNX-SND]` | our write to the KNX bus (an OPC UA client changed a value) |

For `KNX-RSP` and `KNX-SPY` the OPC UA node gets the value and a fresh source timestamp even
when the value is unchanged, and the refresh timer of that datapoint is reset.
If no telegram arrives for the poll interval, a `KNX-POL` question is sent and the normal
outcome is a `KNX-RSP` line right after it (or `[KNX-POL] Brez odgovora` after 10 s).
- `SIGTERM`/`SIGINT` stop the gateway cleanly; `SIGPIPE` is ignored.

## OPC UA security

Design (agreed with the operator): every client logs in with a user name and password (stored as a
hash in `config.json`), the channel is encrypted, encrypted clients must be listed in a trust folder
(strict), all logged-in users may read and write, and an old SCADA can be let in through an optional
unencrypted endpoint.

Build requirements: open62541 built with encryption (`UA_ENABLE_ENCRYPTION`, mbedTLS or OpenSSL) and
`libcrypt-dev` (the Makefile links `-lcrypt`). `opc_security.c` follows the open62541 1.4 API.

Setup (once):

1. Server certificate and key (DER), plus the trust folder:
   ```
   tools/make_pki.sh <hostname> <ip-of-this-pc>
   ```
   The certificate's application URI (`urn:knx-opcua-gateway` by default) must equal `pki.application_uri`
   in `config.json`.
2. A password hash per user (prompts for the password, so it stays out of the shell history):
   ```
   openssl passwd -6
   ```
3. Add the keys from `config.security.example.json` to the top level of your `config.json`
   (`allow_anonymous`, `require_encryption`, `users`, `pki`).
4. Trust each client certificate before its first connection (strict mode rejects unknown ones):
   ```
   tools/trust_client.sh <client-certificate.der|pem>
   ```
   The script prints subject and SHA-256 fingerprint - compare them with the client before keeping the
   file. UaExpert keeps its certificate in its own PKI folder (Settings > Manage Certificates).
5. `make clean && make`, restart the gateway. The server reads certificates only at startup.

Behaviour worth knowing:
- **Fails closed.** The gateway refuses to start if there is no user and anonymous access is off, if a
  `password_hash` is not a crypt(3) hash (plain text is rejected), if the certificate or key is missing,
  or if the trust folder has no `.der` file (open62541 would otherwise accept every client certificate).
- **Weak policies are removed:** Basic128Rsa15 and Basic256 (SHA-1) endpoints are dropped.
- **Lockout:** 5 wrong passwords in a row lock that user for 60 s (constants in `opc_security.c`).
  Logins are logged as `[SECURITY]` lines; passwords are never logged.
- **Old SCADA:** set `"require_encryption": false` to keep an additional endpoint without encryption.
  A user name and password are still required, and the password is encrypted with the server certificate
  inside the login token. Whether the SCADA's OPC UA client supports that is not known - test it. Restrict
  port 4840 with a firewall (for example `sudo ufw allow from 10.80.32.0/24 to any port 4840 proto tcp`),
  because the unencrypted endpoint shares the port with the encrypted ones.
- **Service:** when running under systemd, put `pki/` next to `config.json` in the working directory.
- `config.json` and `pki/server_key.der` must be readable only by the service user (`chmod 600`).
