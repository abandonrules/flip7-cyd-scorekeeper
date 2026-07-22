# pyright: reportUndefinedVariable=false
Import("env")

import os
from pathlib import Path


def read_key(name):
    value = os.environ.get(name, "")
    try:
        key = bytes.fromhex(value)
    except ValueError as error:
        raise RuntimeError(f"{name} must be 32 hexadecimal characters") from error
    if len(key) != 16:
        raise RuntimeError(f"{name} must be 32 hexadecimal characters")
    return key


def initializer(key):
    return ", ".join(f"0x{byte:02x}" for byte in key)


build_dir = Path(env.subst("$BUILD_DIR"))
header = build_dir / "generated_peer_keys.h"
header.parent.mkdir(parents=True, exist_ok=True)
header.write_text(
    "#pragma once\n\n"
    "#include <cstdint>\n\n"
    f"constexpr uint8_t kEspNowPmk[16] = {{{initializer(read_key('FLIP7_ESPNOW_PMK'))}}};\n"
    f"constexpr uint8_t kEspNowLmk[16] = {{{initializer(read_key('FLIP7_ESPNOW_LMK'))}}};\n"
)
env.Append(CPPPATH=[str(build_dir)])
