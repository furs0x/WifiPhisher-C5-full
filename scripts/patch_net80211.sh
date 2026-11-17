#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Linux/macOS equivalent of tools/libwifi_mod.bat
#
# The ESP-IDF prebuilt libnet80211.a exports a strong
# ieee80211_raw_frame_sanity_check(), which rejects raw 802.11 management
# frames. src/wifi/wifiMng.c provides its own version returning 0, but the
# linker prefers the strong symbol from the archive, so on a clean toolchain
# the SDK copy wins and deauth / beacon / evil-twin injection silently fails.
#
# This weakens the copy inside the PlatformIO package so the project's
# definition takes over. It touches the SHARED PlatformIO package directory,
# exactly like the .bat does on Windows.
#
# Usage:
#   ./scripts/patch_net80211.sh            # patch every installed arch
#   ./scripts/patch_net80211.sh esp32s3    # patch one arch
#   ./scripts/patch_net80211.sh --restore  # restore the .orig backups
# ---------------------------------------------------------------------------
set -euo pipefail

SYMBOL="ieee80211_raw_frame_sanity_check"
PIO_PKGS="${PLATFORMIO_PACKAGES_DIR:-$HOME/.platformio/packages}"
FW="$PIO_PKGS/framework-espidf/components/esp_wifi/lib"

if [[ ! -d "$FW" ]]; then
  echo "error: ESP-IDF package not found at $FW" >&2
  echo "       install the platform first: pio run -e esp32s3" >&2
  exit 1
fi

# arch -> objcopy prefix
objcopy_for() {
  case "$1" in
    esp32|esp32s2|esp32s3) echo "xtensa-$1-elf-objcopy" ;;
    esp32c3|esp32c5|esp32c6|esp32c61|esp32h2|esp32p4) echo "riscv32-esp-elf-objcopy" ;;
    *) echo "" ;;
  esac
}

TOOLCHAIN_DIRS=(
  "$PIO_PKGS"/toolchain-xtensa-esp-elf/bin
  "$PIO_PKGS"/toolchain-riscv32-esp/bin
)

find_objcopy() {
  local want="$1"
  for d in "${TOOLCHAIN_DIRS[@]}"; do
    [[ -x "$d/$want" ]] && { echo "$d/$want"; return 0; }
  done
  command -v "$want" 2>/dev/null && return 0
  return 1
}

MODE="patch"
ONLY=""
if [[ "${1:-}" == "--restore" ]]; then MODE="restore"; elif [[ -n "${1:-}" ]]; then ONLY="$1"; fi

patched=0; failed=0; skipped=0

for archdir in "$FW"/*/; do
  arch="$(basename "$archdir")"
  [[ -n "$ONLY" && "$arch" != "$ONLY" ]] && continue
  lib="$archdir/libnet80211.a"

  if [[ "$MODE" == "restore" ]]; then
    if [[ -f "$lib.orig" ]]; then
      cp -f "$lib.orig" "$lib" && echo "restored  $arch"
      patched=$((patched+1))
    fi
    continue
  fi

  [[ -f "$lib" ]] || continue

  oc="$(find_objcopy "$(objcopy_for "$arch")" || true)"
  if [[ -z "$oc" ]]; then
    echo "skip      $arch (no objcopy for this target)"
    skipped=$((skipped+1)); continue
  fi

  # already weak? compare the symbol binding before touching anything
  nm_bin="${oc%objcopy}nm"
  if [[ -x "$nm_bin" ]]; then
    state="$("$nm_bin" -g --defined-only "$lib" 2>/dev/null | awk -v s="$SYMBOL" '$3==s {print $2; exit}')"
    if [[ "$state" == "W" || "$state" == "w" ]]; then
      echo "ok        $arch (already weak)"; skipped=$((skipped+1)); continue
    fi
  fi

  [[ -f "$lib.orig" ]] || cp -f "$lib" "$lib.orig"
  if "$oc" --weaken-symbol="$SYMBOL" "$lib.orig" "$lib" 2>/dev/null; then
    echo "patched   $arch"
    patched=$((patched+1))
  else
    echo "FAILED    $arch" >&2
    failed=$((failed+1))
  fi
done

echo
echo "done: $patched patched, $skipped skipped, $failed failed"
[[ $failed -eq 0 ]] || exit 1
