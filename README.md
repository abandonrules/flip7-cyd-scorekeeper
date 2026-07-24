# Flip7 CYD Scorekeeper

A collection of synchronized two-player games for a fixed pair of
ESP32-2432S028 (Cheap Yellow Display) boards.

## Games

- **Greek Slide** — a shared 4×3 sliding puzzle with alternating turns.
- **Mastermind** — one device privately enters a four-color code and the
  other gets ten duplicate-aware guesses. Roles swap automatically after
  every round and the match score persists between rounds.

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

## ESP-NOW peer keys

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

The paired CYDs offer two synchronized, alternating-turn layouts:

- **Planet Slide:** a 3×3 eight-piece puzzle using the astronomical
  symbols for Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, and
  Neptune.
- **Greek Slide:** the original 4×3 eleven-piece Greek-symbol puzzle.

A correctly positioned piece is visually locked by removing its colored
background. Either player can press `EXIT` at any time, including while a move
is still awaiting acknowledgment or the peer is temporarily offline. EXIT is
synchronized and retried until both devices return to the selector.

### Security and recovery model

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