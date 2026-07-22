# Merging Casper onto a new CrossPoint base

Use this when CrossPoint (or CrossInk) publishes a new firmware and you want a **fresh base** plus **Casper-only features**.

## Goals

Keep:

1. **Casper branding** (name, logo, web titles, serial strings)
2. **Dashboard** theme and sleep option
3. **Dictionary** (lib + selection UI + shortcuts)
4. **KOReader sync** Casper behavior/settings
5. **Defaults**: short power = sleep, long-press menu = dictionary, side long-press = off
6. **UI tweaks**: side long-press ignore safety, reader battery top-right, multi-word dictionary select

Do **not** blindly copy the whole tree over a new release — that reintroduces stale bugs and loses upstream fixes.

## Recommended layout

```
E:\casper\                 # this repo — Casper reference (feature source)
E:\casper-upstream\        # clean checkout of NEW CrossPoint/CrossInk tag
E:\casper-next\            # merge workspace (upstream + Casper patches)
C:\Users\m\CrossInk\       # optional daily driver
```

## Step 1 — Snapshot Casper reference

On the machine that already has a known-good Casper build:

```bat
cd /d E:\casper
git status
git log -1 --oneline
git tag casper-pre-merge-YYYY-MM-DD
```

Note the commit hash. This is the “features we want” snapshot.

## Step 2 — Fetch the new base

```bat
cd /d E:\
git clone --recurse-submodules https://github.com/crosspoint-reader/crosspoint-reader.git casper-upstream
cd casper-upstream
git fetch --tags
git checkout <new-release-tag>
```

If you track CrossInk instead:

```bat
git clone --recurse-submodules https://github.com/uxjulia/CrossInk.git casper-upstream
git checkout <tag-or-branch>
```

## Step 3 — Create merge workspace

```bat
git clone --recurse-submodules E:\casper-upstream E:\casper-next
cd /d E:\casper-next
git checkout -b casper/merge-<version>
```

Optional: add Casper reference as a remote for cherry-picks:

```bat
git remote add casper E:\casper
git fetch casper
```

## Step 4 — Inventory Casper deltas

From `E:\casper`, list paths that differ from the **old** base you forked (not necessarily the new one):

```bat
cd /d E:\casper
git log --oneline upstream/main..HEAD
```

Or compare trees manually. High-signal paths:

### Branding
- `scripts/build_web.py`, `web/templates/base.html`, `web/assets/style.css`
- `src/images/Logo120.h`, `src/images/casper.png`
- `src/activities/boot_sleep/BootActivity.cpp`
- `src/main.cpp`, `lib/hal/HalSystem.cpp`
- `src/CrossPointSettings.cpp` (default device name)
- `scripts/git_branch.py`, `lib/AppVersion/AppVersion.h`
- `include/SimulatorDisplay.h`
- `lib/I18n/translations/english.yaml` (Casper strings)

### Defaults / controls
- `src/CrossPointSettings.h` — defaults for `shortPwrBtn`, `longPressMenuAction`, `sideButtonLongPress`
- `src/SettingsList.h` — controls menu order / power / side / dictionary actions
- `src/activities/reader/ReaderUtils.h` — page-turn release behavior
- `src/activities/reader/EpubReaderActivity.cpp` (+ XTC/TXT) — side long-press off guard
- `src/components/themes/BaseTheme.cpp` — reader battery top-right
- `src/components/UITheme.cpp` — status bar height without bottom battery

### Dashboard
- `src/components/themes/dashboard/*`
- `src/activities/home/HomeActivity.*`
- Theme enum / sleep mode wiring in settings + `UITheme`

### Dictionary
- `lib/Dictionary/**`
- `src/activities/reader/DictionarySelectionActivity.*`
- `src/activities/reader/DictionaryLookupActivity.*`
- Reader menu / power / long-press actions that open dictionary
- `docs/dictionary.md`, `scripts/build_en_dict.py`

