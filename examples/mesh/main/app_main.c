/*
 * mesh — 802.11s secured mesh point on an MM6108.
 *
 * Brings up a single 802.11s mesh interface, joins the mesh, pins a static IP, and
 * logs its established peers. Peering (MPM), authentication (SAE), key exchange
 * (AMPE), data encryption (CCMP), multi-hop forwarding and HWMP path selection all
 * happen inside morselib — a peer link forms on its own as soon as a neighbour
 * running the same Mesh ID on the same channel is heard.
 *
 * One binary runs on every node. Each derives a unique mesh MAC from its own ESP32
 * efuse MAC and a static IP host octet from that MAC, so the same image scales to as
 * many boards as you flash it to, and interoperates with a Linux 802.11s node.
 *
 * Two runtime options are exposed in menuconfig:
 *   EXAMPLE_MESH_AMPDU   A-MPDU aggregation (must be set before the vif comes up)
 *   EXAMPLE_MESH_LEAF    leaf / single-hop mode: peer, but never relay for others
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
#include "esp_system.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/mesh/umac_mesh.h"

#define MESH_ID          CONFIG_EXAMPLE_MESH_ID
#define MESH_S1G_CHAN    CONFIG_EXAMPLE_MESH_S1G_CHANNEL
#define MESH_MAX_PLINKS  CONFIG_EXAMPLE_MESH_MAX_PLINKS
#define MESH_IPV4_PREFIX CONFIG_EXAMPLE_MESH_IPV4_PREFIX
#define MESH_NETMASK     "255.255.255.0"

#define HEARTBEAT_S 5

static const char *TAG = "mesh";

/* This node's mesh MAC, derived from the ESP32 efuse MAC so it is unique per board. */
static uint8_t g_mesh_mac[6];

/* Read-only accessor exported by morselib: 1 = the firmware advertises the A-MPDU
 * capability on this vif, 0 = it does not, -1 = not yet initialised. */
extern int mmwlan_ampdu_capability_advertised(void);

/*
 * Pin a static mesh IP once the interface is up. A plain mesh has no DHCP server, so
 * every node addresses itself; the host octet comes from the mesh MAC to keep one
 * binary running unmodified on every board.
 */
static void mesh_net_task(void *arg)
{
    (void)arg;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
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

    /*
     * The mesh vif transmits using g_mesh_mac, so the netif (and therefore ARP) has to
     * use the same address. If L2 and L3 disagree, peers learn our IP against the wrong
     * MAC and their replies never reach the mesh interface.
     */
    esp_netif_set_mac(netif, g_mesh_mac);

    char ipbuf[20];
    snprintf(ipbuf, sizeof(ipbuf), MESH_IPV4_PREFIX "%u", 100u + (g_mesh_mac[5] & 0x3fu));

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(ipbuf);
    ip.netmask.addr = esp_ip4addr_aton(MESH_NETMASK);
    ip.gw.addr = 0;   /* flat single subnet: a plain mesh node has no gateway */
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    ESP_LOGI(TAG, "static IP %s/24 (netif up=%d)", ipbuf, (int)esp_netif_is_netif_up(netif));
    ESP_LOGI(TAG, "ping any other node on this mesh at " MESH_IPV4_PREFIX "<100 + (mac[5] & 0x3f)>");

    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11s secured mesh point ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    /*
     * A unique mesh MAC per board: the ESP32 efuse MAC with the locally-administered
     * bit set. The MM6108's own factory MAC cannot be used here — it is shared across
     * modules, so two boards would collide on the air.
     */
    esp_read_mac(g_mesh_mac, ESP_MAC_WIFI_STA);
    g_mesh_mac[0] = (g_mesh_mac[0] | 0x02) & 0xFE;

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    /*
     * A-MPDU aggregation. mmwlan_set_ampdu_enabled() may only be called while MMWLAN is
     * inactive, so this has to happen before the mesh interface is started. Aggregation
     * is assembled by the MM6108 firmware; each peer negotiates it over an ADDBA
     * handshake once the link is up.
     */
#if CONFIG_EXAMPLE_MESH_AMPDU
    const bool want_ampdu = true;
#else
    const bool want_ampdu = false;
#endif
    enum mmwlan_status ampdu = mmwlan_set_ampdu_enabled(want_ampdu);
    if (ampdu == MMWLAN_SUCCESS)
    {
        ESP_LOGI(TAG, "A-MPDU aggregation %s", want_ampdu ? "enabled" : "disabled");
    }
    else
    {
        /* Check this rather than only logging it. The call returns MMWLAN_UNAVAILABLE
         * unless the connection state is MMWLAN_STA_DISABLED, and aggregation defaults
         * to ENABLED — so a failure here silently leaves it on, which would make a
         * deliberate `disabled` configuration look like it took effect when it did not. */
        ESP_LOGW(TAG, "mmwlan_set_ampdu_enabled(%d) returned %d — aggregation is UNCHANGED "
                      "(it defaults to enabled), so this node is not in the requested state",
                 (int)want_ampdu, (int)ampdu);
    }

    /*
     * Leaf mode. Runtime-settable before or after mmwlan_mesh_start(); setting it first
     * means the node never relays anything, not even during bring-up.
     */
#if CONFIG_EXAMPLE_MESH_LEAF
    mmwlan_mesh_set_multihop(false);
    ESP_LOGI(TAG, "leaf mode: this node peers but will not forward for others");
#endif

    struct mmwlan_mesh_args args = { 0 };
    memcpy(args.if_addr, g_mesh_mac, sizeof(g_mesh_mac));
    memcpy(args.mesh_id, MESH_ID, strlen(MESH_ID));
    args.mesh_id_len = strlen(MESH_ID);
    args.s1g_chan_num = MESH_S1G_CHAN;
    args.beacon_interval_tu = 100;
    args.max_plinks = MESH_MAX_PLINKS;

    ESP_LOGI(TAG, "starting mesh (id=\"%s\" chan=%d mac=" MACSTR ")",
             MESH_ID, MESH_S1G_CHAN, MAC2STR(g_mesh_mac));

    enum mmwlan_status status = mmwlan_mesh_start(&args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "mmwlan_mesh_start failed, status=%d", (int)status);
        return;
    }
    ESP_LOGI(TAG, "mesh interface up, beaconing on channel %d", MESH_S1G_CHAN);
    ESP_LOGI(TAG, "firmware advertises A-MPDU: %d", mmwlan_ampdu_capability_advertised());

    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);

    /* Heartbeat: which peers this node has established a mesh peer link with. */
    uint32_t uptime_s = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uptime_s++;
        if (uptime_s % HEARTBEAT_S != 0)
        {
            continue;
        }

        uint8_t peers[UMAC_MESH_MAX_PEERS][6] = { { 0 } };
        uint8_t n_peers = mmwlan_mesh_peer_count(peers);

        ESP_LOGI(TAG, "uptime=%" PRIu32 "s established_peers=%u gates_known=%u heap=%" PRIu32,
                 uptime_s, (unsigned)n_peers, (unsigned)mmwlan_mesh_gate_count(),
                 esp_get_free_heap_size());

        for (uint8_t i = 0; i < n_peers; i++)
        {
            ESP_LOGI(TAG, "  peer[%u] " MACSTR, i, MAC2STR(peers[i]));
        }
    }
}
