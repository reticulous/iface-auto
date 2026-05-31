# tr-auto

## What is this?

**tr-auto** is the AutoInterface transport for
[rns](../rns): zero-config RNS over the LAN
using IPv6 link-local multicast for peer discovery and unicast UDP for
data. Wire-compatible with upstream RNS's AutoInterface, so spangap
devices and Python Reticulum nodes peer with no configuration on the
same LAN.

## What this straddle owns

```
tr-auto/
├── esp-idf/
│   ├── include/auto.h
│   └── src/auto.cpp     auto + auto-rx helper
└── browser/
    └── src/
        ├── modules/auto.ts
        └── panels/AutoPanel.vue
```

## How others use it

```cpp
autoInit();    // after rnsdInit and netInit
```

Configuration:

- `s.auto.enable` — on/off (default on, since there's nothing to
  configure)
- `s.auto.group` — optional group hash for segmentation (default: the
  upstream RNS default group hash)

Active whenever `netIsUp()` is true; tears down on `NET_EV_DOWN`.

## Dependencies

- [rns](../rns)
- [spangap-net](../../s/spangap-net) — needs an IP stack.

## Read next

- [INTERNALS.md](INTERNALS.md) — the `auto-rx` helper task, BSD-sockets
  rationale, group/peer model.
- [docs/auto.md](../hw-tdeck/docs/auto.md) — AutoInterface
  black-box doc in the consuming app.
