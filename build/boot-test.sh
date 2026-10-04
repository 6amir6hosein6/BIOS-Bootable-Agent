#!/bin/bash
# boot-test: flashagent.iso in qemu OVMF, feed a task over serial, capture all output
set -u
cd ~/bootable-agent/flashagent
OVMF=/usr/share/OVMF/OVMF_CODE.fd
rm -f build/ovmf-vars-test.fd
cp /usr/share/OVMF/OVMF_VARS.fd build/ovmf-vars-test.fd
rm -f build/boottest.log

{
  sleep 55
  # agent REPL prompt should be up; send the task
  printf 'Identify this machine: CPU, RAM, BIOS version, disks.\n'
  sleep 45
  printf 'exit\n'
  sleep 3
} | timeout 140 qemu-system-x86_64 \
  -machine q35 -cpu max -m 2048 -smp 2 \
  -drive if=pflash,format=raw,readonly=on,file=$OVMF \
  -drive if=pflash,format=raw,file=build/ovmf-vars-test.fd \
  -cdrom flashagent.iso \
  -boot d \
  -serial stdio -display none -no-reboot \
  > build/boottest.log 2>&1

echo "=== qemu done, log size: $(stat -c%s build/boottest.log) bytes ==="
