#!/usr/bin/env python3
"""Build the GitHub Pages ESP web installer payload.

The script expects `pio run` to have produced the standard ESP32 PlatformIO
artifacts. It merges bootloader, partition table, boot_app0, and app firmware
into a single WebSerial-compatible image at offset 0, then writes the static
installer page and manifest into the output directory.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "cyd"
OUT = ROOT / "dist" / "web-installer"
FIRMWARE_NAME = "flip7-cyd-scorekeeper-cyd.bin"
MERGED = OUT / "firmware" / FIRMWARE_NAME


def require(path: Path) -> Path:
    if not path.exists():
        raise SystemExit(f"missing required build artifact: {path}")
    return path


def find_boot_app0() -> Path:
    candidates = [
        BUILD / "boot_app0.bin",
        Path.home()
        / ".platformio"
        / "packages"
        / "framework-arduinoespressif32"
        / "tools"
        / "partitions"
        / "boot_app0.bin",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit("missing required build artifact: boot_app0.bin")


def find_esptool() -> list[str]:
    candidates = [
        Path.home() / ".platformio" / "packages" / "tool-esptoolpy" / "esptool.py",
    ]
    for candidate in candidates:
        if candidate.exists():
            return [sys.executable, str(candidate)]
    return [sys.executable, "-m", "esptool"]


def version_string() -> str:
    sha = os.environ.get("GITHUB_SHA")
    if sha:
        return sha[:12]
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short=12", "HEAD"],
            cwd=ROOT,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return "local"


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    MERGED.parent.mkdir(parents=True, exist_ok=True)

    bootloader = require(BUILD / "bootloader.bin")
    partitions = require(BUILD / "partitions.bin")
    boot_app0 = find_boot_app0()
    firmware = require(BUILD / "firmware.bin")

    subprocess.run(
        [
            *find_esptool(),
            "--chip",
            "esp32",
            "merge_bin",
            "-o",
            str(MERGED),
            "--flash_mode",
            "dio",
            "--flash_freq",
            "40m",
            "--flash_size",
            "4MB",
            "0x1000",
            str(bootloader),
            "0x8000",
            str(partitions),
            "0xe000",
            str(boot_app0),
            "0x10000",
            str(firmware),
        ],
        cwd=ROOT,
        check=True,
    )

    shutil.copy2(ROOT / "web-installer" / "index.html", OUT / "index.html")
    manifest = {
        "name": "Flip7 CYD Scorekeeper",
        "version": version_string(),
        "new_install_prompt_erase": True,
        "builds": [
            {
                "chipFamily": "ESP32",
                "parts": [
                    {
                        "path": f"firmware/{FIRMWARE_NAME}",
                        "offset": 0,
                    }
                ],
            }
        ],
    }
    (OUT / "manifest-cyd.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {OUT}")
    print(f"merged firmware: {MERGED}")


if __name__ == "__main__":
    main()
