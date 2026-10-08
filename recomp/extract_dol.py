#!/usr/bin/env python3
"""Extract main.dol from a GameCube disc image and verify it is the expected build.

Usage: extract_dol.py <image.iso|image.ciso> <out main.dol>
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from dol import DiscImage  # noqa: E402

EXPECTED_ID = b'GM8E01'
EXPECTED_SHA1 = '39a2f928159ad46491e9b1ef0a72613d91e0cc40'


def main():
    img_path, out_path = sys.argv[1:3]
    img = DiscImage(img_path)
    hdr = img.read(0, 0x440)
    if hdr[:6] != EXPECTED_ID:
        sys.exit(f'error: {img_path} is not Metroid Prime (USA) (disc ID {hdr[:6]!r}, expected {EXPECTED_ID!r}).\n'
                 'Note: only uncompressed .iso and .ciso images are supported; convert .rvz/.gcz first (see README).')
    dol_off = struct.unpack('>I', hdr[0x420:0x424])[0]
    dh = img.read(dol_off, 0x100)
    offs = struct.unpack('>18I', dh[0x00:0x48])
    sizes = struct.unpack('>18I', dh[0x90:0xD8])
    dol_size = max(o + s for o, s in zip(offs, sizes) if s)
    dol = img.read(dol_off, dol_size)
    sha1 = hashlib.sha1(dol).hexdigest()
    if sha1 != EXPECTED_SHA1:
        sys.exit(f'error: main.dol SHA-1 {sha1} does not match the supported build ({EXPECTED_SHA1}).\n'
                 'Only Metroid Prime (USA) Rev 2 (v1.02, the Player\'s Choice release) is supported.')
    with open(out_path, 'wb') as f:
        f.write(dol)


if __name__ == '__main__':
    main()
