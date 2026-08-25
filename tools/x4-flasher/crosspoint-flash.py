#!/usr/bin/env python3
"""CrossPoint USB flasher for X3, X4, and X4 Pro.

App images go at 0x10000 — never 0x0 (that overwrites the bootloader).

X3/X4 share the ESP32-C3 .bin. X4 Pro is ESP32-S3 and needs its own image.

CachyOS:  sudo pacman -S tk esptool
           python3 tools/x4-flasher/crosspoint-flash.py

USB-JTAG (ESP32-C3/S3 native USB): stub+compress writes junk (same MD5
mismatch every time). Default is 115200, --no-stub, --no-compress.
If the serial node is not writable, the GUI asks for admin via pkexec.
"""

from __future__ import annotations

import argparse
import glob
import os
import queue
import shutil
import struct
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

OFFSET = "0x10000"
DEFAULT_BAUD = "115200"
BAUDS = ("115200", "230400", "460800", "921600")
ESP_IMAGE_MAGIC = 0xE9
CHIP_ID_C3 = 0x0005
CHIP_ID_S3 = 0x0009


@dataclass(frozen=True)
class Device:
    key: str
    label: str
    short: str
    chip: str
    chip_id: int
    after: str


DEVICES: tuple[Device, ...] = (
    Device("x3", "Xteink X3", "X3", "esp32c3", CHIP_ID_C3, "Reset, then hold Power until it boots."),
    Device("x4", "Xteink X4", "X4", "esp32c3", CHIP_ID_C3, "Reset, then hold Power until it boots."),
    Device("x4pro", "Xteink X4 Pro", "X4 PRO", "esp32s3", CHIP_ID_S3, "Reset, then hold Power until it boots."),
)
DEVICE_BY_KEY = {d.key: d for d in DEVICES}


def downloads_dir() -> Path:
    return Path.home() / "Downloads"


def find_esptool() -> list[str]:
    for name in ("esptool", "esptool.py"):
        path = shutil.which(name)
        if path:
            return [path]
    return [sys.executable, "-m", "esptool"]


def serial_ports() -> list[str]:
    # Prefer real tty nodes. by-id symlinks have the same permission bit.
    ports = sorted(glob.glob("/dev/ttyACM*")) + sorted(glob.glob("/dev/ttyUSB*"))
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


def port_writable(port: str) -> bool:
    try:
        return os.access(port, os.R_OK | os.W_OK)
    except OSError:
        return False


def flash_command(device: Device, port: str, firmware: str, baud: str) -> list[str]:
    # USB-JTAG: compressed stub writes land as the packed stream (same MD5 every
    # retry). --no-stub often does not program SPI flash at all. Stub + uncompressed.
    return find_esptool() + [
        "--chip",
        device.chip,
        "--port",
        port,
        "--baud",
        baud,
        "--before",
        "usb-reset",
        "write-flash",
        "--no-compress",
        "--flash-mode",
        "dio",
        "--flash-size",
        "16MB",
        "--flash-freq",
        "40m",
        OFFSET,
        str(Path(firmware).resolve()),
    ]


def with_pkexec(cmd: list[str]) -> list[str]:
    pkexec = shutil.which("pkexec")
    if not pkexec:
        return cmd
    return [pkexec] + cmd


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


def looks_like_md5_mismatch(text: str) -> bool:
    return "md5 of file does not match" in text.lower()


def run_flash_maybe_pkexec(cmd: list[str], port: str, on_line) -> int:
    collected: list[str] = []

    def capture(line: str) -> None:
        collected.append(line)
        on_line(line)

    to_run = cmd
    if not port_writable(port):
        privileged = with_pkexec(cmd)
        if privileged != cmd:
            on_line("Port is not writable. Asking for admin via pkexec…\n")
            to_run = privileged
    on_line("$ " + " ".join(to_run) + "\n")
    code = run_flash(to_run, capture)
    blob = "".join(collected)
    if code == 0:
        return 0
    if looks_like_permission_denied(blob) and to_run == cmd:
        privileged = with_pkexec(cmd)
        if privileged != cmd:
            on_line("\nPermission denied. Asking for admin via pkexec…\n")
            on_line("$ " + " ".join(privileged) + "\n")
            return run_flash(privileged, on_line)
        on_line("\nPermission denied. Run: sudo usermod -aG uucp $USER  (then log out/in)\n")
    if looks_like_md5_mismatch(blob):
        on_line(
            "\nFlash verify failed. USB-JTAG stub/compress wrote junk. "
            "Use --no-stub --no-compress at 115200 (this GUI does that by default).\n"
        )
    return code


