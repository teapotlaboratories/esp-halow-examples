/*
 * mesh_gate — an 802.11s mesh and a co-channel SoftAP on ONE MM6108, bridged at
 * layer 2 so both sides share a single flat subnet.
 *
 * A gate lets ordinary Wi-Fi HaLow stations reach a mesh without knowing anything
 * about it. An AP client associates, DHCPs an address, and talks to any mesh node
 * directly — no routing, no second subnet, no static routes, nothing to configure on
 * the client.
 *
 * That works because the bridge is at layer 2, using 802.11s Address Extension: a
 * frame from an AP client is injected into the mesh as a 6-address proxied frame
 * carrying the client as its original source, and a proxied frame arriving for one of
 * our clients is delivered onto the AP interface. Mesh nodes learn "that host is
 * reachable via this gate" from the frames themselves, exactly as they would from a
 * Linux gate.
 *
 * Bring-up order matters — the mesh owns the primary interface:
 *
 *     mmhalow_init  ->  mmwlan_mesh_start  ->  mmwlan_ap_enable  ->  bridge wiring
 *
 * Addressing (one flat /24, default 10.9.9.0/24):
 *
 *     mesh interface   10.9.9.<100 + mac[5] & 0x3f>   primary vif, MMWLAN_VIF_STA
 *     AP   interface   10.9.9.1                       secondary vif, MMWLAN_VIF_AP,
 *                                                     DHCP server for AP clients
 *
 * PER-INTERFACE WIRING IS LOAD-BEARING. morselib delivers mesh receives to the
 * MMWLAN_VIF_STA callback slot and AP receives to MMWLAN_VIF_AP, and routes transmits
 * by metadata.vif. So the mesh netif must be fed from, and tagged, VIF_STA and the AP
 * netif VIF_AP. Getting this backwards silently drops one direction.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_types.h"
#include "nvs_flash.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "mmhalow.h"
#include "mmpkt.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#define MESH_ID            CONFIG_EXAMPLE_MESH_ID
#define MESH_MAX_PLINKS    CONFIG_EXAMPLE_MESH_MAX_PLINKS
#define RANN_INTERVAL_MS   CONFIG_EXAMPLE_RANN_INTERVAL_MS
#define AP_SSID            CONFIG_EXAMPLE_AP_SSID
#define AP_PSK             CONFIG_EXAMPLE_AP_PSK
#define AP_MAX_STAS        CONFIG_EXAMPLE_AP_MAX_STAS
#define S1G_CHANNEL        CONFIG_EXAMPLE_S1G_CHANNEL
#define S1G_OPCLASS        CONFIG_EXAMPLE_S1G_OPCLASS
#define SUBNET_PREFIX      CONFIG_EXAMPLE_SUBNET_PREFIX
#define SUBNET_NETMASK     "255.255.255.0"
#define PROXY_ARP_PUSH_MS  CONFIG_EXAMPLE_PROXY_ARP_PUSH_MS

/*
 * Scratch buffers for reframing between the two sides are `static`, not ~1.5 KB stack
 * frames, which would risk overflowing a receive task's stack. That is safe only under
 * a no-concurrent-writers invariant, and it is worth stating explicitly because it is
 * the assumption most easily broken by building on this example:
 *
 *   s_mesh_to_ap, s_ae_to_ap   written only on the MESH receive context
 *   s_ap_to_mesh               written only on the AP receive context
 *
 * Each is written and consumed synchronously within one callback, and no buffer is
 * touched from both contexts. morselib's own proxy scratch relies on the same
 * single-datapath-context invariant. If you ever drive either path from a second
 * context, these must become per-call.
 */

/* Largest Ethernet frame we relay between the two sides. */
#define MAX_ETH_FRAME (14 + 1514)
/* An Ethernet ARP frame: 14 B header + 28 B ARP. */
#define ARP_FRAME_LEN 42
/* LLC/SNAP header prepended to a mesh payload. */
#define SNAP_LEN 8

static const char *TAG = "mesh_gate";

static uint8_t g_mesh_mac[6];
static uint8_t g_ap_bssid[6];      /* the AP interface's own MAC: a frame to it is for us */
static char g_ap_ipv4[16];         /* SUBNET_PREFIX "1" */
static esp_netif_t *g_mesh_netif;
static esp_netif_t *g_ap_netif;
static esp_netif_driver_base_t g_ap_driver_base;

static void arp_forget_mac(const uint8_t *mac);

/* ---------------------------------------------------------------------------
 * Associated AP clients.
 *
 * The gate needs to know which MACs are on its AP side, so it can tell whether an
 * arriving proxied mesh frame is destined for one of its own clients. Written by the
 * AP status callback and read from the mesh receive task, so a short critical section
 * keeps a reader from seeing a torn slot or a stale count mid-removal. Nothing that
 * blocks or transmits runs inside it.
 * ------------------------------------------------------------------------- */
