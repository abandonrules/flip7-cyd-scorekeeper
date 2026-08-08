# Adafruit MacroPad keypad

This directory preserves the working CircuitPython program copied from the
physical Adafruit MacroPad RP2040 used alongside the two CYDs.

## Captured hardware

- Board: Adafruit MacroPad RP2040
- Board ID: `adafruit_macropad_rp2040`
- CircuitPython: 9.2.9
- Device UID: `DF609C8067371A28`
- Captured: 2026-07-23

`code.py` is the active program. It provides six selectable 12-key layouts:
F-keys, numbers, numpad, Ctrl+numpad, letters, and gaming. The encoder rotates
through saved layouts, its button changes which layouts are saved, and the
NeoPixels animate when keys are pressed.

The `lib/` directory is the exact compiled CircuitPython library set present on
the device when this snapshot was taken. `boot_out.txt` records the firmware
identity. `settings.toml` was empty when captured.

Desktop-generated trash, indexing, and filesystem metadata directories were
intentionally excluded.

## Restore to a MacroPad

With a compatible MacroPad running CircuitPython 9.2.9 and its `CIRCUITPY`
volume mounted, back up its current files and then copy this snapshot's
`code.py`, `lib/`, and `settings.toml` to the volume. Do not copy
`boot_out.txt`; CircuitPython generates that file itself.

Example, when the device is mounted at `/run/media/$USER/CIRCUITPY`:

```sh
mkdir -p "$HOME/macropad-backup"
cp -a "/run/media/$USER/CIRCUITPY/code.py" \
      "/run/media/$USER/CIRCUITPY/lib" \
      "$HOME/macropad-backup/"
cp -a code.py lib settings.toml "/run/media/$USER/CIRCUITPY/"
sync
```

CircuitPython automatically reloads `code.py` after the copy completes.
