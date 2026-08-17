# Casper · Data Migration (v0.1.9)

Target layout (current firmware):

```text
/.crosspoint/book_<pathId>/
  package/          # book.bin, thumbs (was epub_<std::hash>)
  rivulet/
  progress.bin
  stats_*.bin
  meta.txt
```

## Shared contract (firmware + host tools)

Defined in `lib/FsHelpers/BookPathId.h` / `.cpp`:

1. **normalizePath** — strip drive letters (`H:`), `\` → `/`, leading `/`, collapse `.` / `..`
2. **id** — FNV-1a 64 of the UTF-8 normalized path → 16 lowercase hex (`book_<id>`)
3. **Marker** — `/.crosspoint/crosspoint_migrate_v2.done`

Legacy packages used `epub_<std::hash>` (ESP32 / libstdc++ Murmur32 seed `0xC70F6907`).

## Host backup migrate (recommended for offline SD copies)

```bash
python tools/sd-migrate/migrate_backup_v019.py "E:\path\to\SD root"
```

Merges `.casper` → `.crosspoint`, rekeys `epub_*` → `book_<id>`, rewrites recent covers, writes the v2 marker, then **deletes `.casper`**.

## Browser tool (copy-only)

1. Chrome or Edge  
2. Open `index.html`  
3. **Choose SD root…** → **Scan** → **Start migration**  

Note: the HTML tool’s README historically targeted `.casper`; current product root is **`.crosspoint`**. Prefer `migrate_backup_v019.py` for full rekey + cleanup.

## Files

| File | Role |
|------|------|
| `migrate_backup_v019.py` | Full host migrate for v0.1.9 layout |
| `index.html` / `migrate.js` | Browser copy-only tool |
| `casper-ghost.png` | Logo |
