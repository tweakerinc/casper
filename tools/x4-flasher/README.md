# CrossPoint USB flasher

Dark Linux GUI (and CLI) for writing a CrossPoint **app** `.bin` at `0x10000`.

| Device | Chip | Image |
|---|---|---|
| X3 | ESP32-C3 | Same CrossPoint `.bin` as X4 |
| X4 | ESP32-C3 | Same CrossPoint `.bin` as X3 |
| X4 Pro | ESP32-S3 | Separate S3 image — do not use the X3/X4 `.bin` |

USB-JTAG often corrupts at 921600. The tool defaults to **460800**, writes DIO/16MB/80MHz, and asks for admin (`pkexec`) if `/dev/ttyACM0` is not writable.

## CachyOS

```bash
sudo pacman -S tk esptool
cd /path/to/casper
git pull
python3 tools/x4-flasher/crosspoint-flash.py
```

Pick **X4**, pick the `.bin`, confirm `/dev/ttyACM0`. A polkit password dialog is normal. After **Hash of data verified**: Reset, then Power.

If MD5 still mismatches, drop baud to **115200** in the GUI.

One-shot group fix so admin is no longer needed:

```bash
sudo usermod -aG uucp $USER   # then log out and back in
```

CLI:

```bash
python3 tools/x4-flasher/crosspoint-flash.py --device x4 --baud 460800 ~/Downloads/CrossPoint-0079-388be0e1.bin
```
