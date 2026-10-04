# FlashAgent — Pre-OS Bare-Metal USB Agent

A from-scratch, statically-linked C agent (~1,200 lines, no external libraries) that boots directly from a USB stick via UEFI — **with no operating system on the target machine**. It connects to a large language model over plain HTTP using a raw-socket client (no libcurl, no OpenSSL), runs local tool calls, and performs real hardware/firmware operations: enumerating components, reading/writing UEFI NVRAM settings (the real "BIOS settings" layer), interacting with disks, framebuffer, and the network, and preparing OS installs from ISOs stored on the stick.

## Verified in QEMU (q35 + OVMF + serial)

- [x] GRUB (EFI) boots from the ESP partition; menu and config load correctly
- [x] Kernel `5.15.0-139` + custom initramfs (3.7 MB) boot correctly
- [x] `/init` (PID 1) runs: `proc`/`sys`/`devtmpfs`/`efivarfs`, loads 34 kernel modules
- [x] USB data partition (ext4) mounts: `mounted USB data partition /dev/sda2`
- [x] DHCP lease applied: `lease of 10.0.2.15 obtained` + IP/route configured
- [x] **Full agentic loop inside the VM**: user → LLM (HTTP) → tool call `hw_scan` → local execution on the VM's real hardware (706 chars) → response back to the LLM → final answer
- [x] Clean agent exit with fallback to a maintenance shell — **no kernel panic**

## Architecture

```
USB stick (GPT, >=4GB)
├── p1: 64M  ESP (FAT32)
│   ├── EFI/BOOT/BOOTX64.EFI   GRUB (grub-mkimage, prefix=/EFI/BOOT)
│   ├── EFI/BOOT/grub.cfg      boot menu
│   ├── vmlinuz                host kernel
│   └── initrd                 custom initramfs (3.7 MB)
└── p2: ext4  (FlashData)
    ├── flashagent.env        LLM_BASE / LLM_KEY / LLM_MODEL
    └── *.iso                 OS install ISOs

Boot: UEFI -> GRUB -> kernel -> initramfs -> /init -> /flashagent (PID 1)
Run:  LLM (plain HTTP, raw socket) <-> tool-calling loop <-> local tools
```

## Agent Tools

| tool | purpose |
|---|---|
| `hw_scan` | CPU / RAM / DMI / BIOS / GPU / USB enumeration via sysfs |
| `disk_list` | Disks and their partitions (blockdev / sfdisk) |
| `efi_vars` | Read and list UEFI NVRAM directly from `efivarfs` |
| `boot_entries` | Read and relabel EFI boot entries |
| `screen` | Framebuffer status + thumbnail from `/dev/fb0` |
| `net` | Link status, ping, HTTP |
| `file_read` / `file_write` | File access |
| `run` | Shell command with timeout |
| `os_install` | Prepare an OS install plan from stick ISOs (autoinstall / unattend) |

## Building

```sh
cd ~/bootable-agent/flashagent
gcc -static -O2 -Wall -Wno-unused-function -o build/flashagent src/flashagent.c
gcc -static -O2 -o build/insmod src/insmod.c
bash build-usb-image.sh          # -> flashagent-usb.img (512 MB)
```

`build-usb-image.sh` sets up a custom GRUB (with the correct prefix), copies the
kernel and initramfs onto the ESP, and creates an ext4 data partition with
`flashagent.env` and space for ISOs.

## Booting from a real USB stick

```sh
sudo dd if=flashagent-usb.img of=/dev/sdX bs=4M oflag=direct status=progress
```

Configure the LLM endpoint and key in `flashagent.env` on partition `p2` before
booting. The agent runs directly on `tty0`/serial; type `exit` to drop to a
maintenance shell.

## Testing in QEMU

```sh
python3 build/mockllm.py &    # mock OpenAI API on port 8977
cp /usr/share/OVMF/OVMF_VARS.fd build/ovmf-vars.fd
qemu-system-x86_64 -machine q35 -cpu max -m 2048 -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/ovmf-vars.fd \
  -drive file=flashagent-usb.img,if=ide,format=raw,cache=unsafe \
  -serial stdio -display none
```

(The test image points its LLM env at `http://10.0.2.2:8977/v1`, i.e. the QEMU
host as seen from slirp NAT.)

## Technical Notes (from real-world testing)

- **GRUB**: build with `grub-mkimage -O x86_64-efi -p /EFI/BOOT`. The monolithic
  image shipped with Ubuntu has the wrong prefix and drops to the `grub>` prompt.
- **`/init`**: must be at the **top level** of the cpio archive as `./init` with
  mode `755` — not only at `bin/init` (which busybox also provides).
- **udhcpc (busybox 1.30.1)**: the initial reason is `bound`, not `bind`; lease
  variables arrive as lowercase environment variables (`ip`, `subnet`, `router`,
  `dns`). The script must be executable.
- **exFAT**: an exFAT volume created on this host uses FUSE and will not mount in
  the VM (the kernel driver rejects it) — so make `p2` **ext4**, which is built
  into the kernel.
- **ISO + OVMF**: the OVMF ISO9660 driver on this QEMU configuration would not
  attach the ISO. A USB image with a GPT + ESP layout is the more reliable,
  realistic path.
- **HTTP auth**: the header must be exactly `Authorization: Bearer <key>` — do
  not add a prefix such as `Authorization: Bearer Bearer ...`.

## Honest Limitations

- **Secure Boot**: a custom boot chain is blocked when Secure Boot is enabled.
- **`os_install`**: this is a planner/executor — it writes a correct
  autoinstall/unattend script and boots the installer, but it is **not** a full
  drop-in replacement for a complete installer.
- **Framebuffer in QEMU**: without a real VGA adapter the framebuffer may be
  empty.
- **Key on the stick**: currently stored in plaintext in `flashagent.env` on the
  data partition (recommended follow-up: LUKS on `p2`).
- **Real LLM**: the endpoint `http://157.66.255.8:4000/v1` (model `qwen3.8`) must
  be reachable from the stick. The key goes in `flashagent.env`. The full agentic
  mechanism is verified end-to-end against a mock server.

## Key Files

| file | description |
|---|---|
| `flashagent-usb.img` | bootable 512 MB USB image |
| `build/flashagent` | static agent binary (~1 MB) |
| `src/flashagent.c` | agent source (~1,200 lines) |
| `src/insmod.c` | static module loader (`finit_module`) |
| `root/init` | pre-OS bootstrap script |
| `build-usb-image.sh` | image builder |
| `build/mockllm.py` | mock OpenAI server for testing |
| `build/fa-boot.log` | verified boot log |

---

## About

FlashAgent is a project from **Kiyan Hoosh** (Kiyan Hoesh AI Computing), a
company that builds AI agents for businesses. It is published open so that the
developer community can build on it.

- Website: [housheyar.ir](https://housheyar.ir)
- Parent company: [kiyanhoosh.ir](https://kiyanhoosh.ir)

The repository contains the clean source only — no secret files, no binary, and
no built image (those contain a live API key and are intentionally excluded for
security reasons).

## License

MIT
