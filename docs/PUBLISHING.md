---
title: Publishing
nav_order: 99
---

# Publishing Casper (CrossInk-style)

CrossInk’s public face is roughly:

1. **GitHub repo** + rich **README**
2. **Releases** with `firmware-*.bin` assets
3. **GitHub Pages** from `docs/` using [Just the Docs](https://just-the-docs.github.io/just-the-docs/) (`docs/_config.yml` + `.github/workflows/pages.yml`)

Casper is set up the same way. Follow this once.

## 1. Create the GitHub repo

1. On GitHub: **New repository** → name e.g. `casper` (public or private for invite-only testers).
2. Do **not** initialize with a README if you will push `E:\casper` as the first commit.
3. Locally (example):

```bat
cd /d E:\casper
git remote remove origin 2>nul
git remote add origin https://github.com/TweakerInc/casper.git
```

4. Search/replace in the repo:
   - `TweakerInc/casper` → your real `user/repo`
   - Files: `README.md`, `docs/index.md`, `docs/installation.md`

5. Push:

```bat
git checkout -B main
git push -u origin main
```

(Or push `casper/reference` and set it as default branch.)

## 2. What not to push

Already gitignored where possible:

- `.pio/`
- `scripts/data/` (huge dictionary sources)
- `docs/*.cxdict` large packs (keep sample only)
- `platformio.local.ini`, Wi‑Fi passwords, KOReader secrets

Remove `docs/CNAME` unless you own that domain (CrossInk’s CNAME must not ship as Casper).

## 3. Enable GitHub Pages

1. Repo **Settings → Pages**
2. Source: **GitHub Actions**
3. Push to `main` touching `docs/**` or run workflow **Deploy Pages** manually
4. Site URL will look like: `https://TweakerInc.github.io/casper/`

Optional custom domain: set `url` / `CNAME` later (see Just the Docs docs).

## 4. First Release (what testers need)

1. Build cleanly:

```bat
pio run -e default
```

2. Copy artifact:

```bat
copy .pio\build\default\firmware-default.bin .
```

3. GitHub → **Releases → Draft a new release**
   - Tag: `v0.1.0` (or `casper-0.1.0`)
   - Title: `Casper v0.1.0`
   - Attach: `firmware-default.bin`
   - Notes: short list of Casper features + “based on CrossPoint” + “flash Custom .bin” + “how to revert”

4. Send testers: **Release link** + [Installation](./installation.md) (or the Pages install page).

## 5. Repo About box (match CrossInk polish)

On the GitHub repo home:

- **Description:** e.g. `Firmware for Xteink X3/X4. Personal CrossPoint-based build (Casper).`
- **Website:** your Pages URL after it deploys
- **Topics:** `firmware` `esp32` `epub` `e-ink` `ereader` `xteink`

## 6. Optional later

- Screenshots in `docs/images/` (Dashboard, dictionary, status bar) linked from README like CrossInk’s photo table
- `tiny` / `xlarge` release assets
- CI build-on-tag (CrossInk has `release.yml`; can port later)

## Checklist before sharing

- [ ] `TweakerInc` replaced everywhere
- [ ] No CrossInk product branding in UI strings you care about
- [ ] LICENSE present; CrossPoint credited in README
- [ ] At least one Release with a `.bin`
- [ ] Pages builds green
- [ ] Install path tested once on a real device
