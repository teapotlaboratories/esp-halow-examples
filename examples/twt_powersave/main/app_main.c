/*
 * twt_powersave — Target Wake Time and WNM-sleep on an MM6108, both sides of the link.
 *
 * Select a role in menuconfig and flash two boards:
 *
 *   EXAMPLE_ROLE_AP    a SoftAP that answers TWT setup requests and WNM-sleep
 *                      requests, buffering downlink for a sleeping station.
 *   EXAMPLE_ROLE_STA   a station that negotiates a TWT agreement and then dozes,
 *                      waking only for its scheduled service periods.
 *
 * TWT lets a station skip DTIM beacons entirely and wake on an agreed schedule
 * instead, which is the difference between a battery node lasting weeks and lasting
 * days. WNM-sleep goes further: the station stays associated while its radio sleeps
 * across many DTIM periods, with the AP holding downlink traffic until it returns.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#define WIFI_SSID     CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PSK      CONFIG_EXAMPLE_WIFI_PSK
#define S1G_CHANNEL   CONFIG_EXAMPLE_S1G_CHANNEL
#define S1G_OPCLASS   CONFIG_EXAMPLE_S1G_OPCLASS

#define HEARTBEAT_MS  30000

/* Read-only accessor exported by morselib: 1 = an agreement is INSTALLED on this
 * flow, 0 = the negotiation is still pending, -1 = no agreement at all. */
extern int mmwlan_twt_agreement_installed(uint16_t flow_id);

static const char *TAG = "twt_powersave";

#if CONFIG_EXAMPLE_ROLE_AP
/* ------------------------------------------------------------------------------
 * SoftAP role — TWT responder and WNM-sleep responder.
 *
 * Neither responder needs to be switched on by the application: the MM6108 enables
 * the TWT responder by default on an AP vif, and the WNM-sleep responder answers a
 * station's WNM-Sleep-Enter/Exit requests as part of the AP's management path. The
 * application's job is to stand the AP up and stay out of the way.
 * ---------------------------------------------------------------------------- */

#define AP_IPV4    CONFIG_EXAMPLE_AP_IPV4
#define AP_NETMASK "255.255.255.0"
#define AP_MAX_STAS 10

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL)
    {
        return;
    }
    if (st->state == MMWLAN_AP_STA_AUTHORIZED)
    {
        ESP_LOGI(TAG, "station " MACSTR " authorized (aid=%u)",
                 MAC2STR(st->mac_addr), (unsigned)st->aid);
    }
    else if (st->state == MMWLAN_AP_STA_UNKNOWN)
    {
        ESP_LOGI(TAG, "station " MACSTR " left", MAC2STR(st->mac_addr));
    }
}

static void assign_static_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL)
    {
        ESP_LOGE(TAG, "netif WIFI_STA_DEF not found");
        return;
    }

    esp_netif_dhcpc_stop(netif);   /* the netif is a DHCP client by default; go static */

    esp_netif_ip_info_t ip = { 0 };
    ip.ip.addr = esp_ip4addr_aton(AP_IPV4);
    ip.gw.addr = esp_ip4addr_aton(AP_IPV4);
    ip.netmask.addr = esp_ip4addr_aton(AP_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));

    /* In AP mode no link-up event is ever fired, so the netif is never brought up and
     * lwIP cannot answer ICMP. Bring it up explicitly — this one call is what makes
     * the AP reachable over IP. */
    esp_netif_action_connected(netif, NULL, 0, NULL);
    ESP_LOGI(TAG, "AP static IP %s, netif up=%d", AP_IPV4, (int)esp_netif_is_netif_up(netif));
}

static void run_ap(void)
{
    ESP_LOGI(TAG, "=== SoftAP: TWT + WNM-sleep responder ===");

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };

    memcpy((char *)cfg.ap.ssid, WIFI_SSID, strlen(WIFI_SSID));
    cfg.ap.ssid_len = strlen(WIFI_SSID);
    memcpy(cfg.ap.passphrase, WIFI_PSK, strlen(WIFI_PSK));
    cfg.ap.passphrase_len = strlen(WIFI_PSK);
    cfg.ap.security_type = MMWLAN_SAE;
    /* PMF is required for WNM-sleep: the WNM-Sleep-Enter/Exit exchange is a robust
     * management frame, so a station without management-frame protection cannot use it. */
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;
    cfg.ap.s1g_chan_num = S1G_CHANNEL;
    cfg.ap.op_class = S1G_OPCLASS;
    cfg.ap.max_stas = AP_MAX_STAS;
    cfg.ap.sta_status_cb = ap_sta_status_cb;

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));
    mmhalow_wifi_start();

    vTaskDelay(pdMS_TO_TICKS(1500));
    assign_static_ip();

    ESP_LOGI(TAG, "SoftAP \"%s\" up on channel %d — waiting for stations",
             WIFI_SSID, S1G_CHANNEL);

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
        ESP_LOGI(TAG, "AP up, buffering downlink for any sleeping station");
    }
}

#else /* CONFIG_EXAMPLE_ROLE_STA */
/* ------------------------------------------------------------------------------
 * Station role — TWT requester, optionally followed by WNM-sleep.
 * ---------------------------------------------------------------------------- */

#define CONNECT_TIMEOUT_MS 30000
#define TWT_INSTALL_WAIT_S 20

static volatile bool s_connected;

static void sta_status_cb(enum mmwlan_sta_state state)
{
    if (state == MMWLAN_STA_CONNECTED)
    {
        s_connected = true;
    }
}

