# Casper (worktree note)

Daily firmware work may live in this folder (`CrossInk` is only a legacy path name).

**Product name is Casper.** User-facing strings, boot logo, web chrome, and docs should not say CrossInk.

## Keep `E:\casper` in sync

After any product change:

1. Build/verify in this worktree if needed.
2. Sync to the reference tree:

```bat
robocopy C:\Users\m\CrossInk E:\casper /E /XD .git .pio .claude /XF tmp_title_chunk.txt /NFL /NDL /NJH /NJS
```

3. On `E:\casper`, commit on `casper/reference` when the change should be part of the next CrossPoint rebase.

See `docs/CASPER_MERGE.md` on `E:\casper` for the full merge procedure when a new CrossPoint release lands.

Must-keep features: dashboard, dictionary, KOReader sync, Casper branding, defaults (short power = sleep, long-press menu = dictionary, side long-press = off), reader battery top-right, dictionary multi-word after a deliberate long-press Select.
