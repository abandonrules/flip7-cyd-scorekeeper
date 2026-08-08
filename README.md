# Flip7 CYD Scorekeeper

A collection of synchronized two-player games for a fixed pair of
ESP32-2432S028 (Cheap Yellow Display) boards.

## Games

- **Greek Slide** — a synchronized 4×3 sliding puzzle using Greek symbols.
- **Planet Slide** — a synchronized 3×3 sliding puzzle using planet symbols.
- **Periodic Order** — a synchronized 4×3 sliding puzzle where 11 random
  chemical elements must be arranged by ascending atomic number.
- **Mastermind** — a two-player codebreaker match with four pegs, six colors,
  duplicate-aware feedback, ten guesses, automatic role swaps, and match score.
- **Aquarium** — a shared idle/waiting screen with synced fish, feeding, hunger,
  and peer synchronization.

The host chooses a game from the home screen. Mastermind uses six colors,
allows repeated colors, reports exact-position (`E`) and color-only (`C`)
matches, and reveals the secret when the round ends.
Both players have an `EXIT` button in landscape mode; either device can use
it to return the synchronized pair to the game-selection screen.

## Goals
- Two synchronized touchscreen scorekeepers
- ESP-NOW networking
- Offline operation
- Save/restore games
- Future web dashboard

## Build & Upload to CYD Boards

This project uses **PlatformIO** to build and flash firmware to the ESP32-2432S028 (Cheap Yellow Display) boards.

### Prerequisites
```sh
# Install PlatformIO (if not already installed)
pip install platformio
# or: pio install --global platformio
```

### One-Time ESP-NOW Key Provisioning
Generate a PMK/LMK pair **once** and use the same keys for both boards:

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

### Build & Upload Workflow
```sh
cd /home/cmayfield/code/games/flip7-cyd-scorekeeper

# Load keys into environment (do this each new shell)
source ~/.config/flip7-cyd-scorekeeper/espnow.env

# Build firmware
pio run

# Upload to connected CYD board (plug in via USB)
pio run --target upload

# Monitor serial output for debugging (use this, not raw cat)
pio device monitor
```

### Key Details

| Item | Value |
|------|-------|
| **Platform** | `espressif32` (ESP32) |
| **Board** | `esp32dev` (Cheap Yellow Display / ESP32-2432S028) |
| **Framework** | Arduino |
| **Monitor speed** | 115200 baud |
| **Pre-build script** | `scripts/configure_peer_keys.py` (generates `generated_peer_keys.h` from env vars) |
| **Libraries** | TFT_eSPI, XPT2046_Touchscreen |
| **Source filter** | Includes `../core/src/*.cpp` for the countdown engine |

### Important Notes
1. **Both boards need the same PMK/LMK** — generate once, `source` the env file before each build
2. **Keys are NOT committed** — stored in `~/.config/flip7-cyd-scorekeeper/espnow.env` (chmod 600)
3. **CI uses ephemeral keys** — CI firmware isn't installed on the physical pair
4. **Serial debugging** — use `pio device monitor` (not raw `cat`) to see proper output with DTR/RTS handled

---

## ESP-NOW Peer Keys (Legacy Reference)

The paired boards use encrypted unicast ESP-NOW. Provision one PMK/LMK pair outside the repository and use the same pair for both firmware uploads:

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
source ~/.config/flip7-cyd-scorekeeper/espnow.env
pio run
```

Do not commit `espnow.env` or disclose its values. CI generates ephemeral build-only keys because CI firmware is not installed on the physical pair.

## Companion hardware

The working CircuitPython keypad program from the companion Adafruit MacroPad
RP2040 is preserved under [`macropad/keypad/`](macropad/keypad/README.md),
including the exact deployed library set and restore instructions.

## Sliding puzzles

The paired CYDs offer three synchronized, alternating-turn layouts:

- **Planet Slide:** a 3×3 eight-piece puzzle using the astronomical
  symbols for Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, and
  Neptune.
- **Greek Slide:** the original 4×3 eleven-piece Greek-symbol puzzle.
- **Periodic Order:** a synchronized 4×3 sliding puzzle where 11 random
  chemical elements must be arranged by ascending atomic number.

A correctly positioned piece is visually locked by removing its colored
background. Either player can press `EXIT` at any time, including while a move
is still awaiting acknowledgment or the peer is temporarily offline. EXIT is
synchronized and retried until both devices return to the selector.

## Security and recovery model

The fixed encrypted CYDs are trusted game appliances. Mastermind hides the
codemaker's secret from the codebreaker's UI, but synchronizes the encrypted
full state so either trusted board can recover the round after the other board
restarts. This is not intended to resist physical extraction or modified
firmware on one of the paired boards.

Game state is currently volatile: one board can restart and reconcile from the
surviving peer, while restarting both boards returns the pair to the game menu.
Receiver-restart replay hardening is tracked in
[issue #11](https://github.com/abandonrules/flip7-cyd-scorekeeper/issues/11).

## Milestones
1. Single-board UI
2. Local persistence
3. ESP-NOW sync
4. Polish