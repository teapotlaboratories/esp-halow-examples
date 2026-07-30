/*
 * gate_bridge.c — the layer-2 bridge datapath.
 *
 * Three receive callbacks move frames between the mesh and the AP:
 *
 *   mesh_rx_cb     mesh -> AP for GROUP-addressed frames, then local delivery
 *   ap_rx_cb       AP -> mesh, proxying a client's unicast or broadcast into the mesh
 *   mesh_ae_rx_cb  mesh -> AP for a proxied frame whose final destination is our client
 *
 * PER-INTERFACE WIRING IS LOAD-BEARING. morselib delivers mesh receives to the
 * MMWLAN_VIF_STA callback slot and AP receives to MMWLAN_VIF_AP, and routes transmits by
 * metadata.vif. The mesh netif must therefore be fed from, and tagged, VIF_STA and the AP
 * netif VIF_AP. Getting this backwards silently drops one direction — no error, no log.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "gate.h"
#include "umac/mesh/umac_mesh.h"

/* ---------------------------------------------------------------------------
 * Associated AP clients.
 *
 * The gate needs to know which MACs are on its AP side, so it can tell whether an
 * arriving proxied mesh frame is destined for one of its own clients. Written by the AP
 * status callback and read from the mesh receive task, so a short critical section keeps a
 * reader from seeing a torn slot or a stale count mid-removal. Nothing that blocks or
 * transmits runs inside it.
 * ------------------------------------------------------------------------- */
#define GATE_MAX_CLIENTS 8

/*
 * This table is the gate's REAL client ceiling, and it must not be smaller than the
 * number of stations the AP is told to admit.
 *
 * If a client associates once the table is full the add is silently skipped, and because
 * only the mesh->AP direction consults the table (is_ap_client, below) that client ends up
 * ONE-WAY: it associates, takes a DHCP lease, and can send into the mesh, but never
 * receives a unicast reply. Nothing logs an error — the running total simply stops
 * climbing while "AP client joined" keeps appearing.
 *
 * The Kconfig range on EXAMPLE_AP_MAX_STAS mirrors this constant, so the two agree by
 * construction today. This assert is what keeps them agreeing if someone widens that
 * range without noticing the coupling.
 *
 * Raising the ceiling meaningfully is not just a bigger array: is_ap_client() runs on
 * EVERY received proxied frame inside a critical section, so a long linear scan would hold
 * interrupts off on the datapath. Past a handful of clients this wants a hash — the same
 * move the mesh path table made when it outgrew 8 entries.
 */
_Static_assert(AP_MAX_STAS <= GATE_MAX_CLIENTS,
               "EXAMPLE_AP_MAX_STAS exceeds GATE_MAX_CLIENTS: clients past the table would "
               "associate but never receive mesh traffic. Raise GATE_MAX_CLIENTS too.");

static uint8_t g_ap_clients[GATE_MAX_CLIENTS][6];
static int g_ap_client_n;
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

int gate_bridge_client_count(void)
{
    taskENTER_CRITICAL(&g_ap_clients_mux);
    int n = g_ap_client_n;
    taskEXIT_CRITICAL(&g_ap_clients_mux);
    return n;
}

int gate_bridge_client_list(uint8_t macs[][6], int cap)
{
    taskENTER_CRITICAL(&g_ap_clients_mux);
    int n = (g_ap_client_n < cap) ? g_ap_client_n : cap;
    for (int i = 0; i < n; i++)
    {
        memcpy(macs[i], g_ap_clients[i], 6);
    }
    taskEXIT_CRITICAL(&g_ap_clients_mux);
    return n;
}

void gate_bridge_ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
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
        ESP_LOGI(GATE_TAG, "AP client joined " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
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
        gate_arp_forget_mac(st->mac_addr);   /* stop advertising a departed host */
        ESP_LOGI(GATE_TAG, "AP client left   " MACSTR " (%d total)", MAC2STR(st->mac_addr), n);
    }
}

/* ---------------------------------------------------------------------------
 * Reframing scratch.
 *
 * These are `static`, not ~1.5 KB stack frames, which would risk overflowing a receive
 * task's stack. That is safe only under a no-concurrent-writers invariant, and it is worth
 * stating because it is the assumption most easily broken by building on this example:
 *
 *   s_mesh_to_ap, s_ae_to_ap   written only on the MESH receive context
 *   s_ap_to_mesh               written only on the AP receive context
 *
 * Each is written and consumed synchronously within one callback, and no buffer is touched
 * from both contexts. morselib's own proxy scratch relies on the same single-datapath-
 * context invariant. If you ever drive either path from a second context, these must
 * become per-call.
 * ------------------------------------------------------------------------- */
static uint8_t s_mesh_to_ap[MAX_ETH_FRAME];
static uint8_t s_ap_to_mesh[SNAP_LEN + 1514];
static uint8_t s_ae_to_ap[MAX_ETH_FRAME];

