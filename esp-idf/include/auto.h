/**
 * auto — AutoInterface interface task.
 *
 * Reticulum's zero-configuration LAN interface: IPv6 link-local
 * multicast peer discovery + unicast UDP data, wire-compatible with
 * upstream RNS `AutoInterface`. Every reachable RNS node on the same
 * link is found automatically — no host/port config.
 *
 *   - discovery: SHA-256 peering token multicast to the group's
 *     well-known IPv6 group address (derived from the group name) on
 *     port 29716; a matching token from a peer adds it.
 *   - data: RNS packets unicast over UDP to each discovered peer's
 *     link-local address on port 42671 (one datagram = one RNS packet,
 *     no on-air framing — same broadcast shape rnsd expects).
 *
 * Self-registers with rnsd as the `auto` interface. Requires WiFi to be
 * up (net owns the radio); active while netIsUp().
 *
 * See docs/auto.md and docs/component-plan.md §8.1 / §11.
 */
#pragma once

void autoInit(void);
