# iface-auto — internals

A C++ reimplementation of upstream Reticulum's `AutoInterface`
(`RNS/Interfaces/AutoInterface.py`), wire-compatible with it. The protocol is
upstream's; the mechanism differs (BSD sockets, a split rx task, ITS to rnsd)
to fit the single-wait-point task model.

## §1 — What this interface adds

Relative to a bare upstream AutoInterface, this straddle provides:

- **A two-task implementation** (`auto` + `auto-rx`) over BSD sockets rather
  than upstream's threaded Python `socketserver`. The rx task isolates the only
  blocking call so the main task keeps a single wait point.
- **rnsd integration.** Self-registration on `RNSD_PORT_IFACE` as interface
  `auto`, the outbound drain (rnsd → peers) and inbound forward (peers → rnsd)
  over ITS, and re-registration if the handle drops.
- **Per-interface IFAC.** `s.auto.ifac_netname` / `secrets.auto.ifac_netkey` /
  `s.auto.ifac_size` are forwarded in the registration payload; the crypto core
  lives in rnsd / microreticulum (see [rns](../rns/INTERNALS.md)), not here.
- **Interface mode.** `s.auto.mode` maps to the rnsd interface-mode enum
  (`full`/`gateway`/`access_point`/`roaming`/`boundary`), driving `fwd`.
- **Live telemetry + a CLI** — `auto.state`/`auto.up`/`auto.peers`/`auto.addr`/
  `auto.group_addr`/`auto.stats.*`, and the `auto` / `auto up|down` /
  `auto peers` commands.
- **Dynamic reconfiguration.** A storage-change subscription on `s.auto` and
  `secrets.auto` re-applies group, mode, and IFAC by tearing the interface down
  and bringing it back up.
- **WiFi-gated lifecycle.** Brings the netif's IPv6 link-local address up via
  `esp_netif`, waits out DAD, picks STA or AP netif, and parks/rebuilds on WiFi
  up/down.

## Two tasks

- **`auto`** — the interface task. Owns the peer table, group state, all
  sockets' lifecycle, registration with rnsd, and every `sendto()` (announces,
  reverse-peering, and data fan-out). Core 0, priority 2, 6 KB PSRAM stack —
  pinned alongside net and rnsd so the ITS hops between them stay on-core.
- **`auto-rx`** — a small helper that `select()`s the three UDP sockets, copies
  each datagram into a PSRAM queue, and notifies `auto`. Core 0, priority 2,
  4 KB PSRAM stack.

### Why two tasks

lwIP **core locking is off** in this build (`CONFIG_LWIP_TCPIP_CORE_LOCKING`
unset), so the raw lwIP API can only be driven from the tcpip thread. BSD
sockets are the only task-safe option, and they offer only blocking `recvfrom`
and `select()`. Running both wait points — the ITS poll and the UDP select — in
one task would break the single-wait-point rule. Splitting `select()` into
`auto-rx` keeps `auto` on one wait point (`itsPoll(nextDeadline())`) and uses
the queue + task-notify as the wakeup channel for datagrams — the same
recv-on-another-context → queue → notify shape the ESP-NOW interface uses.

BSD sockets also give us IPv6 multicast join (`IPV6_ADD_MEMBERSHIP`), zoned
link-local sends (`sin6_scope_id`), and `inet_ntop` address text in one place.
rnsd itself includes no networking headers.

## Protocol (interoperable with upstream)

All values are the AutoInterface defaults; only the **group name** is
configurable (`s.auto.group`, default `"reticulum"`).

- **Group address.** `group_hash = SHA-256(group_name)`. The IPv6 multicast
  discovery address is `ff{flags}{scope}:0:` followed by six hextets of
  `group_hash[2..13]`; with the default temporary(1) / link-local(2) nibbles
  that is `ff12:0:…`. Every node joins it on the active WiFi netif. Different
  group names → different addresses → separate networks.
- **Peering token.** `SHA-256(group_name || link_local_addr_text)`. Each node
  multicasts its token to the group on **port 29716** every 1.6 s. A node that
  receives a token from source `S` recomputes `SHA-256(group_name || text(S))`;
  a match adds/refreshes `S` as a peer. A token from our own address is the
  multicast echo and is ignored.
