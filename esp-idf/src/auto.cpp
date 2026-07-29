/**
 * auto — AutoInterface interface task.
 *
 * RNS over IPv6 link-local multicast discovery + unicast UDP data,
 * wire-compatible with upstream Reticulum AutoInterface
 * (RNS/Interfaces/AutoInterface.py).
 *
 * Protocol (defaults, all interoperable with upstream):
 *   - group name "reticulum" → group_hash = SHA-256(name). The IPv6
 *     multicast discovery address is ff{type}{scope}: + 6 hextets of
 *     group_hash[2..13]; default type=temporary(1), scope=link(2) →
 *     ff12:0:.... Every node joins it on the WiFi netif.
 *   - peering token = SHA-256(name || link_local_addr_text). Each node
 *     multicasts its token to the group on DISCOVERY_PORT every
 *     ANNOUNCE_INTERVAL. On receipt of a token from src S, a node
 *     recomputes SHA-256(name || text(S)); a match adds/refreshes S as
 *     a peer (a token from our own address is the multicast echo).
 *   - reverse peering: a node also unicasts its token to known peers'
 *     UNICAST_DISCOVERY_PORT, so asymmetric multicast still peers both
 *     ways. We listen there and treat it identically to multicast.
 *   - data: an outbound RNS packet is unicast (one UDP datagram each)
 *     to every live peer on DATA_PORT; inbound datagrams from a known
 *     peer are forwarded verbatim to rnsd.
 *
 * Threading: lwIP core locking is OFF in this build, so the lwIP raw
 * API can't be driven off the tcpip thread — we use BSD sockets, which
 * are task-safe. recvfrom() must block somewhere, so a tiny rx helper
 * task select()s the three UDP sockets and hands datagrams to the auto
 * task via a queue + task-notify; itsPoll() then wakes on that notify,
 * on rnsd ITS traffic, or on the announce/peer-job deadline — one wait
 * point, no polling. All sends + all state live on the auto task.
 *
 * WiFi ownership stays with net (gate on netIsUp()); the link-local
 * IPv6 address is brought up on the active netif via esp_netif.
 */
#include "auto.h"
#include "spangap.h"
#include "net.h"
#include "ports.h"
#include "rnsd.h"
#include "mem.h"

#include "esp_netif.h"
#include "esp_heap_caps.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

static const char* TAG = "auto";

#define RNS_MTU                 500     /* registered iface MTU (mR base MTU) */

/* Upstream AutoInterface well-known ports + cadences. */
#define DISCOVERY_PORT          29716
#define UNICAST_DISCOVERY_PORT  (DISCOVERY_PORT + 1)
#define DATA_PORT               42671
#define ANNOUNCE_INTERVAL_MS    1600
#define PEER_JOB_INTERVAL_MS    4000
#define PEERING_TIMEOUT_MS      22000
#define REVERSE_INTERVAL_MS     5200    /* 1.6 s * 3.25 */

/* Multicast address nibbles: flags=temporary(1), scope=link-local(2). */
#define MCAST_FLAGS_TEMPORARY   0x1
#define MCAST_SCOPE_LINK        0x2

#define TOKEN_LEN               32      /* SHA-256 */
#define MAX_PEERS               16
#define RX_QDEPTH               16
#define RX_DGRAM_MAX            1200    /* > any peer's HW_MTU datagram */

/* ─────────────── globals (single-task ownership) ─────────────── */

static TaskHandle_t  s_task     = nullptr;
static TaskHandle_t  s_rxTask   = nullptr;
static QueueHandle_t s_rxQueue  = nullptr;
static volatile bool s_stop     = false;   /* rns stop → break both task loops */
static volatile bool s_parked   = false;   /* true while the main task is parked (stopped); autoStop waits on it */

static int           s_rnsdHandle = -1;

static bool          s_netUp   = false;   /* WiFi available */
static bool          s_enabled = false;   /* s.auto.enable applied */
static bool          s_running = false;   /* sockets up + registered */

