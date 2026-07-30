# TWT and WNM-sleep power-save

Both sides of a low-power HaLow link. Select a role in menuconfig and flash two
boards: one as the SoftAP, one as the sleeping station.

Target Wake Time lets a station stop waking for every DTIM beacon and wake on an
agreed schedule instead. WNM-sleep goes further — the station stays associated while
its radio sleeps across many DTIM periods, with the AP holding downlink traffic until
it comes back. Together they are the difference between a battery node lasting weeks
and lasting days.

## What it demonstrates

| | API |
|---|---|
| TWT requested at association (portable path) | `mmwlan_twt_add_configuration()` before connect |
| TWT requested mid-session | `mmwlan_twt_setup_request()` |
| Tear a TWT agreement down | `mmwlan_twt_teardown()` |
| Agreement state | `mmwlan_twt_agreement_installed()` |
| Radio power-save | `mmwlan_set_power_save_mode()` |
| Extended sleep, still associated | `mmwlan_set_wnm_sleep_enabled_ext()` |
| AP-side TWT + WNM-sleep responders | on by default for an AP vif |

## Configuration

`idf.py menuconfig` → *TWT power-save example configuration*:

| Option | Default | Notes |
|---|---|---|
| `EXAMPLE_ROLE` | Station | Station (requester) or SoftAP (responder) |
| `EXAMPLE_WIFI_SSID` / `_PSK` | `MorseMicroESP32AP` / `12345678` | Must match on both boards |
| `EXAMPLE_S1G_CHANNEL` / `_OPCLASS` | `27` / `68` | 915.5 MHz / 1 MHz, US |
| `EXAMPLE_TWT_WAKE_INTERVAL_US` | `10000000` | 10 s between service periods |
| `EXAMPLE_TWT_MIN_WAKE_DURATION_US` | `65280` | Awake window per service period |
| `EXAMPLE_TWT_ACTION_FRAME` | `n` | Negotiate mid-session instead of at assoc |
| `EXAMPLE_STA_WNM_SLEEP` | `n` | Enter WNM-sleep with chip power-down |

You also need the board's SPI pins and `CONFIG_HALOW_COUNTRY_CODE` — it defaults to
`"??"`, which builds fine but leaves the radio unable to come up.

## Running it

Flash the AP board first, then the station:

```
I (1898) twt_powersave: === Station: TWT requester ===
I (1898) twt_powersave: TWT requester queued for the association request (ret=0)
I (6087) twt_powersave: associated to "MorseMicroESP32AP"
I (6087) twt_powersave: TWT agreement INSTALLED on flow 0 (wake interval 10 s)
I (6087) twt_powersave: power-save enabled — dozing between service periods
I (36088) twt_powersave: still associated, twt_installed=1
```

## Two ways to negotiate TWT, and why the default is the assoc-embedded one

**Assoc-embedded (default).** Registering the TWT configuration with
`mmwlan_twt_add_configuration()` *before* connecting makes morselib carry the request
inside the (re)association IEs, where the AP's assoc-time TWT responder handles it.
hostapd enables such a responder by default, so this path works broadly.

**Mid-session action frame** (`EXAMPLE_TWT_ACTION_FRAME=y`). Associate first, then send
a TWT Setup Request action frame with `mmwlan_twt_setup_request()`. This is
assoc-preserving and can be renegotiated at runtime without dropping the link — but
not every AP answers it, so it is not the portable default.

Either way the negotiation runs
`EMPTY → PENDING_RESPONSE → PENDING_INSTALLATION → INSTALLED`, so an agreement that is
not installed the instant the request goes out is normal. The example polls
`mmwlan_twt_agreement_installed(0)` for up to 20 seconds and reports what it got.

## Power-save has to be enabled explicitly

**TWT and WNM-sleep save nothing unless 802.11 power-save is on.** This is the single
easiest thing to get wrong: everything associates, everything passes traffic, the TWT
agreement installs — and the radio simply never sleeps.

morselib's own default is `MMWLAN_PS_ENABLED`, and `mmhalow_init()` force-*disables*
power-save unless `CONFIG_HALOW_PS_MODE` is set. This example deliberately leaves that
Kconfig **off** and calls `mmwlan_set_power_save_mode(MMWLAN_PS_ENABLED)` itself once
associated — which works regardless of the Kconfig, and is portable.

> **Do not turn `CONFIG_HALOW_PS_MODE` on unless your board has the BUSY and WAKE pins
> wired.** The MM6108 signals over those lines when it may be slept. Without them the
> host and chip cannot agree and **the station never associates** — a 30 second timeout
> with no diagnostic, because morselib's detail does not reach the console. It reads as
> a broken example rather than as a wiring problem.
>
> Both halves are bench-verified. On a board without those pins, setting it to `y`
> breaks association outright. On a fully-wired board it associates in ~5.7 s and TWT
> still reaches INSTALLED — so power-save being active across the SAE handshake is
> *not* a problem in itself. The wiring is the whole story.
>
> Since an example cannot know how your board is wired, it leaves the option off.

## WNM-sleep

With `EXAMPLE_STA_WNM_SLEEP=y` the station enters WNM-sleep with chip power-down after
the TWT phase. It stays associated while the transceiver is powered down, and the AP
buffers its downlink until it exits.

Two requirements:

- **PMF.** The WNM-Sleep-Enter/Exit exchange is a robust management frame, so the link
  needs management-frame protection. The AP role sets `MMWLAN_PMF_REQUIRED`.
- **A responder on the AP.** The AP role here implements one. Against a third-party AP,
  `mmwlan_set_wnm_sleep_enabled_ext()` returning non-success generally means the AP
  declined rather than that anything is wrong locally.

Do not queue traffic for transmission while in WNM-sleep.

## The AP role

The SoftAP needs no application code to be a responder: the MM6108 enables the TWT
responder by default on an AP vif, and the WNM-sleep responder answers Enter/Exit
requests as part of the AP's management path. The role exists in this example so you
have a known-good peer to test the station against.

One detail that is easy to miss: **in AP mode no link-up event is ever fired**, so the
netif is never brought up and lwIP cannot answer ICMP. The example calls
`esp_netif_action_connected()` explicitly — that single call is what makes the AP
reachable over IP.

## Verified on hardware

One board in each role. The AP came up in 3.9 s; the station associated in 6.1 s; the
AP logged it as `authorized (aid=1)`; and the **TWT agreement reached INSTALLED on flow
0**, still installed 66 s later. The request rode in the association IEs — the
assoc-embedded path.

Not covered: `EXAMPLE_TWT_ACTION_FRAME` (mid-session negotiation),
`EXAMPLE_STA_WNM_SLEEP`, and the power saving itself — "dozing" is read from the
agreement state, not measured with a current meter.
