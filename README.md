# FlashAgent — pre-OS AI agent روی فلش (از صفر، بدون OS)

FlashAgent یک ایجنت IT به‌صورت کامل از صفر (C خالص، ~1200 خط، static binary بدون
هیچ کتابخانه خارجی) است که مستقیم از فلش USB با UEFI بوت می‌شود — **بدون هیچ
سیستم‌عاملی روی ماشین هدف**. به LLM (plain HTTP) متصل است، tool call محلی اجرا
می‌کند و کارهای واقعی انجام می‌دهد: شناسایی HW، خواندن/نوشتن تنظیمات UEFI NVRAM
(لایهٔ واقعی «BIOS settings»)، کار با disk، framebuffer، شبکه، و plan نصب OS از
ایزوهای روی فلش.

## مدارک verify شده (qemu q35 + OVMF + serial)

- [x] GRUB (EFI) از ESP بوت می‌شود و منو/کنفیگ درست لود می‌شود
- [x] kernel 5.15.0-139 + initramfs سفارشی (3.7 MB) بوت می‌شود
- [x] `/init` (PID 1) اجرا می‌شود: proc/sys/devtmpfs/**efivarfs**، 34 ماژول
- [x] پارتیشن دادهٔ فلش (ext4) mount می‌شود: `mounted USB data partition /dev/sda2`
- [x] DHCP: `lease of 10.0.2.15 obtained` + IP/route اعمال شده
- [x] **حلقهٔ agentic کامل درون VM**: user → LLM (HTTP) → tool_call `hw_scan` →
      اجرای محلی روی HW واقعی VM (706 chars) → بازخورد به LLM → پاسخ نهایی
- [x] خروج تمیز از agent + fallback به maintenance shell (بدون kernel panic)

## معماری

```
فلش USB (GPT, ≥4GB)
├── p1: 64M  ESP (FAT32)
│   ├── EFI/BOOT/BOOTX64.EFI  GRUB (grub-mkimage, prefix=/EFI/BOOT)
│   ├── EFI/BOOT/grub.cfg     منوی بوت
│   ├── vmlinuz               kernel میزبان
│   └── initrd                initramfs سفارشی (3.7 MB)
└── p2: ext4  (FlashData)
    ├── flashagent.env        LLM_BASE / LLM_KEY / LLM_MODEL
    └── *.iso                 ایزوهای نصب OS

بوت: UEFI → GRUB → kernel → initramfs → /init → /flashagent (PID 1)
اجرا: LLM (plain HTTP, raw socket) ↔ tool-calling loop ↔ ابزارهای محلی
```

## ابزارهای ایجنت

| tool | کار |
|---|---|
| `hw_scan` | CPU/RAM/DMI/BIOS/GPU/USB از sysfs |
| `disk_list` | diskها + پارتیشن‌ها (blockdev/sfdisk) |
| `efi_vars` | خواندن/لیست UEFI NVRAM (مستقیم از efivarfs) |
| `boot_entries` | خواندن/برچسب EFI boot entries |
| `screen` | وضعیت framebuffer + thumbnail از /dev/fb0 |
| `net` | status/ping/http |
| `file_read`/`file_write` | دسترسی به فایل |
| `run` | shell command با timeout |
| `os_install` | plan نصب OS از ایزوهای فلش (autoinstall/unattend) |

## ساخت

```sh
cd ~/bootable-agent/flashagent
gcc -static -O2 -Wall -Wno-unused-function -o build/flashagent src/flashagent.c
gcc -static -O2 -o build/insmod src/insmod.c
bash build-usb-image.sh          # → flashagent-usb.img (512M)
```

`build-usb-image.sh` خودکار: GRUB سفارشی (prefix درست)، kernel + initrd روی ESP،
پارتیشن ext4 با `flashagent.env` و جای ایزو.

## بوت روی فلش واقعی

```sh
sudo dd if=flashagent-usb.img of=/dev/sdX bs=4M oflag=direct status=progress
```

قبل از بوت، LLM endpoint/key را در `flashagent.env` (روی p2) تنظیم کنید.
agent روی tty0/serial مستقیم کار می‌کند؛ با `exit` به maintenance shell برمی‌گردد.

## تست در qemu

```sh
python3 build/mockllm.py &    # mock OpenAI روی :8977
cp /usr/share/OVMF/OVMF_VARS.fd build/ovmf-vars.fd
qemu-system-x86_64 -machine q35 -cpu max -m 2048 -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE.fd \
  -drive if=pflash,format=raw,file=build/ovmf-vars.fd \
  -drive file=flashagent-usb.img,if=ide,format=raw,cache=unsafe \
  -serial stdio -display none
```
(LLM env در تصویر تست به `http://10.0.2.2:8977/v1` یعنی host از دید slirp NAT اشاره دارد.)

## نکات فنی (pitfallها — از تست واقعی)

- **GRUB**: از `grub-mkimage -O x86_64-efi -p /EFI/BOOT` بسازید. تصویر monolithic
  بستهٔ اوبونتو prefix اشتباه دارد و به prompt `grub>` می‌افتد.
- **init**: باید در **سطح بالای cpio** با نام `./init` و permission 755 باشد
  (و نه فقط `bin/init` که busybox است).
- **udhcpc (busybox 1.30.1)**: reason اولیه `bound` است (نه `bind`)؛ متغیرهای
  lease به‌صورت env با حروف کوچک (`ip`, `subnet`, `router`, `dns`) پاس داده
  می‌شوند. script باید executable باشد.
- **exFAT**: ساخت exFAT در این سیستم از FUSE انجام می‌شود و در-VM (درایwer
  kernel) mount نمی‌شود → p2 را **ext4** بسازید (built-in kernel).
- **ISO + OVMF**: درایور ISO9660 OVMF روی این qemu file system را attach نمی‌کرد؛
  تصویر USB (GPT+ESP) مسیر قابل اعتمادتر و واقعی‌تر است.
- **Auth HTTP**: header دقیقاً `Authorization: Bearer <key>` باشد (bug تکرار
  پیشوند با `Authorization: Bearer ***`).

## محدودیت‌ها (صادقانه)

- **Secure Boot**: با Secure Boot فعال، بوت custom block می‌شود.
- **os_install**: planner/executor است (autoinstall/unattend درست می‌کند + boot
  installer)؛ agent جایگزین installer کامل نیست.
- **framebuffer در qemu بدون VGA واقعی**: ممکن است خالی بماند.
- **key روی فلش**: فعلاً در `flashagent.env` پارتیشن داده (پیشنهاد: LUKS روی p2).
- **LLM واقعی**: endpoint `http://157.66.255.8:4000/v1` (model `qwen3.8`) از
  فلش باید قابل‌رس‌بسی باشد؛ key را در `flashagent.env` بگذارید. مکانیک با mock
  کامل verify شده است.

## فایل‌های کلیدی

| فایل | توضیح |
|---|---|
| `flashagent-usb.img` | تصویر فلش بوت‌ابل 512M |
| `build/flashagent` | static agent binary (~1 MB) |
| `src/flashagent.c` | سورس agent (~1200 خط) |
| `src/insmod.c` | static module loader (finit_module) |
| `root/init` | bootstrap پیش‌OS |
| `build-usb-image.sh` | سازندهٔ تصویر |
| `build/mockllm.py` | mock OpenAI برای تست |
| `build/fa-boot.log` | log بوت verify شده |

---

## دربارهٔ ما

FlashAgent پروژه‌ای از **کیان هوش** (Kiyan Hoosh) است؛ شرکت توسعه‌دهندهٔ
ایجنت‌های هوشمند برای کسب‌وکارها. این پروژه آزاد منتشر می‌شود تا در دنیای
توسعه‌دهندگان به اشتراک گذاشته شود.

- وبسایت: [housheyar.ir](https://housheyar.ir)
- سایت مادر شرکت: [kiyanhoosh.ir](https://kiyanhoosh.ir)

---

محتوای این ریپو (سورسِ تمیز — بدون فایل کلید): README، `src/`، `root/init`،
ابزارهای ساخت. فایل‌های حاوی کلید/API، binary، و تصویر فلش ساخته‌شده
عمداً در ریپو قرار گرفته نشده‌اند (علت امنیتی).

## مجوز

MIT License
