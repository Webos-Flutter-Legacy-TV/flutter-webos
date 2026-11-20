#!/usr/bin/env python3
"""Make an HEVC Annex B stream Dolby Vision Profile 8.1 for testing: append
the same RPU NAL unit (type 62, UNSPEC62) to every access unit.

The input needs access unit delimiters (x265 aud=1) and no B-frames. The RPU
comes from quietvoid's dovi_tool test assets (mel_to_81.bin: a start code and
the RPU payload, emulation prevention included); a static RPU is enough for
the TV to switch to Dolby Vision, not to grade the picture.

  hevc-add-rpu.py <in.hevc> <rpu.bin> <out.hevc>
"""
import sys

SC = b"\x00\x00\x00\x01"


def nal_units(data):
    """Yield NAL units (without start codes) of an Annex B stream."""
    i = data.find(b"\x00\x00\x01")
    while i >= 0:
        start = i + 3
        j = data.find(b"\x00\x00\x01", start)
        end = len(data) if j < 0 else j
        nal = data[start:end]
        if j >= 0 and nal.endswith(b"\x00"):
            nal = nal.rstrip(b"\x00")  # the next 4-byte start code's leading zero
        yield nal
        i = j


def main():
    src, rpu_path, dst = sys.argv[1:4]
    rpu = open(rpu_path, "rb").read()
    if rpu.startswith(SC):
        rpu = rpu[4:]
    rpu_nal = SC + bytes([62 << 1, 1]) + rpu  # nal_unit_type 62, layer 0, temporal id 1
    out = bytearray()
    units = 0
    have_vcl = False
    for nal in nal_units(open(src, "rb").read()):
        kind = (nal[0] >> 1) & 0x3F
        if kind == 35 and have_vcl:  # access unit delimiter: close the previous unit
            out += rpu_nal
            units += 1
            have_vcl = False
        if kind < 32:
            have_vcl = True
        out += SC + nal
    if have_vcl:
        out += rpu_nal
        units += 1
    open(dst, "wb").write(out)
    print(f"  {dst}: RPU added to {units} access units")


if __name__ == "__main__":
    main()
