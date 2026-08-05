# Restoring Casper v0.1.8 (pre-Rivulet)

## Git restore points

| Ref | Meaning |
|-----|---------|
| **tag `v0.1.8`** | Snapshot of working firmware before Rivulet engine work |
| **commit** on `main` | `release: snapshot Casper v0.1.8 restore point` |
| **branch** `feature/rivulet-layout-core` | Rivulet implementation (CSS v9 + SECTION v34 metrics) |

## Roll back engine work (keep v0.1.8 tree)

```powershell
git checkout main
git checkout v0.1.8
# or hard reset a broken branch:
# git reset --hard v0.1.8
```

Flash the known-good bin if you still have it:

- `dist/Casper-v0.1.8.bin` (pre-Rivulet)
- After Rivulet builds: `dist/Casper-v0.1.8-rivulet-pr1b.bin` (experimental)

## What Rivulet has landed so far

1. **PR1a** — CSS parse font-size / line-height / float / clear / font-variant, cache **v9**
2. **PR1b-min** — block-level sizeStep + lineHeightPx, layout measure + paint, SECTION **v34**

Opening books rebuilds CSS cache and section pages automatically when versions change.
