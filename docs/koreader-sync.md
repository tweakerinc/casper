---
title: KOReader Sync
nav_order: 6
description: Set up KOReader progress sync — Sync Behavior, Smart Sync, auto-upload
---

# KOReader Sync

Casper can keep reading progress in sync with other devices through a
**KOReader-compatible** sync server (CrossPoint’s server, the public KOReader
server, or your own).

Open **Settings → System → KOReader Sync**.

---

## Settings menu (top to bottom)

| Setting | Purpose |
|---------|---------|
| **Username** / **Password** | Account on the sync server (same on every device). |
| **Sign Up** | Create the account on that server (first device only). |
| **Authenticate** | Test login with existing credentials. |
| **Sync Server URL** | Empty = `https://sync.crosspointreader.com`. Or set another URL (e.g. `https://sync.koreader.rocks`). |
| **Document Matching** | How the server identifies the book: **Filename** (easier across copies) or **Binary** (hash of the file; stricter). |
| **Sync Behavior** | Opens a popup with **four exclusive modes** (pick one). See below. |
| **Percent** or **Time** | Only shown when Sync Behavior is **Percent** or **Time** — the auto-upload threshold/interval. |
| **Upload Metadata** | When On, document title/author metadata is sent with progress (server-dependent). |

---

## Sync Behavior (pick one)

These four options are **mutually exclusive**. Auto-upload and Smart Sync do **not** run side by side.

| Mode | Kind | What happens |
|------|------|----------------|
| **Ask Every Time** | Manual | Reader menu **Sync Progress** fetches remote progress and asks **Apply Remote** vs **Upload Local** when it matters. Leaving a book does **not** auto-upload. |
| **Smart Sync** | Manual | Same as CrossPoint 1.5 Smart Sync: manual **Sync Progress** auto-resolves simple cases (see below). Leaving a book does **not** auto-upload. |
| **Percent** | Auto-upload | Leaving a book may upload local progress if you’ve advanced enough *for this book* since the last upload. No Smart Sync chooser on leave. |
| **Time** | Auto-upload | Leaving a book may upload local progress if enough time has passed *for this book* since the last upload. No Smart Sync chooser on leave. |

### Ask Every Time

After connecting and fetching remote progress, Casper shows a choice when needed:

- **Apply Remote** — jump to the position stored on the server  
- **Upload Local** — push this device’s position to the server  

Use this when you want full control.

### Smart Sync (CrossPoint 1.5 behavior)

Smart Sync is the CrossPoint 1.5 “quiet multi-device” mode for **manual** Sync Progress only. It does **not** auto-upload when you leave a book.

When you run **Sync Progress** (or long-press Menu if set to KOSync), Casper:

1. Connects to Wi‑Fi and fetches remote progress for this book.  
2. In smart mode, may also probe the **alternate document-matching** method (Filename vs Binary) so progress stored under the other ID is less likely to be missed.  
3. Compares local vs remote percentage and **auto-resolves**:

| Situation | Smart Sync action |
|-----------|-------------------|
| No progress on the server yet | **Upload** local |
| Local and remote are effectively the same (~0.1 pp) | Report **already synced** (no change) |
| Local is further ahead | **Upload** local |
| Remote is further ahead | **Apply** remote (jump to server position) |

You don’t get an Apply/Upload chooser in these simple cases. Use **Ask Every Time** if you always want to choose.

### Percent (auto-upload)

- Sync Behavior = **Percent** enables auto-upload on leave.  
- A **Percent** row appears (default **1%**). **Always** means no percent gate (upload every leave).  
- On leave: if progress gain since last successful upload for this book is below the threshold, show a short toast (e.g. `Sync skipped (need +1%)`) and go home without uploading.  
- Manual Sync Progress still works; conflict UI follows normal fetch flow (not the Smart Sync auto-resolve path unless you switch mode).

### Time (auto-upload)

- Sync Behavior = **Time** enables auto-upload on leave.  
- A **Time** row appears (default **1 hour**). **Always** means no time gate.  
- On leave: if not enough time has passed since the last upload for this book, toast (e.g. `Sync skipped (within 1 h)`) and skip.  

---

## Quick setup (CrossPoint server — default)

1. **Settings → System → KOReader Sync**  
2. Enter **Username** and **Password** (same on all devices).  
3. Leave **Sync Server URL** empty → `https://sync.crosspointreader.com`.  
4. **First device:** **Sign Up** once. **Other devices:** **Authenticate** only.  
5. Open **Sync Behavior** and pick one:  
   - **Smart Sync** or **Ask Every Time** for manual sync from the reader menu  
   - **Time** or **Percent** if you want progress pushed automatically when leaving a book  

Accounts are **per server**. Credentials from `sync.koreader.rocks` do **not** exist on the CrossPoint server unless you Sign Up there too.

## Option B: Legacy public KOReader server

1. Set **Sync Server URL** to `https://sync.koreader.rocks`.  
2. Enter username/password → **Authenticate** (or **Sign Up** for a new account).

## Syncing while reading

1. Finish Sign Up / Authenticate.  
2. Reader menu → **Sync Progress**, or set **Long-press Menu** to **KOSync**.  
3. With **Ask Every Time**, pick Apply or Upload when prompted.  
   With **Smart Sync**, Casper decides and returns you to the book.  
4. With **Time** / **Percent**, leaving the book may upload when the gate allows (toasts explain skips).

## Troubleshooting

- **Never asked to choose:** Set **Sync Behavior** to **Ask Every Time**, then use manual **Sync Progress**.  
- **Expected auto-upload but nothing happens:** Sync Behavior must be **Time** or **Percent** (not Smart Sync / Ask). Check skip toasts and credentials.  
- **Auth failed on CrossPoint server:** Run **Sign Up** if the account only exists on `koreader.rocks`.  
- **Wrong book across devices:** Match **Document Matching** (Filename vs Binary) everywhere.  
