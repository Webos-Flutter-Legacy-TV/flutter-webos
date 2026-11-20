#!/usr/bin/env python3
"""Add HDR10 colour boxes to the video sample entry of an MP4 init segment.

ffmpeg's DASH muxer writes neither 'colr' nor the mastering display ('mdcv')
and light level ('clli') boxes for libx265 output, so l1-test-media.sh adds
them here: colr nclx BT.2020 / PQ / BT.2020 NCL, limited range, and the
values given to x265 (P3-D65 primaries, 1000 cd/m2 down to 0.0001; MaxCLL
1000, MaxFALL 400). Boxes already present are left alone.

With --dv81 it also adds a Dolby Vision configuration ('dvcC'): Profile 8,
level 4 (1080p30), RPU and base layer, HDR10-compatible base layer (ID 1).

  mp4-hdr-boxes.py [--dv81] <init.mp4>
"""
import struct
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}
ENTRIES = {b"hvc1", b"hev1", b"avc1", b"avc3", b"vp09", b"av01", b"dvh1", b"dvhe"}


def box(kind, payload):
    return struct.pack(">I", 8 + len(payload)) + kind + payload


DV81 = False


def hdr_boxes(existing):
    out = b""
    if DV81 and b"dvcC" not in existing:
        flags = (8 << 9) | (4 << 3) | (1 << 2) | 1  # profile, level, rpu, (no el), bl
        out += box(b"dvcC", bytes([1, 0]) + struct.pack(">H", flags) + bytes([1 << 4]) + bytes(19))
    if b"colr" not in existing:
        out += box(b"colr", b"nclx" + struct.pack(">HHHB", 9, 16, 9, 0))
    if b"mdcv" not in existing:
        # Display primaries G, B, R then white point (0.00002 units),
        # max / min luminance (0.0001 cd/m2).
        out += box(b"mdcv", struct.pack(">8H2I", 13250, 34500, 7500, 3000, 34000, 16000,
                                        15635, 16450, 10000000, 1))
    if b"clli" not in existing:
        out += box(b"clli", struct.pack(">2H", 1000, 400))
    return out


def walk(data, start, end):
    """Rebuild boxes in data[start:end], adding the HDR boxes to the video entry."""
    out = b""
    pos = start
    while pos + 8 <= end:
        size, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if size < 8 or pos + size > end:
            raise SystemExit(f"bad box at {pos}")
        body = data[pos + 8:pos + size]
        if kind in CONTAINERS:
            body = walk(data, pos + 8, pos + size)
        elif kind == b"stsd":
            body = body[:8] + walk(data, pos + 16, pos + size)
        elif kind in ENTRIES:
            fixed = body[:78]  # VisualSampleEntry fields
            children = body[78:]
            names = set()
            p = 0
            while p + 8 <= len(children):
                n, k = struct.unpack(">I4s", children[p:p + 8])
                names.add(k)
                p += max(n, 8)
            body = fixed + children + hdr_boxes(names)
        out += box(kind, body)
        pos += size
    return out


def main():
    global DV81
    args = sys.argv[1:]
    if args and args[0] == "--dv81":
        DV81 = True
        args = args[1:]
    path = args[0]
    data = open(path, "rb").read()
    new = walk(data, 0, len(data))
    if new != data:
        open(path, "wb").write(new)
        print(f"  {path}: added HDR boxes")


if __name__ == "__main__":
    main()
