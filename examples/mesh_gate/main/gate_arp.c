/*
 * gate_arp.c — proxy ARP across the layer-2 bridge.
 *
 * The weak point of any layer-2 bridge is broadcast ARP resolution across it. A host
 * ARPs for a peer on the other side; the request is broadcast, and broadcast frames are
 * unacknowledged and therefore lossy. The reply then has to make the same crossing.
 * Resolution ends up hit-or-miss even though the data path is fine.
 *
 * The gate fixes this by snooping IP-to-MAC on both sides and answering directly:
 *
 *   reactive   an ARP REQUEST for a host known on the other side is answered
 *              immediately over the reliable link (gate_arp_proxy)
 *   proactive  every known host is periodically taught about the hosts on the other
 *              side over reliable unicast, so neither side ever needs to broadcast
 *              across the bridge at all (gate_arp_start_announce)
 *
 * Both answer with the target's REAL MAC, not the gate's. The requester therefore still
 * addresses the true peer and the bridge carries the actual traffic — the gate only
 * short-circuits the lossy resolution step.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "gate.h"
#include "umac/mesh/umac_mesh.h"

#define ARP_TABLE_MAX     24
#define ARP_ENTRY_TTL_MS  (5u * 60u * 1000u)

struct arp_entry
{
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  side;
    bool     used;
    uint32_t last_ms;
};

static struct arp_entry g_arp[ARP_TABLE_MAX];

/*
 * The table has THREE concurrent accessors — the mesh receive task and the AP receive
 * task both snoop into it, and the announce task ages and reads it — so every access is
 * guarded. Without this, two receive contexts can interleave on the same slot and
 * publish a mismatched IP/MAC pair, which is exactly the kind of fault that shows up as
 * "the bridge works, mostly".
 *
 * Critical sections here only ever scan or copy 24 fixed-size slots: no allocation, no
 * transmit, nothing blocking. The proactive push, which does transmit, snapshots the
 * table under the lock and releases it before sending a single frame.
 */
static portMUX_TYPE g_arp_mux = portMUX_INITIALIZER_UNLOCKED;

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

    taskENTER_CRITICAL(&g_arp_mux);
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
                taskEXIT_CRITICAL(&g_arp_mux);
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
    if (slot >= 0)
    {
        g_arp[slot].ip = ip;
        memcpy(g_arp[slot].mac, mac, 6);
        g_arp[slot].side = side;
        g_arp[slot].last_ms = now;
        g_arp[slot].used = true;   /* publish last, once the entry is fully populated */
    }
    taskEXIT_CRITICAL(&g_arp_mux);
}

void gate_arp_forget_mac(const uint8_t *mac)
{
    taskENTER_CRITICAL(&g_arp_mux);
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used && memcmp(g_arp[i].mac, mac, 6) == 0)
        {
            g_arp[i].used = false;
        }
    }
    taskEXIT_CRITICAL(&g_arp_mux);
}

bool gate_arp_resolve(uint32_t ip, uint8_t *mac_out, uint8_t *side_out)
{
    uint32_t now = now_ms();
    bool found = false;

    taskENTER_CRITICAL(&g_arp_mux);
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used && g_arp[i].ip == ip)
        {
            if ((uint32_t)(now - g_arp[i].last_ms) > ARP_ENTRY_TTL_MS)
            {
                g_arp[i].used = false;   /* expired on read */
                break;
            }
            if (mac_out != NULL)
            {
                memcpy(mac_out, g_arp[i].mac, 6);
            }
            if (side_out != NULL)
            {
                *side_out = g_arp[i].side;
            }
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_arp_mux);
    return found;
}

int gate_arp_entry_count(void)
{
    int n = 0;
    taskENTER_CRITICAL(&g_arp_mux);
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (g_arp[i].used)
        {
            n++;
        }
    }
    taskEXIT_CRITICAL(&g_arp_mux);
    return n;
}

/*
 * Learn a sender's IP-to-MAC from any frame, not just ARP. Snooping IPv4 as well means a
 * host is learned from its ordinary traffic — a single ping is enough — instead of
 * waiting up to a minute for its next gratuitous ARP.
 */
void gate_arp_snoop(const uint8_t *eth, uint32_t elen, uint8_t side)
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
        memcpy(&sip, eth + 14 + 12, 4);          /* IP header source address */
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

/* The same ARP reply as an LLC/SNAP mesh payload, for the proxied-unicast path. */
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

