# Nova — CrossPoint greenfield firmware

**Nova** is a from-scratch application firmware for Xteink X3/X4. It does **not** use CrossPoint or CrossInk product code.

## What it is

| Layer | Source |
|-------|--------|
| Hardware / EPD / SD / power | FreeInk SDK (`../freeink-sdk`) |
| Low-level HAL wrappers | Shared `../lib/hal` (display, GPIO, storage) |
| Drawing / fonts | `../lib/GfxRenderer`, `../lib/EpdFont` |
| **Only reader engine** | `../lib/Rivulet` |
| **Application** | **This tree only** (`nova/src`) |

## Product (v0)

- **Rivulet-only** EPUB reading
- **Penumbra + Bare** UI skins
- **Sleep** screens
- **Wi‑Fi file transfer** + **OPDS** (stubs first, full later)
- **Settings that matter**: font size, margins, anti-ghost refresh, button map, orientation, sleep, Wi‑Fi
- **English / Latin only** — no i18n pack, no BiDi product path
- **Storage**: `/.casper/` only (no `/.crosspoint`)

## Build

From this directory:

```bash
# Windows example
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e default
%USERPROFILE%\.platformio\penv\Scripts\platformio.exe run -e default -t upload
```

Output: `.pio/build/default/firmware.bin`

Legacy CrossPoint (CrossPoint-lineage app) still builds from the **repo root** (`pio run -e default`). Nova is intentionally separate so we can ship a clean binary without gutting the old tree until feature parity.

## Layout

```
nova/src/
  main.cpp           boot + loop
  core/              settings, state, paths, input map, activity stack
  store/             book id + ledger under /.casper
  reader/            EPUB package + Rivulet screen
  ui/                home, library, settings, sleep, themes
  net/               transfer + OPDS (phased)
```

## Phases

See `docs/ROADMAP.md`.
