# Mesh gate — 802.11s mesh + SoftAP, bridged at layer 2

An 802.11s mesh and a co-channel SoftAP on **one** MM6108, bridged so both sides share
a single flat subnet. An ordinary HaLow station associates to the AP, DHCPs an address,
and talks to any mesh node directly — no routing, no second subnet, no static routes,
nothing to configure on the client.

```
        mesh node ──── mesh node
             │            │
             └──── THIS GATE ────┐
                  mesh + SoftAP  │
                                 │
                        AP client (zero-config)
```

## What it demonstrates

| | API |
|---|---|
| Mesh and SoftAP concurrently on one radio | `mmwlan_mesh_start()` then `mmwlan_ap_enable()` |
| Advertise as a discoverable gate | `mmwlan_mesh_set_root_announcements()` |
| Proxy a client's unicast into the mesh | `mmwlan_mesh_tx_proxied()` |
| Proxy a client's broadcast into the mesh | `mmwlan_mesh_tx_group_proxied()` |
| Receive a proxied frame with its endpoints | `mmwlan_mesh_register_ae_rx_cb()` |
| Per-interface receive demultiplexing | `mmwlan_register_rx_pkt_ext_cb()` |
| Which mesh node proxies a given host | `mmwlan_mesh_mpp_lookup()` |
| Gates this node has discovered | `mmwlan_mesh_gate_count()` |

## How the bridge works

The bridge is at layer 2, using **802.11s Address Extension** — a 6-address frame that
carries the original source and final destination in the Mesh Control field, alongside
the mesh-level source and destination.

- **AP client → mesh.** A unicast frame for a mesh node is injected as a proxied
  6-address frame whose original source is the client. Mesh nodes learn "that host is
  reachable via this gate" from the frame itself.
- **Mesh → AP client.** A proxied frame arriving with a final destination matching one
  of our associated clients is delivered onto the AP interface as ordinary Ethernet.
- **Broadcast**, both ways, so ARP and discovery work across the bridge.

The result is a single flat `/24`. Nothing on the client knows a mesh exists.

### CCMP is transparent to all of this

The Address Extension lives in the Mesh Control field of the frame **body**, while the
CCMP AAD covers only the 802.11 MAC header. So encrypted frames bridge without
re-deriving anything, and the whole gate datapath needs no crypto code at all.

## Per-interface wiring is load-bearing

morselib delivers mesh receives to the `MMWLAN_VIF_STA` callback slot and AP receives
to `MMWLAN_VIF_AP`, and routes transmits by `metadata.vif`. The mesh netif must
therefore be fed from, and tagged, `VIF_STA`, and the AP netif `VIF_AP`.

**Getting this backwards silently drops one direction** — no error, no log, traffic
just vanishes one way. It is the first thing to check if half the bridge works.

Note that the mesh is the *primary* interface (`VIF_STA`) despite not being a station,
and the SoftAP is the secondary. Bring-up order follows: `mmwlan_mesh_start()` before
`mmwlan_ap_enable()`.

## Proxy ARP, and why it is not optional

The weak point of any layer-2 bridge is broadcast ARP resolution across it. A host ARPs
for a peer on the other side; the request is broadcast, and broadcast frames are
unacknowledged and lossy. The reply then makes the same crossing. Resolution ends up
hit-or-miss even though the data path itself is fine.

The gate snoops IP-to-MAC on both sides — from ordinary IPv4 traffic as well as ARP, so
a single ping is enough to learn a host — and then answers directly:

- **Reactive.** An ARP REQUEST for a host known on the other side is answered
  immediately over the reliable link.
- **Proactive** (`EXAMPLE_PROXY_ARP_PUSH_MS`, default 3 s). Every known host is
  periodically taught about the hosts on the other side, over reliable unicast, so
  neither side ever needs to broadcast across the bridge at all.

Both answer with the target's **real MAC**, not the gate's. The requester still
addresses the true peer and the bridge carries the actual traffic; the gate only
short-circuits the lossy resolution step.

One subtlety in the proactive push: each reply is addressed **to** the host being
taught. lwIP adds a *new* neighbour entry from an unsolicited reply only when the
target protocol address is the receiver's own — `etharp_input()` passes
`ETHARP_FLAG_TRY_HARD` when the frame is `for_us`, and the update-only
`ETHARP_FLAG_FIND_ONLY` otherwise. A push addressed to anyone else would merely refresh
an entry that already existed, which cannot bootstrap resolution.

## Configuration

`idf.py menuconfig` → *Mesh-gate example configuration*:

| Option | Default | Notes |
|---|---|---|
| `EXAMPLE_MESH_ID` | `morse-mesh` | Match the `mesh` example's nodes |
| `EXAMPLE_MESH_MAX_PLINKS` | `16` | |
| `EXAMPLE_RANN_INTERVAL_MS` | `5000` | Must match every other gate — see below |
| `EXAMPLE_AP_SSID` / `_PSK` | `MorseMicroESP32AP` / `12345678` | SAE, ≥ 8 characters |
| `EXAMPLE_AP_MAX_STAS` | `4` | Bounded by the client table |
| `EXAMPLE_S1G_CHANNEL` / `_OPCLASS` | `27` / `68` | One radio, so necessarily co-channel |
| `EXAMPLE_SUBNET_PREFIX` | `10.9.9.` | AP side `.1`, DHCP from `.2`, mesh from `.100` |
| `EXAMPLE_PROXY_ARP_PUSH_MS` | `3000` | `0` disables the proactive half |

