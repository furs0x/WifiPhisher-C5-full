# WifiPhisher for ESP32 (audited fork)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](./LICENSE)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF-red)](https://docs.espressif.com/projects/esp-idf/)
[![Build](https://img.shields.io/badge/build-PlatformIO-orange)](https://platformio.org/)

This is an audited, hardened fork of [Alexxdal/ESP32WifiPhisher](https://github.com/Alexxdal/ESP32WifiPhisher).
It keeps the upstream feature set (Evil Twin, Karma, deauthentication, phishing portals,
sniffing/recon) and fixes a number of stability and access-control problems found during a
full source review. Firmware for ESP32 / S2 / S3 / C3 / C5 / C6 is built in CI and published
as GitHub Releases.

> **Legal notice:** this tool is for educational purposes and authorized security testing only.
> Use it only against networks and devices you own or have explicit written permission to test.
> See the [Disclaimer](#disclaimer) section before use.

## What this fork changes

Everything upstream does, plus:

- **Dual-band Evil Twin on ESP32-C5.** The rogue AP can now be cloned on the target's real
  5 GHz channel (the radio is switched to dual-band mode first) and only falls back to a
  2.4 GHz channel if the driver rejects the 5 GHz config. Previously the AP was always pinned
  to 2.4 GHz, so 5 GHz-only targets never produced a matching rogue AP.
- **Two Evil Twin modes.**
  - *Clone + Deauth* (default): clone the AP **and** actively knock clients off the real AP
    using standard 802.11 deauth frames (broadcast, spoofing the real BSSID) plus CSA/quiet
    beacon manipulation as a fallback.
  - *Clone only*: stand up the twin and transmit **no** attack frames at all.
- **PMF / WPA3 awareness.** The dashboard flags WPA3/PMF targets, where deauth frames are
  ignored by the client, so you know why a kick may not work.
- **Multi-AP channel tracking.** When one SSID is served by several APs, the tracker now latches
  the first matching BSSID instead of bouncing the radio across every sibling AP on each scan.
- **Stability fixes.** Bounded NVS string reads (no buffer overrun on long stored values),
  non-fatal rogue-AP cloning, and a dashboard that no longer floods the device with overlapping
  status polls or flaps "online / lost" on a fixed 3 s reconnect loop (exponential backoff now).
- **Admin console protection.** The dashboard lives on a random per-device path and requires a
  PIN; the operator WebSocket (`/ws`) requires the same authorization, and the raw
  `admin.html` is no longer reachable by direct URL. Root `/` always serves the public portal
  page, never the operator console.

The full list of findings and fixes is in [SECURITY_HARDENING.md](./SECURITY_HARDENING.md).

## Table of Contents

- [Features](#features)
- [Supported Hardware](#supported-hardware)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Captured data & admin console](#captured-data--admin-console)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [Further Reading](#further-reading)
- [License](#license)
- [Disclaimer](#disclaimer)

## Features

### Reconnaissance

- **Wi-Fi Packet Sniffer & Live Analyzer** — Promiscuous-mode capture with automatic channel
  hopping and a real-time packet analyzer streamed to the web UI, tracking nearby access points
  and clients (RSSI, packet/byte counters).
- **Host Discovery (Subnet Scan)** — ARP sweep of the connected subnet, listing live hosts' IP,
  MAC and hostname (with optional mDNS/SSDP service discovery).
- **Port Scanner** — TCP Connect and TCP SYN (stealth) probing with presets for the top 20/50
  ports, common IoT ports, or a custom list.

### Attacks

- **Evil Twin Attack** — Rogue AP that mimics the target network, with a selectable *Clone +
  Deauth* or *Clone only* mode and dual-band support on the C5.
- **Karma Attack** — Detect devices probing for known networks and answer them to lure them onto
  the rogue AP.
- **Deauther & Advanced Attacks** — Disconnection techniques ranging from classic deauth /
  disassociation frames to authentication/association floods, CSA channel-switch spoofing,
  EAPOL-Logoff/Start manipulation, EAP-Failure injection, WPA3 SAE-flood, PMF downgrade pressure
  and beacon spam.
- **Aircrack** — Capture a client's WPA/WPA2/WPA3 4-way handshake or PMKID and verify a candidate
  password entirely on-device.

### Phishing & Data Capture

- **Multiple Phishing Scenarios** — OS-native captive-portal prompt, fake router firmware-update
  page, fake plugin-update page, fake OAuth/social login, an iCloud-style Apple ID login, or an
  ISP-branded login auto-matched from the target SSID. The served page can be chosen manually or
  auto-selected from the visitor's User-Agent.
- **Captured Credentials Log** — Submitted forms are saved on-device and reviewed/exported only
  through the protected admin channel.
- **Handshake / PMKID Export** — Download a captured handshake (or PMKID-only capture) as a
  `.pcap` compatible with aircrack-ng and hashcat, directly from the web UI.

### Quality of Life

- **Persistent Configuration** — AP SSID/password/channel/TX rate are stored in flash and
  restored after reboot. (The operator's upstream STA password is never persisted.)
- **Bilingual Web UI** — Admin dashboard and captive portal in **English** and **Chinese** only.
- **Browser flashing** — merged single-image `.bin` per chip, flashable at offset `0x0` with
  <https://esptool.spacehuhn.com/> (or `esptool.py`). Serial logs via
  <https://terminal.spacehuhn.com/>.

## Supported Hardware

Firmware is built per-target with PlatformIO:

| PlatformIO environment | Board / chip                  |
|------------------------|-------------------------------|
| `esp32`                | ESP32 (esp32dev)              |
| `esp32s2`              | ESP32-S2                      |
| `esp32s3`              | ESP32-S3                      |
| `esp32c3`              | ESP32-C3                      |
| `esp32c5`              | ESP32-C5 (dual-band)          |
| `esp32c6`              | ESP32-C6                      |
| `cardputer`            | M5Stack Cardputer (ESP32-S3)  |

ESP32-C5 builds target `esp32-c5-devkitc-1`. The headless CI variant for the bare C5 uses
`-DUSE_DUMMY_TFT=1` (no display); see the per-chip releases.

## Getting Started

### Option A: Flash a prebuilt image (recommended)

Grab `esp32c5-merged.bin` (or the matching chip) from the
[Releases](https://github.com/furs0x/WifiPhisher-C5/releases) page and flash the
**merged** image at offset `0x0`.

**Recommended browser flasher:** <https://esptool.spacehuhn.com/>
(select the chip, choose "Merged" / single-file, offset `0x0`, load the `*-merged.bin`).
You can also flash from the CLI:

```bash
esptool.py --chip esp32c5 write_flash 0x0 esp32c5-merged.bin
```

The merged image already contains the bootloader, partition table, application **and** the
SPIFFS filesystem (web UI + portal pages), so you do **not** need a separate filesystem upload.

### Option B: Build from source

```bash
git clone https://github.com/furs0x/WifiPhisher-C5.git
cd WifiPhisher-C5
```

Open the project in VS Code with PlatformIO, select the target environment (e.g. `esp32c5`), then:

1. **General → Upload** (application)
2. **Platform → Upload Filesystem Image** (web UI + portal pages)

### Monitor logs

**Recommended browser serial terminal:** <https://terminal.spacehuhn.com/>
(connect over USB, 115200 baud) — no software install needed. Or from the CLI:

```bash
pio device monitor
```

## Usage

1. **Power on and join the rogue AP.** Default SSID `MagicWifi`. The AP password and channel are
   configurable in the admin console and stored in flash.

2. **Open the admin console.** The dashboard is **not** at a fixed `/admin.html`. On first boot a
   random path and a 4-digit PIN are generated and stored in flash. Read them from any of:
   - the **DASHBOARD ACCESS** panel on the Evil Twin page (AP URL + path + PIN), or
   - the serial console at boot:
     ```
     Admin dashboard: http://192.168.4.1/xxxxxxxx
     Access PIN     : 1234
     ```
   Open `http://<ap-ip>/<random-path>` and enter the PIN. Visiting the bare IP (`/`) shows the
   public portal page, never the console.

3. **Configure the attack.** Select the target network, choose a phishing page, and pick the
   Evil Twin **attack mode**:
   - *Clone + Deauth* — knock clients off the real AP and lure them to the twin.
   - *Clone only* — just mirror the AP; sends no attack frames.
   A WPA3/PMF warning appears when the target uses protected management frames.

4. **Run the attack.** The twin comes up on the target's band/channel; clients that reconnect
   are served the selected portal page.

## Captured data & admin console

- The admin console is protected by a random path **and** a PIN; the operator WebSocket is
  gated by the same authorization and the raw `admin.html` cannot be fetched directly.
- Captured credentials are viewable only from the authenticated console.
- To regenerate the admin path/PIN, erase the NVS partition (e.g.
  `esptool.py --chip esp32c5 erase_region 0x9000 0x6000`) and reboot.

## Roadmap

- **Task stack audit** — raise stack sizes / move large stack frames to heap for the evil-twin,
  password-manager and Karma tasks (tracked upstream as a stack-overflow crash).
- **Complete the Port Scanner** — FIN, NULL, XMAS, ACK and UDP methods (UI present; only TCP
  Connect/SYN implemented).
- **Session Reports** — export a single summary of a test session instead of per-result pulls.
- **Customizable Phishing Templates** — upload a custom logo/template from the web UI.

## Contributing

Contributions are welcome — phishing scenarios, performance, new hardware, and bug fixes.
Please keep changes within authorized-testing use.

## Further Reading

- Schepers, D., Ranganathan, A., & Vanhoef, M. (2022). [*On the Robustness of Wi-Fi
  Deauthentication Countermeasures*](./doc/wisec2022.pdf). WiSec '22.
- Cayre, R., Cauquil, D., & Francillon, A. (2023). [*ESPwn32: Hacking with ESP32
  System-on-Chips*](./doc/woot23-paper22.pdf). WOOT '23.

## License

MIT — see [LICENSE](./LICENSE).

## Disclaimer

This tool is intended strictly for educational purposes and ethical security testing in
controlled environments. Using it against networks or devices without explicit permission is
illegal and punishable by law. You are solely responsible for how you use it.
