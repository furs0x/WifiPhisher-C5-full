# Security hardening

Baseline: `Alexxdal/ESP32WifiPhisher` @ `88f58cb86bbafd6b7e27ca7514137af91b4dd8db`.

## Behavior changes that affect deployment

1. **Management interface is protected by a random path + PIN.**
   The console is no longer served from `/admin.html` or from `/`. On first boot the
   firmware generates a random 8-character path and a 4-digit PIN, stores both in NVS,
   and prints them to the serial console. The dashboard is reachable only at that path,
   and only after the PIN is entered (`?pin=` once, then an `adminpin` cookie). The raw
   `/admin.html` and `/_preview.html` return 404 over the static handler, and `/` always
   serves the public portal page — never the operator console. Regenerate by erasing NVS.

   The operator WebSocket `/ws` requires the same `adminpin` cookie at upgrade time, so
   a client that did not pass the PIN gate cannot pull captured credentials or drive
   attacks. (The visitor-facing `/portal-ws` stays open for the phishing pages only.)

2. **Captured credentials moved out of the static web root.**
   Stored under `/spiffs/private/credentials.log`; the wildcard static handler rejects `passwords`/`credentials`/`private` paths and `..`. Retrieval is only via the authenticated WebSocket `GET_PASSWORDS` command, served as `text/plain` + `nosniff`.

3. **No password persistence or retrieval of the operator's own STA password.**
   Connecting to an upstream Wi-Fi no longer stores its password in NVS; the “last credentials” API returns only the SSID. The UI no longer pre-fills the password field.

4. **Credential logging redacted.** The capture log no longer prints the password; invalid-JSON payload previews are not logged.

5. **DOM XSS fixed for wireless-controlled data.** SSID, probe, packet-analyzer, recon/client, handshake-bank, host-discovery and BLE device fields now use `textContent`/DOM nodes and event listeners instead of interpolated `innerHTML`/inline `onclick`. Static locally-defined SVG icon markup is still assigned, which is not untrusted input.

6. **WebSocket protection.** `/ws` handshake requires the `adminpin` cookie (see #1) and a `Origin` that exactly matches `http://<Host>`. `/portal-ws` only accepts the visitor command whitelist.

7. **NVS string reads are bounds-checked.** `read_string_from_nvs_sized()` refuses a stored value larger than the caller's buffer instead of overflowing it; all fixed-buffer callers use it.

8. **Dual-band Evil Twin (C5).** The radio is switched to dual-band mode before cloning so a 5 GHz target is cloned on its own channel; a rejected 5 GHz config falls back to 2.4 GHz instead of aborting. `wifi_ap_clone()` returns an error instead of `ESP_ERROR_CHECK`-ing, so a rejected config does not reboot the device.

9. **Clone-only Evil Twin mode.** A new attack mode stands the twin up with **zero** attack frames transmitted; the default mode also sends standard 802.11 deauth frames (broadcast) in addition to CSA/quiet beacons.

10. **Multi-AP channel lock.** With several APs sharing one SSID, the tracker latches the first matching BSSID instead of rewriting the operating channel to whichever sibling the scan returned first.

11. **Dashboard poll hygiene.** Status polling runs one request at a time with an 8 s timeout and exponential reconnect backoff (3 s→30 s), instead of overlapping 2 s polls with a 30 s timeout and a fixed 3 s reconnect loop.

12. **Build provenance.** PlatformIO platform pinned to `espressif32@6.12.0`; the C5 fork pinned to commit `8215262…708b4`. `tools/libwifi_mod.bat` still exists but is not wired into the build; run it consciously and in an isolated SDK if raw-frame injection is needed.

## Not yet addressed / verify before relying on it

- Credentials are still stored in plaintext in SPIFFS and NVS is unencrypted on these default configs. Use dedicated hardware, not the operator's main network, and evaluate flash/NVS encryption.
- HTTP is used, not TLS. The admin console is behind a random path + PIN and `/ws` requires the same authorization, but the PIN and cookie travel over plain HTTP, so a client already on the same open AP could in principle sniff an unlocked session. Prefer operating admin over the device's own WPA AP, and treat the PIN as a lock against casual visitors, not a cryptographic guarantee.
- The evil-twin / password-manager / Karma tasks have known small stack sizes; some capture or heavy-scan paths can still hit a stack overflow (tracked on the README roadmap). Raising the task stacks / moving large frames to heap is still pending.
- `esptool.exe` source has not been verified against an official build; build from source where possible.

This snapshot is a hardening patch, not a claim that upstream code is trustworthy. Review every diff before building.

## UI redesign (magpie style)

`data/` follows magpie's visual language (MIT, yetone/magpie), phone-first:

- Design tokens (light `#f4f4f6` / dark `#1a1a1e`, indigo accent `#4f46e5` / `#a5b4fc`), hairline borders, pill chips.
- Three-zone shell (sidebar / header / views); under 760px the sidebar becomes an off-canvas drawer, inputs are 16px so iOS does not zoom on focus.
- Language is **English and Chinese only** (`data/lang/{en,zh}.json`, `data/lang/portal_{en,zh}.json`).

Verification: inline scripts pass `node --check`; firmware builds in CI for all targets; the merged C5 image (bootloader + partition table + app + SPIFFS) has been flashed and boots (admin login verified). Device-side stack-overflow paths remain as noted above.