#define GATE_MAX_CLIENTS 8
static uint8_t g_ap_clients[GATE_MAX_CLIENTS][6];
static volatile int g_ap_client_n;
static portMUX_TYPE g_ap_clients_mux = portMUX_INITIALIZER_UNLOCKED;

static bool is_ap_client(const uint8_t *mac)
{
    bool found = false;
    taskENTER_CRITICAL(&g_ap_clients_mux);
    for (int i = 0; i < g_ap_client_n; i++)
    {
        if (memcmp(g_ap_clients[i], mac, 6) == 0)
        {
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_ap_clients_mux);
    return found;
}

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL)
    {
        return;
    }

    if (st->state == MMWLAN_AP_STA_AUTHORIZED)
    {
        taskENTER_CRITICAL(&g_ap_clients_mux);
        bool present = false;
        for (int i = 0; i < g_ap_client_n; i++)
        {
            if (memcmp(g_ap_clients[i], st->mac_addr, 6) == 0)
            {
                present = true;
                break;
            }
        }
        if (!present && g_ap_client_n < GATE_MAX_CLIENTS)
        {
            memcpy(g_ap_clients[g_ap_client_n++], st->mac_addr, 6);
        }
        int n = g_ap_client_n;
        taskEXIT_CRITICAL(&g_ap_clients_mux);
        ESP_LOGI(TAG, "AP client joined " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
    }
    else if (st->state == MMWLAN_AP_STA_UNKNOWN)
    {
        taskENTER_CRITICAL(&g_ap_clients_mux);
        for (int i = 0; i < g_ap_client_n; i++)
        {
            if (memcmp(g_ap_clients[i], st->mac_addr, 6) == 0)
            {
                memcpy(g_ap_clients[i], g_ap_clients[--g_ap_client_n], 6);  /* swap with last */
                break;
            }
        }
        int n = g_ap_client_n;
        taskEXIT_CRITICAL(&g_ap_clients_mux);
        arp_forget_mac(st->mac_addr);   /* stop advertising a departed host's mapping */
        ESP_LOGI(TAG, "AP client left   " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
    }
}

/* ---------------------------------------------------------------------------
 * A second esp_netif bound to the AP interface.
 *
 * mmhalow creates one netif for the primary (mesh) interface. The AP side needs its
 * own, whose transmit path tags MMWLAN_VIF_AP so morselib egresses on the right
 * interface, and whose receive path is fed by the VIF_AP callback.
 * ------------------------------------------------------------------------- */
static esp_err_t ap_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (mmwlan_tx_wait_until_ready(1000) != MMWLAN_SUCCESS)
    {
        ESP_LOGW(TAG, "AP transmit blocked");
        return ESP_FAIL;
    }

    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(len, 0);
    if (pkt == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, (const uint8_t *)buffer, len);
    mmpkt_close(&v);

    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    md.vif = MMWLAN_VIF_AP;
    return (mmwlan_tx_pkt(pkt, &md) == MMWLAN_SUCCESS) ? ESP_OK : ESP_FAIL;
}

static esp_err_t ap_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    (void)netstack_buf;
    return ap_transmit(h, buffer, len);
}

static void ap_free_rx(void *h, void *buffer)
{
    (void)h;
    struct mmpktview *v = (struct mmpktview *)buffer;
    struct mmpkt *pkt = mmpkt_from_view(v);
    mmpkt_close(&v);
    mmpkt_release(pkt);
}

static esp_err_t ap_driver_post_attach(esp_netif_t *netif, void *args)
{
    esp_netif_driver_base_t *base = (esp_netif_driver_base_t *)args;
    base->netif = netif;
    esp_netif_driver_ifconfig_t ifcfg = {
        .handle = base,
        .transmit = ap_transmit,
        .transmit_wrap = ap_transmit_wrap,
        .driver_free_rx_buffer = ap_free_rx,
    };
    return esp_netif_set_driver_config(netif, &ifcfg);
}

/*
 * Deliver a received frame into the gate's OWN lwIP stack — DHCP, and ARP or ICMP
 * addressed to the gate itself. Bridging between the two sides is done separately, in
 * the receive callbacks below; this is only local delivery.
 *
 * esp_netif_receive() is deliberately not used here. It wraps the frame in a
 * zero-copy, NON-CONTIGUOUS PBUF_REF with a custom-free callback, which causes two
 * problems: lwIP's pbuf_add_header() refuses to prepend a link-layer header onto a
 * non-contiguous pbuf, and the custom-free path risks a double free once we already
 * own the mmpkt.
 *
 * Instead, copy into a contiguous PBUF_RAM with link headroom and inject via
 * netif->input, which is what esp_netif's own wlanif_input does. We own the mmpkt (the
 * callback handed it over), we copied out of it, so we free it exactly once and never
 * pass it on.
 */
