#!/usr/bin/env python3
"""CrossPoint USB flasher for X3, X4, and X4 Pro.

App images go at 0x10000 — never 0x0 (that overwrites the bootloader).

X3 and X4 share the ESP32-C3 CrossPoint .bin. X4 Pro is ESP32-S3 and needs
its own image; flashing a C3 .bin onto it will not boot.

CachyOS:  sudo pacman -S tk esptool
           python3 tools/x4-flasher/crosspoint-flash.py
"""

from __future__ import annotations

import argparse
import glob
import queue
import shutil
import struct
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

OFFSET = "0x10000"
BAUD = "921600"
ESP_IMAGE_MAGIC = 0xE9
# esp_image_header_t.chip_id (little-endian u16 at byte 12)
CHIP_ID_C3 = 0x0005
CHIP_ID_S3 = 0x0009


@dataclass(frozen=True)
class Device:
    key: str
    label: str
    chip: str  # esptool --chip
    chip_id: int
    after: str


DEVICES: tuple[Device, ...] = (
    Device("x3", "Xteink X3", "esp32c3", CHIP_ID_C3, "Reset, then hold Power until it boots."),
    Device("x4", "Xteink X4", "esp32c3", CHIP_ID_C3, "Reset, then hold Power until it boots."),
    Device("x4pro", "Xteink X4 Pro", "esp32s3", CHIP_ID_S3, "Reset, then hold Power until it boots."),
)
DEVICE_BY_LABEL = {d.label: d for d in DEVICES}
DEVICE_BY_KEY = {d.key: d for d in DEVICES}


def downloads_dir() -> Path:
    return Path.home() / "Downloads"


def find_esptool() -> list[str]:
    # Prefer `esptool` (v5); `esptool.py` is deprecated.
    for name in ("esptool", "esptool.py"):
        path = shutil.which(name)
        if path:
            return [path]
    return [sys.executable, "-m", "esptool"]


def serial_ports() -> list[str]:
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
    by_id = Path("/dev/serial/by-id")
    if by_id.is_dir():
        for link in sorted(by_id.iterdir()):
            if link.is_symlink():
                ports.append(str(link))
    seen: set[str] = set()
    out: list[str] = []
    for p in ports:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def default_bin() -> str:
    hits = sorted(downloads_dir().glob("CrossPoint*.bin"))
    if hits:
        return str(hits[-1])
    hits = sorted(downloads_dir().glob("*.bin"))
    return str(hits[-1]) if hits else ""


def read_image_chip_id(path: Path) -> int | None:
    try:
        data = path.read_bytes()[:16]
    except OSError:
        return None
    if len(data) < 14 or data[0] != ESP_IMAGE_MAGIC:
        return None
    return struct.unpack_from("<H", data, 12)[0]


def flash_command(device: Device, port: str, firmware: str) -> list[str]:
    return find_esptool() + [
        "--chip",
        device.chip,
        "--port",
        port,
        "--baud",
        BAUD,
        "write-flash",
        OFFSET,
        firmware,
    ]


def run_flash(cmd: list[str], on_line) -> int:
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        on_line(line)
    return proc.wait()


def looks_like_permission_denied(text: str) -> bool:
    lower = text.lower()
    return "permission denied" in lower or "errno 13" in lower


def run_flash_maybe_pkexec(cmd: list[str], on_line) -> int:
    collected: list[str] = []

    def capture(line: str) -> None:
        collected.append(line)
        on_line(line)

    code = run_flash(cmd, capture)
    if code == 0 or not looks_like_permission_denied("".join(collected)):
        return code
    pkexec = shutil.which("pkexec")
    if not pkexec:
        on_line("\nPermission denied. Run: sudo usermod -aG uucp $USER\n")
        on_line("Then open a new terminal, or flash this once with sudo esptool …\n")
        return code
    on_line("\nPermission denied. Asking for admin via pkexec…\n")
    privileged = [pkexec] + cmd
    on_line("$ " + " ".join(privileged) + "\n")
    return run_flash(privileged, on_line)


