# High-density SoftAP

A HaLow SoftAP configured for many associated stations — up to 255 — with the
per-station state routed to PSRAM instead of internal SRAM. It tracks associations as
they come and go and reports the station count alongside remaining heap, so the memory
cost of each station is visible as the AP fills up.

## What it demonstrates

| | Setting |
|---|---|
| Runtime station ceiling | `mmwlan_ap_args.max_stas` |
| Compile-time table sizing | `CONFIG_HALOW_AP_MAX_STAS` |
| Per-STA state out of internal SRAM | `CONFIG_HALOW_STA_DATA_IN_PSRAM` |
| Association / disassociation events | `mmwlan_ap_args.sta_status_cb` |

## Where the 255 comes from

A HaLow station is addressed in the beacon's traffic indication map. The S1G TIM spans
four partial-virtual-bitmap blocks (`MAX_SUPPORTED_AID` = 256 in `traffic_bitmap.h`,
and the `s1g_tim.c` encoder emits multiple blocks), so valid association IDs run
1..255. 255 is also the ceiling of the public `uint8_t mmwlan_ap_args.max_stas` field —
going higher would require widening that field to `uint16_t`.

## The two ceilings are different, and both matter

| | What it does |
|---|---|
| `CONFIG_HALOW_AP_MAX_STAS` | **Compile-time.** Sizes the per-STA tables and the per-vif TWT agreement table. |
| `mmwlan_ap_args.max_stas` | **Runtime.** How many stations this AP instance will admit. |

Raising the runtime value alone does not enlarge the tables, so it must never exceed
the compile-time one. The example checks this at startup and refuses to continue rather
than overrunning a table:

```c
if (AP_MAX_STAS > CONFIG_HALOW_AP_MAX_STAS) { ... return; }
```

## Memory

Each associated station costs roughly 912 bytes of `umac_sta_data` on the heap, plus
per-STA buffers. At 255 stations that is about 230 KB — which must not come out of the
ESP32's small internal-SRAM pool.

`CONFIG_HALOW_STA_DATA_IN_PSRAM=y` routes both that per-station state and the per-vif
TWT agreement table (otherwise static `.bss` in internal SRAM) to PSRAM. Allocation is
strict PSRAM with no internal-SRAM fallback, and it requires `CONFIG_SPIRAM`, which
comes from the board configuration.

Without it, the default `umac_sta_data` allocation stays in internal SRAM (below
`SPIRAM_MALLOC_ALWAYSINTERNAL`) and the TWT table stays in `.bss` — both of which
bound how many stations actually fit, well below the configured ceiling. The example
warns at startup if a high `max_stas` is combined with PSRAM placement turned off.

The report line makes the cost visible as stations join:

```
I (722) ap_scale: === high-density HaLow SoftAP (up to 255 stations) ===
I (1396) ap_scale: starting SoftAP "MorseMicroESP32AP" on channel 27, max_stas=255
I (3376) ap_scale: AP static IP 192.168.12.1, netif up=1
I (3376) ap_scale: per-STA state in PSRAM: yes
I (6317) ap_scale: station bc:2a:33:96:b2:92 authorized  aid=1    associated=1
I (13377) ap_scale: associated=1/255 (peak 1)  heap: internal=234447 psram=8372308 min_ever=8572136
```

## Configuration

`idf.py menuconfig` → *High-density AP example configuration*:

| Option | Default | Notes |
|---|---|---|
| `EXAMPLE_WIFI_SSID` / `_PSK` | `MorseMicroESP32AP` / `12345678` | SAE, so ≥ 8 characters |
| `EXAMPLE_S1G_CHANNEL` / `_OPCLASS` | `27` / `68` | 915.5 MHz / 1 MHz, US |
| `EXAMPLE_AP_IPV4` | `192.168.12.1` | Static; the AP runs no DHCP server |
| `EXAMPLE_AP_MAX_STAS` | `255` | Runtime ceiling |
| `EXAMPLE_REPORT_INTERVAL_S` | `10` | Station-count / heap report period |

You also need the board's SPI pins and `CONFIG_HALOW_COUNTRY_CODE` — it defaults to
`"??"`, which builds fine but leaves the radio unable to come up.

## Caveats

Station counts above 20 are **experimental**. The vendor's tested configuration is 20,
and the MM6108 firmware's true concurrent-station capacity is its own limit, which is
not published. 255 has been exercised as a build and structural test — allocation,
table sizing, TIM encoding — and on-air with a handful of real stations associating
concurrently over SAE, not with 255 live clients.

Treat the ceiling as "the host stack and TIM encoder support it", not as a throughput
or capacity claim.

## Two details that are easy to miss

- **The AP runs no DHCP server.** `mmhalow` gives even an AP a DHCP *client* netif, so
  the AP pins a static address and each station must do the same.
- **In AP mode no link-up event is ever fired**, so the netif is never brought up and
  lwIP cannot answer ICMP. The example calls `esp_netif_action_connected()` explicitly.
  That one call is what makes the AP reachable over IP.

## Verified on hardware

Came up at `max_stas=255` reporting `per-STA state in PSRAM: yes`. A real HaLow station
associated and was authorized with `aid=1`, the association count tracked the join, and
heap stayed flat across the run (internal ~234 KB, PSRAM ~8.37 MB).

Not covered: anything approaching the ceiling. One client is not 255 — see the caveat
above about counts over 20 being experimental.