static std::string   s_group   = "reticulum";
static uint8_t       s_mode     = RNS_IFACE_MODE_GATEWAY;
static char          s_ifacNetname[32] = "";  /* IFAC network_name (s.) */
static char          s_ifacNetkey[64]  = "";  /* IFAC passphrase (secrets.) */
static uint8_t       s_ifacSize = 0;          /* IFAC access-code length */
static uint8_t       s_announceCap = RNS_IFACE_ANNOUNCE_CAP_DEFAULT;  /* % bw cap for announces */

static int           s_ifIndex = 0;
static struct in6_addr s_ourAddr   = {};
static char          s_ourAddrStr[INET6_ADDRSTRLEN] = {0};
static uint8_t       s_ourToken[TOKEN_LEN] = {0};
static struct in6_addr s_groupAddr = {};
static char          s_groupAddrStr[INET6_ADDRSTRLEN] = {0};

/* Sockets are opened/closed on the auto task, read by the rx task. */
static volatile int  s_discSock = -1;   /* multicast discovery (joined group) */
static volatile int  s_uniSock  = -1;   /* unicast reverse-peering */
static volatile int  s_dataSock = -1;   /* RNS data */

static TickType_t    s_lastAnnounce = 0;
static TickType_t    s_lastPeerJob  = 0;

static uint64_t      s_txBytes = 0, s_rxBytes = 0;
static uint64_t      s_txPackets = 0, s_rxPackets = 0;
static uint64_t      s_txFail = 0, s_rxDrop = 0;

static volatile bool s_configDirty = true;

typedef struct {
    struct in6_addr addr;
    char            str[INET6_ADDRSTRLEN];
    TickType_t      last_heard;
    TickType_t      last_reverse;
} peer_t;
PSRAM_BSS static peer_t s_peers[MAX_PEERS];
static int    s_peerCount = 0;

enum rx_kind_t : uint8_t { RX_DISC = 0, RX_DATA = 1 };
typedef struct {
    uint16_t        len;
    uint8_t         kind;
    struct in6_addr src;
    uint8_t         data[RX_DGRAM_MAX];
} auto_rx_t;

/* ─────────────── helpers ─────────────── */

