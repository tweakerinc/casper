# CrossPoint USB flasher

Dark Linux GUI (and CLI) for writing a CrossPoint **app** `.bin` at `0x10000`.

| Device | Chip | Image |
|---|---|---|
| X3 | ESP32-C3 | Same CrossPoint `.bin` as X4 |
| X4 | ESP32-C3 | Same CrossPoint `.bin` as X3 |
| X4 Pro | ESP32-S3 | Separate S3 image — do not use the X3/X4 `.bin` |

USB-JTAG stub+compress writes junk (identical MD5 mismatch every retry). The tool defaults to **115200**, `--no-stub`, `--no-compress`, DIO/16MB/40MHz, and asks for admin (`pkexec`) if `/dev/ttyACM0` is not writable. Uncompressed flash takes several minutes.

## CachyOS

```bash
sudo pacman -S tk esptool
cd /path/to/casper
git pull
python3 tools/x4-flasher/crosspoint-flash.py
```

Pick **X4**, pick the `.bin`, confirm `/dev/ttyACM0`. A polkit password dialog is normal. After **Hash of data verified**: Reset, then Power.

Leave baud at **115200**. Do not use 921600 on this USB port.

One-shot group fix so admin is no longer needed:

```bash
sudo usermod -aG uucp $USER   # then log out and back in
```

CLI:

```bash
python3 tools/x4-flasher/crosspoint-flash.py --device x4 --baud 115200 ~/Downloads/CrossPoint-0079-388be0e1.bin
```
