/*
 * ibss — 802.11ah IBSS (ad-hoc) cell on an MM6108.
 *
 * Brings up an infrastructure-free peer-to-peer cell: no AP, no association, every
 * node equal. Bring-up mirrors the Linux flow (REMOVE_INTERFACE before
 * ADD_INTERFACE(ADHOC), so IBSS_CONFIG(CREATE) does not return EEXIST) and the
 * beacons carry source_addr = this node's own MAC, exactly as a Linux IBSS node
 * emits them — so peers discover each other from the beacon and a Linux HaLow
 * ad-hoc node interoperates.
 *
 * One binary runs on every node. Each derives its IP from its own MAC and pings
 * every peer it discovers, so the same image scales to as many boards as you flash.
 *
 * Peer bookkeeping is caller-driven by design: the driver reports membership through
 * a callback and the application decides when to age peers out
 * (mmwlan_ibss_age_peers), rather than the driver running its own timer.
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
#include "nvs_flash.h"
#include "ping/ping_sock.h"

#include "mmhalow.h"
#include "mmwlan.h"
#include "umac/ibss/umac_ibss.h"

#define IBSS_SSID         CONFIG_EXAMPLE_IBSS_SSID
#define IBSS_BSSID        CONFIG_EXAMPLE_IBSS_BSSID
#define IBSS_S1G_CHAN     CONFIG_EXAMPLE_IBSS_S1G_CHANNEL
#define IBSS_IPV4_PREFIX  CONFIG_EXAMPLE_IBSS_IPV4_PREFIX
#define IBSS_PEER_TIMEOUT CONFIG_EXAMPLE_IBSS_PEER_TIMEOUT_MS
#define IBSS_NETMASK      "255.255.255.0"

/* Upper bound on peers this example tracks for its own ping bookkeeping. */
#define MAX_TRACKED_PEERS 8

/*
 * Creator / joiner role. With a provisioned, pre-shared BSSID there is only ever one
 * cell, so this only decides who issues IBSS_CONFIG(CREATE) first; everyone else
 * joins. Picking on a MAC bit lets one flat binary self-assign roles with no per-node
 * configuration. If you provision roles yourself, set `create` from your own policy.
 */
#define CREATOR_MAC_BIT 0x80

static const char *TAG = "ibss";

/* Peers discovered by the membership callback, drained by the main loop. */
static uint8_t s_pending[MAX_TRACKED_PEERS][6];
static volatile unsigned s_n_pending;

/* Peers we have already started a ping session for. */
static uint8_t s_pinged[MAX_TRACKED_PEERS][6];
static unsigned s_n_pinged;

static void mac_to_ip(const uint8_t *mac, char *out, size_t outlen)
{
    unsigned octet = mac[5];
    if (octet == 0)
    {
        octet = 1;      /* .0 is the network address */
    }
    if (octet == 255)
    {
        octet = 254;    /* .255 is the broadcast address */
    }
    snprintf(out, outlen, IBSS_IPV4_PREFIX "%u", octet);
}

static bool parse_mac(const char *str, uint8_t *out)
{
    unsigned v[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        if (v[i] > 0xff)
        {
            return false;
        }
        out[i] = (uint8_t)v[i];
    }
    return true;
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno;
    uint32_t elapsed_ms;
    ip_addr_t target = { 0 };
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &target, sizeof(target));
    ESP_LOGI(TAG, "reply from " IPSTR ": seq=%u time=%" PRIu32 " ms",
             IP2STR(&target.u_addr.ip4), seqno, elapsed_ms);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    ESP_LOGW(TAG, "ping timeout seq=%u", seqno);
}

static void start_ping(const char *peer_ip)
{
    ip_addr_t target = { 0 };
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = esp_ip4addr_aton(peer_ip);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 1000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
    };

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) == ESP_OK)
    {
        esp_ping_start(ping);
        ESP_LOGI(TAG, "pinging peer %s", peer_ip);
    }
}

static void setup_static_ip(const char *my_ip)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        return;
    }

    esp_netif_dhcpc_stop(netif);   /* an ad-hoc cell has no DHCP server */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(my_ip);
    ip.gw.addr = esp_ip4addr_aton(my_ip);
    ip.netmask.addr = esp_ip4addr_aton(IBSS_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    /* No link-up event is fired for an IBSS vif, so bring the netif up explicitly —
     * without this lwIP never answers ICMP. */
    esp_netif_action_connected(netif, NULL, 0, NULL);
    ESP_LOGI(TAG, "static IP %s up", my_ip);
}