static void deliver_locally(struct mmpkt *mmpkt, esp_netif_t *esp_netif)
{
    struct mmpktview *v = mmpkt_open(mmpkt);
    uint32_t len = mmpkt_get_data_length(v);
    struct netif *lwip_netif = esp_netif_get_netif_impl(esp_netif);
    struct pbuf *p = NULL;

    if (lwip_netif != NULL && netif_is_up(lwip_netif) && len > 0 && len <= 0xFFFF)
    {
        /* PBUF_LINK reserves PBUF_LINK_HLEN (14 B) of headroom; PBUF_RAM is contiguous. */
        p = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
        if (p != NULL)
        {
            memcpy(p->payload, mmpkt_get_data_start(v), len);
        }
    }

    mmpkt_close(&v);
    mmpkt_release(mmpkt);

    if (p != NULL && lwip_netif->input(p, lwip_netif) != ERR_OK)
    {
        pbuf_free(p);
    }
}

/* ---------------------------------------------------------------------------
 * Proxy ARP.
 *
 * The weak point of any layer-2 bridge is broadcast ARP resolution across it. A host
 * ARPs for a peer on the other side; the request is broadcast, and broadcast frames
 * are unacknowledged and therefore lossy. The reply then has to make the same crossing.
 * Resolution ends up hit-or-miss even though the data path is fine.
 *
 * The gate fixes this by snooping IP-to-MAC on both sides and answering directly. Two
 * mechanisms:
 *
 *   reactive  when an ARP REQUEST arrives for a host we know on the other side, reply
 *             immediately over the reliable link.
 *   proactive periodically teach each known host about every known host on the other
 *             side, over reliable unicast, so neither side ever needs to broadcast.
 *
 * Both answer with the target's REAL MAC, not the gate's. The requester therefore
 * still addresses the true peer and the bridge carries the actual traffic — the gate
 * only short-circuits the lossy resolution step.
 * ------------------------------------------------------------------------- */
#define ARP_TABLE_MAX 24
#define ARP_ENTRY_TTL_MS (5u * 60u * 1000u)

enum { SIDE_AP = 0, SIDE_MESH = 1 };

struct arp_entry
{
    uint32_t ip;
    uint8_t mac[6];
    uint8_t side;
    bool used;
    uint32_t last_ms;
};

static struct arp_entry g_arp[ARP_TABLE_MAX];

static uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void arp_learn(uint32_t ip, const uint8_t *mac, uint8_t side)
{
    if (ip == 0 || (mac[0] & 0x01))
    {
        return;   /* skip 0.0.0.0 and group MACs */
    }

    uint32_t now = now_ms();
    int free_i = -1;
    int lru_i = -1;

    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used)
        {
            if (g_arp[i].ip == ip)
            {
                memcpy(g_arp[i].mac, mac, 6);
                g_arp[i].side = side;
                g_arp[i].last_ms = now;
                return;
            }
            if (lru_i < 0 || (int32_t)(g_arp[i].last_ms - g_arp[lru_i].last_ms) < 0)
            {
                lru_i = i;
            }
        }
        else if (free_i < 0)
        {
            free_i = i;
        }
    }

    int slot = (free_i >= 0) ? free_i : lru_i;
    if (slot < 0)
    {
        return;
    }
    g_arp[slot].ip = ip;
    memcpy(g_arp[slot].mac, mac, 6);
    g_arp[slot].side = side;
    g_arp[slot].last_ms = now;
    g_arp[slot].used = true;   /* publish last, once the entry is fully populated */
}

static void arp_forget_mac(const uint8_t *mac)
{
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used && memcmp(g_arp[i].mac, mac, 6) == 0)
        {
            g_arp[i].used = false;
        }
    }
}

static bool arp_lookup(uint32_t ip, uint8_t *mac_out, uint8_t *side_out)
{
    uint32_t now = now_ms();
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used && g_arp[i].ip == ip)
        {
            if ((uint32_t)(now - g_arp[i].last_ms) > ARP_ENTRY_TTL_MS)
            {
                g_arp[i].used = false;
                return false;
            }
            memcpy(mac_out, g_arp[i].mac, 6);
            *side_out = g_arp[i].side;
            return true;
        }
    }
    return false;
}

/*
 * Learn a sender's IP-to-MAC from any frame, not just ARP. Snooping IPv4 as well means
 * a host is learned from its ordinary traffic — a single ping is enough — instead of
 * waiting up to a minute for its next gratuitous ARP.
 */
static void arp_snoop(const uint8_t *eth, uint32_t elen, uint8_t side)
{
    if (elen < 14)
    {
        return;
    }

    if (eth[12] == 0x08 && eth[13] == 0x06 && elen >= 14 + 28)          /* ARP */
    {
        uint32_t spa;
        memcpy(&spa, eth + 14 + 14, 4);          /* ARP sender protocol address */
        arp_learn(spa, eth + 14 + 8, side);      /* ARP sender hardware address */
    }
    else if (eth[12] == 0x08 && eth[13] == 0x00 && elen >= 14 + 20)     /* IPv4 */
    {
        uint32_t sip;
        memcpy(&sip, eth + 14 + 12, 4);          /* IP source address */
        arp_learn(sip, eth + 6, side);           /* Ethernet source MAC */
    }
}

