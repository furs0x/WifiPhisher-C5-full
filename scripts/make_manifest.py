#!/usr/bin/env python3
"""Generate an ESP Web Tools `manifest.json` from built firmware files.

ESP Web Tools expects:

    {
      "name": str,
      "version": str,
      "builds": [
        { "chipFamily": "ESP32-S3",
          "parts": [ {"path": "x.bin", "offset": 0}, ... ] }
      ]
    }

Offsets are DECIMAL in the official format (65536 == 0x10000).

Two modes
---------
1. Split parts (default when files exist):
   bootloader.bin / partitions.bin / firmware.bin from a PlatformIO build dir.

2. Merged image (--merged):
   one `<env>-merged.bin` produced by scripts/merge_bins.py, written at 0.

Bootloader offsets mirror scripts/merge_bins.py:
    ESP32 / ESP32-S2 -> 0x1000 (4096)
    ESP32-C5         -> 0x2000 (8192)
    everything else  -> 0x0

Usage
-----
    python3 scripts/make_manifest.py --dir build --version 1.0.0 -o data/manifest.json
    python3 scripts/make_manifest.py --merged dist/esp32s3-merged.bin --chip ESP32-S3 \
        --name ESP32WifiPhisher -o data/manifest.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

BOOTLOADER_OFFSET = {
    "ESP32": 0x1000,
    "ESP32-S2": 0x1000,
    "ESP32-C3": 0x0,
    "ESP32-C5": 0x2000,
    "ESP32-C6": 0x0,
    "ESP32-C61": 0x0,
    "ESP32-H2": 0x0,
    "ESP32-P4": 0x0,
    "ESP32-S3": 0x0,
}
PARTITION_OFFSET = 0x8000
APP_OFFSET = 0x10000


def merged_build(chip: str, merged: Path) -> dict:
    entry = {"chipFamily": chip, "parts": [{"path": merged.name, "offset": 0}]}
    return entry


def split_build(chip: str, build_dir: Path) -> dict | None:
    def find(*names: str) -> Path | None:
        for n in names:
            p = build_dir / n
            if p.exists():
                return p
        return None

    boot = find("bootloader.bin")
    part = find("partitions.bin", "partition-table.bin")
    app = find("firmware.bin")
    if not (boot and part and app):
        return None
    return {
        "chipFamily": chip,
        "parts": [
            {"path": boot.name, "offset": BOOTLOADER_OFFSET.get(chip, 0x0)},
            {"path": part.name, "offset": PARTITION_OFFSET},
            {"path": app.name, "offset": APP_OFFSET},
        ],
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Generate an ESP Web Tools manifest.json")
    ap.add_argument("--name", default="ESP32WifiPhisher")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--chip", help="chipFamily, e.g. ESP32-S3")
    ap.add_argument("--dir", type=Path, help="build dir with bootloader/partitions/firmware .bin")
    ap.add_argument("--merged", type=Path, help="single merged .bin (written at offset 0)")
    ap.add_argument("-o", "--out", type=Path, default=Path("manifest.json"))
    args = ap.parse_args(argv)

    if not args.chip:
        ap.error("--chip is required")

    builds: list[dict] = []
    if args.merged:
        if not args.merged.exists():
            ap.error(f"merged image not found: {args.merged}")
        builds.append(merged_build(args.chip, args.merged))
    elif args.dir:
        b = split_build(args.chip, args.dir)
        if not b:
            ap.error(f"could not find bootloader/partitions/firmware .bin in {args.dir}")
        builds.append(b)
    else:
        ap.error("provide --merged or --dir")

    manifest = {"name": args.name, "version": args.version, "builds": builds}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {args.out} ({len(builds)} build, chip={args.chip})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
