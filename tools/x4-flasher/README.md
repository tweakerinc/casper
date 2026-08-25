# CrossPoint USB flasher

Small Linux GUI (and CLI) for writing a CrossPoint **app** `.bin` at `0x10000`.

| Device | Chip | Image |
|---|---|---|
| X3 | ESP32-C3 | Same CrossPoint `.bin` as X4 |
| X4 | ESP32-C3 | Same CrossPoint `.bin` as X3 |
| X4 Pro | ESP32-S3 | Separate S3 image — do not use the X3/X4 `.bin` |

## CachyOS

```bash
sudo pacman -S tk esptool
python3 tools/x4-flasher/crosspoint-flash.py
```

Pick the device, pick the `.bin` (Downloads is the default folder), confirm `/dev/ttyACM0`, Flash. After **Hash of data verified**: Reset, then Power.

If the port is permission-denied, `sudo usermod -aG uucp $USER` and open a new terminal, or run the script with `sudo -E`.

CLI:

```bash
python3 tools/x4-flasher/crosspoint-flash.py --device x4 ~/Downloads/CrossPoint-0079-388be0e1.bin
python3 tools/x4-flasher/crosspoint-flash.py --device x3 --port /dev/ttyACM0 firmware.bin
python3 tools/x4-flasher/crosspoint-flash.py --device x4pro firmware.bin
```