/* Build a 42-byte Ethernet ARP REPLY telling req_mac/req_ip that tpa is at tpa_mac. */
static void arp_build_reply(uint8_t *out, const uint8_t *req_mac, uint32_t req_ip,
                            uint32_t tpa, const uint8_t *tpa_mac)
{
    memcpy(out, req_mac, 6);         /* Ethernet destination = the requester   */
    memcpy(out + 6, tpa_mac, 6);     /* Ethernet source = the answered host    */
    out[12] = 0x08; out[13] = 0x06;  /* ethertype ARP                          */
    out[14] = 0x00; out[15] = 0x01;  /* hardware type ethernet                 */
    out[16] = 0x08; out[17] = 0x00;  /* protocol type IPv4                     */
    out[18] = 6;    out[19] = 4;     /* hardware / protocol address lengths    */
    out[20] = 0x00; out[21] = 0x02;  /* operation = REPLY                      */
    memcpy(out + 22, tpa_mac, 6);    /* sender hardware = the answered host    */
    memcpy(out + 28, &tpa, 4);       /* sender protocol = the answered host IP */
    memcpy(out + 34, req_mac, 6);    /* target hardware = the requester        */
    memcpy(out + 38, &req_ip, 4);    /* target protocol = the requester IP     */
}

/* Build the same ARP reply as an LLC/SNAP mesh payload. */
static void arp_build_snap_reply(uint8_t *snap, const uint8_t *req_mac, uint32_t req_ip,
                                 uint32_t tpa, const uint8_t *tpa_mac)
{
    snap[0] = 0xaa; snap[1] = 0xaa; snap[2] = 0x03;
    snap[3] = 0x00; snap[4] = 0x00; snap[5] = 0x00;
    snap[6] = 0x08; snap[7] = 0x06;                 /* ethertype ARP */

    uint8_t *a = snap + SNAP_LEN;
    a[0] = 0; a[1] = 1;                             /* hardware type ethernet */
    a[2] = 0x08; a[3] = 0;                          /* protocol type IPv4     */
    a[4] = 6; a[5] = 4;                             /* address lengths        */
    a[6] = 0; a[7] = 2;                             /* operation = REPLY      */
    memcpy(a + 8, tpa_mac, 6);
    memcpy(a + 14, &tpa, 4);
    memcpy(a + 18, req_mac, 6);
    memcpy(a + 24, &req_ip, 4);
}

static void tx_ap_frame(const uint8_t *frame, uint32_t len)
{
    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(len, 0);
    if (pkt == NULL)
    {
        return;
    }
    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, frame, len);
    mmpkt_close(&v);

    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    md.vif = MMWLAN_VIF_AP;
    (void)mmwlan_tx_pkt(pkt, &md);
}

/*
 * If this frame is an ARP REQUEST for a host we know on the OTHER side, answer it over
 * the reliable link and return true — the caller then skips the lossy bridge for it.
 */
static bool proxy_arp(const uint8_t *eth, uint32_t elen, uint8_t from_side)
{
    if (elen < 14 + 28 || eth[12] != 0x08 || eth[13] != 0x06)
    {
        return false;   /* not ARP */
    }

    const uint8_t *arp = eth + 14;
    if (arp[6] != 0x00 || arp[7] != 0x01)
    {
        return false;   /* not a REQUEST */
    }

    uint32_t tpa;
    memcpy(&tpa, arp + 24, 4);

    uint8_t target_mac[6];
    uint8_t target_side;
    if (!arp_lookup(tpa, target_mac, &target_side) || target_side == from_side)
    {
        return false;   /* unknown, or on the same side as the requester */
    }

    uint32_t spa;
    memcpy(&spa, arp + 14, 4);
    const uint8_t *req_mac = arp + 8;

    if (from_side == SIDE_AP)
    {
        uint8_t reply[ARP_FRAME_LEN];   /* per call: no cross-task race with the push task */
        arp_build_reply(reply, req_mac, spa, tpa, target_mac);
        tx_ap_frame(reply, sizeof(reply));
    }
    else
    {
        uint8_t snap[SNAP_LEN + 28];
        arp_build_snap_reply(snap, req_mac, spa, tpa, target_mac);
        (void)mmwlan_mesh_tx_proxied(req_mac, target_mac, snap, sizeof(snap));
    }

    ESP_LOGI(TAG, "proxy-ARP: told %s " MACSTR " that %s%u is at " MACSTR,
             from_side == SIDE_AP ? "AP client" : "mesh node", MAC2STR(req_mac),
             SUBNET_PREFIX, ((const uint8_t *)&tpa)[3], MAC2STR(target_mac));
    return true;
}

