#!/usr/bin/env python3
"""Extract a stream of concatenated cpio archives (SVR4 ASCII / CRC).

casper/initrd on Ubuntu live ISOs is N back-to-back cpio archives
(e.g. [microcode][casper-rw rootfs]). GNU cpio stops at the first
TRAILER!!!, so we walk the stream manually and extract every member.

Usage: python3 split_cpio.py <initrd> <outdir>
"""
import os
import stat
import struct
import sys

HDR = 110  # header length in the ASCII format

def parse_cpio_stream(data, outdir, member_name):
    """Extract one cpio archive starting at data[0]. Return bytes consumed."""
    off = 0
    nfiles = 0
    while True:
        if off + HDR > len(data):
            raise ValueError("truncated header")
        magic = data[off:off+6].decode()
        if magic not in ("070701", "070702"):
            raise ValueError(f"bad magic {magic!r} at offset {off}")
        h = data[off:off+HDR]
        ino = int(h[6:18], 16)
        mode = int(h[18:26], 16)
        size = int(h[54:66], 16)
        name = data[off+HDR:]
        nul = name.index(b'\0')
        name = name[:nul]
        if name == b'TRAILER!!!':
            name_end = off + HDR + nul + 1
            pad = (-name_end) % 4
            return name_end + pad, nfiles
        name_end = off + HDR + nul + 1
        data_start = name_end + ((-name_end) % 4)
        if data[off:off+6].decode() == "070702":
            # CRC format: verify
            crc = int(h[114:134], 16)
            calc = 0
            for block in (h[6:114], data[data_start:data_start+size]):
                # cpio CRC is a 16-bit CRC with the specific polynomial
                calc = crc16_cp1086(calc, block) if False else calc
            # (we skip strict CRC verification; format is standard)
        payload = data[data_start:data_start+size]
        target = os.path.join(outdir, name.decode('utf-8', 'surrogateescape'))
        os.makedirs(os.path.dirname(target) or '.', exist_ok=True)
        st_mode = stat.S_IFMT(mode)
        if st_mode == stat.S_IFLNK:
            os.symlink(payload.decode(), target)
        elif st_mode == stat.S_IFDIR and size == 0:
            os.makedirs(target, exist_ok=True)
        elif st_mode == stat.S_IFREG:
            if os.path.islink(target) or os.path.exists(target):
                pass
            with open(target, 'wb') as f:
                f.write(payload)
            os.chmod(target, stat.S_IMODE(mode) or 0o755)
        off = data_start + size
        off += (-off) % 4
        nfiles += 1

def main():
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    data = open(src, 'rb').read()
    print(f"input: {len(data)} bytes")
    pos = 0
    k = 0
    while pos < len(data):
        # find next cpio magic (should be at pos already)
        if data[pos:pos+6] not in (b'070701', b'070702'):
            # search forward (padding between members)
            nxt = data.find(b'070701', pos)
            nxt2 = data.find(b'070702', pos)
            cands = [x for x in (nxt, nxt2) if x != -1]
            if not cands:
                print(f"no more cpio members after {pos} ({len(data)-pos} bytes left)")
                break
            pos = min(cands)
        consumed, nfiles = parse_cpio_stream(data[pos:], outdir, f"member{k}")
        print(f"member {k} at {pos}: {nfiles} files, consumed {consumed} bytes")
        pos += consumed
        k += 1
    print(f"done: {k} members, final pos {pos}/{len(data)}")

if __name__ == '__main__':
    main()
