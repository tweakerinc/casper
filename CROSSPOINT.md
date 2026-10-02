# CrossPoint (on CrossPoint Reader 1.5.0)

**Location:** `C:\Users\m\Documents\CrossPoint`  
**Base:** [crosspoint-reader `release/1.5.0`](https://github.com/crosspoint-reader/crosspoint-reader/tree/release/1.5.0)  
**Branch:** `casper/ui-on-1.5`  
**Photo tour:** [docs/casper.md](./docs/casper.md)

## Goal

Stable **CrossPoint 1.5 engine**, with a **CrossPoint UI overlay**:
- Product branding (logo, “CrossPoint” strings, web portal titles, User-Agent)
- Factory look-and-feel defaults
- **Dashboard / Minimal home themes** + reading stats (ported onto 1.5 APIs)
- CrossInk-style home interaction, reader chrome, and dictionary presentation

| In scope | Out of scope (intentionally) |
|----------|------------------------------|
| Boot logo + “CrossPoint” wordmark | Custom dictionary stack / multi-word selection from old fork |
| Product name strings, web titles, User-Agent | Battery gauge poll changes / HalPowerManager rewrites |
| Serial / settings version labels as CrossPoint | CrossInk KOReader Auto Upload Options rewrites |
| Default settings that affect look/feel | Side-button release-only / hybrid page-turn experiments |
| Light sleep wallpaper default; clean default sleep screen | Full CrossInk HomeActivity rewrite / carousel artwork |
| Dashboard + Minimal themes + book/global reading stats | CXDict / `lib/Dictionary` experimental stack |
| Minimal/Dashboard home button map (Menu/Browse/Settings/Read) | Full CrossInk dark-mode / clipping stack |
| Reader chrome redesign + Book Stats screen | Sleep-screen stats modes (Dashboard/Minimal cover+stats sleep) unless already trivial |

## Strategy: 1.5 bones + UI overlay

CrossPoint does **not** replace the CrossPoint 1.5 reader pipeline. It:

1. Keeps stock EPUB/TXT/XTC, dictionary folder (`/dictionaries` StarDict), KOReader sync, and HAL as shipped in 1.5.
2. Extends theme APIs only where needed (`drawRecentBookCover` optional stats args; `drawStatusBar` optional time-left).
3. Adds thin **Minimal** / **Dashboard** themes and stats persistence used by the home screen.
4. Opens books with **Rivulet** and updates recents/progress so Dashboard counters stay current.

CrossInk is a **reference design**, not a tree to robocopy.

## Critical API differences (1.5 vs CrossInk)

| Area | CrossPoint 1.5 | CrossInk / CrossPoint overlay |
|------|----------------|---------------------------|
| `drawRecentBookCover` | base signature without stats | extended with defaulted `stats`, `progressPercent`, `globalStats`, `chapterTitle` |
| MinimalTheme | n/a | metrics + `drawButtonHints` selection highlight |
| Cover thumbs | height-keyed `getThumbBmpPath(height)` | Dashboard uses height-only paths |
| Fonts | Noto + UI 10/12 | Lexend aliases map to nearest UI/Noto IDs |
| i18n | no `STR_STATS_*` / home short labels | keys added in `english.yaml` (other langs fall back) |
| `HalStorage` | `HalFile` only | stats code uses `HalFile` (not raw `FsFile`) |
| Dictionary | StarDict via `util/Dictionary.cpp` | same backend; popup-style definition UI only |

## Themes

`UI_THEME` (append-only):

| Value | Name |
|------:|------|
| 0 | Classic |
| 1 | Lyra |
| 2 | Lyra Extended (3 covers) |
| 3 | RoundedRaff |
| 4 | Minimal |
| 5 | **Dashboard** (default) |
| 6 | Lyra Carousel (metrics-only stub; full carousel art deferred) |

### One-time home migration (`casperHomeMigrated`)

If SD `settings.json` lacks `casperHomeMigrated` (or it is 0), load forces:

- `uiTheme = Dashboard`
- thin book progress bar, battery + page% on, `statusBarTimeLeft = Book`
- then sets `casperHomeMigrated = 1` and resaves

**Users can still change the theme in Settings** after migration. Fresh defaults already use Dashboard.

### Dashboard / Minimal home buttons

When theme is Minimal or Dashboard (full-bleed cover, no bottom menu list):

| Physical front | Action |
|----------------|--------|
| BACK | Open popup menu (Recent Books, OPDS if any, File Transfer); closes menu if open |
| CONFIRM | File Browser |
| LEFT | Settings |
| RIGHT | Continue Reading (if a recent book exists) |

Hints: **Menu / Browse / Settings / Read**. Side Up/Down cycles hint selection; Confirm activates the selected slot. Classic themes keep Resume/Select/Up/Down + bottom menu list.

## Reading stats

- Per-book: `cachePath/stats_v5.bin` via `BookReadingStats`
- Global: `/.crosspoint/global_stats.bin` via `GlobalReadingStats`
- Home loads first recent book’s stats + simple EPUB progress % for Dashboard
- Full **Book Stats** screen (`BookStatsActivity` / `BookStatsView`): per-book + this-device + all-devices (if synced), date edit when RTC present. Lifetime card is **2×3** (no Pages Turned / Days Read), matching Dashboard.
- Open via long-press menu **Reading Stats**.

### Cache paths (no auto-migration)

Stats live only under this firmware’s cache path:

```text
/.crosspoint/epub_<std::hash(filepath)>/stats_v5.bin
```

There is **no** automatic copy from older CrossInk FNV-64 folders (that was removed — rare, and not worth running on every open). Start dates and counters for a book are whatever is in the current path; re-read to rebuild, or copy files manually if you know the old hash folder.

- **Rename note:** moving/renaming a book changes the path hash → a new empty cache folder.
- Reader onEnter/onExit updates sessions + total reading seconds (fail-soft)
- Forward page turns with 2s–10min dwell update pace / `totalPagesTurned` (for **pages/min** and time-left)

### Sleep-screen stats (docs only — not implemented)

CrossInk can show book cover + reading stats on the sleep screen (Dashboard/Minimal stats sleep modes) instead of logo/blank. CrossPoint does **not** implement those modes unless they become trivial later.

### Dashboard layout (CrossPoint tweak)

**Book column:**

- X3 (RTC): Time, Time Left, Progress, Daily Avg, Pages/Min, Started, Finish  
- X4 (no RTC): Time, Time Left, Progress, Pages/Min, Sessions, Avg Session, Pages  

**Under-cover meta:**

- X3: Day streak + morning/afternoon/evening/night reader type  
- X4: Book sessions + progress % (no calendar streaks)

**Lifetime card — even 2×3 grid:**

| Sessions | Reading Time | Pages/Min |
| Avg Session | Books Read | X3: Streak / X4: Pages Turned |

## Reader chrome (CrossPoint redesign)

All top-row text uses **SMALL_FONT_ID** (same as battery %).

| Position | Content |
|----------|---------|
| Top-left | `"64% Complete"` when book progress % enabled |
| Top-center | Clock (X3 RTC), SMALL_FONT_ID |
| Top-right | Battery icon + % |
| Bottom-left | Time-left label (`"12m in Book"` / `"Learning Pace"`) |
| Bottom-center | Chapter/book title when enabled |
| Bottom-right | `"Pg. 1/40"` (chapter page count; not bare `1/40`) |
| Bottom edge | Thin progress bar (default Book + Thin; `fillMargin` into bezel) |

Status Bar settings **preview** uses the same `drawStatusBar` layout.

Content margins permanently reserve ~24px top chrome + status-bar bottom pad so text clears top icons and bottom chrome (including dictionary button hints zone).

`statusBarTimeLeft` is editable under Customise Status Bar (Hide / Book / Chapter).

## Power button (short vs long)

| Setting | Default | Notes |
|---------|---------|--------|
| Short power | Sleep | Wake verification stays short (~10 ms) when short=Sleep |
| Long power | Force refresh | Hold ≥ `getPowerButtonLongPressDuration()` (~500 ms) |
| Long-press menu (Confirm) | Dictionary | See long-press list below |

`casperControlsMigrated` one-shot forces short=Sleep, long=ForceRefresh, long-press menu=Dictionary.

Global long hold while pressed triggers sleep/refresh immediately (CrossInk-style). Release dispatches short vs long action. Page-turn and footnotes honor short vs long independently of global sleep/refresh.

## Long-press Confirm menu (append-only)

| Index | Action |
|------:|--------|
| 0 | KOSync |
| 1 | Disabled |
| 2 | Bookmark |
| 3 | Dictionary |
| 4 | Sleep |
| 5 | Force refresh |
| 6 | File browser |
| 7 | Screenshot |
| 8 | Footnotes |
| 9 | File transfer |
| 10 | Reading stats |

## Dictionary presentation

### StarDict folder structure (CrossPoint / CrossPoint 1.5)

**One folder per pack** under `/dictionaries/` or `/.dictionaries/`:

```text
/dictionaries/<FolderName>/<stem>.idx
/dictionaries/<FolderName>/<stem>.dict   # or .dict.dz
/dictionaries/<FolderName>/<stem>.ifo    # optional
/dictionaries/<FolderName>/<stem>.qidx   # device-built sample index
```

- Folder name = Settings label; exactly one `.idx` stem per folder.  
- Release zip: `English/`, `English-Spanish/`, `Spanish-English/` (see `dist/dictionaries/README.txt`, `docs/dictionary.md`).  
- **CXDict is not used** — backend is `util/Dictionary.cpp` + StarDict only.

### UI

- Multi-select packs; auto-enable all on first open if none selected  
- Mode titles: **Dictionary Lookup** / **Multi-Word Selection**  
- Definition card over reader snapshot; bold headword + regular pronunciation beside; centered pack names  
- Lookup: stems, Spanish clitics, multi-word windows + per-token fallback

## What CrossPoint 1.5 already provides

- Stock EPUB/TXT/XTC reader pipeline  
- Stock dictionary (folder under `/dictionaries`) — **not** experimental CXDict UI  
- Stock KOReader sync as shipped in 1.5  
- Built-in fonts: **Noto Serif / Noto Sans** + UI 10/12  

## OTA updates (GitHub Releases)

CrossPoint pulls firmware from **this fork only** — never CrossPoint stock releases.

| Item | Value |
|------|--------|
| API endpoint | `https://api.github.com/repos/TweakerInc/casper/releases/latest` |
| Build flag | `-DCROSSPOINT_OTA_RELEASE_URL=...` in `platformio.ini` `[base]` |
| Device menu | Settings → System → **Check for updates** |
| Asset name | **`CrossPoint-v0.1.0`** or **`CrossPoint-v0.1.0.bin`** (also accepts `firmware.bin`) |
| Version compare | Semver tags `v0.1.0`, `v0.1.1`, … vs `CROSSPOINT_VERSION` |

### Publishing a release

GitHub’s “Source code” zip is **always** the **git tag’s commit**. Uploading only a `.bin` does **not** update source. Ship source and binary from the **same** commit.

1. Bump `[casper] version` in `platformio.ini` (e.g. `v0.2.0`) — must match the tag.
2. Commit everything that belongs in that firmware (not serial logs / temp dirs).
3. Build a clean release binary (not the default env’s branch+SHA suffix):
   ```bash
   pio run -e gh_release
   ```
   Post-script copies to `dist/CrossPoint-<version>.bin` (e.g. `dist/CrossPoint-v0.1.8.bin`).
4. **Push source before or when creating the release** (remote `casper` = `tweakerinc/casper`):
   ```bash
   git push casper HEAD:main
   git tag -a v0.1.8 -m "CrossPoint v0.1.8"
   git push casper v0.1.8
   ```
   If the tag already exists on an old commit and you are re-shipping that version:
   ```bash
   git tag -f -a v0.1.8 -m "CrossPoint v0.1.8"
   git push casper v0.1.8 --force
   ```
5. Create/edit the GitHub release on **TweakerInc/casper** with tag **exactly** matching the version (`v0.1.8`). Prefer “Choose a tag” → the tag you just pushed (not a floating “main” without a tag).
6. Attach `dist/CrossPoint-v0.1.8.bin` (or `CrossPoint-v0.1.8` without extension if you prefer).
7. On device: **Settings → System → Check for updates** → Wi‑Fi → confirm install.
   Device reboots into the new partition on success.

**Checklist:** version string in firmware == git tag == release tag == bin filename version.

### Testing notes

- Device must already run a build that includes this OTA client (flash once via USB/SD).
- Latest release tag must be **strictly newer** than the on-device version.
- Dev builds (`default` env) show `v0.1.0-<branch>-<sha>`; a clean `v0.1.0` release is treated as the same base version (use `v0.1.1+` to upgrade those units).
- Fallback: Settings → System → **SD firmware update** if Wi‑Fi OTA fails.
- Repo must be **public** (or the API needs auth, which this client does not send).

## Fonts (built-in)

- **Literata** — default reader body  
- **Source Serif 4** — UI chrome + alternate reader  
- Bitter / Lexend are **not** shipping built-ins (Bitter removed; Lexend not product). SD packs still work for extra faces.

## Factory defaults (CrossPoint)

| Setting | Value |
|---------|--------|
| UI theme | Dashboard (one-time `casperHomeMigrated`) |
| Sleep wallpaper | Light |
| Short power | Sleep |
| Long power | Force refresh (`casperControlsMigrated`) |
| Long-press menu | Dictionary |
| Font size | Small (12 pt Noto) |
| Status bar | Book progress %, chapter pages, thin book bar, battery, time-left Book |

## Dashboard home controls (Minimal / Dashboard)

| Front button | Action |
|--------------|--------|
| Back | Menu popup (Recent Books, OPDS, File Transfer) |
| Confirm | Browse files |
| Left | Settings |
| Right | Read (continue current book) |

## Dashboard covers

Home covers use **cover-fill** (scale = max of width/height ratios, centered overflow, rounded mask). Thumbs generated at `DashboardMetrics::homeCoverImageWidth × homeCoverImageHeight`.

## Not ported / remaining gaps

- Full **Lyra Carousel** artwork and selection chrome (stub uses Lyra drawing + carousel metrics)
- Multi-book home swap / carousel SD frame cache from CrossInk
- Full CrossInk time-left estimator (session pace floors, progress floor); CrossPoint uses simplified pace × remaining pages
- Extra built-in families beyond Literata + Source Serif (flash budget)
- CXDict / multi-word selection stack
- Sleep-screen stats modes (cover + stats on sleep)
- Dark mode / clipping stack

## Build

```bat
cd /d C:\Users\m\Documents\CrossPoint
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -e default
```

I18n after editing YAML:

```bat
python scripts/gen_i18n.py lib/I18n/translations lib/I18n/
```

(also runs automatically as a PlatformIO pre-step if configured)

## Relation to other trees

| Path | Role |
|------|------|
| `C:\Users\m\Documents\CrossPoint` | **CrossPoint** = CrossPoint 1.5 + branding + Dashboard UI overlay |
| `E:\casper` | Experimental fork — reference only |
| `C:\Users\m\CrossInk` | Daily experimental worktree / design reference |

Do **not** blindly robocopy experimental trees onto this project.
