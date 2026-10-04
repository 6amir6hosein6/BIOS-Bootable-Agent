#!/usr/bin/env python3
"""Robust single-stream cpio (070701/070702) extractor for concatenated
archives. Walks the whole byte stream entry-by-entry, so concatenated
cpio members (microcode + casper rootfs) all come out. 4-byte alignment
handled exactly per the cpio spec."""
import os, stat, sys

def align4(x): return (x + 3) & ~3

def main(src, outdir):
    os.makedirs(outdir, exist_ok=True)
    data = open(src, 'rb').read()
    n = len(data)
    o = 0
    total = 0
    archives = 0
    while o + 110 <= n:
        magic = data[o:o+6]
        if magic not in (b'070701', b'070702'):
            # find next cpio header (padding/zeros between archives)
            nxt = data.find(b'070701', o)
            nxt2 = data.find(b'070702', o)
            cands = [x for x in (nxt, nxt2) if x != -1]
            if not cands:
                break
            o = min(cands)
            continue
        h = data[o:o+110]
        try:
            file_size = int(h[54:62], 16)
            namesize  = int(h[102:110], 16)
            mode      = int(h[18:26], 16)
        except ValueError:
            o += 1
            continue
        name_end = align4(o + 110 + namesize)
        name = data[o+110:name_end]
        nul = name.find(b'\0')
        name = name[:nul] if nul != -1 else name
        data_start = name_end
        if name == b'TRAILER!!!':
            o = align4(data_start + file_size)
            archives += 1
            continue
        payload = data[data_start:data_start+file_size]
        target = os.path.join(outdir, name.decode('utf-8','surrogateescape'))
        tdir = os.path.dirname(target)
        if tdir: os.makedirs(tdir, exist_ok=True)
        fm = stat.S_IFMT(mode)
        try:
            if fm == stat.S_IFDIR and file_size == 0:
                os.makedirs(target, exist_ok=True)
            elif fm == stat.S_IFLNK:
                if os.path.lexists(target): os.remove(target)
                os.symlink(payload.decode('utf-8','surrogateescape'), target)
            elif fm == stat.S_IFREG:
                if os.path.lexists(target) and not os.path.isdir(target):
                    os.remove(target)
                with open(target,'wb') as f: f.write(payload)
                try: os.chmod(target, stat.S_IMODE(mode) or 0o644)
                except: pass
            # fifos/sockets/chr/blk: skip
        except Exception as e:
            pass
        o = align4(data_start + file_size)
        total += 1
    print(f"extracted {total} entries, {archives} TRAILERs, final offset {o}/{n}")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