/*
 * Proactive half. Reactive proxy ARP still depends on the requester's broadcast
 * reaching us. Periodically teaching every known host about the hosts on the other
 * side, over reliable unicast, removes that dependency entirely.
 *
 * Each push is addressed TO the host being taught — its MAC and its IP go in the
 * reply's target fields. That matters: lwIP adds a new neighbour entry from an
 * unsolicited reply only when the target protocol address is the receiver's own
 * (etharp_input passes ETHARP_FLAG_TRY_HARD when `for_us`, and the update-only
 * ETHARP_FLAG_FIND_ONLY otherwise). A reply addressed to anyone else would merely
 * refresh an entry that already existed, which is not enough to bootstrap resolution.
 */
static void arp_announce_once(void)
{
    uint32_t now = now_ms();
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used && (uint32_t)(now - g_arp[i].last_ms) > ARP_ENTRY_TTL_MS)
        {
            g_arp[i].used = false;
        }
    }

    int pushed = 0;
    for (int a = 0; a < ARP_TABLE_MAX; a++)
    {
        if (!g_arp[a].used)
        {
            continue;
        }
        for (int b = 0; b < ARP_TABLE_MAX; b++)
        {
            if (!g_arp[b].used || g_arp[a].side == g_arp[b].side)
            {
                continue;   /* only teach about the other side */
            }

            if (g_arp[a].side == SIDE_AP)
            {
                uint8_t reply[ARP_FRAME_LEN];
                arp_build_reply(reply, g_arp[a].mac, g_arp[a].ip, g_arp[b].ip, g_arp[b].mac);
                tx_ap_frame(reply, sizeof(reply));
            }
            else
            {
                uint8_t snap[SNAP_LEN + 28];
                arp_build_snap_reply(snap, g_arp[a].mac, g_arp[a].ip, g_arp[b].ip, g_arp[b].mac);
                (void)mmwlan_mesh_tx_proxied(g_arp[a].mac, g_arp[b].mac, snap, sizeof(snap));
            }
            pushed++;
        }
    }

    if (pushed > 0)
    {
        ESP_LOGD(TAG, "proxy-ARP: refreshed %d cross-bridge mapping(s)", pushed);
    }
}

static void arp_announce_task(void *arg)
{
    (void)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(PROXY_ARP_PUSH_MS));
        arp_announce_once();
    }
}

/* ---------------------------------------------------------------------------
 * Mesh receive: bridge group-addressed mesh frames onto the AP, then deliver locally.
 *
 * A broadcast from a mesh node (an ARP request for an AP client, say) is re-emitted on
 * the AP interface so clients see it. The datapath has already prepended the Ethernet
 * header, so those exact bytes go back out.
 *
 * This cannot loop: it only fires on mesh receive, and the mesh duplicate-suppression
 * and own-source-address drop already discard the gate's own re-broadcasts.
 * ------------------------------------------------------------------------- */
static uint8_t s_mesh_to_ap[MAX_ETH_FRAME];

static void mesh_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;

    struct mmpktview *v = mmpkt_open(mmpkt);
    const uint8_t *eth = mmpkt_get_data_start(v);
    uint32_t elen = mmpkt_get_data_length(v);
    uint32_t buflen = 0;

    if (eth != NULL && elen >= 14 && elen <= sizeof(s_mesh_to_ap))
    {
        memcpy(s_mesh_to_ap, eth, elen);   /* copy out, then close before any transmit */
        buflen = elen;
    }
    mmpkt_close(&v);

    bool answered = false;
    uint32_t bridge_len = 0;

    if (buflen > 0)
    {
        arp_snoop(s_mesh_to_ap, buflen, SIDE_MESH);
        answered = proxy_arp(s_mesh_to_ap, buflen, SIDE_MESH);
        if (!answered && (s_mesh_to_ap[0] & 0x01) && g_ap_client_n > 0)
        {
            bridge_len = buflen;   /* group-addressed and unanswered: bridge to the AP */
        }
    }

    if (answered)
    {
        mmpkt_release(mmpkt);   /* resolved reliably; neither bridge nor deliver */
        return;
    }

    if (bridge_len > 0)
    {
        struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(bridge_len, 0);
        if (pkt != NULL)
        {
            struct mmpktview *tv = mmpkt_open(pkt);
            mmpkt_append_data(tv, s_mesh_to_ap, bridge_len);
            mmpkt_close(&tv);

            struct mmwlan_tx_metadata tmd = MMWLAN_TX_METADATA_INIT;
            tmd.vif = MMWLAN_VIF_AP;
            if (mmwlan_tx_pkt(pkt, &tmd) == MMWLAN_SUCCESS)
            {
                ESP_LOGD(TAG, "bridged mesh->AP broadcast: %" PRIu32 " B from " MACSTR,
                         bridge_len, MAC2STR(s_mesh_to_ap + 6));
            }
        }
    }

    deliver_locally(mmpkt, (esp_netif_t *)arg);
}

