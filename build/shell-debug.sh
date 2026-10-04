#!/bin/bash
# interactive: boot ISO, drive the UEFI shell via serial to diagnose boot
set -u
cd ~/bootable-agent/flashagent
OVMF=/usr/share/OVMF/OVMF_CODE.fd
rm -f build/ovmf-vars-shell.fd build/shell.log
cp /usr/share/OVMF/OVMF_VARS.fd build/ovmf-vars-shell.fd
rm -f /tmp/qshell.fifo
mkfifo /tmp/qshell.fifo

timeout 160 qemu-system-x86_64 \
  -machine q35 -cpu max -m 2048 -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=$OVMF \
  -drive if=pflash,format=raw,file=build/ovmf-vars-shell.fd \
  -cdrom flashagent.iso -boot d \
  -serial stdio -display none -no-reboot \
  < /tmp/qshell.fifo > build/shell.log 2>&1 &
QPID=$!

# keep the fifo open (qemu stdin)
exec 3</tmp/qshell.fifo

sleep 25
{
  echo "MAP"
  sleep 3
  echo "map -r"
  sleep 2
  echo "ls fs0:/"
  sleep 2
  echo "ls fs0:\\EFI\\BOOT"
  sleep 2
  echo "load \\EFI\\BOOT\\BOOTX64.EFI"
  sleep 4
  echo "go"
  sleep 25
} >&3
exec 3<&-
wait $QPID 2>/dev/null
echo "=== shell log (cleaned) ==="
sed 's/\x1b\[[0-9;=]*[a-zA-Z=]//g' build/shell.log | tr '\r' '\n' | sed 's/  */ /g' | grep -vE '^\s*$' | head -100