> **The RANN interval goes onto the wire as a raw number**, with no
> millisecond-to-TU conversion. Every gate on the same mesh — including a Linux one —
> must use the same value.

You also need the board's SPI pins and `CONFIG_HALOW_COUNTRY_CODE` — it defaults to
`"??"`, which builds fine but leaves the radio unable to come up.

This example ships its own `partitions.csv`. Linking the mesh stack, the AP stack and a
second lwIP netif into one image does not fit the stock single-app-large layout; 2.5 MB
of application still fits comfortably on a 4 MB part.

## Running it

Flash this image on the gate board and the `mesh` example on one or more others, then
associate any HaLow station to the AP:

```
I (1399) mesh_gate: starting mesh (id="morse-mesh" chan=27 mac=e2:72:a1:f8:f0:08)
I (2249) mesh_gate: mesh interface up (primary)
I (2249) mesh_gate: gate announcements on (IS_GATE, every 5000 ms)
I (2699) mesh_gate: AP interface up (secondary), concurrent with the mesh
I (2700) mesh_gate: AP netif up: 10.9.9.1/24 with DHCP server, MAC be:2a:33:96:b2:33
I (2700) mesh_gate: gate running: mesh SA e2:72:a1:f8:f0:08, AP BSSID be:2a:33:96:b2:33
I (5943) mesh_gate: AP client joined bc:2a:33:96:b2:92 (1 total)
I (6700) mesh_gate: mesh netif static IP 10.9.9.108
I (6700) mesh_gate: bridge ready: AP clients and mesh nodes share 10.9.9.0/24
```

The client should now ping any mesh node with no further configuration. Set the log
level to debug to see individual frames crossing the bridge.

## Addressing layout

| | Address |
|---|---|
| Gate, AP side | `10.9.9.1` |
| AP clients | `10.9.9.2` upwards, by DHCP |
| Mesh nodes | `10.9.9.<100 + (mac[5] & 0x3f)>`, static |

The DHCP pool spans `CONFIG_LWIP_DHCPS_MAX_STATION_NUM` leases from `.2`, so at the
default of 8 it stays clear of the mesh range. If you raise that Kconfig towards 100,
pin the pool explicitly with `esp_netif_dhcps_option()` to keep it below `.100`.

The AP netif hands out **no router option**. A pure layer-2 bridge does not route
off-subnet, so a default route pointing at the gate would only black-hole traffic.

## Implementation notes

Three things in this example are less obvious than they look:

**The AP address goes in the *inherent* config.** The netif has to be born with its
address so the automatic DHCP-server start sees a valid one. Setting it afterwards does
not work: a `0.0.0.0` address makes the DHCP server fail to obtain a PCB, and a later
`esp_netif_set_ip_info()` then aborts with `DHCP_NOT_STOPPED`.

**Local delivery copies into a contiguous pbuf.** `esp_netif_receive()` wraps the frame
in a zero-copy, non-contiguous `PBUF_REF` with a custom-free callback. lwIP's
`pbuf_add_header()` refuses to prepend a link-layer header onto a non-contiguous pbuf,
and the custom-free path risks a double free once the mmpkt is already owned. The
example copies into a contiguous `PBUF_RAM` with link headroom and injects via
`netif->input`, which is what `esp_netif`'s own `wlanif_input` does.

**The Address-Extension callback returns `true`.** That tells the datapath the frame was
consumed, so it is not *also* delivered into the gate's own lwIP stack — which would
re-inject it back into the mesh.

## Interoperability

Gate discovery, the root announcement and the beacon gate bit are ported from
`net/mac80211` and verified in both directions against a mainline Linux 802.11s node: a
Linux gate bridging an off-mesh host to an ESP32 mesh node, and a Linux node
discovering an ESP32 gate, re-flooding its announcement, and learning a proxied source
in its `mpp` table.

## Verified on hardware

Three ESP32-S3 + FGH100M boards: this gate, one `mesh` node, and one AP client.

The gate brought the mesh (2.2 s) and the SoftAP (2.7 s) up **concurrently on one
MM6108**, emitted IS_GATE announcements, and served DHCP on `10.9.9.1`. The mesh node
reported `gates_known=1` — it discovered the gate from its RANN, with no prior traffic.
The client associated, DHCPed `10.9.9.2`, and reached the mesh node at `10.9.9.100`:
**13/15 replies with TTL=64**.

TTL is the load-bearing detail. A routed hop would have decremented it to 63; 64 proves
the traffic crossed a genuine layer-2 bridge. The run exercised DHCP, cross-bridge ARP,
proxied frames in both directions, the round trip, and proxy-ARP.

Judged by the Rimba regression fixture `test-mesh-gate-sta`, which reported `PASS`
against this example acting as the gate.
