"""Generates the package icons (pure Python, no extra libraries).

Design: dark background, blue screen outline with the AirPlay-style triangle.
Usage: python -I tools/make_assets.py package/Assets
"""
import struct
import sys
import zlib
from pathlib import Path

BG = (10, 13, 23)
BLUE = (77, 158, 255)
WHITE = (255, 255, 255)


def png(path, w, h, pixel):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    data += chunk(b"IEND", b"")
    Path(path).write_bytes(data)


def make_pixel(w, h):
    s = min(w, h)

    def px(x, y):
        # Coordinates relative to the image center, normalized to [-1, 1] based on the shorter edge
        u = (x - w / 2) / (s / 2)
        v = (y - h / 2) / (s / 2)
        # Screen outline (rounded rectangle, border only)
        bw, bh, t = 0.78, 0.50, 0.07
        cy = -0.12
        dx, dy = abs(u) - bw, abs(v - cy) - bh
        outer = max(dx, dy) <= 0
        inner = abs(u) <= bw - t and abs(v - cy) <= bh - t
        if outer and not inner:
            return (*BLUE, 255)
        # AirPlay triangle below the screen
        ty0, ty1 = 0.38, 0.72
        if ty0 <= v <= ty1:
            half = (v - ty0) / (ty1 - ty0) * 0.30
            if abs(u) <= half:
                return (*WHITE, 255)
        return (*BG, 255)

    return px


def main(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    sizes = {
        "Square44x44Logo.png": (44, 44),
        "Square150x150Logo.png": (150, 150),
        "StoreLogo.png": (50, 50),
        "Wide310x150Logo.png": (310, 150),
        "SplashScreen.png": (620, 300),
    }
    for name, (w, h) in sizes.items():
        png(out / name, w, h, make_pixel(w, h))
        print("written:", out / name)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "package/Assets")
