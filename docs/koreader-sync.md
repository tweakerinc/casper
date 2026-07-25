---
title: KOReader Sync
nav_order: 6
description: Set up KOReader progress sync (Sign Up, servers, Smart Sync, auto-upload)
---

# KOReader Sync

Casper follows **CrossPoint 1.5** KOReader-compatible progress sync, plus Casper
**auto-upload on book close** (Time / Percent / Adaptive).

## Quick setup (CrossPoint Sync Server — default)

When **Sync Server URL** is left empty, the device uses  
`https://sync.crosspointreader.com` (standard KOReader protocol + exact position for CrossPoint↔CrossPoint).

1. **Settings → System → KOReader Sync**
2. Set **Username** and **Password** (plain password; firmware MD5s it for the API). Use the same values on every device.
3. Leave **Sync Server URL** empty (or set `https://sync.crosspointreader.com`).
4. On the **first** device: run **Sign Up** once to create the account.
5. On **other** devices: enter the same credentials and run **Authenticate** only.

Accounts are **per server**. Credentials from `sync.koreader.rocks` do **not** exist on the CrossPoint server — either **Sign Up** again on the CrossPoint server or use Option B below.

## Option B: Legacy public KOReader server

1. Set **Sync Server URL** to `https://sync.koreader.rocks` (required; empty URL is CrossPoint now).
2. Enter existing KOReader Sync username/password.
3. **Authenticate** (or **Sign Up** if you need a new account).

## Sync Behavior (manual Sync Progress)

| Setting | What it does |
|---------|----------------|
| **Ask Every Time** | After fetch, show **Apply Remote** vs **Upload Local** so you choose. |
| **Smart Sync** (default for new configs) | Auto-resolves simple cases: upload if no remote / local further; apply remote if remote further; “already synced” if equal. |

**Important:** Sync Behavior only applies to **manual** sync (reader menu **Sync Progress**, or long-press Menu = KOSync). It does **not** pause auto-upload to ask you.

To get the choose-each-time UI:

1. Open **KOReader Sync** settings.
2. Set **Sync Behavior** to **Ask Every Time** (always listed; cycle with Select).
3. Open a book → **Confirm** → **Sync Progress** (or long-press Confirm if set to KOSync).

## Auto Upload on Close (Casper)

Optional. When **Yes**, leaving the book for Home can push progress without opening the compare UI.

| Upload Type | Gate |
|-------------|------|
| **Time** | Upload if enough minutes since last auto-upload *for this book* (0 = Always). |
| **Percent** | Upload if progress advanced by at least the threshold since last upload *for this book* (0 = Always). |
| **Adaptive** | Upload on every leave (no time/percent gate). |

Skip reasons (time window / percent not met / no login) show a short toast.

## Syncing while reading

1. Finish setup (Sign Up or Authenticate).
2. Reader menu → **Sync Progress**, or bind **Long-press Menu** to **KOSync**.
3. With **Ask Every Time**, pick Apply or Upload. With **Smart Sync**, the device decides.

## Troubleshooting

- **Never asked to choose:** Check **Sync Behavior** is **Ask Every Time**, not Smart Sync. Also disable **Auto Upload on Close** if you only want interactive sync.
- **Auth failed on CrossPoint server:** Run **Sign Up** once if the account was only created on `koreader.rocks`.
- **Wrong book position across devices:** Prefer matching **Document Matching** (Filename vs Binary) on all devices; use the same EPUB file identity where possible.
