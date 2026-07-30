# Examples

## Upstream Morse Micro examples

The starting point for any HaLow bring-up — station, SoftAP, scanning, throughput.

| Example | What it does |
|---|---|
| [`scan`](scan/) | Boot the MM6108 and scan for HaLow APs |
| [`sta_connect`](sta_connect/) | Associate to a HaLow AP as a station |
| [`softap`](softap/) | Run a HaLow SoftAP |
| [`sta_reboot`](sta_reboot/) | Station association across reboots |
| [`dual_if`](dual_if/) | HaLow alongside a second interface |
| [`iperf`](iperf/) | Throughput measurement |
| [`porting_assistant`](porting_assistant/) | Validate a new board's SPI / HAL port |

## Fork examples

The features this fork adds on top: 802.11s meshing, mesh security, a mesh-to-Wi-Fi
layer-2 bridge, ad-hoc networking, and battery-leaf power-save.

| Example | What it does |
|---|---|
| [`mesh`](mesh/) | Join an encrypted 802.11s mesh — peering, HWMP, multi-hop relaying. A-MPDU aggregation and leaf mode are menuconfig options |
| [`mesh_gate`](mesh_gate/) | A mesh **and** a co-channel SoftAP on one radio, layer-2 bridged so AP clients reach mesh nodes zero-config on one flat subnet |
| [`ibss`](ibss/) | Infrastructure-free ad-hoc cell: no AP, no association, peer discovery from beacons |
| [`twt_powersave`](twt_powersave/) | Both sides of a low-power link — a TWT-requesting station that dozes, and a SoftAP that answers TWT and WNM-sleep |
| [`ap_scale`](ap_scale/) | A high-density SoftAP: up to 255 stations, per-STA state in PSRAM |

## Building the upstream examples

Each is a standalone ESP-IDF project that resolves the component from the ESP Component
Registry, so it builds the usual way:

```
cd <example>
idf.py set-target esp32s3
idf.py menuconfig
idf.py build flash monitor
```

## Building the fork examples

> **The command above does not work for the five fork examples yet.**
>
> They follow the same upstream idiom — including a `main/idf_component.yml` that
> resolves `morsemicro/halow` from the registry — but every one of them uses an API
> this fork added *after* the `2.10.4-esp32-2` fork point:
>
> | Example | Needs | Added by |
> |---|---|---|
> | `mesh`, `mesh_gate` | `umac/mesh/umac_mesh.h` | `c16e9a8a` |
> | `ibss` | `umac/ibss/umac_ibss.h` | `f4778430` |
> | `twt_powersave` | `mmwlan_twt_setup_request()` | `eeb15af9` |
> | `mesh`, `twt_powersave` | `mmwlan_*_advertised()` / `_installed()` | `3c8ded0d` |
> | `ap_scale` | `CONFIG_HALOW_AP_MAX_STAS` | `6a698edf` |
>
> So `idf.py build` fetches the published 2.10.4 component and **fails to compile**. It
> fails loudly rather than producing a binary that quietly lacks the feature, but it
> fails.
>
> Until this fork's component is resolvable — published, or pinned by path — build them
> from the [Rimba](https://github.com/teapotlaboratories/rimba) superproject, which
> supplies the component, the MM6108 firmware and a board configuration:
>
> ```
> make example-build EXAMPLE=mesh BOARD=<board>
> make example-flash EXAMPLE=mesh PORT=/dev/ttyACM0
> ```

## Configuration that is never optional

Two things in `menuconfig` have no usable default:

- **`CONFIG_HALOW_COUNTRY_CODE`** (*Wi-Fi HaLow Connection Manager*). It defaults to
  `"??"`, which builds perfectly and then leaves the radio unable to come up — the
  channel list is empty, so scanning and beaconing simply fail. Valid values are
  `AU CA EU GB IN JP KR NZ US`.
- **The SPI and control pins** (*Morse Micro Shim Configuration*): `MM_RESET_N`,
  `MM_WAKE`, `MM_BUSY`, `MM_SPI_IRQ`, `MM_SPI_CS`, `MM_SPI_SCK`, `MM_SPI_MISO`,
  `MM_SPI_MOSI`. The defaults are one particular development board's wiring and are
  very unlikely to match yours.

A dead radio on a build that compiled cleanly is almost always one of those two.