/*
 * Mesh receive: bridge group-addressed mesh frames onto the AP, then deliver locally.
 *
 * A broadcast from a mesh node (an ARP request for an AP client, say) is re-emitted on the
 * AP interface so clients see it. The datapath has already prepended the Ethernet header,
 * so those exact bytes go back out.
 *
 * This cannot loop: it only fires on mesh receive, and the mesh duplicate-suppression and
 * own-source-address drop already discard the gate's own re-broadcasts.
 */
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
        gate_arp_snoop(s_mesh_to_ap, buflen, SIDE_MESH);
        answered = gate_arp_proxy(s_mesh_to_ap, buflen, SIDE_MESH);
        if (!answered && (s_mesh_to_ap[0] & 0x01) && gate_bridge_client_count() > 0)
        {
            bridge_len = buflen;   /* group-addressed and unanswered: bridge to the AP */
        }
    }

    if (answered)
    {
        mmpkt_release(mmpkt);   /* resolved reliably; neither bridge nor deliver */
        return;
    }

    if (bridge_len > 0 && gate_netif_tx_ap(s_mesh_to_ap, bridge_len))
    {
        ESP_LOGD(GATE_TAG, "bridged mesh->AP broadcast: %" PRIu32 " B from " MACSTR,
                 bridge_len, MAC2STR(s_mesh_to_ap + 6));
    }

    gate_netif_deliver_locally(mmpkt, (esp_netif_t *)arg);
}

/*
 * AP receive: proxy an AP client's traffic into the mesh.
 *
 * A unicast frame destined to a mesh node (not to the gate itself, not group-addressed) is
 * injected into the mesh as a proxied 6-address frame carrying the client as its original
 * source. A broadcast is injected as a proxied group frame so mesh nodes see it and learn
 * that the client is reachable via us. Either way the Ethernet payload
 * [dst][src][ethertype][L3] is re-encapsulated as [LLC/SNAP][L3] for the mesh.
 *
 * Frames addressed to the gate itself — DHCP, ARP or ICMP for the gate — fall through to
 * local delivery. Broadcasts do both, because a bridge floods to every port and the gate's
 * own stack may also want them.
 */
static void ap_rx_cb(struct mmpkt *mmpkt, const struct mmwlan_rx_metadata *md, void *arg)
{
    (void)md;

    struct mmpktview *v = mmpkt_open(mmpkt);
    const uint8_t *eth = mmpkt_get_data_start(v);
    uint32_t elen = mmpkt_get_data_length(v);

    if (eth != NULL && elen >= 14)
    {
        gate_arp_snoop(eth, elen, SIDE_AP);
        if (gate_arp_proxy(eth, elen, SIDE_AP))
        {
            mmpkt_close(&v);
            mmpkt_release(mmpkt);
            return;   /* resolved reliably — skip the lossy bridge */
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
            ESP_LOGD(GATE_TAG, "proxied AP->mesh: %" PRIu32 " B from " MACSTR " to " MACSTR,
                     SNAP_LEN + l3_len, MAC2STR(src), MAC2STR(dst));
            mmpkt_release(mmpkt);
            return;
        }
        /* Nothing in the mesh routes this destination — it may be another AP client. Fall
         * through to local delivery rather than dropping it silently. */
    }

    if (proxy_group)
    {
        (void)mmwlan_mesh_tx_group_proxied(src, s_ap_to_mesh, SNAP_LEN + l3_len);
    }

    gate_netif_deliver_locally(mmpkt, (esp_netif_t *)arg);
}

/*
 * A proxied mesh frame arriving for one of our AP clients.
 *
 * The plain mesh receive callback cannot see the proxied endpoints — they live in the Mesh
 * Control field, which is stripped before that callback runs — so this dedicated hook is
 * what makes the mesh-to-AP direction possible.
 *
 * Returning true tells the datapath we consumed the frame, so it is NOT also delivered
 * into the gate's own lwIP stack. Without that, the gate would re-inject it into the mesh.
 */
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
            memcpy(s_ae_to_ap, eaddr1, 6);        /* destination = the AP client          */
            memcpy(s_ae_to_ap + 6, eaddr2, 6);    /* source = the original off-mesh host  */
            s_ae_to_ap[12] = payload[6];          /* ethertype recovered from the SNAP    */
            s_ae_to_ap[13] = payload[7];
            memcpy(s_ae_to_ap + 14, payload + SNAP_LEN, l3_len);

            gate_arp_snoop(s_ae_to_ap, 14 + l3_len, SIDE_MESH);

            if (gate_netif_tx_ap(s_ae_to_ap, 14 + l3_len))
            {
                ESP_LOGD(GATE_TAG, "delivered mesh->AP: %" PRIu32 " B to " MACSTR
                                   " (from " MACSTR ")",
                         14 + l3_len, MAC2STR(eaddr1), MAC2STR(eaddr2));
            }
        }
    }

    return true;   /* ours, whether delivered or dropped — never fall through */
}

void gate_bridge_register(void)
{
    /* Replace mmhalow's single receive callback with per-interface ones. Mesh traffic
     * arrives on the VIF_STA slot, AP traffic on VIF_AP. */
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_STA, mesh_rx_cb, g_mesh_netif)
                    != MMWLAN_SUCCESS);
    ESP_ERROR_CHECK(mmwlan_register_rx_pkt_ext_cb(MMWLAN_VIF_AP, ap_rx_cb, g_ap_netif)
                    != MMWLAN_SUCCESS);
    /* And the hook that can see a proxied frame's endpoints, for the mesh-to-AP leg. */
    mmwlan_mesh_register_ae_rx_cb(mesh_ae_rx_cb, NULL);
}