def chip_mismatch_message(device: Device, firmware: Path) -> str | None:
    chip_id = read_image_chip_id(firmware)
    if chip_id is None or chip_id == device.chip_id:
        return None
    got = {CHIP_ID_C3: "ESP32-C3 (X3/X4)", CHIP_ID_S3: "ESP32-S3 (X4 Pro)"}.get(chip_id, f"chip_id 0x{chip_id:04x}")
    return (
        f"This .bin looks like {got}, but {device.label} is {device.chip}. "
        "X3 and X4 share one C3 image. X4 Pro needs a separate S3 image."
    )


def launch_gui() -> int:
    try:
        import tkinter as tk
        from tkinter import filedialog, font as tkfont, messagebox
    except ImportError:
        print("tkinter missing. On CachyOS: sudo pacman -S tk", file=sys.stderr)
        return 1

    C_BG = "#0b0c10"
    C_PANEL = "#12141c"
    C_RAISED = "#1a1d27"
    C_LINE = "#2c3140"
    C_TEXT = "#f2f0ea"
    C_MUTED = "#8b8d97"
    C_AMBER = "#e8a020"
    C_AMBER_DIM = "#8a5a10"
    C_GREEN = "#3dd68c"
    C_RED = "#ff5c5c"
    C_LOG = "#07080b"

    def first_font(root: tk.Tk, names: tuple[str, ...], size: int, weight: str = "normal") -> tkfont.Font:
        available = {name.lower() for name in tkfont.families(root)}
        chosen = "sans-serif"
        for name in names:
            if name.lower() in available:
                chosen = name
                break
        return tkfont.Font(root=root, family=chosen, size=size, weight=weight)

    class Flasher(tk.Tk):
        def __init__(self) -> None:
            super().__init__()
            self.title("CROSSPOINT  ·  FLASH BAY")
            self.geometry("860x640")
            self.minsize(780, 580)
            self.configure(bg=C_BG)
            self.device_key = tk.StringVar(value="x4")
            self.bin_path = tk.StringVar(value=default_bin())
            self.port = tk.StringVar()
            self.baud = tk.StringVar(value=DEFAULT_BAUD)
            self.status = tk.StringVar(value="Ready")
            self._log_q: queue.Queue[str] = queue.Queue()
            self._busy = False
            self._device_btns: dict[str, tk.Button] = {}

            self.font_title = first_font(self, ("Eurostile", "Orbitron", "Noto Sans", "Inter", "Cantarell"), 22, "bold")
            self.font_sub = first_font(self, ("Noto Sans", "Inter", "Cantarell"), 10)
            self.font_label = first_font(self, ("Noto Sans", "Inter", "Cantarell"), 9, "bold")
            self.font_body = first_font(self, ("Noto Sans", "Inter", "Cantarell"), 11)
            self.font_mono = first_font(self, ("JetBrains Mono", "Source Code Pro", "Noto Sans Mono", "monospace"), 10)
            self.font_flash = first_font(self, ("Noto Sans", "Inter", "Cantarell"), 13, "bold")

            header = tk.Frame(self, bg=C_BG)
            header.pack(fill="x", padx=28, pady=(22, 8))
            tk.Label(header, text="CROSSPOINT", font=self.font_title, fg=C_TEXT, bg=C_BG, anchor="w").pack(side="left")
            tk.Label(header, text="FLASH BAY", font=self.font_sub, fg=C_AMBER, bg=C_BG, anchor="e").pack(side="right", pady=(10, 0))
            tk.Frame(self, bg=C_AMBER, height=2).pack(fill="x", padx=28)

            body = tk.Frame(self, bg=C_BG)
            body.pack(fill="both", expand=True, padx=28, pady=18)

            tk.Label(body, text="TARGET", font=self.font_label, fg=C_MUTED, bg=C_BG).pack(anchor="w")
            pills = tk.Frame(body, bg=C_BG)
            pills.pack(fill="x", pady=(6, 16))
            for dev in DEVICES:
                btn = tk.Button(
                    pills,
                    text=dev.short,
                    font=self.font_body,
                    bd=0,
                    relief="flat",
                    padx=18,
                    pady=8,
                    command=lambda key=dev.key: self._select_device(key),
                    cursor="hand2",
                )
                btn.pack(side="left", padx=(0, 8))
                self._device_btns[dev.key] = btn

            tk.Label(body, text="FIRMWARE", font=self.font_label, fg=C_MUTED, bg=C_BG).pack(anchor="w")
            file_row = tk.Frame(body, bg=C_RAISED, highlightbackground=C_LINE, highlightthickness=1)
            file_row.pack(fill="x", pady=(6, 16))
            tk.Entry(
                file_row,
                textvariable=self.bin_path,
                font=self.font_mono,
                bd=0,
                relief="flat",
                fg=C_TEXT,
                bg=C_RAISED,
                insertbackground=C_AMBER,
                highlightthickness=0,
            ).pack(side="left", fill="x", expand=True, padx=12, pady=10)
            tk.Button(
                file_row,
                text="BROWSE",
                font=self.font_label,
                fg=C_BG,
                bg=C_AMBER,
                activebackground=C_TEXT,
                activeforeground=C_BG,
                bd=0,
                padx=16,
                command=self._browse,
                cursor="hand2",
            ).pack(side="right", padx=6, pady=6)

            io = tk.Frame(body, bg=C_BG)
            io.pack(fill="x", pady=(0, 16))
            left = tk.Frame(io, bg=C_BG)
            left.pack(side="left", fill="x", expand=True)
            right = tk.Frame(io, bg=C_BG)
            right.pack(side="right", padx=(16, 0))

            tk.Label(left, text="PORT", font=self.font_label, fg=C_MUTED, bg=C_BG).pack(anchor="w")
            port_row = tk.Frame(left, bg=C_BG)
            port_row.pack(fill="x", pady=(6, 0))
            self.port_menu = tk.OptionMenu(port_row, self.port, "")
            self.port_menu.configure(
                font=self.font_mono,
                fg=C_TEXT,
                bg=C_RAISED,
                activebackground=C_PANEL,
                activeforeground=C_TEXT,
                highlightthickness=1,
                highlightbackground=C_LINE,
                bd=0,
                indicatoron=0,
                width=28,
                anchor="w",
            )
            self.port_menu["menu"].configure(bg=C_RAISED, fg=C_TEXT, activebackground=C_AMBER_DIM, activeforeground=C_TEXT, bd=0)
            self.port_menu.pack(side="left", fill="x", expand=True)
            tk.Button(
                port_row,
                text="↻",
                font=self.font_flash,
                fg=C_TEXT,
                bg=C_RAISED,
                activebackground=C_PANEL,
                bd=0,
                padx=12,
                command=self._refresh_ports,
                cursor="hand2",
            ).pack(side="left", padx=(8, 0))

            tk.Label(right, text="BAUD", font=self.font_label, fg=C_MUTED, bg=C_BG).pack(anchor="w")
            baud_menu = tk.OptionMenu(right, self.baud, *BAUDS)
            baud_menu.configure(
                font=self.font_mono,
                fg=C_TEXT,
                bg=C_RAISED,
                activebackground=C_PANEL,
                activeforeground=C_TEXT,
                highlightthickness=1,
                highlightbackground=C_LINE,
                bd=0,
                indicatoron=0,
                width=10,
            )
            baud_menu["menu"].configure(bg=C_RAISED, fg=C_TEXT, activebackground=C_AMBER_DIM, bd=0)
            baud_menu.pack(pady=(6, 0))

            self.flash_btn = tk.Button(
                body,
                text="FLASH  ·  0x10000",
                font=self.font_flash,
                fg=C_BG,
                bg=C_AMBER,
                activebackground=C_TEXT,
                activeforeground=C_BG,
                bd=0,
                pady=14,
                command=self._flash,
                cursor="hand2",
            )
            self.flash_btn.pack(fill="x", pady=(4, 16))

            tk.Label(body, text="CONSOLE", font=self.font_label, fg=C_MUTED, bg=C_BG).pack(anchor="w")
            log_wrap = tk.Frame(body, bg=C_LINE)
            log_wrap.pack(fill="both", expand=True, pady=(6, 0))
            self.log = tk.Text(
                log_wrap,
                font=self.font_mono,
                bg=C_LOG,
                fg="#c6c9d1",
                insertbackground=C_AMBER,
                bd=0,
                highlightthickness=0,
                wrap="word",
                padx=12,
                pady=10,
            )
            self.log.pack(fill="both", expand=True, padx=1, pady=1)
            self.log.tag_configure("ok", foreground=C_GREEN)
            self.log.tag_configure("err", foreground=C_RED)
            self.log.tag_configure("warn", foreground=C_AMBER)
            self.log.tag_configure("cmd", foreground="#9aa7c7")

            footer = tk.Frame(self, bg=C_PANEL)
            footer.pack(fill="x", side="bottom")
            tk.Label(footer, textvariable=self.status, font=self.font_sub, fg=C_MUTED, bg=C_PANEL, anchor="w").pack(
                fill="x", padx=28, pady=10
            )

            self._select_device("x4")
            self._refresh_ports()
            self.after(100, self._drain_log)
            self._append(
                "USB-JTAG: uncompressed ROM loader at 115200. Expect several minutes. Admin prompt if the port is locked.\n",
                "warn",
            )

        def _device(self) -> Device:
            return DEVICE_BY_KEY[self.device_key.get()]

        def _select_device(self, key: str) -> None:
            self.device_key.set(key)
            for k, btn in self._device_btns.items():
                on = k == key
                btn.configure(bg=C_AMBER if on else C_RAISED, fg=C_BG if on else C_TEXT, activebackground=C_TEXT if on else C_PANEL)
            dev = DEVICE_BY_KEY[key]
            self.status.set(f"{dev.label}  ·  {dev.chip.upper()}  ·  app @ {OFFSET}")

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
            menu = self.port_menu["menu"]
            menu.delete(0, "end")
            for p in ports:
                menu.add_command(label=p, command=lambda value=p: self.port.set(value))
            if ports and (not self.port.get() or self.port.get() not in ports):
                self.port.set(ports[0])
            if not ports:
                self.port.set("")
                self.status.set("No serial port — plug in, wake the device, then refresh")
            else:
                locked = "" if port_writable(self.port.get()) else "  ·  will ask for admin"
                self.status.set(f"{self._device().label}  ·  {self.port.get()}{locked}")

        def _append(self, text: str, tag: str | None = None) -> None:
            if tag is None:
                lower = text.lower()
                if "fatal" in lower or "does not match" in lower or "permission denied" in lower:
                    tag = "err"
                elif "verified" in lower or "done." in lower:
                    tag = "ok"
                elif text.startswith("$ ") or "pkexec" in lower:
                    tag = "cmd"
                elif "warning" in lower or "deprecated" in lower:
                    tag = "warn"
            self.log.insert("end", text, tag or ())
            self.log.see("end")

        def _drain_log(self) -> None:
            while True:
                try:
                    item = self._log_q.get_nowait()
                except queue.Empty:
                    break
                if item == "__DONE__":
                    self._busy = False
                    self.flash_btn.configure(state="normal", text="FLASH  ·  0x10000", bg=C_AMBER)
                else:
                    self._append(item)
            self.after(100, self._drain_log)

        def _flash(self) -> None:
            if self._busy:
                return
            device = self._device()
            firmware = self.bin_path.get().strip()
            port = self.port.get().strip()
            baud = self.baud.get().strip() or DEFAULT_BAUD
            if not firmware or not Path(firmware).is_file():
                messagebox.showerror("Missing file", "Pick a .bin first.")
                return
            if not port:
                messagebox.showerror("No port", "Plug in the device, wake it, then refresh ports.")
                return
            mismatch = chip_mismatch_message(device, Path(firmware))
            if mismatch and not messagebox.askyesno("Chip mismatch", mismatch + "\n\nFlash anyway?"):
                return

            cmd = flash_command(device, port, firmware, baud)
            self._busy = True
            self.flash_btn.configure(state="disabled", text="FLASHING…", bg=C_AMBER_DIM)
            self.status.set(f"Flashing {device.short} at {baud} baud…")
            self._append("\n")

            def worker() -> None:
                try:
                    code = run_flash_maybe_pkexec(cmd, port, self._log_q.put)
                    if code == 0:
                        self._log_q.put(f"\nDone. On the {device.label}: {device.after}\n")
                    else:
                        self._log_q.put(f"\nFailed (exit {code}).\n")
                except FileNotFoundError:
                    self._log_q.put("esptool not found. Install: sudo pacman -S esptool\n")
                except Exception as exc:  # host tool: surface any failure in the log
                    self._log_q.put(f"\n{exc}\n")
                finally:
                    self._log_q.put("__DONE__")

            threading.Thread(target=worker, daemon=True).start()

    Flasher().mainloop()
    return 0


