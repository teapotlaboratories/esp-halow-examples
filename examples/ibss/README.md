# IBSS (ad-hoc)

An infrastructure-free 802.11ah cell: no access point, no association, every node
equal. Nodes discover each other from beacons and exchange data directly. One binary
runs on every node.

IBSS is an alternative to the `mesh` example when you want peer-to-peer connectivity
without 802.11s path selection — flat, single-hop, and considerably simpler.

## What it demonstrates

| | API |
|---|---|
| Start / stop an ad-hoc cell | `mmwlan_ibss_start()`, `mmwlan_ibss_stop()` |
| Membership notifications | `mmwlan_ibss_register_peer_cb()` |
| Application-driven peer ageing | `mmwlan_ibss_age_peers()` |
| Peer enumeration | `mmwlan_ibss_peer_count()` |
| Per-peer signal strength | `mmwlan_ibss_get_peer_rssi()` |

## Configuration

`idf.py menuconfig` → *IBSS example configuration*:

| Option | Default | Notes |
|---|---|---|
| `EXAMPLE_IBSS_SSID` | `morse-ibss` | Must match on every node |
| `EXAMPLE_IBSS_BSSID` | `02:12:34:56:78:9a` | Must match on every node |
| `EXAMPLE_IBSS_S1G_CHANNEL` | `27` | 915.5 MHz / 1 MHz, US op-class 68 |
| `EXAMPLE_IBSS_IPV4_PREFIX` | `192.168.13.` | Host octet derived from the MAC |
| `EXAMPLE_IBSS_PEER_TIMEOUT_MS` | `30000` | Age-out threshold |
| `EXAMPLE_IBSS_PING_PEERS` | `y` | Ping each peer as it appears |

You also need the board's SPI pins and `CONFIG_HALOW_COUNTRY_CODE` — it defaults to
`"??"`, which builds fine but leaves the radio unable to come up.

## Running it

Flash two or more boards with the same image:

```
I (722) ibss: === 802.11ah IBSS (ad-hoc) node ===
I (1396) ibss: MAC 68:24:99:44:6b:b7 -> role=CREATOR ip=192.168.13.183
I (1396) ibss: starting IBSS (ssid="morse-ibss" bssid=02:12:34:56:78:9a chan=27) [CREATE]
I (2301) ibss: IBSS up
I (2321) ibss: peer joined bc:2a:33:96:b2:92
I (3803) ibss: static IP 192.168.13.183 up
I (6803) ibss: peer bc:2a:33:96:b2:92 -> 192.168.13.146
I (6806) ibss: pinging peer 192.168.13.146
I (7879) ibss: reply from 192.168.13.146: seq=2 time=46 ms
```

## Provisioned BSSID, and why there is no TSF merge

Every node is deployed already knowing the cell's BSSID, so there is only ever one
cell. IBSS TSF merge (`ieee80211_rx_bss_info` in `net/mac80211/ibss.c`) exists to let
*uncoordinated* ad-hoc nodes — each of which rolled its own random BSSID — converge
onto a single cell. With a pre-shared BSSID that situation never arises, so merge is
out of scope here.

The creator/joiner split follows from the same assumption. It only decides who issues
`IBSS_CONFIG(CREATE)` first; everyone else joins the same cell either way. The example
picks the role from a MAC bit purely so that one flat binary self-assigns without
per-node configuration — set `args.create` from your own policy if you provision roles.

## Bring-up order

`mmwlan_ibss_start()` performs a teardown-first bring-up: `REMOVE_INTERFACE` before
`ADD_INTERFACE(ADHOC)`, mirroring the Linux sequence. Without the teardown,
`IBSS_CONFIG(CREATE)` returns `EEXIST` against an interface left over from a previous
mode.

Beacons carry `source_addr` = the node's own MAC — a real IBSS beacon, as Linux emits
them — which is what lets peers discover each other from the beacon and what makes a
Linux `morse_driver` ad-hoc node interoperate. Setting `args.if_addr` is therefore not
optional in practice.

## Peer ageing is the application's job

The driver reports membership through the callback but runs no ageing timer of its
own. The application calls `mmwlan_ibss_age_peers(threshold_ms)` on whatever schedule
suits it — this example does so every 3 seconds. A node that never calls it keeps
departed peers in its table indefinitely.

The callback runs on the receive context, so it should stay trivial. This example only
records the peer MAC there and starts the ping session from the main loop.

## Addressing

An ad-hoc cell has no DHCP server, so each node addresses itself. The example derives
the host octet from its own MAC (`192.168.13.<mac[5]>`, clamped away from `.0` and
`.255`). No link-up event is fired for an IBSS vif, so the example calls
`esp_netif_action_connected()` explicitly — without it lwIP never answers ICMP.

## Verified on hardware

Two ESP32-S3 + FGH100M boards running the same image. Roles self-assigned correctly
(one CREATOR, one JOINER) from their distinct MM6108 MACs, the cell came up in 2.3 s,
and each discovered the other from its beacon before IP was even configured. Both
pinged the other continuously — 95 of 98 replies, with no peer flapping (`peer left`
never fired).