/* Teach `to_mac`/`to_ip` on `to_side` that `host_ip` is at `host_mac`. */
static void arp_teach(uint8_t to_side, const uint8_t *to_mac, uint32_t to_ip,
                      uint32_t host_ip, const uint8_t *host_mac)
{
    if (to_side == SIDE_AP)
    {
        uint8_t reply[ARP_FRAME_LEN];   /* per call — no cross-task shared buffer */
        arp_build_reply(reply, to_mac, to_ip, host_ip, host_mac);
        (void)gate_netif_tx_ap(reply, sizeof(reply));
    }
    else
    {
        uint8_t snap[SNAP_LEN + 28];
        arp_build_snap_reply(snap, to_mac, to_ip, host_ip, host_mac);
        (void)mmwlan_mesh_tx_proxied(to_mac, host_mac, snap, sizeof(snap));
    }
}

bool gate_arp_proxy(const uint8_t *eth, uint32_t elen, uint8_t from_side)
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
    if (!gate_arp_resolve(tpa, target_mac, &target_side) || target_side == from_side)
    {
        return false;   /* unknown, or on the same side as the requester */
    }

    uint32_t spa;
    memcpy(&spa, arp + 14, 4);
    const uint8_t *req_mac = arp + 8;

    arp_teach(from_side, req_mac, spa, tpa, target_mac);

    ESP_LOGI(GATE_TAG, "proxy-ARP: told %s " MACSTR " that %s%u is at " MACSTR,
             from_side == SIDE_AP ? "AP client" : "mesh node", MAC2STR(req_mac),
             SUBNET_PREFIX, ((const uint8_t *)&tpa)[3], MAC2STR(target_mac));
    return true;
}

/*
 * Proactive half. Reactive proxy ARP still depends on the requester's broadcast reaching
 * us. Periodically teaching every known host about the hosts on the other side, over
 * reliable unicast, removes that dependency entirely.
 *
 * Each push is addressed TO the host being taught — its MAC and IP go in the reply's
 * target fields. That matters: lwIP adds a NEW neighbour entry from an unsolicited reply
 * only when the target protocol address is the receiver's own (etharp_input passes
 * ETHARP_FLAG_TRY_HARD when `for_us`, and the update-only ETHARP_FLAG_FIND_ONLY
 * otherwise). A reply addressed to anyone else would merely refresh an entry that
 * already existed, which cannot bootstrap resolution.
 *
 * Transmitting must not happen under the table lock, so the table is aged and snapshotted
 * in one critical section and every frame is sent afterwards from the snapshot.
 */
struct arp_view
{
    uint32_t ip;
    uint8_t  mac[6];
    uint8_t  side;
};

static void arp_announce_once(void)
{
    struct arp_view view[ARP_TABLE_MAX];
    int n = 0;
    uint32_t now = now_ms();

    taskENTER_CRITICAL(&g_arp_mux);
    for (int i = 0; i < ARP_TABLE_MAX; i++)
    {
        if (!g_arp[i].used)
        {
            continue;
        }
        if ((uint32_t)(now - g_arp[i].last_ms) > ARP_ENTRY_TTL_MS)
        {
            g_arp[i].used = false;   /* age out departed/stale hosts */
            continue;
        }
        view[n].ip = g_arp[i].ip;
        memcpy(view[n].mac, g_arp[i].mac, 6);
        view[n].side = g_arp[i].side;
        n++;
    }
    taskEXIT_CRITICAL(&g_arp_mux);

    int pushed = 0;
    for (int a = 0; a < n; a++)
    {
        for (int b = 0; b < n; b++)
        {
            if (view[a].side == view[b].side)
            {
                continue;   /* only teach about the other side */
            }
            arp_teach(view[a].side, view[a].mac, view[a].ip, view[b].ip, view[b].mac);
            pushed++;
        }
    }

    if (pushed > 0)
    {
        ESP_LOGD(GATE_TAG, "proxy-ARP: refreshed %d cross-bridge mapping(s)", pushed);
    }
}

static void arp_announce_task(void *arg)
{
    uint32_t period_ms = (uint32_t)(uintptr_t)arg;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(period_ms));
        arp_announce_once();
    }
}

void gate_arp_start_announce(uint32_t period_ms)
{
    if (period_ms == 0)
    {
        ESP_LOGW(GATE_TAG, "proactive proxy-ARP disabled — cross-bridge resolution will "
                           "depend on lossy broadcast ARP");
        return;
    }
    xTaskCreate(arp_announce_task, "gate_arp", 4096, (void *)(uintptr_t)period_ms, 4, NULL);
}
