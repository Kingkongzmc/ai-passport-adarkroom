#!/usr/bin/env python3
"""Minimal BMP(24bit) -> PNG converter, stdlib only (zlib + struct)."""
import struct, sys, zlib

def main(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    off = struct.unpack_from("<I", data, 10)[0]
    w = struct.unpack_from("<i", data, 18)[0]
    h = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    assert bpp == 24, f"unsupported bpp {bpp}"
    row_size = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        i = off + (h - 1 - y) * row_size  # BMP 自下而上
        row = bytearray()
        for x in range(w):
            b, g, r = data[i + x*3], data[i + x*3 + 1], data[i + x*3 + 2]
            row += bytes((r, g, b))
        rows.append(bytes(row))
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c))

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(dst, "wb") as f:
        f.write(png)
    print(f"{src} -> {dst} ({w}x{h})")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