/* ---------------------------------------------------------------------------
 * AP receive: proxy an AP client's traffic into the mesh.
 *
 * A unicast frame destined to a mesh node (not to the gate itself, not group-addressed)
 * is injected into the mesh as a proxied 6-address frame carrying the client as its
 * original source. A broadcast is injected as a proxied group frame so mesh nodes see
 * it and learn that the client is reachable via us. Either way the Ethernet payload
 * [dst][src][ethertype][L3] is re-encapsulated as [LLC/SNAP][L3] for the mesh.
 *
 * Frames addressed to the gate itself — DHCP, ARP or ICMP for the gate — fall through
 * to local delivery. Broadcasts do both, because a bridge floods to every port and the
 * gate's own stack may also want them.
 * ------------------------------------------------------------------------- */
static uint8_t s_ap_to_mesh[SNAP_LEN + 1514];

static void ap_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;

    struct mmpktview *v = mmpkt_open(mmpkt);
    const uint8_t *eth = mmpkt_get_data_start(v);
    uint32_t elen = mmpkt_get_data_length(v);

    if (eth != NULL && elen >= 14)
    {
        arp_snoop(eth, elen, SIDE_AP);
        if (proxy_arp(eth, elen, SIDE_AP))
        {
            mmpkt_close(&v);
            mmpkt_release(mmpkt);
            return;
        }
    }

    bool proxy_unicast = false;
    bool proxy_group = false;
    uint8_t dst[6];
    uint8_t src[6];
    uint32_t l3_len = 0;

    if (eth != NULL && elen >= 14)
    {
        l3_len = elen - 14;
        bool is_group = (eth[0] & 0x01);
        bool for_gate = (memcmp(eth, g_ap_bssid, 6) == 0 || memcmp(eth, g_mesh_mac, 6) == 0);

        if (l3_len <= sizeof(s_ap_to_mesh) - SNAP_LEN)
        {
            memcpy(dst, eth, 6);
            memcpy(src, eth + 6, 6);

            s_ap_to_mesh[0] = 0xaa; s_ap_to_mesh[1] = 0xaa; s_ap_to_mesh[2] = 0x03;
            s_ap_to_mesh[3] = 0x00; s_ap_to_mesh[4] = 0x00; s_ap_to_mesh[5] = 0x00;
            s_ap_to_mesh[6] = eth[12];
            s_ap_to_mesh[7] = eth[13];
            memcpy(s_ap_to_mesh + SNAP_LEN, eth + 14, l3_len);

            if (is_group)
            {
                proxy_group = true;
            }
            else if (!for_gate)
            {
                proxy_unicast = true;
            }
        }
    }
    mmpkt_close(&v);

    if (proxy_unicast)
    {
        if (mmwlan_mesh_tx_proxied(dst, src, s_ap_to_mesh, SNAP_LEN + l3_len))
        {
            ESP_LOGD(TAG, "proxied AP->mesh: %" PRIu32 " B from " MACSTR " to " MACSTR,
                     SNAP_LEN + l3_len, MAC2STR(src), MAC2STR(dst));
            mmpkt_release(mmpkt);
            return;
        }
        /* Nothing in the mesh routes this destination — it may be another AP client.
         * Fall through to local delivery rather than dropping it silently. */
    }

    if (proxy_group)
    {
        (void)mmwlan_mesh_tx_group_proxied(src, s_ap_to_mesh, SNAP_LEN + l3_len);
    }

    deliver_locally(mmpkt, (esp_netif_t *)arg);
}

/* ---------------------------------------------------------------------------
 * Proxied mesh frame arriving for one of our AP clients.
 *
 * The plain mesh receive callback cannot see the proxied endpoints — they live in the
 * Mesh Control field, which is stripped before that callback runs — so this dedicated
 * hook is what makes the mesh-to-AP direction possible.
 *
 * Returning true tells the datapath we consumed the frame, so it is NOT also delivered
 * into the gate's own lwIP stack. Without that, the gate would re-inject the frame back
 * into the mesh.
 * ------------------------------------------------------------------------- */
static uint8_t s_ae_to_ap[MAX_ETH_FRAME];

