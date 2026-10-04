# Reader integration validation — development only

This is not RC5 and is not approved as a new device-test release. The owner is
still testing RC4. No main merge, candidate-branch update, release or tag was made.

## Exact source and CI

- Branch: `fix/rc5-reader-navigation`.
- Functional integration: `0ebbe61951dbc2787a0584ec3a946eb8a942ee6f`.
- Additional transient-cancellation repair and current firmware source:
  `a46af3e4a257772e2707ff06f7d584107600ed00`.
- GitHub Actions run `37180282492`: source job `111371293549`, test job
  `111371328202` and production build job `111371328190` all succeeded.
- The workflow was submitted at `a94a6b63`; its source step verified/applied the
  reviewed patch and supplied `a46af3e4` to BOTH downstream checkouts. The downloaded
  build and test artifacts' commit.txt agree on that functional source.
- Build artifact `11294853968`: archive SHA-256
  `c0ee837eead91504eb686c4a6a170bb6c51ead8129d79028207fe141c2c2b440`.
- Test artifact `11294662228`: archive SHA-256
  `3f6029fbb85b87f47811d393d03883c4697b7125fabf8e48e1625c36ddfe52a8`.

The ESP32-C3 X3/X4 production application compiled successfully. The development
image is 5,785,472 bytes, SHA-256
`d3801bc66bd8c828812532060a1c23f99f74478a433fe36b3f244312a0170d42`.
Its seven image segments, chip identifier, XOR checksum, appended SHA-256 and
partition size passed independent read-only checks. The matching ELF hash also
matches the build record. These checks do not establish hardware bootability.

The version remains v0.2.0-rc4 with development build marker a46af3e. The artifact
is explicitly named reader-development.bin and contains a NOT-A-RELEASE notice.
Do not rename it RC5 or confuse it with the owner's installed bdf774d firmware.

## What actually passed

- Regular CMake suite: 399/399 tests.
- Actual-source Rivulet sanitizer suite: 4,017 assertions. It reproduces the
  background-to-foreground false-ready failure before the fix and rejects the
  invalid destination after it. One-shot input pulses no longer turn an aborted
  first-page or map-extension layout into a terminal navigation error.
- Actual storage-adapter lifecycle suite: 62 checks; cover retry policy: 20.
  Invalid-read assertions were not disabled.
- Publisher navigation suite: 48 synthetic CI checks. A separate local run with
  the supplied private EPUB passed 1,409 checks, retaining detailed NCX names and
  original NAV destinations rather than exposing generic internal-file sections.
- Production UI-policy and popup-wrapper tests passed. They check initial versus
  repeated refresh calls, polarity/scrub behavior and bounded UTF-8 message layout.
- Production-component suite passed in CI with public and synthetic fixtures.
- The same local component suite passed with the private EPUB: 126 source sections,
  every host-layout page forward/backward, coordinator-based exact endings and
  saved-anchor reopening; 19,627 navigation checks. The 547 host-layout pages use
  mocked font metrics and are NOT a physical-device page count.
- Existing private progressive-cover pixel and paired-output comparisons still
  pass. This repair does not claim a new cover-performance improvement.

The loader/engine/storage-adapter implementations are real source. Lower hardware,
filesystem/RTOS, ZIP transport, image geometry and font metrics are mocked as
specified by each harness. The complete ActivityManager/input/render-task lifecycle
is not emulated. Actual on-device timing, fonts, ghosting and long-session stability
remain to be tested. No private EPUB, prose, cover or font was committed.

## Scope and outstanding work

See rc5-reader-audit.md for the individual reproduced defects and repairs. The
main fixes are safe promotion of a genuinely prepared destination, keeping the
active page through chapter selection, explicit recovery after a bad saved
position, cancellation-result latching, bounded error text, publisher TOC
selection, and avoiding repeated list-opening reinforcement refreshes.

In-file fragment anchors are still ignored by some reader entrypoints. Several
font/restore/bookmark/link/reflow operations still take the synchronous legacy
loading path. Cold first-page streaming, complete scheduler/input-latency work,
full menu responsiveness and wake acknowledgement remain unfinished. These are
tracked blockers, not work to hide behind a passing build.

A new RC4 failure log and photo are still needed to attribute the user's latest
clipped placeholder to a specific path; previously uploaded RC2/RC3 logs do not
prove which path failed in that new report. Do not ask the owner to wipe caches
or reading history to make this investigation easier.
