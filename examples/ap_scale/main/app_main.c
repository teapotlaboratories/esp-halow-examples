/*
 * ap_scale — a high-density HaLow SoftAP.
 *
 * A SoftAP configured for many associated stations: up to 255, with the per-station
 * state routed to PSRAM instead of internal SRAM. It tracks associations as they come
 * and go and reports the station count alongside remaining heap, so the memory cost
 * of each station is visible as the AP fills up.
 *
 * The 255 ceiling comes from the S1G traffic indication map. A HaLow TIM spans four
 * partial-virtual-bitmap blocks (MAX_SUPPORTED_AID = 256), so valid association IDs
 * run 1..255 — and 255 is also the maximum value of the public uint8_t
 * mmwlan_ap_args.max_stas field.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "mmhalow.h"
#include "mmwlan.h"

#define WIFI_SSID       CONFIG_EXAMPLE_WIFI_SSID
#define WIFI_PSK        CONFIG_EXAMPLE_WIFI_PSK
#define S1G_CHANNEL     CONFIG_EXAMPLE_S1G_CHANNEL
#define S1G_OPCLASS     CONFIG_EXAMPLE_S1G_OPCLASS
#define AP_IPV4         CONFIG_EXAMPLE_AP_IPV4
#define AP_MAX_STAS     CONFIG_EXAMPLE_AP_MAX_STAS
#define REPORT_S        CONFIG_EXAMPLE_REPORT_INTERVAL_S
#define AP_NETMASK      "255.255.255.0"

static const char *TAG = "ap_scale";

/* Associated-station count, maintained from the status callback. The callback runs on
 * the AP status task while the report loop reads it, so it is only ever touched
 * through these two helpers and kept to a single word. */
static volatile int32_t s_sta_count;
static volatile int32_t s_peak_sta_count;

static void ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg)
{
    (void)arg;
    if (st == NULL)
    {
        return;
    }

    if (st->state == MMWLAN_AP_STA_AUTHORIZED)
    {
        int32_t n = ++s_sta_count;
        if (n > s_peak_sta_count)
        {
            s_peak_sta_count = n;
        }
        ESP_LOGI(TAG, "station " MACSTR " authorized  aid=%-3u  associated=%" PRId32,
                 MAC2STR(st->mac_addr), (unsigned)st->aid, n);
    }
    else if (st->state == MMWLAN_AP_STA_UNKNOWN)
    {
        int32_t n = s_sta_count > 0 ? --s_sta_count : 0;
        ESP_LOGI(TAG, "station " MACSTR " left        associated=%" PRId32,
                 MAC2STR(st->mac_addr), n);
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
     * lwIP cannot answer ICMP. Bring it up explicitly. */
    esp_netif_action_connected(netif, NULL, 0, NULL);
    ESP_LOGI(TAG, "AP static IP %s, netif up=%d", AP_IPV4, (int)esp_netif_is_netif_up(netif));
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== high-density HaLow SoftAP (up to %d stations) ===", AP_MAX_STAS);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

    mmhalow_init(NULL);
    mmhalow_print_version_info();

    if (AP_MAX_STAS > CONFIG_HALOW_AP_MAX_STAS)
    {
        /* The per-STA tables are sized at compile time from CONFIG_HALOW_AP_MAX_STAS;
         * asking for more at runtime does not enlarge them. */
        ESP_LOGE(TAG, "EXAMPLE_AP_MAX_STAS=%d exceeds CONFIG_HALOW_AP_MAX_STAS=%d",
                 AP_MAX_STAS, CONFIG_HALOW_AP_MAX_STAS);
        return;
    }

#if !CONFIG_HALOW_STA_DATA_IN_PSRAM
    if (AP_MAX_STAS > 20)
    {
        ESP_LOGW(TAG, "%d stations without CONFIG_HALOW_STA_DATA_IN_PSRAM — per-STA state "
                      "will come from internal SRAM and is likely to exhaust it",
                 AP_MAX_STAS);
    }
#endif

    mmhalow_wifi_config_t cfg = { .ap = MMWLAN_AP_ARGS_INIT };

    memcpy((char *)cfg.ap.ssid, WIFI_SSID, strlen(WIFI_SSID));
    cfg.ap.ssid_len = strlen(WIFI_SSID);
    memcpy(cfg.ap.passphrase, WIFI_PSK, strlen(WIFI_PSK));
    cfg.ap.passphrase_len = strlen(WIFI_PSK);
    cfg.ap.security_type = MMWLAN_SAE;
    cfg.ap.pmf_mode = MMWLAN_PMF_REQUIRED;
    cfg.ap.s1g_chan_num = S1G_CHANNEL;
    cfg.ap.op_class = S1G_OPCLASS;
    cfg.ap.max_stas = AP_MAX_STAS;
    cfg.ap.sta_status_cb = ap_sta_status_cb;

    ESP_ERROR_CHECK(mmhalow_set_config(WIFI_IF_AP, &cfg));

    ESP_LOGI(TAG, "starting SoftAP \"%s\" on channel %d, max_stas=%d",
             WIFI_SSID, S1G_CHANNEL, AP_MAX_STAS);
    mmhalow_wifi_start();

    vTaskDelay(pdMS_TO_TICKS(1500));
    assign_static_ip();

    ESP_LOGI(TAG, "per-STA state in PSRAM: %s",
#if CONFIG_HALOW_STA_DATA_IN_PSRAM
             "yes"
#else
             "no"
#endif
    );

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(REPORT_S * 1000));
        ESP_LOGI(TAG,
                 "associated=%" PRId32 "/%d (peak %" PRId32 ")  "
                 "heap: internal=%u psram=%u min_ever=%" PRIu32,
                 s_sta_count, AP_MAX_STAS, s_peak_sta_count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 esp_get_minimum_free_heap_size());
    }
}
