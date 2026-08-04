# SPDX-License-Identifier: Apache-2.0

idf_component_register(
    SRCS "app_main.c"
         "gate_netif.c"
         "gate_bridge.c"
         "gate_arp.c"
    INCLUDE_DIRS "."
    REQUIRES halow nvs_flash esp_wifi esp_netif esp_event lwip
)