def chip_mismatch_message(device: Device, firmware: Path) -> str | None:
    chip_id = read_image_chip_id(firmware)
    if chip_id is None:
        return None
    if chip_id == device.chip_id:
        return None
    got = {CHIP_ID_C3: "ESP32-C3 (X3/X4)", CHIP_ID_S3: "ESP32-S3 (X4 Pro)"}.get(chip_id, f"chip_id 0x{chip_id:04x}")
    return (
        f"This .bin looks like {got}, but {device.label} is {device.chip}. "
        "X3 and X4 share one C3 image. X4 Pro needs a separate S3 image."
    )


def launch_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:
        print("tkinter missing. On CachyOS: sudo pacman -S tk", file=sys.stderr)
        return 1

    class Flasher(tk.Tk):
        def __init__(self) -> None:
            super().__init__()
            self.title("CrossPoint flasher")
            self.geometry("760x500")
            self.device_label = tk.StringVar(value=DEVICE_BY_KEY["x4"].label)
            self.bin_path = tk.StringVar(value=default_bin())
            self.port = tk.StringVar()
            self._log_q: queue.Queue[str] = queue.Queue()
            self._busy = False

            pad = {"padx": 10, "pady": 6}
            ttk.Label(self, text="Device").grid(row=0, column=0, sticky="w", **pad)
            self.device_box = ttk.Combobox(
                self,
                textvariable=self.device_label,
                values=[d.label for d in DEVICES],
                state="readonly",
            )
            self.device_box.grid(row=0, column=1, sticky="ew", **pad)

            ttk.Label(self, text="Firmware (.bin)").grid(row=1, column=0, sticky="w", **pad)
            ttk.Entry(self, textvariable=self.bin_path).grid(row=1, column=1, sticky="ew", **pad)
            ttk.Button(self, text="Browse…", command=self._browse).grid(row=1, column=2, **pad)

            ttk.Label(self, text="Serial port").grid(row=2, column=0, sticky="w", **pad)
            self.port_box = ttk.Combobox(self, textvariable=self.port, state="readonly")
            self.port_box.grid(row=2, column=1, sticky="ew", **pad)
            ttk.Button(self, text="Refresh", command=self._refresh_ports).grid(row=2, column=2, **pad)

            ttk.Label(
                self,
                text="Offset 0x10000 (app image). X3/X4 = ESP32-C3 same .bin. X4 Pro = ESP32-S3.",
            ).grid(row=3, column=0, columnspan=3, sticky="w", **pad)

            self.flash_btn = ttk.Button(self, text="Flash", command=self._flash)
            self.flash_btn.grid(row=4, column=0, columnspan=3, sticky="ew", padx=10, pady=8)

            self.log = tk.Text(self, height=18, wrap="word")
            self.log.grid(row=5, column=0, columnspan=3, sticky="nsew", padx=10, pady=(0, 10))
            self.grid_columnconfigure(1, weight=1)
            self.grid_rowconfigure(5, weight=1)

            self._refresh_ports()
            self.after(100, self._drain_log)

        def _device(self) -> Device:
            return DEVICE_BY_LABEL[self.device_label.get()]

        def _browse(self) -> None:
            initial = downloads_dir() if downloads_dir().is_dir() else Path.home()
            path = filedialog.askopenfilename(
                title="Pick firmware",
                initialdir=str(initial),
                filetypes=[("Firmware", "*.bin"), ("All files", "*")],
            )
            if path:
                self.bin_path.set(path)

        def _refresh_ports(self) -> None:
            ports = serial_ports()
            self.port_box["values"] = ports
            if ports and (not self.port.get() or self.port.get() not in ports):
                acm = [p for p in ports if "ttyACM" in p]
                self.port.set(acm[0] if acm else ports[0])
            if not ports:
                self.port.set("")
                self._append("No serial port. Plug in the device, wake it, then Refresh.\n")

        def _append(self, text: str) -> None:
            self.log.insert("end", text)
            self.log.see("end")

        def _drain_log(self) -> None:
            while True:
                try:
                    item = self._log_q.get_nowait()
                except queue.Empty:
                    break
                if item == "__DONE__":
                    self._busy = False
                    self.flash_btn.state(["!disabled"])
                else:
                    self._append(item)
            self.after(100, self._drain_log)

        def _flash(self) -> None:
            if self._busy:
                return
            device = self._device()
            firmware = self.bin_path.get().strip()
            port = self.port.get().strip()
            if not firmware or not Path(firmware).is_file():
                messagebox.showerror("Missing file", "Pick a .bin first.")
                return
            if not port:
                messagebox.showerror("No port", "No serial port. Plug in and wake the device.")
                return
            mismatch = chip_mismatch_message(device, Path(firmware))
            if mismatch and not messagebox.askyesno("Chip mismatch", mismatch + "\n\nFlash anyway?"):
                return

            cmd = flash_command(device, port, firmware)
            self._busy = True
            self.flash_btn.state(["disabled"])
            self._append("\n$ " + " ".join(cmd) + "\n")

            def worker() -> None:
                try:
                    code = run_flash_maybe_pkexec(cmd, self._log_q.put)
                    if code == 0:
                        self._log_q.put(f"\nDone. On the {device.label}: {device.after}\n")
                    else:
                        self._log_q.put(
                            "\nFailed. Permission denied: sudo usermod -aG uucp $USER "
                            "then open a new terminal, or:\n"
                            "  sudo esptool --chip esp32c3 --port /dev/ttyACM0 --baud 921600 "
                            "write-flash 0x10000 /path/to/firmware.bin\n"
                        )
                except FileNotFoundError:
                    self._log_q.put("esptool not found. Install: sudo pacman -S esptool\n")
                except Exception as exc:  # host tool: surface any failure in the log
                    self._log_q.put(f"\n{exc}\n")
                finally:
                    self._log_q.put("__DONE__")

            threading.Thread(target=worker, daemon=True).start()

    Flasher().mainloop()
    return 0