static void hashOf(const std::string& s, uint8_t out[TOKEN_LEN]) {
    rnsdSha256(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
}

/* Derive the group's IPv6 multicast discovery address from the name,
 * matching AutoInterface.__init__: ff{flags}{scope}:0:<6 hextets of
 * group_hash[2..13]>. */
static void computeGroupAddr(void) {
    uint8_t g[TOKEN_LEN];
    hashOf(s_group, g);
    std::memset(&s_groupAddr, 0, sizeof(s_groupAddr));
    uint8_t* b = reinterpret_cast<uint8_t*>(&s_groupAddr);
    b[0] = 0xff;
    b[1] = (uint8_t)((MCAST_FLAGS_TEMPORARY << 4) | MCAST_SCOPE_LINK);
    /* b[2..3] stay 0 (the "0" hextet); b[4..15] = group_hash[2..13]. */
    std::memcpy(&b[4], &g[2], 12);
    inet_ntop(AF_INET6, &s_groupAddr, s_groupAddrStr, sizeof(s_groupAddrStr));
}

/* token = SHA-256(group_name || addr_text). */
static void computeToken(const char* addrStr, uint8_t out[TOKEN_LEN]) {
    std::string m = s_group;
    m.append(addrStr);
    hashOf(m, out);
}

static bool isOurAddr(const struct in6_addr& a) {
    return std::memcmp(&a, &s_ourAddr, sizeof(a)) == 0;
}

static peer_t* findPeer(const struct in6_addr& a) {
    for (int i = 0; i < s_peerCount; i++)
        if (std::memcmp(&s_peers[i].addr, &a, sizeof(a)) == 0) return &s_peers[i];
    return nullptr;
}

/* ─────────────── publish ─────────────── */

static void publishState(const char* state) {
    storageBegin();
    storageSet("auto.state", state);
    storageSet("auto.up", s_running ? 1 : 0);
    storageEnd();
}

static void publishStats(void) {
    /* One bracket → one storage op instead of 7 sync round-trips per second. */
    storageBegin();
    storageSet("auto.peers", s_peerCount);
    storageSet("auto.stats.tx_bytes",   (int)(s_txBytes   & 0x7fffffff));
    storageSet("auto.stats.rx_bytes",   (int)(s_rxBytes   & 0x7fffffff));
    storageSet("auto.stats.tx_packets", (int)(s_txPackets & 0x7fffffff));
    storageSet("auto.stats.rx_packets", (int)(s_rxPackets & 0x7fffffff));
    storageSet("auto.stats.tx_fail",    (int)(s_txFail    & 0x7fffffff));
    storageSet("auto.stats.rx_drop",    (int)(s_rxDrop    & 0x7fffffff));
    storageEnd();
}

/* ─────────────── rnsd registration ─────────────── */

static void onRnsdRecv(int handle, size_t bytesAvail);
static void onRnsdDisconnect(int ref);

static void deregisterFromRnsd(void) {
    if (s_rnsdHandle >= 0) { itsDisconnect(s_rnsdHandle); s_rnsdHandle = -1; }
}

static bool registerWithRnsd(void) {
    deregisterFromRnsd();
    rnsd_iface_t reg = {};
    safeStrncpy(reg.name, "auto", sizeof(reg.name));
    reg.mtu     = RNS_MTU;
    reg.bitrate = 10 * 1000 * 1000;   /* AutoInterface BITRATE_GUESS */
    reg.mode    = s_mode;
    reg.in = reg.out = 1;
    reg.fwd = (s_mode == RNS_IFACE_MODE_GATEWAY || s_mode == RNS_IFACE_MODE_FULL) ? 1 : 0;
    reg.rpt = 0;
    reg.ifac_size = s_ifacSize;
    reg.announce_cap = s_announceCap;
    reg.point_to_point = 1;   /* switched/multicast LAN: every peer hears every
                                 other, so no hidden-node problem */
    safeStrncpy(reg.ifac_netname, s_ifacNetname, sizeof(reg.ifac_netname));
    safeStrncpy(reg.ifac_netkey,  s_ifacNetkey,  sizeof(reg.ifac_netkey));
    s_rnsdHandle = itsConnect("rnsd", RNSD_PORT_IFACE, &reg, sizeof(reg),
                              pdMS_TO_TICKS(500), 1, onRnsdRecv, onRnsdDisconnect);
    if (s_rnsdHandle < 0) { warn("rnsd register failed"); return false; }
    info("registered as iface auto (group=%s addr=%s)",
         s_group.c_str(), s_ourAddrStr);
    return true;
}

static void onRnsdDisconnect(int /*ref*/) {
    s_rnsdHandle = -1;   /* task loop re-registers while running */
}

/* ─────────────── sockets ─────────────── */

static int makeSock(uint16_t port, bool joinGroup) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) { err("socket: %s (errno %d)", strerror(errno), errno); return -1; }

    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));

    struct sockaddr_in6 sa = {};   /* zero = bind to [::] (in6addr_any) */
    sa.sin6_family = AF_INET6;
    sa.sin6_port   = htons(port);
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        err("bind :%u: %s (errno %d)", (unsigned)port, strerror(errno), errno);
        lwip_close(fd);
        return -1;
    }

    if (joinGroup) {
        struct ipv6_mreq mreq = {};
        std::memcpy(&mreq.ipv6mr_multiaddr, &s_groupAddr, sizeof(s_groupAddr));
        mreq.ipv6mr_interface = (unsigned)s_ifIndex;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            err("join %s: %s (errno %d)", s_groupAddrStr, strerror(errno), errno);
            lwip_close(fd);
            return -1;
        }
    }

    /* select() in the rx task gates recvfrom; non-block is belt-and-braces. */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static void closeSockets(void) {
    int a = s_discSock, b = s_uniSock, c = s_dataSock;
    s_discSock = -1;   /* rx task drops them next loop */
    s_uniSock  = -1;
    s_dataSock = -1;
    if (a >= 0) lwip_close(a);
    if (b >= 0) lwip_close(b);
    if (c >= 0) lwip_close(c);
}

static bool openSockets(void) {
    int d = makeSock(DISCOVERY_PORT, true);
    if (d < 0) return false;
    int u = makeSock(UNICAST_DISCOVERY_PORT, false);
    if (u < 0) { lwip_close(d); return false; }
    int da = makeSock(DATA_PORT, false);
    if (da < 0) { lwip_close(d); lwip_close(u); return false; }
    s_discSock = d; s_uniSock = u; s_dataSock = da;
    if (s_rxTask) xTaskNotifyGive(s_rxTask);   /* wake the parked rx task onto the fresh sockets */
    return true;
}

