# Casper

Casper is a personal e-reader firmware for **Xteink X3/X4** (ESP32-C3), based on [CrossInk](https://github.com/uxjulia/CrossInk) / [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

This tree at `E:\casper` is the **Casper reference project**: keep branding, dashboard, dictionary, KOReader sync, and control defaults here so they can be re-applied cleanly when CrossPoint ships a new base firmware.

## Highlights (Casper)

- **Casper branding** — boot logo, device names, web portal, serial version strings
- **Dashboard** home theme (cover + reading stats)
- **Offline dictionary** — word selection, hyphen compounds, multi-word phrases
- **KOReader Sync** — credentials + adaptive progress options
- **Defaults**
  - Short power button → **Sleep**
  - Long-press menu → **Dictionary**
  - Side long-press → **Ignore** (no accidental multi-page while resting a finger)
- **Reader battery** top-right (matches dashboard)

## Docs

| Doc | Purpose |
|-----|---------|
| [CASPER.md](./CASPER.md) | What this project is and what must survive merges |
| [docs/CASPER_MERGE.md](./docs/CASPER_MERGE.md) | How to rebase Casper onto a new CrossPoint release |
| [docs/dictionary.md](./docs/dictionary.md) | Dictionary packs and format |
| [AGENTS.md](./AGENTS.md) | Engineering rules for ESP32-C3 firmware work |

## Build

PlatformIO, `default` env:

```bat
cd /d E:\casper
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e default
```

Firmware binary: `.pio\build\default\firmware-default.bin`

## Relationship to other trees

| Location | Use |
|----------|-----|
| `E:\casper` | Casper reference / merge source of truth |
| `C:\Users\m\CrossInk` | Optional daily worktree (may be ahead or messy) |
| New CrossPoint tag | Fresh base for the next Casper build |

When a new CrossPoint firmware is available, follow **[docs/CASPER_MERGE.md](./docs/CASPER_MERGE.md)**.

## Upstream

Casper is not a clean-room rewrite. It layers product choices on open CrossPoint/CrossInk work. Respect upstream licenses (`LICENSE`) and attribute CrossPoint/CrossInk as required.

## Hardware

Confirmed target class: Xteink X3 and X4. ESP32-C3 constraints apply (no PSRAM, tight heap) — see `AGENTS.md`.
