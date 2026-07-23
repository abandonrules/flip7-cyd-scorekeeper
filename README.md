# Flip7 CYD Scorekeeper

A multiplayer Flip 7 scorekeeper for two ESP32-2432S028 (Cheap Yellow Display) boards.

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

## Milestones
1. Single-board UI
2. Local persistence
3. ESP-NOW sync
4. Polish
