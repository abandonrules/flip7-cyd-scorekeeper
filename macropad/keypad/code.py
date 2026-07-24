# SPDX-FileCopyrightText: 2022 Dan Halbert for Adafruit Industries
#
# SPDX-License-Identifier: MIT

"""MacroPad: tap cycles all sets; dbl-tap toggles saved round; rotate cycles saved round."""

import time
import displayio
import terminalio
from adafruit_display_text import label
from adafruit_macropad import MacroPad
from adafruit_hid.keycode import Keycode

KEY_SETS = (
    {
        "name": "F-Keys",
        "labels": (
            "f1", "f2", "f3",
            "f4", "f5", "f6",
            "f7", "f8", "f9",
            "f10", "f11", "f12",
        ),
        "splash": (
            "F1", "F2", "F3",
            "F4", "F5", "F6",
            "F7", "F8", "F9",
            "F10", "F11", "F12",
        ),
        "codes": (
            Keycode.F1, Keycode.F2, Keycode.F3,
            Keycode.F4, Keycode.F5, Keycode.F6,
            Keycode.F7, Keycode.F8, Keycode.F9,
            Keycode.F10, Keycode.F11, Keycode.F12,
        ),
    },
    {
        "name": "Numbers",
        "labels": (
            "7", "8", "9",
            "4", "5", "6",
            "1", "2", "3",
            "0", "bs", "en",
        ),
        "splash": (
            "7", "8", "9",
            "4", "5", "6",
            "1", "2", "3",
            "0", "BS", "EN",
        ),
        "codes": (
            Keycode.SEVEN, Keycode.EIGHT, Keycode.NINE,
            Keycode.FOUR, Keycode.FIVE, Keycode.SIX,
            Keycode.ONE, Keycode.TWO, Keycode.THREE,
            Keycode.ZERO, Keycode.BACKSPACE, Keycode.ENTER,
        ),
    },
    {
        "name": "NumPad",
        "labels": (
            "7", "8", "9",
            "4", "5", "6",
            "1", "2", "3",
            "0", ".", "en",
        ),
        "splash": (
            "7", "8", "9",
            "4", "5", "6",
            "1", "2", "3",
            "0", ".", "EN",
        ),
        # Main keyboard number keys work without Num Lock on the host.
        "codes": (
            Keycode.SEVEN, Keycode.EIGHT, Keycode.NINE,
            Keycode.FOUR, Keycode.FIVE, Keycode.SIX,
            Keycode.ONE, Keycode.TWO, Keycode.THREE,
            Keycode.ZERO, Keycode.PERIOD, Keycode.KEYPAD_ENTER,
        ),
    },
    {
        "name": "NumPad+Ctrl",
        "modifiers": (Keycode.CONTROL,),
        "labels": (
            "^7", "^8", "^9",
            "^4", "^5", "^6",
            "^1", "^2", "^3",
            "^0", "^.", "^en",
        ),
        "splash": (
            "Ctrl+7", "Ctrl+8", "Ctrl+9",
            "Ctrl+4", "Ctrl+5", "Ctrl+6",
            "Ctrl+1", "Ctrl+2", "Ctrl+3",
            "Ctrl+0", "Ctrl+.", "Ctrl+En",
        ),
        "codes": (
            Keycode.SEVEN, Keycode.EIGHT, Keycode.NINE,
            Keycode.FOUR, Keycode.FIVE, Keycode.SIX,
            Keycode.ONE, Keycode.TWO, Keycode.THREE,
            Keycode.ZERO, Keycode.PERIOD, Keycode.KEYPAD_ENTER,
        ),
    },
    {
        "name": "Letters",
        "labels": (
            "a", "b", "c",
            "d", "e", "f",
            "g", "h", "i",
            "j", "k", "l",
        ),
        "splash": (
            "A", "B", "C",
            "D", "E", "F",
            "G", "H", "I",
            "J", "K", "L",
        ),
        "codes": (
            Keycode.A, Keycode.B, Keycode.C,
            Keycode.D, Keycode.E, Keycode.F,
            Keycode.G, Keycode.H, Keycode.I,
            Keycode.J, Keycode.K, Keycode.L,
        ),
    },
    {
        "name": "Gaming",
        "gaming": True,
        "labels": (
            "1", "2", "3",
            "r", "f", "v",
            "q", "w", "e",
            "a", "s", "d",
        ),
        "splash": (
            "1", "2", "3",
            "R", "F", "V",
            "Q", "W", "E",
            "A", "S", "D",
        ),
        "codes": (
            Keycode.ONE, Keycode.TWO, Keycode.THREE,
            Keycode.R, Keycode.F, Keycode.V,
            Keycode.Q, Keycode.W, Keycode.E,
            Keycode.A, Keycode.S, Keycode.D,
        ),
    },
)