/*
 * Membership callback, fired on PEER_ADDED / PEER_REMOVED. It runs on the receive
 * context, so it stays trivial: record the peer and let the main loop do the work.
 */
static void peer_cb(const uint8_t *mac, enum mmwlan_ibss_peer_event ev, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "peer %s " MACSTR,
             ev == MMWLAN_IBSS_PEER_ADDED ? "joined" : "left", MAC2STR(mac));

    if (ev != MMWLAN_IBSS_PEER_ADDED)
    {
        return;
    }
    if (s_n_pending < MAX_TRACKED_PEERS)
    {
        memcpy(s_pending[s_n_pending++], mac, 6);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== 802.11ah IBSS (ad-hoc) node ===");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    uint8_t bssid[6];
    if (!parse_mac(IBSS_BSSID, bssid))
    {
        ESP_LOGE(TAG, "EXAMPLE_IBSS_BSSID \"%s\" is not a valid MAC address", IBSS_BSSID);
        return;
    }

    uint8_t mac[6] = { 0 };
    mmwlan_get_mac_addr(mac);

    bool create = ((mac[0] & CREATOR_MAC_BIT) == 0);
    char my_ip[16];
    mac_to_ip(mac, my_ip, sizeof(my_ip));

    ESP_LOGI(TAG, "MAC " MACSTR " -> role=%s ip=%s",
             MAC2STR(mac), create ? "CREATOR" : "JOINER", my_ip);

    mmwlan_ibss_register_peer_cb(peer_cb, NULL);

    struct mmwlan_ibss_args args = { 0 };
    memcpy(args.bssid, bssid, sizeof(args.bssid));
    memcpy(args.ssid, IBSS_SSID, strlen(IBSS_SSID));
    args.ssid_len = strlen(IBSS_SSID);
    args.create = create;
    args.s1g_chan_num = IBSS_S1G_CHAN;
    args.beacon_interval_tu = 100;
    /* if_addr = our own MAC, so our beacons carry source_addr = our MAC — a real IBSS
     * beacon, the way Linux emits it. Peers discover us from it. */
    memcpy(args.if_addr, mac, sizeof(args.if_addr));

    ESP_LOGI(TAG, "starting IBSS (ssid=\"%s\" bssid=" MACSTR " chan=%d) [%s]",
             IBSS_SSID, MAC2STR(bssid), IBSS_S1G_CHAN, create ? "CREATE" : "JOIN");

    enum mmwlan_status status = mmwlan_ibss_start(&args);
    if (status != MMWLAN_SUCCESS)
    {
        ESP_LOGE(TAG, "mmwlan_ibss_start failed, status=%d", (int)status);
        return;
    }
    ESP_LOGI(TAG, "IBSS up");

    vTaskDelay(pdMS_TO_TICKS(1500));
    setup_static_ip(my_ip);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(3000));

#if CONFIG_EXAMPLE_IBSS_PING_PEERS
        /* Start a ping session for each newly discovered peer, exactly once. */
        unsigned n_pending = s_n_pending;
        for (unsigned i = 0; i < n_pending && i < MAX_TRACKED_PEERS; i++)
        {
            bool already = false;
            for (unsigned j = 0; j < s_n_pinged; j++)
            {
                if (memcmp(s_pinged[j], s_pending[i], 6) == 0)
                {
                    already = true;
                    break;
                }
            }
            if (already || s_n_pinged >= MAX_TRACKED_PEERS)
            {
                continue;
            }

            char peer_ip[16];
            mac_to_ip(s_pending[i], peer_ip, sizeof(peer_ip));
            ESP_LOGI(TAG, "peer " MACSTR " -> %s", MAC2STR(s_pending[i]), peer_ip);
            start_ping(peer_ip);
            memcpy(s_pinged[s_n_pinged++], s_pending[i], 6);
        }
#endif
        s_n_pending = 0;

        /* Age out peers we have not heard from. Caller-driven, per the IBSS API. */
        mmwlan_ibss_age_peers(IBSS_PEER_TIMEOUT);
    }
}
