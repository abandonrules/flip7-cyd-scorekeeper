#!/usr/bin/env python3
"""
Build, upload, and monitor Flip7 CYD firmware.

Loads ESP-NOW keys from ~/.config/flip7-cyd-scorekeeper/espnow.env,
sets FLIP7_ESPNOW_PMK and FLIP7_ESPNOW_LMK in the environment, then
delegates to PlatformIO.

Usage:
    python3 scripts/upload.py                         # build + upload to all boards
    python3 scripts/upload.py --build                 # build only (no upload)
    python3 scripts/upload.py --monitor               # build + upload + monitor last port
    python3 scripts/upload.py --monitor-only          # open monitor on all detected ports
    python3 scripts/upload.py --monitor-only --port /dev/ttyUSB0  # monitor one board
    python3 scripts/upload.py --port /dev/ttyUSB0     # upload to a specific port only
"""

import argparse
import glob
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


ENV_FILE = Path.home() / ".config" / "flip7-cyd-scorekeeper" / "espnow.env"
REQUIRED_KEYS = ("FLIP7_ESPNOW_PMK", "FLIP7_ESPNOW_LMK")

# Glob patterns for USB serial adapters in priority order.
PORT_PATTERNS = ["/dev/ttyUSB*", "/dev/ttyACM*"]


def load_env_file(path: Path) -> dict[str, str]:
    """Parse `export KEY=VALUE` lines from a shell env file."""
    if not path.exists():
        print(f"ERROR: ESP-NOW key file not found: {path}", file=sys.stderr)
        print(
            "Run the one-time provisioning step from the README to create it.",
            file=sys.stderr,
        )
        sys.exit(1)

    env: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):]
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        env[key.strip()] = value.strip()

    for key in REQUIRED_KEYS:
        if key not in env:
            print(f"ERROR: {key} not found in {path}", file=sys.stderr)
            sys.exit(1)

    return env


def detect_ports() -> list[str]:
    """Return sorted list of all connected USB serial ports."""
    ports: list[str] = []
    for pattern in PORT_PATTERNS:
        ports.extend(sorted(glob.glob(pattern)))
    return ports


def run(cmd: list[str], env: dict[str, str]) -> int:
    """Run a subprocess, streaming output, return exit code."""
    merged = {**os.environ, **env}
    print(f"\n>>> {' '.join(cmd)}\n", flush=True)
    result = subprocess.run(cmd, env=merged)
    return result.returncode


def build(esp_env: dict[str, str]) -> None:
    rc = run(["pio", "run"], esp_env)
    if rc != 0:
        print(f"\nBuild failed (exit {rc})", file=sys.stderr)
        sys.exit(rc)


def release_port(port: str) -> None:
    """Kill any process holding an exclusive lock on the port, then wait for it to release."""
    try:
        result = subprocess.run(
            ["fuser", port], capture_output=True, text=True
        )
        pids = result.stdout.split()
        if not pids:
            return
        print(f"  Releasing {port} from PID(s): {', '.join(pids)}")
        for pid in pids:
            try:
                os.kill(int(pid), signal.SIGTERM)
            except (ProcessLookupError, ValueError):
                pass
        # Wait up to 2 seconds for the lock to drop
        for _ in range(20):
            time.sleep(0.1)
            check = subprocess.run(["fuser", port], capture_output=True, text=True)
            if not check.stdout.strip():
                break
    except FileNotFoundError:
        pass  # fuser not available; proceed anyway


def upload_to(port: str, esp_env: dict[str, str]) -> bool:
    """Upload to a single port. Returns True on success."""
    print(f"\n{'='*60}")
    print(f"  Uploading to {port}")
    print(f"{'='*60}")
    release_port(port)
    rc = run(["pio", "run", "--target", "upload", "--upload-port", port], esp_env)
    if rc != 0:
        print(f"\nUpload to {port} failed (exit {rc})", file=sys.stderr)
        return False
    print(f"\nUpload to {port} complete.")
    return True


def main() -> None:
    parser = argparse.ArgumentParser(description="Build, upload, and monitor CYD firmware")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--build", action="store_true", help="Compile only; no upload")
    group.add_argument(
        "--monitor", action="store_true",
        help="Build, upload to all boards, then open serial monitor on the last port",
    )
    group.add_argument(
        "--monitor-only", action="store_true",
        help="Skip build/upload; open serial monitor on the target port(s)",
    )
    parser.add_argument(
        "--port", metavar="PORT",
        help="Target a specific port (e.g. /dev/ttyUSB0)",
    )
    args = parser.parse_args()

    project_root = Path(__file__).parent.parent
    os.chdir(project_root)

    esp_env = load_env_file(ENV_FILE)

    # --monitor-only: skip build and upload entirely
    if args.monitor_only:
        port = args.port or _single_detected_port()
        print(f"Opening monitor on {port}")
        run(["pio", "device", "monitor", "--port", port], esp_env)
        return

    build(esp_env)

    if args.build:
        print("\nBuild complete.")
        return

    # Determine target ports
    if args.port:
        ports = [args.port]
    else:
        ports = detect_ports()
        if not ports:
            print("ERROR: No serial ports found. Is a CYD board plugged in?",
                  file=sys.stderr)
            sys.exit(1)

    print(f"\nFound {len(ports)} board(s): {', '.join(ports)}")

    failed: list[str] = []
    for port in ports:
        if not upload_to(port, esp_env):
            failed.append(port)

    if failed:
        print(f"\nERROR: Upload failed for: {', '.join(failed)}", file=sys.stderr)
        sys.exit(1)

    print(f"\nAll {len(ports)} board(s) flashed successfully.")

    if args.monitor:
        # Monitor the last port
        run(["pio", "device", "monitor", "--port", ports[-1]], esp_env)


def _single_detected_port() -> str:
    """Return the first detected port, or exit with an error."""
    ports = detect_ports()
    if not ports:
        print("ERROR: No serial ports found.", file=sys.stderr)
        sys.exit(1)
    return ports[0]


if __name__ == "__main__":
    main()