FKEYS_INDEX = 0
GAMING_INDEX = 5

PIXEL_OFF = 0x000000
FADE_SECONDS = 2.5
FADE_STOPS = (
    0x0000FF,  # blue
    0x00FFFF,  # cyan
    0x00FF00,  # green
    0xFFFF00,  # yellow
    0xFF8000,  # orange
    0xFF0000,  # red
    0x000000,  # off
)
DEFAULT_SPLASH_SECONDS = 0.35
DOUBLE_TAP_WINDOW = 0.35
SAVE_FLASH_SECONDS = 0.5

macropad = MacroPad()
macropad.display.auto_refresh = False
macropad.pixels.auto_write = False

GRID_ROW_SPACING = 13
GRID_CENTER_X = macropad.display.width // 2
GRID_CENTER_Y = macropad.display.height // 2

display_group = displayio.Group()
row_labels = []
for row in range(4):
    row_y = int(GRID_CENTER_Y + (row - 1.5) * GRID_ROW_SPACING)
    row_labels.append(
        label.Label(
            terminalio.FONT,
            text="",
            color=0xFFFFFF,
            anchored_position=(GRID_CENTER_X, row_y),
            anchor_point=(0.5, 0.5),
        )
    )
    display_group.append(row_labels[row])

rotation_badge = label.Label(
    terminalio.FONT,
    text="",
    color=0xFFFFFF,
    anchored_position=(2, 2),
    anchor_point=(0, 0),
)
display_group.append(rotation_badge)