static bool mesh_ae_rx_cb(const uint8_t *eaddr1, const uint8_t *eaddr2,
                          const uint8_t *payload, uint32_t len, void *arg)
{
    (void)arg;

    if (!is_ap_client(eaddr1))
    {
        return false;   /* not for our AP: leave normal mesh delivery and relaying alone */
    }

    if (len >= SNAP_LEN && payload[0] == 0xaa && payload[1] == 0xaa && payload[2] == 0x03)
    {
        uint32_t l3_len = len - SNAP_LEN;
        if (l3_len <= sizeof(s_ae_to_ap) - 14)
        {
            memcpy(s_ae_to_ap, eaddr1, 6);        /* destination = the AP client        */
            memcpy(s_ae_to_ap + 6, eaddr2, 6);    /* source = the original off-mesh host */
            s_ae_to_ap[12] = payload[6];          /* ethertype recovered from the SNAP   */
            s_ae_to_ap[13] = payload[7];
            memcpy(s_ae_to_ap + 14, payload + SNAP_LEN, l3_len);

            arp_snoop(s_ae_to_ap, 14 + l3_len, SIDE_MESH);

            struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(14 + l3_len, 0);
            if (pkt != NULL)
            {
                struct mmpktview *v = mmpkt_open(pkt);
                mmpkt_append_data(v, s_ae_to_ap, 14 + l3_len);
                mmpkt_close(&v);

                struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
                md.vif = MMWLAN_VIF_AP;
                if (mmwlan_tx_pkt(pkt, &md) == MMWLAN_SUCCESS)
                {
                    ESP_LOGD(TAG, "delivered mesh->AP: %" PRIu32 " B to " MACSTR
                                  " (from " MACSTR ")",
                             14 + l3_len, MAC2STR(eaddr1), MAC2STR(eaddr2));
                }
            }
        }
    }

    return true;   /* ours, whether delivered or dropped — never fall through */
}

/* ------------------------------------------------------------------------- */

static void setup_ap_netif(void)
{
    /*
     * The AP netif carries a DHCP server so clients are zero-config on the flat subnet:
     * the gate hands out an address and the bridge plus proxy ARP let the client reach
     * any mesh node directly, with no route.
     *
     * The address goes in the INHERENT config, so the netif is born with it and the
     * automatic DHCP-server start sees a valid server address. Setting it afterwards
     * instead does not work: a 0.0.0.0 address makes the DHCP server fail to obtain a
     * PCB, and a later esp_netif_set_ip_info() then aborts with DHCP_NOT_STOPPED.
     *
     * The lease pool starts at .2 and spans CONFIG_LWIP_DHCPS_MAX_STATION_NUM leases,
     * so at the default of 8 it stays clear of the mesh nodes' .100 upwards. If you
     * raise that Kconfig towards 100, pin the range explicitly with
     * esp_netif_dhcps_option() to keep the pool below .100.
     */
    static esp_netif_ip_info_t ap_ip;   /* runtime-initialised: esp_ip4addr_aton is not const */
    ap_ip.ip.addr = esp_ip4addr_aton(g_ap_ipv4);
    ap_ip.netmask.addr = esp_ip4addr_aton(SUBNET_NETMASK);
    /* No router option. A pure layer-2 bridge does not route off-subnet, so handing
     * clients a default route here would only black-hole their traffic. */
    ap_ip.gw.addr = 0;

    esp_netif_inherent_config_t ap_base_cfg = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ap_ip,
        .get_ip_event = 0,
        .lost_ip_event = 0,
        .if_key = "HALOW_GWAP",
        .if_desc = "gw-ap",
        .route_prio = 50,
        .bridge_info = NULL,
    };
    esp_netif_config_t ap_cfg = {
        .base = &ap_base_cfg,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,   /* generic ethernet glue */
    };

    g_ap_netif = esp_netif_new(&ap_cfg);
    assert(g_ap_netif != NULL);

    g_ap_driver_base.post_attach = ap_driver_post_attach;
    ESP_ERROR_CHECK(esp_netif_attach(g_ap_netif, &g_ap_driver_base));

    /*
     * The AP netif's link-layer address must be the AP interface's BSSID, so ARP on the
     * subnet resolves to it. Fail fast if it is unavailable: a zeroed g_ap_bssid would
     * make ap_rx_cb misclassify a frame addressed to the gate's own AP MAC as
     * client-to-mesh and wrongly proxy it.
     */
    uint8_t bssid[MMWLAN_MAC_ADDR_LEN] = { 0 };
    ESP_ERROR_CHECK(mmwlan_ap_get_bssid(bssid) == MMWLAN_SUCCESS ? ESP_OK : ESP_FAIL);
    esp_netif_set_mac(g_ap_netif, bssid);
    memcpy(g_ap_bssid, bssid, 6);

    /*
     * Create and attach the underlying lwIP netif and its input path, then bring it up.
     * Without action_start the receive path would dereference a NULL input function on
     * the first frame. AUTOUP plus the inherent address starts the DHCP server.
     */
    esp_netif_action_start(g_ap_netif, NULL, 0, NULL);
    esp_netif_action_connected(g_ap_netif, NULL, 0, NULL);   /* no link event in AP mode */

    ESP_LOGI(TAG, "AP netif up: %s/24 with DHCP server, MAC " MACSTR, g_ap_ipv4, MAC2STR(bssid));
}