static void run_sta(void)
{
    ESP_LOGI(TAG, "=== Station: TWT requester ===");

    mmhalow_wifi_config_t cfg = { .sta = MMWLAN_STA_ARGS_INIT };
    memcpy(cfg.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    cfg.sta.ssid_len = strlen(WIFI_SSID);
    memcpy(cfg.sta.passphrase, WIFI_PSK, strlen(WIFI_PSK));
    cfg.sta.passphrase_len = strlen(WIFI_PSK);
    cfg.sta.security_type = MMWLAN_SAE;
    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_STA, &cfg));

    struct mmwlan_twt_config_args twt = MMWLAN_TWT_CONFIG_ARGS_INIT;
    twt.twt_mode = MMWLAN_TWT_REQUESTER;
    twt.twt_wake_interval_us = CONFIG_EXAMPLE_TWT_WAKE_INTERVAL_US;
    twt.twt_min_wake_duration_us = CONFIG_EXAMPLE_TWT_MIN_WAKE_DURATION_US;
    twt.twt_setup_command = MMWLAN_TWT_SETUP_REQUEST;

#if !CONFIG_EXAMPLE_TWT_ACTION_FRAME
    /*
     * Assoc-embedded path (the default, and the portable one). Registering the TWT
     * configuration BEFORE connecting makes morselib carry the request inside the
     * (re)association IEs, where an AP's assoc-time TWT responder handles it. hostapd
     * enables such a responder by default, so this works against APs that ignore a
     * mid-session action frame entirely.
     */
    enum mmwlan_status ts = mmwlan_twt_add_configuration(&twt);
    ESP_LOGI(TAG, "TWT requester queued for the association request (ret=%d)", (int)ts);
#endif

    mmhalow_connect(sta_status_cb);

    int waited_ms = 0;
    while (!s_connected && waited_ms < CONNECT_TIMEOUT_MS)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited_ms += 100;
    }
    if (!s_connected)
    {
        ESP_LOGE(TAG, "no association to \"%s\" within %d ms — is the AP up?",
                 WIFI_SSID, CONNECT_TIMEOUT_MS);
        for (;;)
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    ESP_LOGI(TAG, "associated to \"%s\"", WIFI_SSID);

#if CONFIG_EXAMPLE_TWT_ACTION_FRAME
    /*
     * Mid-session path. Assoc-preserving and renegotiable at runtime, but it depends on
     * the AP answering a TWT Setup Request action frame — not every AP does.
     */
    enum mmwlan_status ts = mmwlan_twt_setup_request(&twt);
    ESP_LOGI(TAG, "TWT Setup Request action frame sent (ret=%d)", (int)ts);
#endif

    /*
     * Wait for the agreement to reach INSTALLED. The negotiation runs
     * EMPTY -> PENDING_RESPONSE -> PENDING_INSTALLATION -> INSTALLED, so a non-installed
     * state right after the request is normal, not a failure.
     */
    int installed = -1;
    for (int i = 0; i < TWT_INSTALL_WAIT_S; i++)
    {
        installed = mmwlan_twt_agreement_installed(0);
        if (installed == 1)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (installed == 1)
    {
        ESP_LOGI(TAG, "TWT agreement INSTALLED on flow 0 (wake interval %d s)",
                 (int)(CONFIG_EXAMPLE_TWT_WAKE_INTERVAL_US / 1000000));
    }
    else
    {
        ESP_LOGW(TAG, "no TWT agreement after %d s (state=%d) — the AP may not have a "
                      "responder for this path; power-save still works without TWT",
                 TWT_INSTALL_WAIT_S, installed);
    }

    /*
     * Turn on 802.11 power-save. This is required for TWT or WNM-sleep to save anything.
     *
     * Note that mmhalow_init() force-disables power-save unless CONFIG_HALOW_PS_MODE is
     * set (see mmhalow.c). This example sets that Kconfig, but enabling power-save here
     * explicitly means the example still behaves correctly if it is turned off — a
     * silently-disabled power-save is otherwise very hard to spot, since everything
     * works, just at full current.
     */
    mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED);
    ESP_LOGI(TAG, "power-save enabled — dozing between service periods");

#if CONFIG_EXAMPLE_STA_WNM_SLEEP
    /*
     * WNM-sleep: stay associated while the radio sleeps across many DTIM periods, with
     * the AP buffering downlink until we return. chip_powerdown_enabled additionally
     * powers the transceiver down for the duration, which is the lowest-power state
     * that still keeps the association alive.
     *
     * Do not queue traffic for transmission while in WNM-sleep.
     */
    vTaskDelay(pdMS_TO_TICKS(2000));
    struct mmwlan_set_wnm_sleep_enabled_args wnm = MMWLAN_SET_WNM_SLEEP_ENABLED_ARGS_INIT;
    wnm.wnm_sleep_enabled = true;
    wnm.chip_powerdown_enabled = true;
    enum mmwlan_status ws = mmwlan_set_wnm_sleep_enabled_ext(&wnm);
    if (ws == MMWLAN_SUCCESS)
    {
        ESP_LOGI(TAG, "WNM-sleep entered with chip power-down");
    }
    else
    {
        ESP_LOGW(TAG, "WNM-sleep not entered (ret=%d) — the AP has to accept the request, "
                      "and PMF is required", (int)ws);
    }
#endif

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
        ESP_LOGI(TAG, "still associated, twt_installed=%d",
                 mmwlan_twt_agreement_installed(0));
    }
}
#endif /* CONFIG_EXAMPLE_ROLE_AP */

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(500));   /* let the USB-Serial-JTAG console attach */

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

#if CONFIG_EXAMPLE_ROLE_AP
    run_ap();
#else
    run_sta();
#endif
}
