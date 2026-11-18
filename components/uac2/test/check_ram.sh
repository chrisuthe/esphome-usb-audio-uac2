#!/bin/sh
# Checks in a firmware build that the USB host controller driver's interrupt path sits in RAM.
# Usage: check_ram.sh BUILD_DIR
# The controller driver has one microframe (125 us) to enable an isochronous channel again after each transfer.
# Run from flash, its interrupt handler was sometimes too slow for that on an ESP32-S31, and the next transfer lost
# its first packet (espressif/esp-usb issue 654). usb_host_uac/linker.lf places the two source files in RAM; this is
# the check that it took.
# It reads the ELF's section and symbol tables with readelf, which needs no toolchain for the chip: a function is in
# flash when its address lies in the .flash.text section.
ELF="$1/build/firmware.elf"
[ -f "$ELF" ] || { echo "USB interrupt path: no $ELF"; exit 1; }
# Address and size of .flash.text. The section's index is one field or two ("[10]", "[ 9]"), so count from its name
RANGE=$(readelf -S -W "$ELF" | awk '{ for (i = 1; i <= NF; i++) if ($i == ".flash.text") { print $(i + 2), $(i + 4); exit } }')
[ -n "$RANGE" ] || { echo "USB interrupt path: no .flash.text section in $ELF"; exit 1; }
start=$((0x${RANGE% *}))
end=$((start + 0x${RANGE#* }))
SYMBOLS=$(readelf -s -W "$ELF")
flash=0
missing=0
for f in intr_hdlr_main usb_dwc_hal_decode_intr usb_dwc_hal_get_chan_pending_intr usb_dwc_hal_chan_decode_intr \
    usb_dwc_hal_chan_activate; do
  addr=$(echo "$SYMBOLS" | awk -v f="$f" '$4 == "FUNC" && $8 == f { print $2; exit }')
  if [ -z "$addr" ]; then
    missing=$((missing + 1)); echo "  $f: not in the symbol table"
  elif [ $((0x$addr)) -ge "$start" ] && [ $((0x$addr)) -lt "$end" ]; then
    flash=$((flash + 1)); echo "  $f at 0x$addr: in flash"
  fi
done
[ "$flash" = 0 ] && [ "$missing" = 0 ] && echo "USB interrupt path: in RAM" && exit 0
echo "USB interrupt path: FAIL, of 5 functions $flash are in flash and $missing were not found"
exit 1
