"""Write the raw configuration descriptor of each attached USB audio device to <vid>_<pid>.bin.

Reads what Windows cached at enumeration (through libusb), so no device is opened or disturbed.
Usage: dump_desc.py OUTDIR
"""

import sys
from pathlib import Path

import libusb_package
import usb.core

AUDIO_CLASS = 1


def raw_config(cfg) -> bytes:
    """Rebuild the descriptor bytes from libusb's parsed tree; it keeps every class-specific descriptor as 'extra'."""
    out = bytearray(
        [9, 2, cfg.wTotalLength & 0xFF, cfg.wTotalLength >> 8, cfg.bNumInterfaces, cfg.bConfigurationValue,
         cfg.iConfiguration, cfg.bmAttributes, cfg.bMaxPower]
    )
    out += bytes(cfg.extra_descriptors)
    for intf in cfg:
        out += bytes(
            [9, 4, intf.bInterfaceNumber, intf.bAlternateSetting, intf.bNumEndpoints, intf.bInterfaceClass,
             intf.bInterfaceSubClass, intf.bInterfaceProtocol, intf.iInterface]
        )
        out += bytes(intf.extra_descriptors)
        for ep in intf:
            head = [ep.bLength, 5, ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize & 0xFF,
                    ep.wMaxPacketSize >> 8, ep.bInterval]
            if ep.bLength >= 9:
                head += [ep.bRefresh, ep.bSynchAddress]
            out += bytes(head)
            out += bytes(ep.extra_descriptors)
    return bytes(out)


outdir = Path(sys.argv[1])
for dev in libusb_package.find(find_all=True):
    try:
        cfg = dev[0]
    except usb.core.USBError:
        continue
    if not any(intf.bInterfaceClass == AUDIO_CLASS for intf in cfg):
        continue
    raw = raw_config(cfg)
    name = f"{dev.idVendor:04x}_{dev.idProduct:04x}.bin"
    (outdir / name).write_bytes(raw)
    speed = {1: "low", 2: "full", 3: "high", 4: "super"}.get(dev.speed, str(dev.speed))
    print(f"{name}: {len(raw)} bytes rebuilt, wTotalLength {cfg.wTotalLength}, speed {speed}")
