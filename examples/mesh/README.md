# 802.11s mesh point

An ESP32 + MM6108 joining an encrypted 802.11s Wi-Fi HaLow mesh: peering, multi-hop
forwarding, and per-node addressing. One binary runs on every node.

Everything that makes the mesh work — Mesh Peering Management, SAE authentication,
AMPE key exchange, CCMP data encryption, HWMP path selection and multi-hop relaying —
runs inside `morselib`. The application's job is only to start the interface and give
it an IP address; a peer link forms automatically as soon as a neighbour running the
same Mesh ID on the same channel is heard.

## What it demonstrates

| | API |
|---|---|
| Start / stop a mesh interface | `mmwlan_mesh_start()`, `mmwlan_mesh_stop()` |
| Established peer telemetry | `mmwlan_mesh_peer_count()` |
| Discovered mesh gates | `mmwlan_mesh_gate_count()` |
| A-MPDU aggregation | `mmwlan_set_ampdu_enabled()` |
| Leaf / single-hop mode | `mmwlan_mesh_set_multihop()` |

## Configuration

`idf.py menuconfig` → *Mesh example configuration*:

| Option | Default | Notes |
|---|---|---|
| `EXAMPLE_MESH_ID` | `morse-mesh` | Must match on every node |
| `EXAMPLE_MESH_S1G_CHANNEL` | `27` | 915.5 MHz / 1 MHz, US op-class 68 |
| `EXAMPLE_MESH_MAX_PLINKS` | `16` | Peer table is sized to 16 |
| `EXAMPLE_MESH_IPV4_PREFIX` | `10.9.9.` | Host octet derived from the MAC |
| `EXAMPLE_MESH_AMPDU` | `y` | Aggregation on the mesh interface |
| `EXAMPLE_MESH_LEAF` | `n` | Peer, but never relay for others |

You also need to set the board's SPI pins and, critically,
`CONFIG_HALOW_COUNTRY_CODE` — it defaults to `"??"`, which builds fine but leaves the
radio unable to come up.

## Running it

Flash two or more boards with the same image. Each derives a unique mesh MAC from its
own efuse MAC and a static address `10.9.9.<100 + (mac[5] & 0x3f)>`, so no per-node
configuration is needed:

```
I (720) mesh: === 802.11s secured mesh point ===
I (1409) mesh: A-MPDU aggregation enabled
I (1409) mesh: starting mesh (id="morse-mesh" chan=27 mac=e2:72:a1:f8:ef:a4)
I (2295) mesh: mesh interface up, beaconing on channel 27
I (2295) mesh: firmware advertises A-MPDU: 1
I (5795) mesh: static IP 10.9.9.136/24 (netif up=1)
I (7295) mesh: uptime=5s established_peers=1 gates_known=0 heap=8623796
I (7295) mesh:   peer[0] e2:72:a1:f8:f9:40
```

Once `established_peers` is non-zero the nodes can reach each other over IP — ping one
node's address from another.

## Addressing

A plain mesh has no DHCP server, so each node addresses itself from its own MAC. Two
details matter:

- **The netif MAC must equal the mesh MAC.** The mesh vif transmits as `if_addr`; if
  the netif keeps a different address then peers learn our IP against the wrong MAC and
  their replies never reach the mesh interface. The example calls
  `esp_netif_set_mac()` with the same address it passed to `mmwlan_mesh_start()`.
- **No gateway.** A plain mesh node is on one flat subnet with its peers. See the
  `mesh_gate` example for reaching hosts *off* the mesh.

## A-MPDU aggregation

`mmwlan_set_ampdu_enabled()` may only be called while MMWLAN is inactive, so the
example sets it before `mmwlan_mesh_start()`. Aggregation is assembled by the MM6108
firmware and negotiated per peer over a Block Ack (ADDBA) handshake. On a three-node
bench it is worth roughly +37% single-hop and +38% relay throughput.

`mmwlan_ampdu_capability_advertised()` reports whether the firmware advertises the
capability on this vif — necessary, but not sufficient: real aggregation also needs a
peer that completes the ADDBA handshake.

## Leaf mode

With `EXAMPLE_MESH_LEAF=y` the node peers with its direct neighbours as normal but
never relays another node's traffic, never uses a multi-hop path for its own traffic,
and emits no HWMP PREQ/PREP/PERR. Because it stops advertising paths as well as
stopping forwarding, peers do not route through it — it is not a black hole. This is
stricter than the Linux `dot11MeshForwarding`, which only stops forwarding for others.

Useful for a battery-powered node that should not spend energy relaying for the mesh.

## Mesh security

The mesh runs secured by default: SAE authentication, AMPE key exchange, and CCMP on
data frames, ported from `net/mac80211` and interoperable with a mainline Linux
802.11s node.

> **The SAE password is currently a compile-time constant inside `morselib`**
> (`MESH_SAE_PASSWORD` in `umac_mesh.c`), not an argument to `mmwlan_mesh_start()`.
> Change it there, and keep it matched with any Linux peer's `sae_password`. Exposing
> it through `struct mmwlan_mesh_args` is outstanding work.

Mesh peers deliberately run **MFP=no**, matching `net/mac80211` — the Linux mesh has no
`ieee80211w`. Robust management frames (ADDBA/DELBA, unicast PREP/PERR) are therefore
neither protected on transmit nor rejected if unprotected on receive. This is
interop-load-bearing: a Linux peer answers only the *unprotected* ADDBA, so requiring
PMF would break cross-vendor Block Ack. Data confidentiality and integrity are
unaffected.

## Verified on hardware

Two ESP32-S3 + FGH100M boards (MM6108 firmware 1.17.8). Both reached
`established_peers` within 5 s over SAE + AMPE, the firmware advertised A-MPDU, and
each pinned its derived address (`10.9.9.136` / `10.9.9.100`).

A mainline Linux 802.11s node then joined the same mesh, peered with **both** ESP nodes
(`iw station dump` → `mesh plink: ESTAB`) and pinged them: 10/10 to one, 9/10 to the
other — the one loss being the first packet, which pays for HWMP path discovery. The
ESP side correspondingly reported `established_peers=2` including the Linux MAC. Heap
was flat across the run.

Not covered: `EXAMPLE_MESH_LEAF`, `EXAMPLE_MESH_AMPDU=n`, or any throughput figure.
