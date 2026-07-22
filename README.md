# Casper

Casper is personal e-reader firmware for **Xteink X3/X4** (ESP32-C3), built cleanly on top of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

**Product branding is Casper only** (boot, UI, web, docs). This folder may still be named `CrossInk` on disk for historical reasons; the reference merge tree is `E:\casper`.

## Highlights

- Casper branding (logo, device names, web portal, serial)
- Dashboard home theme (cover + reading stats)
- Offline dictionary (word cursor, hyphen compounds, multi-word phrases)
- KOReader Sync with adaptive options
- Defaults: short power → sleep, long-press menu → dictionary, side long-press → ignore
- Reader battery percentage top-right (matches dashboard)

## Docs

| Doc | Purpose |
|-----|---------|
| [USER_GUIDE.md](./USER_GUIDE.md) | End-user guide |
| [CASPER.md](./CASPER.md) | Dual-tree sync note |
| [docs/DEV_NOTES.md](./docs/DEV_NOTES.md) | Engineering gotchas |
| [AGENTS.md](./AGENTS.md) | Firmware engineering rules |
| `E:\casper\docs\CASPER_MERGE.md` | Rebase onto new CrossPoint releases |

## Build

```bat
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e default
```

Firmware: `.pio\build\default\firmware-default.bin`

## Sync to Casper reference

After product changes:

```bat
robocopy C:\Users\m\CrossInk E:\casper /E /XD .git .pio .claude /XF tmp_title_chunk.txt /NFL /NDL /NJH /NJS
```

Then commit on `E:\casper` branch `casper/reference` when you want the change in the merge baseline.

## Upstream

Casper layers product choices on CrossPoint/CrossInk open work. Respect `LICENSE` and attribute upstream as required.

## Hardware

Xteink X3 and X4. ESP32-C3 constraints apply — see `AGENTS.md`.
