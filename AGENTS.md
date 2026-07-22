# Casper — Shared Agent Guide

This is the canonical repo instruction file for Casper firmware work.

Project: Open-source e-reader firmware for Xteink X3/X4 (ESP32-C3), built as a clean product layer on CrossPoint Reader. **Product branding is Casper only** — do not reintroduce CrossInk user-facing names or logos.

## Dual trees

- Daily work may live under `C:\Users\m\CrossInk` (legacy folder name).
- Reference / merge source of truth: `E:\casper`.
- Keep meaningful product changes synced to `E:\casper` so rebases onto new CrossPoint releases stay easy.
- See `docs/DEV_NOTES.md` and `docs/CASPER_MERGE.md` (on the Casper tree).

## Core Rules

- Role: Senior Embedded Systems Engineer for ESP-IDF / Arduino-ESP32 work.
- The ESP32-C3 has no PSRAM and about 380 KB usable RAM. Stability beats features.
- Cite file paths and line numbers before proposing non-trivial changes.
- Do not assume ESP-IDF or SDK API availability. Verify in SDK paths or the live code.
- Do not claim performance or memory wins without explaining the mechanism (heap churn, flash vs DRAM, stack size).
- Justify new heap allocations or explain why stack/static storage is not suitable.
- After proposing or making a fix, say how to verify it on hardware.

## Hardware Constraints

- MCU: ESP32-C3, single-core RISC-V at 160 MHz.
- Display: 800x480 e-ink.
- Single framebuffer only: `800 * 480 / 8 = 48000` bytes.
- Storage is SD via SdFat. On real hardware, only one reader can hold a file open at a time.

## Resource Rules

1. Keep local stack usage small. Anything meaningfully larger than 256 bytes should be justified.
2. Avoid repeated heap churn in loops. Allocate once in `onEnter()`, reuse, and free in `onExit()`.
3. Large constant tables should be `static const` so they live in flash, not DRAM.
4. Avoid `std::string` and Arduino `String` in hot paths. Prefer `string_view`, `char[]`, and `snprintf`.
5. All user-facing UI strings must use `tr(STR_*)`. Logs may be hardcoded.
6. Prefer `constexpr` for compile-time constants.
7. Reserve `std::vector` capacity before push loops.
8. Debounce persistent writes. Do not write progress on every page turn.
9. `new` is not nothrow on ESP32. With exceptions disabled, bare `new` calls `abort()` on allocation failure. Use `new (std::nothrow)` or `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`.
10. Prefer `makeUniqueNoThrow<T>()` / `makeUniqueNoThrow<T[]>()` for owned heap allocations.
11. Use raw `malloc` or `new (std::nothrow)` only when a C or SDK API takes ownership; comment ownership transfer.

## HAL And Platform Rules

- Use HAL classes, not SDK classes, in app code.
- File I/O uses `FsFile`, not Arduino `File`.
- Always close files explicitly.
- Use `MappedInputManager::Button::*` enums for button logic.

## C++ / Embedded Gotchas

- `string_view::data()` is not null-terminated. Do not pass it directly to C APIs.
- ISR handlers need `IRAM_ATTR`, and ISR-read data must be in DRAM, not flash-only storage.
- Never call `xSemaphoreTake()` from an ISR. Use ISR-safe give APIs.
- Do not cast unaligned `uint8_t*` data to wider pointer types. Use `memcpy`.
- No exceptions. No `abort()`. Log before returning failure.
- Avoid `std::function` in hot paths and library code; prefer function pointers or a small context/callback struct.
- Keep template use deliberate. Prefer explicit instantiation in a `.cpp` file for shared templates.

## Error Handling

- Prefer `LOG_ERR(...)` plus `return false` for recoverable failures.
- Prefer `LOG_ERR(...)` plus a known fallback when the app can continue safely.
- Use `assert(false)` only for truly impossible fatal states.
- Use `ESP.restart()` only for intentional recovery flows, such as completing OTA.
- Always log before returning failure from allocation, file, parse, network, or hardware paths.

## Activity Lifecycle

- Activities are heap-allocated and deleted on exit.
- Allocate long-lived buffers and tasks in `onEnter()`.
- Free resources in reverse order in `onExit()`.
- Delete FreeRTOS tasks before the activity is destroyed.
- Close open file handles in `onExit()`.
- Typical task stacks: 2048 simple rendering; 4096 network or EPUB parsing.

## UI And Input

- Do not hardcode screen dimensions like `800` or `480`; use renderer dimensions and orientation helpers.
- Use `renderer.getOrientedViewableTRBL()` for bezel-safe layout.
- Use logical `MappedInputManager::Button::*` in activities; raw hardware indices only in mapping code.
- Route UI drawing through `UITheme` / `GUI` where practical.
- User-facing text must use `tr(STR_*)`; logs can remain hardcoded.
- Product strings must say **Casper**, never CrossInk, in UI/docs/web chrome.

## Build And Verification

- PlatformIO is the source of truth. Personal overrides belong in `platformio.local.ini`.
- Host may be macOS, Linux, WSL, or Windows. Check the environment before platform-specific shell advice.
- Logging uses `LOG_INF`, `LOG_DBG`, and `LOG_ERR`.
- Simulator env: `simulator` when present.
- Common validation:
  - `pio run -e simulator` for simulator-facing UI/reader work
  - `pio run -e default` for firmware compile validation
  - `pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high` for static analysis

## Generated Files

- Do not edit generated files directly.
- Web portal headers under `src/network/html/*.generated.h` come from `scripts/build_web.py` and `web/`.
- I18n under `lib/I18n/` comes from `lib/I18n/translations/*.yaml` via `scripts/gen_i18n.py`.

## Cache Format

- EPUB cache lives under `.crosspoint/epub_<hash>/`.
- If you change binary cache layouts, bump the format version and document it in `docs/file-formats.md`.
- Clear the relevant cache when testing parser/layout/image/cache format changes.

## Git Workflow

- Check `git status --short` before edits and before reporting results.
- Do not commit unless the user explicitly asks.
- Branch prefixes: `feat/`, `fix/`, `docs/`, `refactor/`, `test/`, `chore/`.
- Suggested messages: `<type>: <short summary>`.

## Changelog

When features are added or issues fixed, add a user-facing entry to `CHANGELOG.md`.

### Types

- Added / Changed / Deprecated / Removed / Fixed / Security
