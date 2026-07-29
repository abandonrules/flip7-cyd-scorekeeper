# Flip7 CYD Scorekeeper

Synchronized two-player games for a fixed pair of ESP32-2432S028R
Cheap Yellow Display boards. The boards talk directly over encrypted ESP-NOW,
so the pair works offline with no router, phone, or web service once flashed.

Live browser flasher:
https://abandonrules.github.io/flip7-cyd-scorekeeper/

## Current firmware

The active integration branch is `feat/greek-slide-planets-rebased`.
It currently ships:

- **Planet Slide** — a synchronized 3×3 sliding puzzle using planet symbols.
- **Greek Slide** — a synchronized 4×3 sliding puzzle using Greek symbols.
- **Periodic Order** — a synchronized 4×3 sliding puzzle where 11 random
  chemical elements must be arranged by ascending atomic number.
- **Mastermind** — a two-player codebreaker match with four pegs, six colors,
  duplicate-aware feedback, ten guesses, automatic role swaps, and match score.
- **Aquarium** — a shared idle/waiting screen with synced fish, feeding, hunger,
  and peer synchronization.

The host board starts games from Home when the peer is online. Both boards have
an `EXIT` path back to Home for synchronized games; EXIT is retried while a peer
is temporarily offline so the pair converges when the link returns.

## Hardware

Tested target:

- Two ESP32-2432S028R Cheap Yellow Display boards
- ILI9341 TFT over HSPI
- XPT2046 touch controller
- USB data cables for flashing and serial logs

PlatformIO environment: `cyd` in [`platformio.ini`](platformio.ini).

## Browser flashing

Use the published GitHub Pages installer for demo/easy flashing:

https://abandonrules.github.io/flip7-cyd-scorekeeper/

The page is pair-oriented:

1. Connect CYD 1 with a data USB cable and press `Flash CYD 1`.
2. Unplug CYD 1, plug in CYD 2, and keep the page open.
3. Press `Flash CYD 2`.

Both buttons use the same `manifest-cyd.json` firmware manifest. WebSerial can
flash only one connected board at a time.

Important: firmware built by CI uses ephemeral build-only ESP-NOW keys. That is
fine for a public/demo flasher, but the private paired devices should be built
locally with your real shared keys.

The Pages workflow is [`pages.yml`](.github/workflows/pages.yml). It builds the
firmware, merges the ESP32 bootloader/partition/boot_app0/app images into one
offset-0 browser-flashable binary, and publishes `dist/web-installer/`.

To generate the same site locally after `pio run`:

```sh
python3 scripts/build_web_installer.py
```

Generated output is written to `dist/web-installer/` and is ignored by git.

## Local build and upload

Install PlatformIO, then provision one PMK/LMK pair outside the repository:

```sh
mkdir -p ~/.config/flip7-cyd-scorekeeper
chmod 700 ~/.config/flip7-cyd-scorekeeper
python3 - <<'PY'
from pathlib import Path
import os, secrets
path = Path.home() / ".config/flip7-cyd-scorekeeper/espnow.env"
path.write_text(
    f"export FLIP7_ESPNOW_PMK={secrets.token_hex(16)}\n"
    f"export FLIP7_ESPNOW_LMK={secrets.token_hex(16)}\n"
)
os.chmod(path, 0o600)
PY
```

Build with the shared keys loaded:

```sh
source ~/.config/flip7-cyd-scorekeeper/espnow.env
pio run
```

Upload to each board by serial port:

```sh
source ~/.config/flip7-cyd-scorekeeper/espnow.env
pio run --target upload --upload-port /dev/serial/by-path/<first-cyd>
pio run --target upload --upload-port /dev/serial/by-path/<second-cyd>
```

Do not commit `espnow.env` or disclose its values. CI intentionally generates
ephemeral keys because CI artifacts are not the private paired-device build.

## Power saving and waiting screens

The firmware tracks local touch activity:

- after 30 seconds idle, the pair enters the Aquarium idle screen;
- after 60 seconds idle, the CYD backlight turns off;
- ESP-NOW remains active, so heartbeats, retries, and in-RAM game state survive;
- the first touch wakes the screen and is consumed so it does not accidentally
  make a game move.

Power-saving waits for pending synchronized game work to become idle before
starting Aquarium or turning off the backlight.

When boards are waiting for a peer or host, Aquarium is used as the waiting
screen. It is feedable and syncs fish when the peer is available.

## Synchronization model

- ESP-NOW packets use a project wire protocol in [`include/protocol.h`](include/protocol.h).
- Puzzle and Mastermind game state carry game/revision counters and ACK/retry
  flows so interrupted delivery can converge.
- The host owns authoritative game starts and full-state reconciliation.
- Aquarium uses event and sync packets for start/exit/food/fish snapshots.
- Receiver restart replay hardening is still tracked in
  [issue #11](https://github.com/abandonrules/flip7-cyd-scorekeeper/issues/11).

The fixed encrypted CYDs are trusted game appliances. Mastermind hides the
codemaker's secret from the codebreaker's UI, but synchronizes encrypted full
state so either trusted board can recover a round after the other board
restarts. This is not intended to resist physical extraction or modified
firmware on one paired board.

Game state is currently volatile: one board can restart and reconcile from the
surviving peer, while restarting both boards returns the pair to Home.

## Tests and verification

The GitHub Actions build workflow runs PlatformIO and host-side C++ logic tests.
For a local check:

```sh
source ~/.config/flip7-cyd-scorekeeper/espnow.env
pio run

g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/puzzle_logic_test.cpp -o /tmp/puzzle_logic_test && /tmp/puzzle_logic_test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/mastermind_logic_test.cpp -o /tmp/mastermind_logic_test && /tmp/mastermind_logic_test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/power_save_logic_test.cpp -o /tmp/power_save_logic_test && /tmp/power_save_logic_test
g++ -std=c++17 -Wall -Wextra -Werror -Iinclude test/aquarium_logic_test.cpp -o /tmp/aquarium_logic_test && /tmp/aquarium_logic_test
```

Hardware-ready changes should also be flashed to both CYDs and visually checked
on the real displays.

## Repository layout

- [`src/main.cpp`](src/main.cpp) — firmware UI, touch handling, ESP-NOW runtime,
  game dispatch, Aquarium, and power-save integration.
- [`include/`](include/) — pure game/protocol/power-save logic used by firmware
  and host-side tests.
- [`test/`](test/) — small host-side C++ logic tests.
- [`scripts/configure_peer_keys.py`](scripts/configure_peer_keys.py) — injects
  ESP-NOW keys into the PlatformIO build.
- [`scripts/build_web_installer.py`](scripts/build_web_installer.py) — builds
  the GitHub Pages/WebSerial installer artifact.
- [`web-installer/`](web-installer/) — source HTML for the browser flasher.
- [`macropad/keypad/`](macropad/keypad/README.md) — preserved CircuitPython
  keypad companion program and deployed library notes.

## Open follow-up work

Notable open issues:

- [#23 Mastermind guesser screen redesign](https://github.com/abandonrules/flip7-cyd-scorekeeper/issues/23)
- [#11 Harden ESP-NOW replay protection across receiver restarts](https://github.com/abandonrules/flip7-cyd-scorekeeper/issues/11)

See the GitHub issue tracker for the full backlog.