- **Reverse peering.** A node also unicasts its token to known peers' **port
  29717** every ~5.2 s, so asymmetric multicast still peers both directions. We
  listen there and treat it exactly like a multicast token.
- **Data.** An outbound RNS packet is unicast as one UDP datagram to every live
  peer on **port 42671**; an inbound datagram from a known peer is forwarded
  verbatim to rnsd. One datagram is one RNS packet — no on-air framing.
- **Peer expiry.** A peer not heard from in 22 s is dropped.
- **Peer cap.** The peer table holds at most 16 peers (PSRAM-resident);
  tokens from further nodes are ignored once it is full.

The token is hashed over the *text* form of the IPv6 address, so the device's
`inet_ntop` output must match the peer's. lwIP and CPython both emit RFC 5952
(lowercase, compressed, no scope suffix), so they agree.

## Registration

`auto` registers as a single interface (peers are not individual interfaces from
rnsd's point of view — this matches upstream AutoInterface and lets the path
table treat the group as one neighbour): `mtu = 500` (mR base MTU; links stay at
500, `LINK_MTU_DISCOVERY` off), `bitrate = 10 Mbit/s` (AutoInterface's guess),
mode from `s.auto.mode`, `in = out = 1`, and `fwd = 1` for `gateway`/`full`. It
presents a broadcast medium: one interface whose outbound packets fan out as
unicast datagrams to all peers.

## Task topology

```
        ┌──────────────── auto task ─────────────────┐
        │  peer table · announce/peer-job timers ·    │
        │  all sendto() · all ITS                     │
        │                                             │
 rnsd ◄─┤ s_rnsdHandle (RNSD_PORT_IFACE, "auto")      │
        │        ▲                    │               │
        │   itsRecv (outbound)   itsSend (inbound)    │
        │        │                    ▼               │
        │   drainOutbound        drainRx ◄── s_rxQueue (PSRAM)
        └────────┼─────────────────────▲──────────────┘
                 │ sendto()             │ xTaskNotifyGive
        discovery / unicast / data sockets
                 ▲                      │
                 └──── auto-rx task: select() + recvfrom() ──┘
```

The auto task's single wait point is `itsPoll(nextDeadline())`. It wakes on:
rnsd ITS traffic (outbound packets), a notify from `auto-rx` (a datagram
arrived), a storage-subscription change (`s.auto.*` / `secrets.auto.*`), or the
computed announce / peer-job deadline (capped at 1 s so stats publish at ≥1 Hz).
`recvfrom` blocks only inside `auto-rx`, which `select()`s with a 1 s cap so it
re-reads the socket set after the auto task opens or closes them.

## Lifecycle

The task waits for the `rns.ready` flag (bounded 120 s; it exits if rnsd never
comes up — there is no point running without rnsd). It then calls `netUp()` to
ask net to bring WiFi up, waits for a valid clock, and enters the loop.

Bring-up is gated on `netIsUp()`. On enable + WiFi up it brings the netif's IPv6
link-local address up with `esp_netif_create_ip6_linklocal`, retrying on the
deadline loop until DAD assigns it (`auto.state = waiting_addr`), then opens the
sockets, registers with rnsd, and starts announcing (`auto.state = up`). On WiFi
down, group change, mode change, IFAC change, or disable it tears the sockets +
registration down; `auto-rx` parks itself once the sockets become `-1`.

`applyConfig()` re-reads all keys on any `s.auto`/`secrets.auto` change. A change
to group, mode, or IFAC while running tears down and rebuilds so the new
settings take effect; a group change recomputes the multicast address.

## Pitfalls

- **lwIP core locking is off**, so never call the raw lwIP API from these tasks
  — only BSD sockets are task-safe. This is the whole reason for the `auto-rx`
  split; don't collapse the two tasks back into one.
- **Token text must be RFC 5952.** The peering token hashes the *text* IPv6
  address; if `inet_ntop` ever emitted a scope suffix or non-canonical form,
  peering with CPython nodes would silently fail.
- **Sockets are owned by the auto task**, opened/closed only there; `auto-rx`
  reads the `volatile` socket fds and drops them to `-1` on the next loop. Don't
  close a socket from the rx task.
- **rnsd must be up first.** `requires: reticulous/rns` topo-orders rnsd ahead of
  this interface so `RNSD_PORT_IFACE` exists before registration.
