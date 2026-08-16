# iface-auto

## What is this?

**iface-auto** is the AutoInterface for [rns](../rns): zero-configuration RNS
over a local WiFi link. One task finds every reachable Reticulum node on the
same link automatically — there is no host or port to configure — and carries
RNS packets between them over UDP. It is wire-compatible with upstream
Reticulum's `AutoInterface`, so a spangap device and a desktop Reticulum node
on the same LAN peer with nothing configured on either side.

## Origins

It is a from-scratch C++ reimplementation of upstream Reticulum's
`AutoInterface` (`RNS/Interfaces/AutoInterface.py`), reproducing that
interface's group-address derivation, peering-token exchange, and well-known
ports byte-for-byte. See [INTERNALS.md](INTERNALS.md) for the protocol details
and the points where the device implementation differs in mechanism (BSD
sockets, a split rx task) while staying wire-identical.

## What it does

The interface runs as two tasks (`auto` and a small `auto-rx` helper) and
self-registers with **rnsd** as a single interface named `auto`. It needs an IP
network, so it asks **spangap-net** to bring WiFi up — but **only when enabled**:
the `netUp()` request goes through the enable-driven `applyConfig` reconcile
(now-at-boot and on `s.auto` changes), not an unconditional boot call, so a
disabled interface never powers the radio. It then stays active while
`netIsUp()`. Peer discovery uses IPv6 link-local multicast; data uses unicast
UDP to each discovered peer. From rnsd's point of view it presents one
broadcast-style interface whose outbound packets fan out as a unicast datagram
to every live peer — the same shape as the LoRa and ESP-NOW interfaces.

It starts automatically when the straddle is in the build; there is nothing to
call. It is **off by default** (`s.auto.enable` defaults to `0`) — enable it to
join the link.

A typical bring-up: with WiFi joined, set `s.auto.enable = 1` (or `auto up`).
The task brings up the netif's IPv6 link-local address, opens its three UDP
sockets, registers the `auto` interface with rnsd, and begins announcing. Other
nodes sharing the same **group name** (`s.auto.group`, default `"reticulum"`)
appear as peers within a few seconds; announces and traffic then flow over the
`auto` interface like any other rnsd interface.

## Public surface

The interface connects to rnsd's **`RNSD_PORT_IFACE`** with an `rnsd_iface_t`
payload (see `rns/esp-idf/include/ports.h`) and registers:

| field | value |
|---|---|
| name | `auto` |
| mtu | 500 (mR base MTU) |
| bitrate | 10 Mbit/s (AutoInterface's bitrate guess) |
| mode | from `s.auto.mode` (default `gateway`) |
| in / out | 1 / 1 |
| fwd | 1 when mode is `gateway` or `full`, else 0 |
| IFAC | `ifac_netname` / `ifac_netkey` / `ifac_size`, when set |

Well-known UDP ports (upstream AutoInterface values): **29716** multicast
discovery, **29717** unicast reverse-peering, **42671** data.

### IFAC (access-coded networks)

Setting an IFAC network name and key restricts the interface to peers sharing
the same credentials: every packet is masked and signed on egress and verified
on ingress, byte-compatible with upstream Reticulum IFAC. The credentials are
passed to rnsd in the registration payload; rnsd owns the key derivation and the
per-packet transform. See the IFAC section of [rns](../rns/INTERNALS.md) for the
crypto core. Leaving both `s.auto.ifac_netname` and `secrets.auto.ifac_netkey`
empty runs the interface open.

## Storage variables

Settings (`s.*`, persisted and synced to the browser):

| key | default | meaning |
|---|---|---|
| `s.auto.enable` | `0` | interface on/off |
| `s.auto.group` | `"reticulum"` | group **name** (upstream `group_id`); selects which network to join |
| `s.auto.mode` | `"gateway"` | interface mode: `full`, `gateway`, `access_point`, `roaming`, `boundary` |
| `s.auto.ifac_netname` | `""` | IFAC network name (empty = open) |
| `s.auto.ifac_size` | `0` | IFAC access-code length in bytes (rnsd clamps to 1–64 when IFAC is active) |
| `s.auto.retain_announces` | `1` | Keep the announces heard on the LAN, not just forward them. On by default: the peer set is bounded by the LAN, and these are usually your own nodes. |
| `s.auto.policy_manual` | `0` | Set this interface's transit policy by hand instead of inferring it from `mode`. Off = auto, which is stock behaviour and leaves `route_for` unread. |
| `s.auto.route_for` | `0` | Read only when `policy_manual = 1`. `1` = we provide transport for the nodes on this LAN: we relay announces towards them, we search on their behalf, and their paths get `s.rnsd.path.ttl_custody`. `0` = we still talk to them as an endpoint, we just don't work for them. Answering a path request for a destination we already know is never gated by this. See `rns/README.md`. |

Secrets (`secrets.*`, persisted on-device, never synced to the browser):

| key | default | meaning |
|---|---|---|
| `secrets.auto.ifac_netkey` | `""` | IFAC passphrase (empty = open) |

Telemetry (published read-only):

| key | meaning |
|---|---|
| `auto.state` | `down`, `waiting_wifi`, `waiting_addr`, `up`, `error`, or `rnsd_unavailable` |
| `auto.up` | `1` while sockets are open and the interface is registered, else `0` |
| `auto.peers` | discovered peer count |
| `auto.addr` | our IPv6 link-local address (text) |
| `auto.group_addr` | the group's IPv6 multicast address (text) |
| `auto.stats.tx_bytes`, `auto.stats.tx_packets`, `auto.stats.tx_fail` | outbound counters |
| `auto.stats.rx_bytes`, `auto.stats.rx_packets`, `auto.stats.rx_drop` | inbound counters |

The settings pane (LCD) and storage defaults are generated from the `settings:`
block in `straddle.yaml`, on both surfaces.

## CLI

```
auto                 # state, group, mcast + our address, peer count, tx/rx counters
auto up | down       # set s.auto.enable
auto peers           # discovered peers + last-heard age
```

## Verifying against desktop Reticulum

Run `rnsd` on a workstation on the same WiFi LAN with a default AutoInterface
(nothing to configure on either side):

```ini
[[Default Interface]]
  type = AutoInterface
  interface_enabled = true
```

`auto peers` on the device should list the workstation within a few seconds;
`rnstatus` on the workstation should list the device; and the browser Status /
Map shows each side's announces as paths under interface `auto`.

## Dependencies

- [rns](../rns) — rnsd, the IFAC core, the interface registration port.
- [spangap-net](../../s/spangap-net) — owns the WiFi radio and the IP stack.

## Read next

- [INTERNALS.md](INTERNALS.md) — the two-task model, the protocol, the
  BSD-sockets rationale, registration, and lifecycle.
