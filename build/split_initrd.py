#!/usr/bin/env python3
"""Split a multi-member gzip cpio initrd into individual cpio archives.

Ubuntu casper initrds are a single gzip stream with multiple members,
each member being one cpio archive (a "microcode + main" split, or
sometimes just repeated). We walk the gzip magic offsets and decompress
exactly one member at a time.
"""
import gzip
import os
import struct
import sys

def find_gzip_members(data: bytes):
    """Yield byte offsets where a new gzip member (magic 1f 8b) begins.

    A new member begins right after the previous member's compressed
    stream ends. We rely on gzip's own framing: after decompressing one
    member, the next 2 bytes should again be 1f 8b (or end of data).
    """
    offsets = []
    i = 0
    n = len(data)
    # First member
    if data[0:2] == b'\x1f\x8b':
        offsets.append(0)
    # Walk: decompress each member to learn its raw compressed length.
    idx = 0
    while idx < len(offsets):
        start = offsets[idx]
        # Use a streaming decompressor that tells us how many input bytes
        # were consumed for this single member.
        d = zlib_decompress_one(data[start:])
        consumed = d[1]
        if consumed is None:
            break
        nxt = start + consumed
        if nxt < n and data[nxt:nxt+2] == b'\x1f\x8b':
            offsets.append(nxt)
        idx += 1
    return offsets

import zlib

def zlib_decompress_one(blob: bytes):
    """Decompress exactly one gzip member. Return (raw_bytes, input_consumed)
    or (None, None) on failure."""
    d = zlib.decompressobj(31)  # 31 = gzip window
    try:
        out = d.decompress(blob)
        out += d.flush()
    except Exception:
        return None, None
    # d.eof_offset = bytes of input consumed for this stream
    consumed = d.eof_offset
    return out, consumed

def main():
    src = sys.argv[1]
    outdir = sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    data = open(src, 'rb').read()
    print(f"input: {len(data)} bytes")
    offs = find_gzip_members(data)
    print(f"members at offsets: {offs}")
    for k, off in enumerate(offs):
        raw, consumed = zlib_decompress_one(data[off:])
        if raw is None:
            print(f"member {k}: FAILED to decompress")
            continue
        magic = raw[:2]
        print(f"member {k}: in={consumed} out={len(raw)} magic={magic.hex()} "
              f"(cpio={magic==b'07'})")
        with open(os.path.join(outdir, f"member{k}.cpio"), 'wb') as f:
            f.write(raw)

if __name__ == '__main__':
    main()