def launch_cli(device: Device, port: str, firmware: str) -> int:
    path = Path(firmware)
    if not path.is_file():
        print(f"No such file: {firmware}", file=sys.stderr)
        return 1
    if not port:
        ports = serial_ports()
        port = next((p for p in ports if "ttyACM" in p), ports[0] if ports else "")
    if not port:
        print("No serial port. Plug in and wake the device.", file=sys.stderr)
        return 1
    mismatch = chip_mismatch_message(device, path)
    if mismatch:
        print(mismatch, file=sys.stderr)
        return 2
    cmd = flash_command(device, port, str(path))
    print("$ " + " ".join(cmd), flush=True)
    try:
        code = run_flash_maybe_pkexec(cmd, lambda line: print(line, end="", flush=True))
    except FileNotFoundError:
        print("esptool not found. Install: sudo pacman -S esptool", file=sys.stderr)
        return 1
    if code == 0:
        print(f"Done. On the {device.label}: {device.after}")
    return code


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Flash CrossPoint to X3, X4, or X4 Pro.")
    parser.add_argument("--device", choices=[d.key for d in DEVICES], help="x3, x4, or x4pro")
    parser.add_argument("--port", help="Serial port, e.g. /dev/ttyACM0")
    parser.add_argument("firmware", nargs="?", help="Path to the .bin")
    args = parser.parse_args(argv)
    if args.firmware or args.device:
        if not args.device or not args.firmware:
            parser.error("CLI mode needs both --device and a .bin path")
        return launch_cli(DEVICE_BY_KEY[args.device], args.port or "", args.firmware)
    return launch_gui()


if __name__ == "__main__":
    raise SystemExit(main())
