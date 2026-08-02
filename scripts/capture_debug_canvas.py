#!/usr/bin/env python3
"""Capture DebugCanvas framebuffers from connected CYDs as PNG files."""

import argparse
import binascii
import glob
import struct
import time
import zlib
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import serial


def read_exact(connection: serial.Serial, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        chunk = connection.read(length - len(data))
        if not chunk:
            raise TimeoutError(
                f"Timed out after {len(data)} of {length} framebuffer bytes"
            )
        data.extend(chunk)
    return bytes(data)


def decode_rle(payload: bytes, pixel_count: int, depth: int) -> bytes:
    rgb = bytearray()
    offset = 0
    decoded_pixels = 0
    color_size = 1 if depth == 8 else 2

    while offset < len(payload):
        if offset + 1 + color_size > len(payload):
            raise ValueError("Truncated RLE run")
        run_length = payload[offset]
        offset += 1
        if depth == 8:
            value = payload[offset]
            offset += 1
            red = ((value >> 5) & 0x07) * 255 // 7
            green = ((value >> 2) & 0x07) * 255 // 7
            blue = (value & 0x03) * 255 // 3
        else:
            value = struct.unpack_from(">H", payload, offset)[0]
            offset += 2
            red = ((value >> 11) & 0x1F) * 255 // 31
            green = ((value >> 5) & 0x3F) * 255 // 63
            blue = (value & 0x1F) * 255 // 31
        rgb.extend(bytes((red, green, blue)) * run_length)
        decoded_pixels += run_length

    if decoded_pixels != pixel_count:
        raise ValueError(
            f"Decoded {decoded_pixels} pixels; expected {pixel_count}"
        )
    return bytes(rgb)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    checksum = binascii.crc32(kind)
    checksum = binascii.crc32(data, checksum) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", checksum)


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    stride = width * 3
    scanlines = b"".join(
        b"\x00" + rgb[row * stride:(row + 1) * stride]
        for row in range(height)
    )
    signature = b"\x89PNG\r\n\x1a\n"
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        signature
        + png_chunk(b"IHDR", header)
        + png_chunk(b"IDAT", zlib.compress(scanlines, 9))
        + png_chunk(b"IEND", b"")
    )


def capture(port: str, output_dir: Path, timeout: float) -> tuple[Path, str]:
    with serial.Serial(port, 115200, timeout=timeout, exclusive=True) as connection:
        time.sleep(0.15)
        connection.reset_input_buffer()
        connection.write(b"S")
        connection.flush()

        deadline = time.monotonic() + timeout
        header = b""
        while time.monotonic() < deadline:
            line = connection.readline()
            if line.startswith(b"DCFRAME "):
                header = line
                break
            if line.startswith(b"DCFRAME_ERROR "):
                raise RuntimeError(line.decode("utf-8", errors="replace").strip())
        if not header:
            raise TimeoutError(f"No DebugCanvas frame header received from {port}")

        parts = header.decode("ascii").strip().split(maxsplit=7)
        if len(parts) != 8 or parts[1] != "1":
            raise ValueError(f"Unsupported frame header: {header!r}")
        width, height, depth, encoded_size = map(int, parts[2:6])
        screen_name = parts[6] if len(parts) == 7 else parts[7]
        if depth not in (8, 16):
            raise ValueError(f"Unsupported color depth: {depth}")

        payload = read_exact(connection, encoded_size)
        rgb = decode_rle(payload, width * height, depth)

    output_path = output_dir / f"cyd-{Path(port).name}.png"
    write_png(output_path, width, height, rgb)
    return output_path, screen_name


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("ports", nargs="*", help="Serial ports; defaults to both CYDs")
    parser.add_argument("--output-dir", type=Path, default=Path("/tmp"))
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    ports = args.ports or sorted(
        glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    )
    if not ports:
        raise SystemExit("No CYD serial ports found")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    with ThreadPoolExecutor(max_workers=len(ports)) as executor:
        futures = [
            executor.submit(capture, port, args.output_dir, args.timeout)
            for port in ports
        ]
        for port, future in zip(ports, futures):
            path, screen_name = future.result()
            print(f"{port}: {screen_name} -> {path}")


if __name__ == "__main__":
    main()