static void mesh_net_task(void *arg)
{
    (void)arg;

    esp_netif_t *netif = g_mesh_netif;
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "mesh netif not found");
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < 60 && !esp_netif_is_netif_up(netif); i++)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_netif_dhcpc_stop(netif);
    esp_netif_set_mac(netif, g_mesh_mac);

    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), SUBNET_PREFIX "%u", 100u + (g_mesh_mac[5] & 0x3fu));

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.netmask.addr = esp_ip4addr_aton(SUBNET_NETMASK);
    ip.gw.addr = 0;   /* the gate is a layer-2 bridge, not a router */
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    ESP_LOGI(TAG, "mesh netif static IP %s", ipbuf);
    ESP_LOGI(TAG, "bridge ready: AP clients and mesh nodes share %s0/24", SUBNET_PREFIX);

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== mesh-gate: 802.11s mesh + SoftAP bridged on one MM6108 ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    snprintf(g_ap_ipv4, sizeof(g_ap_ipv4), SUBNET_PREFIX "1");

    /* A unique mesh MAC per board: the efuse MAC with the locally-administered bit set.
     * The MM6108's factory MAC is shared across modules and would collide on the air. */
    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);            /* creates the primary (mesh) netif and boots morselib */
    mmhalow_print_version_info();
    g_mesh_netif = mmhalow_get_netif();

    /* --- 1. mesh on the primary interface --------------------------------- */
    struct mmwlan_mesh_args mesh_args = { 0 };
    memcpy(mesh_args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(mesh_args.mesh_id, MESH_ID, strlen(MESH_ID));
    mesh_args.mesh_id_len = strlen(MESH_ID);
    mesh_args.s1g_chan_num = S1G_CHANNEL;
    mesh_args.beacon_interval_tu = 100;
    mesh_args.max_plinks = MESH_MAX_PLINKS;

    ESP_LOGI(TAG, "starting mesh (id=\"%s\" chan=%d mac=" MACSTR ")",
             MESH_ID, S1G_CHANNEL, MAC2STR(g_mesh_mac));

    enum mmwlan_status status = mmwlan_mesh_start(&mesh_args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "mmwlan_mesh_start failed, status=%d", (int)status);
        return;
    }
    ESP_LOGI(TAG, "mesh interface up (primary)");

    /*
     * Advertise as a discoverable gate: flood a proactive root announcement carrying
     * the IS_GATE flag. This mirrors dot11MeshHWMPRootMode = PROACTIVE_RANN plus
     * dot11MeshGateAnnouncementProtocol on a Linux gate, and is what lets a mesh node
     * that has never seen our traffic discover a way off the mesh.
     */
    mmwlan_mesh_set_root_announcements(true, true, RANN_INTERVAL_MS);
    ESP_LOGI(TAG, "gate announcements on (IS_GATE, every %d ms)", RANN_INTERVAL_MS);

    /* --- 2. SoftAP on the secondary interface ------------------------------ */
    struct mmwlan_ap_args ap_args = MMWLAN_AP_ARGS_INIT;
    memcpy((char *)ap_args.ssid, AP_SSID, strlen(AP_SSID));
    ap_args.ssid_len = strlen(AP_SSID);
    memcpy(ap_args.passphrase, AP_PSK, strlen(AP_PSK));
    ap_args.passphrase_len = strlen(AP_PSK);
    ap_args.security_type = MMWLAN_SAE;
    ap_args.pmf_mode = MMWLAN_PMF_REQUIRED;
    ap_args.s1g_chan_num = S1G_CHANNEL;
    ap_args.op_class = S1G_OPCLASS;
    ap_args.max_stas = AP_MAX_STAS;
    ap_args.sta_status_cb = ap_sta_status_cb;

    ESP_LOGI(TAG, "starting SoftAP \"%s\" alongside the mesh on channel %d",
             AP_SSID, S1G_CHANNEL);

    status = mmwlan_ap_enable(&ap_args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "mmwlan_ap_enable failed, status=%d (the mesh stays up)", (int)status);
        return;
    }
    ESP_LOGI(TAG, "AP interface up (secondary), concurrent with the mesh");

    /* --- 3. bridge wiring -------------------------------------------------- */
    setup_ap_netif();

    /* Replace mmhalow's single receive callback with per-interface ones. Mesh traffic
     * arrives on the VIF_STA slot, AP traffic on VIF_AP. */
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA, mesh_rx_cb, g_mesh_netif)
                    != MMWLAN_SUCCESS);
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_AP, ap_rx_cb, g_ap_netif)
                    != MMWLAN_SUCCESS);
    /* And the hook that can see a proxied frame's endpoints, for the mesh-to-AP leg. */
    mmwlan_mesh_register_ae_rx_cb(mesh_ae_rx_cb, NULL);

    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

#if CONFIG_EXAMPLE_PROXY_ARP_PUSH_MS > 0
    xTaskCreate(arp_announce_task, "gate_arp", 4096, NULL, 4, NULL);
#else
    ESP_LOGW(TAG, "proactive proxy-ARP disabled — cross-bridge resolution will depend "
                  "on lossy broadcast ARP");
#endif

    ESP_LOGI(TAG, "gate running: mesh SA " MACSTR ", AP BSSID " MACSTR,
             MAC2STR(g_mesh_mac), MAC2STR(g_ap_bssid));
}
