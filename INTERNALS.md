# reticulous-auto — internals

## Two tasks

- `auto` — the transport task. Owns the peer table, group state,
  registration with rnsd, and the outbound UDP unicast send path.
- `auto-rx` — a small helper task that `select()`s the UDP sockets and
  hands datagrams to the `auto` task via queue + task-notify.

## Why two tasks?

lwIP **core locking is off** in spangap's sdkconfig, so the raw
(non-BSD) lwIP API can't be driven from anything other than the tcpip
thread. The BSD-sockets API is the only thing safe to call from a
spangap task — and it gives us only blocking `recvfrom` and
`select()`.

Running both wait points (ITS poll + UDP select) in one task would
require giving up the single-wait-point rule. Splitting `select()`
into `auto-rx` keeps `auto` with one wait point (`itsPoll`) and uses
the queue + notify as the wakeup channel for datagrams.

## Group / peer model

- **Multicast peer discovery** on `ff02::<group-hash>:<port>` IPv6
  link-local.
- **Unicast UDP data** sent peer-to-peer to discovered link-local
  addresses (no broadcast on the data path).
- Group hash maps to a port in the upstream RNS scheme; we copy that
  mapping exactly so we're wire-compatible.

## Iface registration

`auto` registers itself with rnsd as a single iface
(`autoif.<group-hash>`); peers are not individual ifaces from rnsd's
point of view. This matches upstream RNS AutoInterface behaviour and
lets the path table treat the multicast group as a single neighbour.

## Lifecycle

- On `NET_EV_UP`: open the multicast socket(s), join the group, register
  the iface, kick the announce.
- On `NET_EV_DOWN`: deregister, close sockets.
- Continuously: send peer-discovery announces on a schedule;
  `auto-rx` feeds incoming datagrams.
