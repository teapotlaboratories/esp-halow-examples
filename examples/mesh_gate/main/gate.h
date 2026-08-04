/*
 * gate.h — internal interfaces shared by the mesh-gate example's translation units.
 *
 * The gate is split by concern so each piece can be read on its own:
 *
 *   app_main.c     orchestration: bring the mesh up, then the AP, then wire the bridge
 *   gate_netif.c   the second esp_netif for the AP side, local delivery, addressing
 *   gate_bridge.c  the datapath — the three receive callbacks that move frames across
 *   gate_arp.c     proxy ARP: learn IP-to-MAC on both sides and answer across the bridge
 *
 * This is not a public API. It is the seam between files in one example, and it is
 * deliberately narrow so that the reusable parts are visible if you want to lift them
 * into a component of your own.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_netif.h"
#include "mmpkt.h"
#include "mmwlan.h"

/* ---- configuration, from Kconfig.projbuild -------------------------------- */
#define MESH_ID            CONFIG_EXAMPLE_MESH_ID
#define MESH_MAX_PLINKS    CONFIG_EXAMPLE_MESH_MAX_PLINKS
#define RANN_INTERVAL_MS   CONFIG_EXAMPLE_RANN_INTERVAL_MS
#define AP_SSID            CONFIG_EXAMPLE_AP_SSID
#define AP_PSK             CONFIG_EXAMPLE_AP_PSK
#define AP_MAX_STAS        CONFIG_EXAMPLE_AP_MAX_STAS
#define S1G_CHANNEL        CONFIG_EXAMPLE_S1G_CHANNEL
#define S1G_OPCLASS        CONFIG_EXAMPLE_S1G_OPCLASS
#define SUBNET_PREFIX      CONFIG_EXAMPLE_SUBNET_PREFIX
#define PROXY_ARP_PUSH_MS  CONFIG_EXAMPLE_PROXY_ARP_PUSH_MS
#define SUBNET_NETMASK     "255.255.255.0"

/* Largest Ethernet frame relayed between the two sides. */
#define MAX_ETH_FRAME  (14 + 1514)
/* An Ethernet ARP frame: 14 B header + 28 B ARP. */
#define ARP_FRAME_LEN  42
/* LLC/SNAP header prepended to a mesh payload. */
#define SNAP_LEN       8

/* Which side of the bridge a host sits on. */
enum { SIDE_AP = 0, SIDE_MESH = 1 };

/* One log tag for the whole example, so output reads as one component. */
extern const char *GATE_TAG;

/* ---- state owned by app_main.c, read by the other units ------------------- */
extern uint8_t g_mesh_mac[6];   /* our mesh SA; also the mesh netif's L2 address */
extern uint8_t g_ap_bssid[6];   /* the AP vif's MAC: a frame to it is for us, not to proxy */
extern char    g_ap_ipv4[16];   /* SUBNET_PREFIX "1" */
extern esp_netif_t *g_mesh_netif;
extern esp_netif_t *g_ap_netif;

/* ---- gate_netif.c --------------------------------------------------------- */

/* Create the AP-side esp_netif (DHCP server, VIF_AP transmit) and bring it up. Also
 * captures the AP BSSID into g_ap_bssid. Call after mmwlan_ap_enable(). */
void gate_netif_setup_ap(void);

/* Pin the mesh netif's static address once it is up. Spawns a short-lived task. */
void gate_netif_start_mesh_addressing(void);

/* Hand a received frame to the gate's OWN lwIP stack (DHCP, ARP/ICMP to the gate).
 * Takes ownership of `pkt` and always releases it exactly once. */
void gate_netif_deliver_locally(struct mmpkt *pkt, esp_netif_t *netif);

/* Transmit a raw Ethernet frame on the AP vif. NON-BLOCKING: safe from a receive
 * context. Returns false if the frame could not be queued. */
bool gate_netif_tx_ap(const uint8_t *frame, uint32_t len);

/* ---- gate_arp.c ---------------------------------------------------------- */

/* Learn the sender's IP-to-MAC from any ARP or IPv4 frame arriving on `side`. */
void gate_arp_snoop(const uint8_t *eth, uint32_t elen, uint8_t side);

/* If `eth` is an ARP REQUEST for a host known on the OTHER side, answer it over the
 * reliable link and return true — the caller then skips the lossy bridge for it. */
bool gate_arp_proxy(const uint8_t *eth, uint32_t elen, uint8_t from_side);

/* Drop a departed host's mapping so we stop advertising it. */
void gate_arp_forget_mac(const uint8_t *mac);

/* Start the proactive push task. period_ms == 0 leaves only reactive proxy ARP. */
void gate_arp_start_announce(uint32_t period_ms);

/* Introspection: how many cross-bridge mappings are currently held. */
int gate_arp_entry_count(void);

/* Introspection: resolve a learned host. Either out pointer may be NULL. */
bool gate_arp_resolve(uint32_t ip, uint8_t *mac_out, uint8_t *side_out);

/* ---- gate_bridge.c ------------------------------------------------------- */

/* Register the per-vif receive callbacks and the Address-Extension hook. Call after
 * both interfaces are up and the AP netif exists. */
void gate_bridge_register(void);

/* AP association callback — maintains the client set the bridge keys off. */
void gate_bridge_ap_sta_status_cb(const struct mmwlan_ap_sta_status *st, void *arg);

/* Introspection: how many stations are associated to our AP. */
int gate_bridge_client_count(void);

/* Introspection: copy up to `cap` associated client MACs out. Returns how many. */
int gate_bridge_client_list(uint8_t macs[][6], int cap);