### KOReader
- `lib/KOReaderSync/**`
- `src/activities/reader/KOReaderSyncActivity.*`
- `src/activities/settings/KOReaderSettingsActivity.*`
- Settings list entries / Adaptive naming / defaults

## Step 5 — Apply in layers

Apply and build after each layer so failures stay small.

### Layer A — Branding only
Copy or cherry-pick branding files. Build. Boot should say Casper.

### Layer B — Defaults
Port `CrossPointSettings.h` default members carefully (do not wipe new upstream settings fields). Prefer setting defaults only; keep new enums/fields from upstream.

### Layer C — Dictionary
Add `lib/Dictionary`, activities, menu hooks, i18n strings. Build. Test lookup offline with SD packs.

### Layer D — KOReader
Port Casper KOReader behavior onto upstream KOReader code if upstream already has KOReader. Prefer three-way merge over full-file replace.

### Layer E — Dashboard
Port theme + home integration. Resolve conflicts with upstream home themes.

### Layer F — Controls / reader polish
Side long-press, battery position, dictionary multi-word selection, status bar.

## Step 6 — Settings compatibility

- Prefer **JSON settings migration** paths already in `JsonSettingsIO` / `CrossPointSettings`
- If you add new defaults, existing devices keep saved values; document “reset settings” only if required
- Never drop new upstream settings keys when re-applying Casper defaults

## Step 7 — Verification checklist

Flash `default` build to hardware.

### Branding
- [ ] Boot logo / “Casper” title
- [ ] Serial log version line says Casper
- [ ] Web portal title says Casper

### Defaults (factory reset or new device)
- [ ] Short power → sleep
- [ ] Long-press menu → dictionary
- [ ] Side long-press action → Ignore (no multi-page on hold)

### Dashboard
- [ ] Theme selectable; home shows cover + stats as before

### Dictionary
- [ ] Long-press menu opens dictionary selection
- [ ] Hyphenated word (e.g. `fetid-smelling`) resolves
- [ ] Long-press Select → multi-word phrase → Done looks up
- [ ] Packs load from `/.crosspoint/dict/`

### KOReader
- [ ] Settings UI present; credentials save
- [ ] Sync run completes against your server
- [ ] Adaptive / furthest-ahead behavior intact if still desired

### Controls polish
- [ ] Rest hand on side button with long-press Ignore → no page jump
- [ ] Battery % top-right while reading
- [ ] Progress still bottom status bar

### Regression
- [ ] Open large EPUB, page turn, sleep/wake
- [ ] No new crash on home ↔ reader
- [ ] OTA / file transfer still works if you use them

## Step 8 — Promote

When `E:\casper-next` is good:

```bat
cd /d E:\casper-next
git tag casper-vX.Y-on-crosspoint-Z
```

Optionally replace or update `E:\casper`:

```bat
rem after backup
robocopy E:\casper-next E:\casper /E /XD .git .pio
cd /d E:\casper
git add -A
git commit -m "chore: Casper rebased onto CrossPoint Z"
```

Or reset `E:\casper` to track `casper-next` history if you prefer a single lineage.

## Diff helpers

```bat
rem feature files only (example)
git -C E:\casper diff --stat <old-base>..HEAD -- lib/Dictionary src/activities/reader/Dictionary* src/components/themes/dashboard

rem three-way file merge hint
git merge-file -p casper_file upstream_base_file upstream_new_file
```

## What not to port blindly

- Entire `CHANGELOG.md` history mess — rewrite Casper notes under Unreleased
- Generated i18n (`I18nStrings.cpp`) if your build regenerates from YAML
- `.pio/`, `compile_commands.json`, `platformio.local.ini`
- Huge `scripts/data/` dictionary dumps — rebuild packs instead
- Unrelated experiments still sitting only in `C:\Users\m\CrossInk`

## After merge

Update `CASPER.md` if defaults or feature list changed.  
Keep one green hardware flash log (version string + date) in release notes.
