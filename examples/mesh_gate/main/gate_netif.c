/*
 * gate_netif.c — the AP-side network interface, local delivery, and addressing.
 *
 * mmhalow creates one esp_netif, bound to the primary (mesh) interface. The AP side
 * needs its own: a second esp_netif whose transmit path tags MMWLAN_VIF_AP so morselib
 * egresses on the right interface, and which runs a DHCP server so AP clients are
 * zero-config on the shared subnet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_mac.h"

/* esp_netif.h must precede these two — they use esp_netif_t and the config structs it
 * declares, and do not include it themselves. */
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_types.h"

#include "lwip/netif.h"
#include "lwip/pbuf.h"

#include "gate.h"

static esp_netif_driver_base_t g_ap_driver_base;

/* ---- AP vif transmit ------------------------------------------------------ */

bool gate_netif_tx_ap(const uint8_t *frame, uint32_t len)
{
    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(len, 0);
    if (pkt == NULL)
    {
        return false;
    }

    struct mmpktview *v = mmpkt_open(pkt);
    mmpkt_append_data(v, frame, len);
    mmpkt_close(&v);

    struct mmwlan_tx_metadata md = MMWLAN_TX_METADATA_INIT;
    md.vif = MMWLAN_VIF_AP;   /* load-bearing: routes this frame to the AP interface */
    return mmwlan_tx_pkt(pkt, &md) == MMWLAN_SUCCESS;
}

/*
 * esp_netif's transmit hook. Unlike gate_netif_tx_ap() this MAY block, because it runs on
 * the lwIP task rather than a receive callback — so it waits for transmit capacity instead
 * of dropping the frame.
 */
static esp_err_t ap_transmit(void *h, void *buffer, size_t len)
{
    (void)h;
    if (mmwlan_tx_wait_until_ready(1000) != MMWLAN_SUCCESS)
    {
        ESP_LOGW(GATE_TAG, "AP transmit blocked");
        return ESP_FAIL;
    }
    return gate_netif_tx_ap((const uint8_t *)buffer, (uint32_t)len) ? ESP_OK : ESP_FAIL;
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

/* ---- local delivery ------------------------------------------------------- */

/*
 * Hand a received frame to the gate's OWN lwIP stack — DHCP, and ARP or ICMP addressed
 * to the gate itself. Bridging between the two sides happens in gate_bridge.c; this is
 * only local delivery.
 *
 * esp_netif_receive() is deliberately not used. It wraps the frame in a zero-copy,
 * NON-CONTIGUOUS PBUF_REF with a custom-free callback, which causes two problems: lwIP's
 * pbuf_add_header() refuses to prepend a link-layer header onto a non-contiguous pbuf,
 * and the custom-free path risks a double free once we already own the mmpkt.
 *
 * Instead, copy into a contiguous PBUF_RAM with link headroom and inject via
 * netif->input, which is what esp_netif's own wlanif_input does. We own the mmpkt (the
 * callback handed it over), we copied out of it, so we free it exactly once and never
 * pass it on.
 */
void gate_netif_deliver_locally(struct mmpkt *mmpkt, esp_netif_t *esp_netif)
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

/* ---- AP netif bring-up ---------------------------------------------------- */

void gate_netif_setup_ap(void)
{
    /*
     * The AP netif carries a DHCP server so clients are zero-config on the flat subnet:
     * the gate hands out an address and the bridge plus proxy ARP let the client reach
     * any mesh node directly, with no route.
     *
     * The address goes in the INHERENT config, so the netif is born with it and the
     * automatic DHCP-server start sees a valid server address. Setting it afterwards
     * does not work: a 0.0.0.0 address makes the DHCP server fail to obtain a PCB, and a
     * later esp_netif_set_ip_info() then aborts with DHCP_NOT_STOPPED.
     *
     * The lease pool starts at .2 and spans CONFIG_LWIP_DHCPS_MAX_STATION_NUM leases, so
     * at the default of 8 it stays clear of the mesh nodes' .100 upwards. If you raise
     * that Kconfig towards 100, pin the range explicitly with esp_netif_dhcps_option()
     * to keep the pool below .100.
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
     * make the bridge misclassify a frame addressed to the gate's own AP MAC as
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

    ESP_LOGI(GATE_TAG, "AP netif up: %s/24 with DHCP server, MAC " MACSTR,
             g_ap_ipv4, MAC2STR(bssid));
}

/* ---- mesh-side addressing ------------------------------------------------- */

static void mesh_net_task(void *arg)
{
    (void)arg;

    esp_netif_t *netif = g_mesh_netif;
    if (netif == NULL)
    {
        ESP_LOGE(GATE_TAG, "mesh netif not found");
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

    ESP_LOGI(GATE_TAG, "mesh netif static IP %s", ipbuf);
    ESP_LOGI(GATE_TAG, "bridge ready: AP clients and mesh nodes share %s0/24", SUBNET_PREFIX);

    vTaskDelete(NULL);
}

void gate_netif_start_mesh_addressing(void)
{
    xTaskCreate(mesh_net_task, "mesh_net", 4096, NULL, 5, NULL);
}
