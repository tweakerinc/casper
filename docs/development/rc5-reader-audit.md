# RC5 reader integration work — NOT a released candidate

Baseline firmware: bdf774db35df5c141b6d9059f74134f8b130660d (RC4).
Work is isolated on `fix/rc5-reader-navigation`. RC4's candidate branch, main,
release tags and the owner's installed firmware are not changed by this work.
The owner is still testing RC4; do not label a development compile as RC5.

## Reproduced and repaired

1. **False destination readiness.** A background worker deliberately released its
   paint page. Selecting that same chapter's first page attempted to recreate it;
   if allocation failed, the coordinator's `firstPrepared_` shortcut still
   returned NavigationReady before checking the failure. The new regression
   reproduced RC4's readyEngine!=null with zero spans, then passed with the fix.
   Readiness now requires an actual owned paint page, at the coordinator API and
   at the reader's final commit. Allocation failures remain explicit failures.
2. **Chapter picker bypassed the new engine transaction.** The reader released
   its active engine before displaying the picker, then loadTocChapter chose the
   old blocking loadSpine path because no active engine remained. A failed choice
   had no live page to restore. The picker now keeps the active page and releases
   only speculative work; explicit selection always uses the cooperative target
   request, including recovery from an initial saved-position failure.
3. **A bad saved position locked out a readable book.** The error screen only
   returned Home. Select now opens the existing chapter picker. An explicit choice
   clears the stale resume intent, not the stored user anchor. Saved progress is
   updated only when a new destination commits. Back still leaves. This does not
   invent a fallback position or discard ambiguous old anchors.
4. **Error messages could be wider than the panel.** BaseTheme::drawPopup sized
   itself from the whole unwrapped string. It now wraps with fixed storage,
   respects UTF-8 boundaries and clamps its frame to screen bounds. Short text
   retains the same styling. The render path checks layout before clearing the
   framebuffer and no longer labels a failed layout as an Empty page.
5. **Secondary state-machine/cache hazards.** Invalid requests cannot reuse an
   older pending destination; explicit TOC requests do not silently skip an
   empty source; a completed map with an impossible target terminates rather
   than staying Working. Map expansion reports actual advancement. Paint-cache
   loads commit the page only after map updates succeed. Deferred behind-page
   preparation no longer synchronously writes that optional paint cache.
6. **Chapter names.** Integrates the previously prepared navigation-source fix:
   choose a verified richer publisher NCX, preserving original NAV destinations,
   labels and hierarchy. Do not invent Section rows when a valid TOC exists.
7. **Repeated Library/Recents refresh overhead.** Preserve the opening cleanup,
   but use the existing ordinary menu refresh on subsequent list moves. On X3
   light UI this avoids an extra soft-reinforcement pulse on every arrow press.
   No waveform constants, menu geometry, reader fonts or statistics policy changed.

## Evidence and limits

The focused real-source suite passes 4,000 assertions, including an allocation-
failure sweep through background-to-foreground promotion. Actual HAL lifecycle
and misuse checks pass. The publisher navigation parser suite passes against
synthetic data and the privately supplied book.

A new production-component harness joins ChapterLoader, ReadinessCoordinator,
RivuletEngine and the actual storage adapter. Unlike the old first-page check,
it walks EVERY page forward and backward, verifies the last page through the
coordinator, saves/reloads its position, and injects transfer/cancellation failures.
The supplied 126-section EPUB passes 547 host-layout pages and 19,627 checks.
These page counts use mocked font metrics: they are not the device's page total.
The existing private cover reference and paired-output checks still pass.

The UI policy test compiles the actual refresh-policy header and popup wrapper
against panel/font mocks; it verifies opening vs repeat pulse counts, polarity/
hard-scrub handling and UTF-8 message bounds. It does not measure physical ghosting.
The full ActivityManager/input/render task choreography is not emulated by the
component tests. A complete production compile is separately required; inspect
CI results rather than inferring build success from this note.

No new RC4 logs or screen photo accompanied the latest clipped-placeholder report.
The attached detailed logs currently inspected are older RC2/RC3 sessions. The
new bugs above are proven in source/tests; the exact latest on-device failure
cannot yet be attributed to one of them. Private EPUB/prose/images/fonts stay
local and are not committed or included in the source distribution.

## Remaining release blockers / work, not claimed fixed

- Fragment destinations (`file.xhtml#id`) are logged but still ignored by some
  reader entry points. The TOC parser preserving anchors is NOT equivalent to
  the page engine resolving them. Books with multiple chapters in one XHTML
  need a persisted source-anchor index and tests through the activity layer.
- Font/UI restoration and several bookmark/link/reflow paths still call the
  synchronous legacy loadSpine path with very large pagination budgets. Those
  must converge on the same transaction before claiming all navigation robust.
- A parser upgrade that changes normalized mid-chapter offsets can still reject
  an old anchor; exact recovery or an explicit user choice is needed, not a
  silent page-zero fallback. This patch supplies the choice, not universal remap.
- Cold source extraction/conversion/publication and optional scans still contain
  long operations. Full first-page streaming and bounded input-latency scheduling
  are unfinished. Keeping the watchdog alive does not make an operation responsive.
- Settings and chapter-menu input still has release-based navigation; repeated
  screen drawing, SD metadata reads and input-to-panel latency need profiling.
  The Library/Recents refresh repair is not the complete menu responsiveness work.
- Wake acknowledgement is still pending. This work does not change sleep visuals.
- Hardware RAM fragmentation, actual SD latency, font fallback, final glyph/image
  placement, X3/X4 panels, battery use and prolonged sessions require device tests.

Do not wipe the SD card or erase reading history as a diagnostic shortcut. Do not
publish RC5 as ready merely because this development branch compiles.
