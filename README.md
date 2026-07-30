# esp-halow-examples

Example applications for [`mm-esp32-halow`](https://github.com/teapotlaboratories/mm-esp32-halow)
— Wi-Fi HaLow (802.11ah) on an **ESP32-S3 + Morse Micro MM6108**.

Upstream `esp-halow` gives you HaLow **STA + SoftAP**. That fork adds 802.11s meshing,
mesh security, a mesh↔Wi-Fi layer-2 bridge, ad-hoc networking and TWT battery-leaf
sleep. These are the examples for those features — one per feature area, each a
complete, buildable project.

> **Experimental & AI-assisted.** Part of the experimental
> [Rimba](https://github.com/teapotlaboratories/rimba) research protocol —
> **not production-ready**. Much of the code and documentation here is produced with
> AI-assisted (agentic) coding, directed and reviewed by the maintainer.
>
> Every example here has been **run on real hardware** (see below), but each is a
> demonstration of one feature, not a hardened application. Treat them as a starting
> point rather than a reference implementation.

## The examples

| Example | What it demonstrates | Verified on hardware |
|---|---|---|
| [`mesh`](examples/mesh/) | Join an encrypted 802.11s mesh — peering, HWMP, multi-hop relaying. A-MPDU aggregation and leaf mode are menuconfig options | Peers ESTAB (SAE+AMPE) in ~5 s; A-MPDU advertised; **interoperates with a mainline Linux 802.11s node** — pings both ways |
| [`mesh_gate`](examples/mesh_gate/) | A mesh **and** a co-channel SoftAP on one radio, layer-2 bridged so AP clients reach mesh nodes zero-config on one flat subnet | 3-node bench: client DHCPs `10.9.9.2`, reaches a mesh node **13/15 with TTL=64** — a pure L2 bridge, no routed hop. Mesh node discovered the gate by RANN |
| [`ibss`](examples/ibss/) | Infrastructure-free ad-hoc cell: no AP, no association, beacon-based peer discovery | 2-board cell up in 2.3 s, roles self-assigned, mutual beacon discovery, bidirectional ping 95/98 |
| [`twt_powersave`](examples/twt_powersave/) | Both sides of a low-power link — a TWT-requesting station that dozes, and a SoftAP answering TWT and WNM-sleep | Associated in ~6 s, AP saw `aid=1`, **TWT agreement INSTALLED on flow 0**, held 66 s |
| [`ap_scale`](examples/ap_scale/) | A high-density SoftAP: up to 255 stations, per-STA state in PSRAM | Came up at `max_stas=255` with per-STA state in PSRAM; real client authorized; heap flat over the run |

Each has its own README covering the APIs it uses, the options it exposes, and the
traps specific to it.

### What that verification does and does not cover

Tested on an ESP32-S3 + Quectel FGH100M bench (three boards, MM6108 firmware 1.17.8),
plus a Raspberry Pi HaLow node running mainline `morse_driver` for the mesh interop.

It covers each example's **default configuration** end to end. It does **not** cover
every menuconfig combination — notably `EXAMPLE_TWT_ACTION_FRAME` (mid-session TWT),
`EXAMPLE_STA_WNM_SLEEP`, `EXAMPLE_MESH_LEAF`, and `EXAMPLE_MESH_AMPDU=n` are untested.
Nor does it make any power claim: "dozing" is read from the negotiated agreement state,
not measured with a current meter.

Bench nodes sit close together at full transmit power, which causes receiver overload
and occasional single-digit packet loss. That is a property of the test bench, not of
these examples — the Rimba regression fixtures cap transmit power to avoid it, and
these do not.

## Getting started

Everything is vendored — the HaLow component, the MM6108 firmware, ESP-IDF, and the
toolchain. **Two commands from a fresh clone to a built binary:**

```bash
git clone --recurse-submodules https://github.com/teapotlaboratories/esp-halow-examples
cd esp-halow-examples

make install-toolchain          # once per checkout (~3.8 GB, into ./.espressif)
make build APP=mesh             # and you are building
```

That is the whole setup. No `. export.sh`, no manual `pip install cmake ninja`, no
system packages, nothing written outside this directory.

If you cloned without `--recurse-submodules`, run `git submodule update --init
--recursive` first — the Makefile will tell you so if you forget.

### What `install-toolchain` does, and where it puts things

The ESP-IDF submodule gives you ESP-IDF's *source* — build system, components,
`idf.py`. It does not contain compilers. Those (`xtensa-esp-elf` gcc/gdb, `openocd`,
and a Python virtualenv) are gigabytes of architecture-specific binaries that ESP-IDF
downloads separately.

ESP-IDF normally puts them in `~/.espressif`, shared machine-wide. **This repo pins them
inside the checkout instead** (`IDF_TOOLS_PATH=./.espressif`), so:

- the checkout is self-contained — `rm -rf .espressif` removes every trace;
- you get exactly the toolchain version this ESP-IDF asks for, not whatever another
  project last installed into the shared location;
- **other ESP-IDF projects on your machine are untouched** and keep using their own.

The cost is that it is not shared: about 3.8 GB per checkout (3.2 GB of tools — which
includes `riscv32-esp-elf`, because the ESP32-S3's ULP coprocessor is RISC-V — plus a
496 MB download cache you can delete afterwards, and an 89 MB virtualenv). The chip
target is read from your board overlay, so it installs only what your board needs.

It also installs `cmake` and `ninja` into that virtualenv, pinned to `cmake==3.30.5`
(ESP-IDF's build system does not work with cmake 4). ESP-IDF does not bundle them on
Linux and `idf.py` refuses to run without them.

> **Prefer the machine-wide install, or already have ESP-IDF?** Both are one variable:
>
> ```bash
> make build APP=mesh IDF_TOOLS_PATH=$HOME/.espressif   # share the usual location
> make build APP=mesh IDF_PATH=/path/to/your/esp-idf    # use your own ESP-IDF (v5.4.2+)
> ```
>
> The vendored copies exist to pin a known-good version, not to force their use.

Then build and flash. No `. export.sh` needed — the Makefile sources the ESP-IDF
environment for you:

```bash
make list                                            # what can I build?
make build APP=mesh BOARD=proto1-fgh100m
make flash APP=mesh BOARD=proto1-fgh100m PORT=/dev/ttyACM0
make monitor APP=mesh PORT=/dev/ttyACM0              # Ctrl-] to quit
make flash-monitor APP=mesh PORT=/dev/ttyACM0        # both
```

`make help` lists every target. `APP` selects the example, `BOARD` selects the board
overlay, `PORT` is the serial device. Builds are **out-of-source**: `make build
APP=mesh BOARD=proto1-fgh100m` writes to `build/mesh/proto1-fgh100m/`, never inside the
example. Each app+board pair gets its own generated `sdkconfig`, so switching boards
never reuses a stale config.

## Configuring an example

```bash
make menuconfig APP=mesh
```

Each example exposes its own options — Mesh ID, channel, SSID, TWT interval and so on —
under its *… example configuration* menu. See the example's README for the full table.

## Boards

Board overlays live in [`boards/<BOARD>/`](boards/) and own the SPI wiring, the chip
target, the flash/PSRAM layout, the regulatory domain and the MM6108 firmware + Board
Config File. `proto1-fgh100m` (Seeed XIAO ESP32-S3 + Quectel FGH100M) ships as the
default.

To add your own, copy that directory and edit it. **Two settings have no usable
default and are the cause of almost every "it built but the radio is dead":**

- **`CONFIG_HALOW_COUNTRY_CODE`** — defaults to `"??"`, which builds perfectly and
  leaves the channel list empty, so nothing scans or beacons. Valid:
  `AU CA EU GB IN JP KR NZ US`.
- **The SPI and control pins** — `MM_RESET_N`, `MM_WAKE`, `MM_BUSY`, `MM_SPI_IRQ`,
  `MM_SPI_CS`, `MM_SPI_SCK`, `MM_SPI_MISO`, `MM_SPI_MOSI`. The component's Kconfig
  defaults are one particular development board's wiring and are unlikely to match
  yours.

Because these live in the board overlay rather than in each example, `make` always
applies them — which is why the examples here are buildable as-is, and why building one
by hand with a bare `idf.py` in the example directory is not recommended.

## Layout

| Path | What |
|---|---|
| `Makefile` | build/flash/monitor wrapper around `idf.py` |
| `examples/<name>/` | the examples — one ESP-IDF project each |
| `boards/<name>/` | board overlays (pins, target, country, firmware + BCF) |
| `components/halow/` | submodule → `mm-esp32-halow` (the HaLow component) |
| `vendor/esp-idf/` | submodule → ESP-IDF, pinned v5.4.2 |
| `vendor/morse-firmware/` | submodule → MM6108 firmware blobs, pinned 1.17.8 |
| `cmake/mm-fw-gen/` | generates the `firmware` component from `vendor/morse-firmware` at build time |
| `build/<APP>/<BOARD>/` | build output (gitignored) |

No firmware blobs are committed: `cmake/mm-fw-gen/` converts the upstream ELF in
`vendor/morse-firmware` to Morse TLV `.mbin` at build time, so the chip firmware version
is whatever that submodule is pinned at (**1.17.8**). Keep it matched to any Linux HaLow
nodes you interoperate with.

## Hardware

- **MCU** — ESP32-S3 with PSRAM. `proto1-fgh100m` assumes an ESP32-S3R8: 16 MB QIO
  flash, 8 MB octal PSRAM. `ap_scale` needs PSRAM; the rest are far more forgiving.
- **Radio** — Morse Micro MM6108 over SPI, chip ID `0x0306`.
- **Console** — native USB-Serial-JTAG, enumerating as `/dev/ttyACM*`, used for both
  flashing and the serial monitor.

## License

Apache-2.0, following upstream Morse Micro.