/* Fill a sockaddr_in6 for a unicast/multicast send, zoned to our netif. */
static void fillDest(struct sockaddr_in6& sa, const struct in6_addr& addr, uint16_t port) {
    std::memset(&sa, 0, sizeof(sa));
    sa.sin6_family   = AF_INET6;
    sa.sin6_port     = htons(port);
    sa.sin6_addr     = addr;
    sa.sin6_scope_id = (uint32_t)s_ifIndex;
}

/* ─────────────── bring-up / tear-down ─────────────── */

static esp_netif_t* pickNetif(void) {
    if (netIsStaConnected()) {
        esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (n) return n;
    }
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

static void teardown(void) {
    /* rx task self-parks once the sockets become -1 (see autoRxTaskMain). */
    closeSockets();
    deregisterFromRnsd();
    s_peerCount = 0;
    s_running = false;
    if (s_netUp) publishState("waiting_addr"); else publishState("waiting_wifi");
}

/* Attempt to come up. May return without s_running set while the
 * link-local address is still being assigned (DAD) — the loop retries. */
static void tryBringUp(void) {
    if (s_running) return;
    if (!s_netUp)  { publishState("waiting_wifi"); return; }

    esp_netif_t* nif = pickNetif();
    if (!nif) { publishState("waiting_wifi"); return; }

    esp_netif_create_ip6_linklocal(nif);   /* idempotent; kicks off DAD */

    esp_ip6_addr_t ll;
    if (esp_netif_get_ip6_linklocal(nif, &ll) != ESP_OK) {
        publishState("waiting_addr");
        return;
    }

    s_ifIndex = esp_netif_get_netif_impl_index(nif);
    std::memcpy(&s_ourAddr, ll.addr, sizeof(s_ourAddr));
    inet_ntop(AF_INET6, &s_ourAddr, s_ourAddrStr, sizeof(s_ourAddrStr));
    computeToken(s_ourAddrStr, s_ourToken);

    if (!openSockets()) { publishState("error"); return; }

    s_peerCount = 0;
    s_running = true;
    s_lastAnnounce = 0;   /* announce immediately */
    s_lastPeerJob  = xTaskGetTickCount();
    publishState("up");
    storageBegin();
    storageSet("auto.addr", s_ourAddrStr);
    storageSet("auto.group_addr", s_groupAddrStr);
    storageEnd();
    info("up: addr=%s group=%s (%s)", s_ourAddrStr, s_group.c_str(), s_groupAddrStr);

    if (!registerWithRnsd()) publishState("rnsd_unavailable");
}

/* ─────────────── discovery ─────────────── */

static void sendToken(int fd, const struct in6_addr& addr, uint16_t port) {
    struct sockaddr_in6 sa;
    fillDest(sa, addr, port);
    int n = sendto(fd, s_ourToken, TOKEN_LEN, 0, (struct sockaddr*)&sa, sizeof(sa));
    if (n != TOKEN_LEN) s_txFail++;
}

static void sendAnnounce(void) {
    if (s_discSock >= 0) sendToken(s_discSock, s_groupAddr, DISCOVERY_PORT);
}

static void handleDiscovery(const struct in6_addr& src, const uint8_t* data, size_t len) {
    if (len != TOKEN_LEN) return;
    if (isOurAddr(src)) return;            /* multicast echo of ourselves */

    char srcStr[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &src, srcStr, sizeof(srcStr));

    uint8_t expect[TOKEN_LEN];
    computeToken(srcStr, expect);
    if (std::memcmp(data, expect, TOKEN_LEN) != 0) {
        dbg("bad peering token from %s", srcStr);
        return;
    }

    peer_t* p = findPeer(src);
    if (p) { p->last_heard = xTaskGetTickCount(); return; }
    if (s_peerCount >= MAX_PEERS) { warn("peer table full, ignoring %s", srcStr); return; }

    p = &s_peers[s_peerCount++];
    p->addr = src;
    safeStrncpy(p->str, srcStr, sizeof(p->str));
    p->last_heard   = xTaskGetTickCount();
    p->last_reverse = 0;                    /* reverse-announce on next job */
    info("peer up: %s (%d total)", srcStr, s_peerCount);
}

/* Expire silent peers; reverse-announce to live ones. */
static void peerJob(void) {
    TickType_t now = xTaskGetTickCount();
    for (int i = 0; i < s_peerCount; ) {
        if ((now - s_peers[i].last_heard) > pdMS_TO_TICKS(PEERING_TIMEOUT_MS)) {
            info("peer down: %s (timeout)", s_peers[i].str);
            s_peers[i] = s_peers[--s_peerCount];   /* swap-remove */
            continue;
        }
        if (s_uniSock >= 0 &&
            (now - s_peers[i].last_reverse) > pdMS_TO_TICKS(REVERSE_INTERVAL_MS)) {
            sendToken(s_uniSock, s_peers[i].addr, UNICAST_DISCOVERY_PORT);
            s_peers[i].last_reverse = now;
        }
        i++;
    }
}

/* ─────────────── inbound (rx queue → rnsd) ─────────────── */

static void drainRx(void) {
    if (!s_rxQueue) return;
    PSRAM_BSS static auto_rx_t slot;   /* one task; static avoids 1.2 KB of stack churn */
    while (xQueueReceive(s_rxQueue, &slot, 0) == pdTRUE) {
        if (!s_running) continue;
        if (slot.kind == RX_DISC) {
            handleDiscovery(slot.src, slot.data, slot.len);
            continue;
        }
        /* RX_DATA: accept only from a known peer, refresh its liveness. */
        peer_t* p = findPeer(slot.src);
        if (!p) continue;
        p->last_heard = xTaskGetTickCount();
        s_rxPackets++;
        s_rxBytes += slot.len;
        if (s_rnsdHandle < 0) continue;
        size_t s = itsSend(s_rnsdHandle, slot.data, slot.len, pdMS_TO_TICKS(100));
        if (s == 0) warn("rnsd ITS send dropped (%u B)", (unsigned)slot.len);
    }
}

/* ─────────────── outbound (rnsd → peers) ─────────────── */

static void drainOutbound(void) {
    if (!s_running || s_rnsdHandle < 0) return;
    PSRAM_BSS static uint8_t pkt[RNS_MTU + 16];
    while (itsBytesAvailable(s_rnsdHandle) > 0) {
        size_t n = itsRecv(s_rnsdHandle, pkt, sizeof(pkt), 0);
        if (n == 0) break;
        s_txPackets++;
        s_txBytes += n;
        for (int i = 0; i < s_peerCount; i++) {
            struct sockaddr_in6 sa;
            fillDest(sa, s_peers[i].addr, DATA_PORT);
            if (sendto(s_dataSock, pkt, n, 0, (struct sockaddr*)&sa, sizeof(sa)) != (int)n)
                s_txFail++;
        }
    }
}

static void onRnsdRecv(int /*handle*/, size_t /*bytesAvail*/) {
    drainOutbound();
}

/* ─────────────── config / net events ─────────────── */

static void applyConfig(void) {
    s_enabled = storageGetInt("s.auto.enable", 0) != 0;

    char group[48];
    storageGetStr("s.auto.group", group, sizeof(group), "reticulum");
    if (group[0] == '\0') safeStrncpy(group, "reticulum", sizeof(group));

    char mode[24];
    storageGetStr("s.auto.mode", mode, sizeof(mode), "gateway");
    uint8_t m;
    if      (strcmp(mode, "full")         == 0) m = RNS_IFACE_MODE_FULL;
    else if (strcmp(mode, "gateway")      == 0) m = RNS_IFACE_MODE_GATEWAY;
    else if (strcmp(mode, "access_point") == 0) m = RNS_IFACE_MODE_ACCESS_POINT;
    else if (strcmp(mode, "roaming")      == 0) m = RNS_IFACE_MODE_ROAMING;
    else if (strcmp(mode, "boundary")     == 0) m = RNS_IFACE_MODE_BOUNDARY;
    else                                        m = RNS_IFACE_MODE_GATEWAY;

    char ifn[sizeof(s_ifacNetname)] = ""; storageGetStr("s.auto.ifac_netname", ifn, sizeof(ifn), "");
    char ifk[sizeof(s_ifacNetkey)]  = ""; storageGetStr("secrets.auto.ifac_netkey", ifk, sizeof(ifk), "");
    uint8_t ifs = (uint8_t)storageGetInt("s.auto.ifac_size", 0);
    uint8_t acap = (uint8_t)storageGetInt("s.auto.announce_cap", RNS_IFACE_ANNOUNCE_CAP_DEFAULT);

    bool groupChanged = s_group != group;
    bool changed = groupChanged || (m != s_mode)
                   || strcmp(ifn, s_ifacNetname) != 0 || strcmp(ifk, s_ifacNetkey) != 0
                   || ifs != s_ifacSize || acap != s_announceCap;
    s_group = group;
    s_mode  = m;
    safeStrncpy(s_ifacNetname, ifn, sizeof(s_ifacNetname));
    safeStrncpy(s_ifacNetkey,  ifk, sizeof(s_ifacNetkey));
    s_ifacSize = ifs;
    s_announceCap = acap;
    if (groupChanged) { computeGroupAddr(); storageSet("auto.group_addr", s_groupAddrStr); }

    if (!s_enabled) { teardown(); publishState("down"); return; }
    /* Enabled but no IP yet — ask net to bring WiFi up. Gated here (not at boot)
     * so only an enabled interface powers the radio; netUp itself no-ops when
     * s.net.wifi.enable=0. onNetUp() re-runs applyConfig once it's available;
     * tryBringUp() no-ops to waiting_wifi until then. */
    if (!s_netUp) netUp();
    if (s_running && changed) teardown();   /* re-apply group/mode */
    if (!s_running) tryBringUp();
}

static void onCfgChange(const char* /*key*/, const char* /*val*/) {
    s_configDirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void onNetUp(const char*) {
    s_netUp = true;
    s_configDirty = true;
    if (s_task) xTaskNotifyGive(s_task);
}

static void onNetDown(const char*) {
    s_netUp = false;
    teardown();
    publishState("waiting_wifi");
}

/* ─────────────── rx helper task ─────────────── */

static void rxRecvOne(int fd, uint8_t kind) {
    PSRAM_BSS static auto_rx_t slot;   /* only the rx task touches it; keeps its stack small */
    struct sockaddr_in6 sa;
    socklen_t sl = sizeof(sa);
    int n = recvfrom(fd, slot.data, sizeof(slot.data), 0, (struct sockaddr*)&sa, &sl);
    if (n <= 0) return;
    slot.len  = (uint16_t)n;
    slot.kind = kind;
    slot.src  = sa.sin6_addr;
    if (xQueueSend(s_rxQueue, &slot, 0) != pdTRUE) { s_rxDrop++; return; }
    if (s_task) xTaskNotifyGive(s_task);
}

static void autoRxTaskMain(void*) {
    /* Park, don't delete: this task lives across rns stop/start. When stopped it
     * blocks on the notify above (sockets are -1 after the main task's teardown);
     * when resumed and openSockets() reopens them it selects again. */
    for (;;) {
        int d = s_discSock, u = s_uniSock, da = s_dataSock;
        if (d < 0 && u < 0 && da < 0) {
            /* No sockets (wifi down / interface torn down, or main task parked on
             * stop): block until openSockets() opens them and notifies us, instead
             * of spinning at 5 Hz. On rns stop the main task's teardown() drops the
             * sockets to -1, so we naturally idle here; autoStart() notifies us to
             * re-check and resume once the sockets reopen. Never deleted. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (d  >= 0) { FD_SET(d,  &rfds); if (d  > maxfd) maxfd = d;  }
        if (u  >= 0) { FD_SET(u,  &rfds); if (u  > maxfd) maxfd = u;  }
        if (da >= 0) { FD_SET(da, &rfds); if (da > maxfd) maxfd = da; }

        struct timeval tv = { 1, 0 };   /* 1 s cap to re-read socket set / re-check s_stop */
        int n = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (n <= 0) continue;

        if (d  >= 0 && FD_ISSET(d,  &rfds)) rxRecvOne(d,  RX_DISC);
        if (u  >= 0 && FD_ISSET(u,  &rfds)) rxRecvOne(u,  RX_DISC);
        if (da >= 0 && FD_ISSET(da, &rfds)) rxRecvOne(da, RX_DATA);
    }
}

/* ─────────────── CLI ─────────────── */

static void cliAuto(const char* args) {
    if (args && strcmp(args, "help") == 0) { cliPrintf("%-*s AutoInterface status; up/down; peers\n", CLI_HELP_COL, "auto [...]"); return; }
    if (args && cliWantsHelp(args)) {
        cliPrintf("%-*s AutoInterface status\n",   CLI_HELP_COL, "auto");
        cliPrintf("%-*s enable/disable\n",         CLI_HELP_COL, "auto up|down");
        cliPrintf("%-*s list discovered peers\n",  CLI_HELP_COL, "auto peers");
        return;
    }
    if (args && strcmp(args, "up") == 0)   { storageSet("s.auto.enable", 1); cliPrintf("enabled\n");  return; }
    if (args && strcmp(args, "down") == 0) { storageSet("s.auto.enable", 0); cliPrintf("disabled\n"); return; }
    if (args && strcmp(args, "peers") == 0) {
        if (s_peerCount == 0) { cliPrintf("(no peers)\n"); return; }
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < s_peerCount; i++)
            cliPrintf("%-40s last heard %ums ago\n", s_peers[i].str,
                      (unsigned)((now - s_peers[i].last_heard) * portTICK_PERIOD_MS));
        return;
    }

    cliPrintf("state:    %s\n",
              s_running   ? "up"
              : !s_enabled ? "down"
              : !s_netUp   ? "waiting wifi"
                           : "waiting address");
    cliPrintf("group:    %s\n", s_group.c_str());
    cliPrintf("mcast:    %s\n", s_groupAddrStr[0] ? s_groupAddrStr : "(n/a)");
    if (s_running) cliPrintf("address:  %s\n", s_ourAddrStr);
    cliPrintf("peers:    %d\n", s_peerCount);
    cliPrintf("rx:       %llu packets, %llu bytes (drop %llu)\n",
              (unsigned long long)s_rxPackets, (unsigned long long)s_rxBytes,
              (unsigned long long)s_rxDrop);
    cliPrintf("tx:       %llu packets, %llu bytes (fail %llu)\n",
              (unsigned long long)s_txPackets, (unsigned long long)s_txBytes,
              (unsigned long long)s_txFail);
}

/* ─────────────── task ─────────────── */

static TickType_t nextDeadline(void) {
    if (!s_running) {
        /* Waiting on DAD / netif while enabled — retry soon; else idle. */
        return (s_enabled && s_netUp) ? pdMS_TO_TICKS(500) : portMAX_DELAY;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t toAnn = pdMS_TO_TICKS(ANNOUNCE_INTERVAL_MS) - (now - s_lastAnnounce);
    TickType_t toJob = pdMS_TO_TICKS(PEER_JOB_INTERVAL_MS) - (now - s_lastPeerJob);
    if ((int32_t)toAnn < 0) toAnn = 0;
    if ((int32_t)toJob < 0) toJob = 0;
    TickType_t d = toAnn < toJob ? toAnn : toJob;
    TickType_t cap = pdMS_TO_TICKS(1000);   /* publish stats at ≥1 Hz */
    return d < cap ? d : cap;
}

static void autoTaskMain(void*) {
    info("[%s] task up", TAG);

    /* No boot barrier here: the RNS orchestrator only calls autoStart() (which
     * spawns this task) after rnsd is up and past its boot window. */
    itsClientInit(2);
    s_rxQueue = xQueueCreateWithCaps(RX_QDEPTH, sizeof(auto_rx_t), MALLOC_CAP_SPIRAM);

    computeGroupAddr();
    storageSet("auto.group_addr", s_groupAddrStr);
    publishState("down");

    s_netUp = netIsUp();
    /* Register net callbacks once for the process — net's registry is append-only
     * (no unregister), so re-registering per rns start would pile up duplicates.
     * The callbacks guard s_task, so staying live across a stop is harmless. */
    static bool s_netCbsRegistered = false;
    if (!s_netCbsRegistered) {
        s_netCbsRegistered = true;
        netRegister(NET_EV_UP,   onNetUp);
        netRegister(NET_EV_DOWN, onNetDown);
    }
    storageSubscribeChanges("s.auto", onCfgChange);
    storageSubscribeChanges("secrets.auto", onCfgChange);  /* IFAC passphrase */

  for (;;) {   /* Park, don't delete: this task lives across rns stop/start, so its
                * ITS slot + rx queue + net-cb registration are reused, not leaked. */
    /* (Re)apply config on entry + each resume → reconcile against s.auto.enable
     * (a disabled interface must not power the WiFi radio), then bring sockets up +
     * register while enabled + netUp. applyConfig() asks net to bring WiFi up only
     * when enabled — every bring-up request routes through the enable gate. */
    s_configDirty = true;
    while (!s_stop) {
        if (s_configDirty) { s_configDirty = false; applyConfig(); }

        if (s_enabled && s_netUp && !s_running) tryBringUp();

        drainRx();

        if (s_running) {
            if (s_rnsdHandle < 0) registerWithRnsd();   /* re-register if dropped */
            TickType_t now = xTaskGetTickCount();
            if ((now - s_lastAnnounce) >= pdMS_TO_TICKS(ANNOUNCE_INTERVAL_MS)) {
                sendAnnounce(); s_lastAnnounce = now;
            }
            if ((now - s_lastPeerJob) >= pdMS_TO_TICKS(PEER_JOB_INTERVAL_MS)) {
                peerJob(); s_lastPeerJob = now;
            }
            drainOutbound();
        }

        publishStats();
        itsPoll(nextDeadline());
    }   /* end while(!s_stop) */

    /* rns stop: tear down sockets (disc/uni/data → -1) and deregister from rnsd
     * (frees rnsd's iface slot; resets peers). The rx task self-parks once the
     * sockets go -1. Keep the ITS slot + rx queue for the next start. Then PARK on
     * the inbox until autoStart() clears s_stop and notifies. */
    teardown();
    s_parked = true;
    info("[%s] stopped", TAG);
    while (s_stop) itsPoll(portMAX_DELAY);
    s_parked = false;
  }
}

/* ── RNS lifecycle hooks (registered with the orchestrator; see rnsServiceRegister) ── */
static void autoStart(void) {
    s_stop = false;
    if (!s_task) {
        /* Spawn the rx helper first (main task's openSockets notifies it), then the
         * main task — same args onInit used. */
        s_rxTask = spawnTask(autoRxTaskMain, "auto-rx", 4096, nullptr, 2, 0, STACK_PSRAM);
        s_task   = spawnTask(autoTaskMain, TAG, 6144, nullptr, 2, 0, STACK_PSRAM);
    } else {
        xTaskNotifyGive(s_task);     /* un-park the resident main task */
        xTaskNotifyGive(s_rxTask);   /* un-park the rx helper */
    }
}

static void autoStop(void) {
    if (!s_task || s_stop) return;
    s_stop = true;
    xTaskNotifyGive(s_task);     /* break the itsPoll work loop; the task parks, not deleted */
    xTaskNotifyGive(s_rxTask);   /* break the park; select() re-checks via its 1 s cap */
    for (int i = 0; i < 300 && !s_parked; i++) delay(10);   /* await main-task park */
    if (!s_parked) warn("[%s] stop timed out", TAG);
}

void AutoService::onInit() {

    /* The settings pane + storage defaults are generated from the settings:
     * block in straddle.yaml (LCD pane gated on spangap-lcd; web kept by
     * AutoPanel.vue via web: false). */
    cliRegisterCmd("auto", cliAuto);

    /* Register with the RNS orchestrator instead of self-spawning: rnsStart()
     * calls autoStart() (spawning both the main + rx tasks, Core 0 alongside net +
     * rnsd, prio 2, PSRAM stacks) once rnsd is up and past its boot window, and
     * rnsStop() calls autoStop(). */
    rnsServiceRegister(TAG, autoStart, autoStop, RNS_PHASE_IFACE);
}
