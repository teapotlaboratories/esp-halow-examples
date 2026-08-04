/*
 * mesh_gate — an 802.11s mesh and a co-channel SoftAP on ONE MM6108, bridged at layer 2
 * so both sides share a single flat subnet.
 *
 * A gate lets ordinary Wi-Fi HaLow stations reach a mesh without knowing anything about
 * it. An AP client associates, DHCPs an address, and talks to any mesh node directly — no
 * routing, no second subnet, no static routes, nothing to configure on the client.
 *
 * That works because the bridge is at layer 2, using 802.11s Address Extension: a frame
 * from an AP client is injected into the mesh as a 6-address proxied frame carrying the
 * client as its original source, and a proxied frame arriving for one of our clients is
 * delivered onto the AP interface. Mesh nodes learn "that host is reachable via this gate"
 * from the frames themselves, exactly as they would from a Linux gate.
 *
 * This file is only the bring-up order, which matters — the mesh owns the primary
 * interface:
 *
 *     mmhalow_init  ->  mmwlan_mesh_start  ->  mmwlan_ap_enable  ->  bridge wiring
 *
 * The parts worth reading separately:
 *
 *     gate_netif.c   the AP-side esp_netif, local delivery, addressing
 *     gate_bridge.c  the datapath — the receive callbacks that move frames across
 *     gate_arp.c     proxy ARP, which is what makes resolution across the bridge reliable
 *
 * Addressing (one flat /24, default 10.9.9.0/24):
 *
 *     mesh interface   10.9.9.<100 + mac[5] & 0x3f>   primary vif, MMWLAN_VIF_STA
 *     AP   interface   10.9.9.1                       secondary vif, MMWLAN_VIF_AP,
 *                                                     DHCP server for AP clients
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
#include "esp_system.h"
#include "nvs_flash.h"

#include "gate.h"
#include "mmhalow.h"
#include "umac/mesh/umac_mesh.h"

const char *GATE_TAG = "mesh_gate";

uint8_t g_mesh_mac[6];
uint8_t g_ap_bssid[6];
char    g_ap_ipv4[16];
esp_netif_t *g_mesh_netif;
esp_netif_t *g_ap_netif;

#define STATUS_INTERVAL_S 15

/*
 * Report what the gate can see. The interesting part of a bridge is its learned state, so
 * surface it rather than making you add printf()s: who is associated, how many
 * cross-bridge mappings are held, how many mesh peers, and whether other gates exist.
 */
static void log_status(uint32_t uptime_s)
{
    uint8_t peers[UMAC_MESH_MAX_PEERS][6] = { { 0 } };
    uint8_t n_peers = mmwlan_mesh_peer_count(peers);

    ESP_LOGI(GATE_TAG,
             "uptime=%" PRIu32 "s  ap_clients=%d  mesh_peers=%u  arp_mappings=%d  "
             "gates_known=%u  heap=%" PRIu32,
             uptime_s, gate_bridge_client_count(), (unsigned)n_peers,
             gate_arp_entry_count(), (unsigned)mmwlan_mesh_gate_count(),
             esp_get_free_heap_size());

    uint8_t clients[8][6];
    int n_clients = gate_bridge_client_list(clients, 8);
    for (int i = 0; i < n_clients; i++)
    {
        ESP_LOGI(GATE_TAG, "  ap client[%d] " MACSTR, i, MAC2STR(clients[i]));
    }
    for (uint8_t i = 0; i < n_peers; i++)
    {
        ESP_LOGI(GATE_TAG, "  mesh peer[%u] " MACSTR, i, MAC2STR(peers[i]));
    }
}

void app_main(void)
{
    ESP_LOGI(GATE_TAG, "=== mesh-gate: 802.11s mesh + SoftAP bridged on one MM6108 ===");

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

    ESP_LOGI(GATE_TAG, "starting mesh (id=\"%s\" chan=%d mac=" MACSTR ")",
             MESH_ID, S1G_CHANNEL, MAC2STR(g_mesh_mac));

    enum mmwlan_status status = mmwlan_mesh_start(&mesh_args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(GATE_TAG, "mmwlan_mesh_start failed, status=%d", (int)status);
        return;
    }
    ESP_LOGI(GATE_TAG, "mesh interface up (primary)");

    /*
     * Advertise as a discoverable gate: flood a proactive root announcement carrying the
     * IS_GATE flag. This mirrors dot11MeshHWMPRootMode = PROACTIVE_RANN plus
     * dot11MeshGateAnnouncementProtocol on a Linux gate, and is what lets a mesh node that
     * has never seen our traffic discover a way off the mesh.
     */
    mmwlan_mesh_set_root_announcements(true, true, RANN_INTERVAL_MS);
    ESP_LOGI(GATE_TAG, "gate announcements on (IS_GATE, every %d ms)", RANN_INTERVAL_MS);

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
    ap_args.sta_status_cb = gate_bridge_ap_sta_status_cb;

    ESP_LOGI(GATE_TAG, "starting SoftAP \"%s\" alongside the mesh on channel %d",
             AP_SSID, S1G_CHANNEL);

    status = mmwlan_ap_enable(&ap_args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(GATE_TAG, "mmwlan_ap_enable failed, status=%d (the mesh stays up)",
                 (int)status);
        return;
    }
    ESP_LOGI(GATE_TAG, "AP interface up (secondary), concurrent with the mesh");

    /* --- 3. bridge wiring -------------------------------------------------- */
    gate_netif_setup_ap();          /* second netif + DHCP server; captures the AP BSSID */
    /* Before gate_bridge_register(), deliberately: the announce task is woken by handle from
     * the receive path, so a host learned before the task exists drops its wake silently. */
    gate_arp_start_announce(PROXY_ARP_PUSH_MS);
    gate_bridge_register();         /* per-vif receive callbacks + the AE hook           */
    gate_netif_start_mesh_addressing();

    ESP_LOGI(GATE_TAG, "gate running: mesh SA " MACSTR ", AP BSSID " MACSTR,
             MAC2STR(g_mesh_mac), MAC2STR(g_ap_bssid));

    uint32_t uptime_s = 0;
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++uptime_s % STATUS_INTERVAL_S == 0)
        {
            log_status(uptime_s);
        }
    }
}
