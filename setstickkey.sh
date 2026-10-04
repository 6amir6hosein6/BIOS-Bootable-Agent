#!/usr/bin/env bash
# Set the real LLM key on the FlashAgent stick (data partition p2).
# Run THIS in your own terminal. It reads the key from ~/.hermes/.env in a
# single Python process (never through shell vars or grep), verifies it,
# and writes /mnt/usb/flashagent.env on the stick.
#
# REQUIRES: the stick to have the FlashAgent layout (GPT, p2 ext4).
# If the stick is a plain single-partition flash, re-flash it first:
#   sudo dd if=$HOME/bootable-agent/flashagent/flashagent-usb.img of=/dev/sdX bs=4M oflag=direct conv=fsync status=progress
#
# Usage: DEVICE=sda2 ./setstickkey.sh   (default sda2)
set -euo pipefail

DEV="${DEVICE:-sda2}"
DROOT="/dev/${DEV%[0-9]*}"
MNT="/mnt/fa_stick"

# 0. layout check (needs root to read the partition table)
if [ ! -b "$DROOT" ]; then
  echo "ERROR: $DROOT not present. Is the flash plugged in? (lsblk)"
  exit 1
fi
P1PT="$(sudo lsblk -dnpo NAME "$DROOT" | head -1 || true)"
P2PT="$(sudo lsblk -dnpo NAME "$DROOT" | tail -1 || true)"
if [ -z "$P2PT" ]; then
  echo "ERROR: $DROOT has only one partition — the FlashAgent image is NOT on this stick."
  echo "It must be a single-partition exFAT (original factory state)."
  echo "Re-flash first:"
  echo "  sudo dd if=\$HOME/bootable-agent/flashagent/flashagent-usb.img of=$DROOT bs=4M oflag=direct conv=fsync status=progress"
  exit 1
fi

# 1. unmount everything on the stick (GNOME auto-mount)
for mp in $(lsblk -rnpo MOUNTPOINT "$DROOT" 2>/dev/null); do
  [ -n "$mp" ] && sudo umount "$mp" 2>/dev/null || true
done

# 2. do the whole key job in ONE python process (read .env -> verify -> write)
#    ($HOME is the real user's home; inside sudo python ~ would be /root)
sudo python3 - "$P2PT" "$MNT" "$HOME/.hermes/.env" <<'PYEOF'
import sys, os, hashlib, subprocess, stat

p2, mnt, envfile = sys.argv[1], sys.argv[2], sys.argv[3]
GOOD_SHA16 = "8dbe859123de65e3"   # sha256[:16] of the known-good key in ~/.hermes/.env

def h(b): return hashlib.sha256(b).hexdigest()[:16]

# read raw key bytes from .env
key = None
for line in open(envfile, "rb").read().split(b"\n"):
    if b"HERMES_CUSTOM_157_66_255_8_4000_API_KEY" in line:
        key = line.split(b"=", 1)[1].strip()
        break
if not key:
    sys.exit("FATAL: key not found in ~/.hermes/.env")
print(f"  source key: len={len(key)} sha16={h(key)}")
if h(key) != GOOD_SHA16:
    sys.exit(f"FATAL: source key in .env does not match expected hash {GOOD_SHA16}.\n"
             f"  (masking or .env changed — aborting, nothing written)")

# mount p2
os.makedirs(mnt, exist_ok=True)
subprocess.run(["mount", p2, mnt], check=True)
try:
    path = os.path.join(mnt, "flashagent.env")
    cfg = b"LLM_BASE=http://157.66.255.8:4000/v1\nLLM_KEY=***" + key + b"\nLLM_MODEL=qwen3.8\n"
    with open(path, "wb") as f:
        f.write(cfg)
    os.chmod(path, 0o644)
    # verify what actually landed on the stick
    stored = [l for l in open(path, "rb").read().split(b"\n")
              if l.startswith(b"LLM_KEY=")][0][8:]
    print(f"  stick key : len={len(stored)} sha16={h(stored)}")
    ok = (stored == key)
    print("  VERDICT:", "MATCH — stick has the correct key" if ok
          else "MISMATCH — do NOT boot, tell me the two hashes")
    if not ok:
        sys.exit(1)
finally:
    subprocess.run(["umount", mnt], check=False)

PYEOF

echo "DONE. Unplug and boot from the stick (UEFI, Secure Boot off)."
