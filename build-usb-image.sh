#!/bin/bash
# build-usb-image.sh — build flashagent-usb.img: a bootable USB stick image
# Layout: GPT
#   p1: 64M  EFI System (FAT32)  -> /EFI/BOOT/BOOTX64.EFI (GRUB), /vmlinuz, /initrd, /grub.cfg
#   p2: rest data (exFAT)        -> ISOs for OS install, flashagent.env overrides
# Then: sudo dd if=flashagent-usb.img of=/dev/sdX bs=4M oflag=direct
set -euo pipefail
cd "$(dirname "$0")"
K=$(uname -r)
IMG=flashagent-usb.img
SIZE_MB=512

STAGE=$(mktemp -d /tmp/fasb.XXXXXX)
ESP=$STAGE/esp
DATA=$STAGE/data
mkdir -p "$ESP/EFI/BOOT" "$DATA"

# --- stage ESP contents (as user) ---
# custom EFI GRUB with prefix=/EFI/BOOT so it auto-loads /EFI/BOOT/grub.cfg
# (built once by grub-mkimage; the package monolithic image has a wrong prefix
#  and drops to the grub> prompt instead of loading the config)
if [ ! -s build/grub/grubx64.efi ]; then
  mkdir -p build/grub
  grub-mkimage -O x86_64-efi -p /EFI/BOOT -d /usr/lib/grub/x86_64-efi \
    -o build/grub/grubx64.efi \
    boot configfile normal search search_fs_file part_gpt fat ext2 iso9660 udf exfat \
    linuxefi efi_uga efi_gop all_video terminal serial chain loopback
fi
cp build/grub/grubx64.efi "$ESP/EFI/BOOT/BOOTX64.EFI"
sudo cp "/boot/vmlinuz-$K" "$ESP/vmlinuz"
cp root/initramfs-flashagent.cgz "$ESP/initrd"
# UEFI GRUB loads its config from the directory of BOOTX64.EFI -> /EFI/BOOT/grub.cfg
cat > "$ESP/EFI/BOOT/grub.cfg" <<'EOF'
set default=0
set timeout=3
menuentry "FlashAgent - pre-OS agent (tty0+serial)" {
  linux /vmlinuz console=tty0 console=ttyS0,115200
  initrd /initrd
}
menuentry "FlashAgent - serial only" {
  linux /vmlinuz console=ttyS0,115200
  initrd /initrd
}
EOF
echo flashagent-esp-magic > "$ESP/FAID"

# --- stage data partition contents ---
# LLM config: raw copy of the user's ~/.hermes/.env (byte copy — the key is
# never re-typed or re-expanded, so it can't be mangled in transit).
# /init sources it as the last fallback if LLM_KEY is still unset.
cp -f ~/.hermes/.env "$DATA/.env"
head -c 4194304 /dev/urandom > "$DATA/sample-ubuntu.iso"

# --- build the image ---
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=$SIZE_MB status=none
sync
sgdisk -Z "$IMG" -n 1:1M:65M -t 1:EF00 -c 1:ESP -n 2:66M:0 -t 2:0700 -c 2:FlashData >/dev/null

sudo losetup -P -f "$IMG"
LOOP=$(losetup -j "$PWD/$IMG" | head -1 | cut -d: -f1)
trap 'sudo losetup -d "$LOOP" 2>/dev/null || true; rm -rf "$STAGE"' EXIT
sudo partprobe "$LOOP" 2>/dev/null || true
sleep 1

P1="${LOOP}p1"; P2="${LOOP}p2"
i=0
until [ -b "$P1" ]; do sleep 1; i=$((i+1)); [ $i -ge 10 ] && { echo "part not ready"; exit 1; }; done

sudo mkfs.fat -F32 -n ESP "$P1" >/dev/null 2>&1 || sudo mkfs.vfat -F32 -n ESP "$P1" >/dev/null
# data partition: ext4 (built into the kernel — no module/FUSE/format quirks;
# the host's exFAT is FUSE-based and the in-VM kernel exfat driver rejects it)
sudo mkfs.ext4 -F -L FlashData "$P2" >/dev/null 2>&1 || sudo mkfs.ext4 -F "$P2" >/dev/null

M1=$STAGE/m1; M2=$STAGE/m2
mkdir -p "$M1" "$M2"
sudo mount "$P1" "$M1"
sudo mount "$P2" "$M2"

sudo cp -rL "$ESP/." "$M1/"
sudo cp -rL "$DATA/." "$M2/"
sudo umount "$M1" "$M2" 2>/dev/null || true
sync

echo "=== built $IMG ==="
ls -la "$IMG"
sgdisk -p "$IMG" 2>/dev/null || true
