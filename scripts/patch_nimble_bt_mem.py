"""Ensure NimBLE-Arduino defines _btLibraryInUse for controller-only builds.

With CrossPoint-style CONFIG_BT_NIMBLE_ENABLED=n / CONTROLLER_ONLY, Arduino's
esp32-hal-bt.c is compiled as stubs and omits _btLibraryInUse. NimBLE 2.3.8
still includes esp32-hal-bt-mem.h which needs that symbol — inject a definition.
"""

from pathlib import Path

Import("env")  # type: ignore  # PlatformIO

MARKER = "Casper: provide symbol when controller-only stubs omit it"
INJECT = (
    f'// {MARKER}\n'
    'extern "C" { bool _btLibraryInUse = true; }\n'
)


def patch_file(path: Path) -> None:
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        return
    needle = '#include "esp32-hal-bt-mem.h"'
    if needle not in text:
        return
    path.write_text(text.replace(needle, needle + "\n" + INJECT, 1), encoding="utf-8")
    print(f"Patched NimBLE _btLibraryInUse: {path}")


def main() -> None:
    root = Path(env["PROJECT_DIR"])  # type: ignore
    for cand in root.glob(".pio/libdeps/*/NimBLE-Arduino/src/NimBLEDevice.cpp"):
        patch_file(cand)


main()