splash_label = label.Label(
    terminalio.FONT,
    text="",
    color=0xFFFFFF,
    scale=2,
    anchored_position=(macropad.display.width // 2, macropad.display.height // 2),
    anchor_point=(0.5, 0.5),
)
display_group.append(splash_label)
macropad.display.root_group = display_group

set_index = FKEYS_INDEX
saved_round = [FKEYS_INDEX, GAMING_INDEX]
round_cursor = 0
last_encoder = macropad.encoder
last_encoder_switch = macropad.encoder_switch_debounced.pressed
splash_until = 0.0
pending_single_tap = 0.0
key_fade_start = [None] * 12


def rgb_lerp(color_a, color_b, frac):
    """Blend two 24-bit RGB colors."""
    r1 = (color_a >> 16) & 0xFF
    g1 = (color_a >> 8) & 0xFF
    b1 = color_a & 0xFF
    r2 = (color_b >> 16) & 0xFF
    g2 = (color_b >> 8) & 0xFF
    b2 = color_b & 0xFF
    r = int(r1 + (r2 - r1) * frac)
    g = int(g1 + (g2 - g1) * frac)
    b = int(b1 + (b2 - b1) * frac)
    return (r << 16) | (g << 8) | b


def start_key_fade(key_number):
    """Flash a key, then fade it out through several colors."""
    key_fade_start[key_number] = time.monotonic()
    macropad.pixels[key_number] = FADE_STOPS[0]
    macropad.pixels.show()


def update_key_fades():
    """Animate any keys that are fading out."""
    now = time.monotonic()
    changed = False
    for i in range(12):
        fade_start = key_fade_start[i]
        if fade_start is None:
            if macropad.pixels[i] != PIXEL_OFF:
                macropad.pixels[i] = PIXEL_OFF
                changed = True
            continue

        elapsed = now - fade_start
        if elapsed >= FADE_SECONDS:
            key_fade_start[i] = None
            macropad.pixels[i] = PIXEL_OFF
            changed = True
            continue

        progress = elapsed / FADE_SECONDS
        step = progress * (len(FADE_STOPS) - 1)
        stop = min(int(step), len(FADE_STOPS) - 2)
        frac = step - stop
        color = rgb_lerp(FADE_STOPS[stop], FADE_STOPS[stop + 1], frac)
        if macropad.pixels[i] != color:
            macropad.pixels[i] = color
            changed = True

    if changed:
        macropad.pixels.show()


def rotation_badge_text(index):
    """Show rotation slot when this set is saved in the round."""
    if index in saved_round:
        return "R{}".format(saved_round.index(index) + 1)
    return ""


def show_grid(index):
    """Show the 3x4 key grid for the active set."""
    current = KEY_SETS[index]
    splash_label.text = ""
    rotation_badge.text = rotation_badge_text(index)
    for row in range(4):
        chunk = current["labels"][row * 3 : row * 3 + 3]
        row_labels[row].text = ":".join(chunk) + ":"
    macropad.display.refresh()


def show_splash(text):
    """Show a single key full screen."""
    for row_label in row_labels:
        row_label.text = ""
    rotation_badge.text = ""
    splash_label.text = text
    macropad.display.refresh()


def show_status(text):
    """Brief full-screen status message."""
    for row_label in row_labels:
        row_label.text = ""
    rotation_badge.text = ""
    splash_label.text = text
    macropad.display.refresh()


def toggle_round_save():
    """Add current mode to the round, or remove it if already saved."""
    global round_cursor
    if set_index in saved_round:
        round_cursor = saved_round.index(set_index)
        saved_round.remove(set_index)
        if saved_round:
            round_cursor %= len(saved_round)
        else:
            round_cursor = 0
        show_status("REMOVED")
    else:
        saved_round.append(set_index)
        round_cursor = len(saved_round) - 1
        show_status("SAVED")


def step_saved_round(delta):
    """Move to the next or previous mode in the saved rotation."""
    global set_index, round_cursor
    if not saved_round:
        return
    if set_index in saved_round:
        round_cursor = saved_round.index(set_index)
    round_cursor = (round_cursor + delta) % len(saved_round)
    set_index = saved_round[round_cursor]
    show_grid(set_index)


def step_mode(delta):
    """Move to the next or previous key set."""
    global set_index
    set_index = (set_index + delta) % len(KEY_SETS)
    show_grid(set_index)


show_grid(set_index)
macropad.pixels.fill(PIXEL_OFF)
macropad.pixels.show()

while True:
    now = time.monotonic()

    if splash_until and now >= splash_until:
        splash_until = 0.0
        show_grid(set_index)

    if pending_single_tap and now - pending_single_tap > DOUBLE_TAP_WINDOW:
        pending_single_tap = 0.0
        step_mode(1)

    position = macropad.encoder
    if position != last_encoder:
        delta = position - last_encoder
        last_encoder = position
        step_saved_round(delta)

    macropad.encoder_switch_debounced.update()
    encoder_switch = macropad.encoder_switch_debounced.pressed
    if encoder_switch and not last_encoder_switch:
        if pending_single_tap and now - pending_single_tap <= DOUBLE_TAP_WINDOW:
            pending_single_tap = 0.0
            splash_until = now + SAVE_FLASH_SECONDS
            toggle_round_save()
        else:
            pending_single_tap = now
    last_encoder_switch = encoder_switch

    update_key_fades()

    event = macropad.keys.events.get()
    if not event:
        continue

    key_number = event.key_number
    current_set = KEY_SETS[set_index]
    code = current_set["codes"][key_number]
    splash_text = current_set["splash"][key_number]
    is_gaming = current_set.get("gaming", False)
    modifiers = current_set.get("modifiers", ())

    if event.pressed:
        for modifier in modifiers:
            macropad.keyboard.press(modifier)
        macropad.keyboard.press(code)
        start_key_fade(key_number)
        if not is_gaming:
            splash_until = 0.0
            show_splash(splash_text)

    if event.released:
        macropad.keyboard.release(code)
        for modifier in modifiers:
            macropad.keyboard.release(modifier)
        if not is_gaming:
            splash_seconds = current_set.get("splash_seconds", DEFAULT_SPLASH_SECONDS)
            if splash_seconds:
                splash_until = time.monotonic() + splash_seconds
            else:
                show_grid(set_index)