def launch_cli(device: Device, port: str, firmware: str, baud: str) -> int:
    path = Path(firmware)
    if not path.is_file():
        print(f"No such file: {firmware}", file=sys.stderr)
        return 1
    if not port:
        ports = serial_ports()
        port = ports[0] if ports else ""
    if not port:
        print("No serial port. Plug in and wake the device.", file=sys.stderr)
        return 1
    mismatch = chip_mismatch_message(device, path)
    if mismatch:
        print(mismatch, file=sys.stderr)
        return 2
    cmd = flash_command(device, port, str(path), baud)
    try:
        code = run_flash_maybe_pkexec(cmd, port, lambda line: print(line, end="", flush=True))
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
    parser.add_argument("--baud", default=DEFAULT_BAUD, help=f"Default {DEFAULT_BAUD} (USB-JTAG; do not use 921600)")
    parser.add_argument("firmware", nargs="?", help="Path to the .bin")
    args = parser.parse_args(argv)
    if args.firmware or args.device:
        if not args.device or not args.firmware:
            parser.error("CLI mode needs both --device and a .bin path")
        return launch_cli(DEVICE_BY_KEY[args.device], args.port or "", args.firmware, args.baud)
    return launch_gui()


if __name__ == "__main__":
    raise SystemExit(main())
