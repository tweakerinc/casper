---
title: Installation
nav_order: 2
---

# Installation

Flash **Casper** onto an Xteink X3 or X4. You can always reinstall official CrossPoint firmware afterward.

## Web installer (recommended)

1. Download the `firmware-*.bin` for your build from the [releases page](https://github.com/TweakerInc/casper/releases).
2. Connect your Xteink X3 or X4 via USB-C and wake/unlock the device.
3. Open <https://crosspointreader.com/#flash-tools> and choose your device.
4. Select **Custom .bin**.
5. Choose the Casper `firmware-*.bin` and click **Flash**.

### Revert to official firmware

Flash the latest official build from <https://crosspointreader.com/#flash-tools> (not Custom .bin).

## Command line

These steps are for macOS and Linux. On Windows, prefer the web installer.

Install `esptool`:

```sh
pip3 install esptool
```

Download the firmware from [Releases](https://github.com/TweakerInc/casper/releases), connect USB-C, then find the port:

```sh
# Linux
dmesg | grep tty

# macOS
ls /dev/cu.*
```

Flash (offset `0x10000` matches the usual CrossPoint app partition):

```sh
# Linux — adjust port and path
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin

# macOS — adjust port and path
esptool.py --chip esp32c3 --port /dev/cu.usbmodem2101 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

## After flashing

1. Confirm boot shows **Casper** branding.
2. Optional: copy dictionary packs to `/.crosspoint/dict/` on the SD card — see [Dictionary](./dictionary.md).
3. If anything feels wrong, reflash official CrossPoint and open an issue on the Casper repo with device (X3/X4) and version.

## Which `.bin`?

| Asset name (typical) | When to use |
|----------------------|-------------|
| `firmware-default.bin` | Full default build (recommended for testers) |
| `firmware-tiny.bin` | Smaller font set if you publish a tiny env |
| `firmware-xlarge.bin` | Larger font set if you publish an xlarge env |

See [Font Build Variants](./font-build-variants.md) if you ship multiple variants.
