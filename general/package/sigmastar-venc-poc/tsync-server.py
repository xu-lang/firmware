#!/usr/bin/env python3
import socket
import struct
import sys
import time
import tkinter as tk
import ctypes


WINDOW_BG = "black"
MS_FG = "white"
INFO_FG = "#00ff66"


def display_refresh_hz(default=60):
    if sys.platform != "win32":
        return default

    class DevMode(ctypes.Structure):
        _fields_ = [
            ("dmDeviceName", ctypes.c_wchar * 32),
            ("dmSpecVersion", ctypes.c_ushort),
            ("dmDriverVersion", ctypes.c_ushort),
            ("dmSize", ctypes.c_ushort),
            ("dmDriverExtra", ctypes.c_ushort),
            ("dmFields", ctypes.c_uint32),
            ("dmOrientation", ctypes.c_short),
            ("dmPaperSize", ctypes.c_short),
            ("dmPaperLength", ctypes.c_short),
            ("dmPaperWidth", ctypes.c_short),
            ("dmScale", ctypes.c_short),
            ("dmCopies", ctypes.c_short),
            ("dmDefaultSource", ctypes.c_short),
            ("dmPrintQuality", ctypes.c_short),
            ("dmColor", ctypes.c_short),
            ("dmDuplex", ctypes.c_short),
            ("dmYResolution", ctypes.c_short),
            ("dmTTOption", ctypes.c_short),
            ("dmCollate", ctypes.c_short),
            ("dmFormName", ctypes.c_wchar * 32),
            ("dmLogPixels", ctypes.c_ushort),
            ("dmBitsPerPel", ctypes.c_uint32),
            ("dmPelsWidth", ctypes.c_uint32),
            ("dmPelsHeight", ctypes.c_uint32),
            ("dmDisplayFlags", ctypes.c_uint32),
            ("dmDisplayFrequency", ctypes.c_uint32),
        ]

    mode = DevMode()
    mode.dmSize = ctypes.sizeof(mode)
    if ctypes.windll.user32.EnumDisplaySettingsW(None, -1, ctypes.byref(mode)) and mode.dmDisplayFrequency:
        return int(mode.dmDisplayFrequency)
    return default


def enable_high_resolution_timer():
    if sys.platform != "win32":
        return None
    winmm = ctypes.WinDLL("winmm")
    if winmm.timeBeginPeriod(1) != 0:
        return None
    return winmm


def disable_high_resolution_timer(winmm):
    if winmm is not None:
        winmm.timeEndPeriod(1)


def current_ms_text():
    return f"{int(time.time() * 1000) % 10000:04d}"


def handle_sync(sock, verbose):
    sync_count = 0
    while True:
        try:
            data, addr = sock.recvfrom(1024)
        except BlockingIOError:
            return sync_count
        if len(data) < 12 or data[:4] != b"PSYN":
            continue
        t1 = struct.unpack("<Q", data[4:12])[0]
        t2 = int(time.time() * 1_000_000)
        t3 = int(time.time() * 1_000_000)
        rsp = struct.pack("<4sQQQ", b"PSYN", t1, t2, t3)
        sock.sendto(rsp, addr)
        sync_count += 1
        if verbose:
            print(f"sync from {addr[0]}:{addr[1]} t1={t1}", flush=True)


class Display:
    def __init__(self, root, sock, port, verbose):
        self.root = root
        self.sock = sock
        self.port = port
        self.verbose = verbose
        self.target_hz = display_refresh_hz()
        self.interval_s = 1.0 / self.target_hz
        self.next_tick_t = time.perf_counter()
        self.sync_count = 0
        self.frame_count = 0
        self.refresh_hz = 0.0
        self.last_rate_t = time.perf_counter()

        root.title(f"tsync-server:{port}")
        root.configure(bg=WINDOW_BG)
        root.geometry("640x360")

        self.ms_label = tk.Label(
            root,
            text="000",
            bg=WINDOW_BG,
            fg=MS_FG,
            font=("DejaVu Sans Mono", 180, "bold"),
        )
        self.ms_label.pack(expand=True, fill="both")

        self.info_label = tk.Label(
            root,
            text=f"target {self.target_hz} Hz  actual 0.0 Hz  syncs 0",
            bg=WINDOW_BG,
            fg=INFO_FG,
            font=("DejaVu Sans Mono", 24),
        )
        self.info_label.pack(fill="x", pady=(0, 18))

        root.bind("<Escape>", lambda _event: root.destroy())
        root.after(1, self.tick)

    def tick(self):
        self.sync_count += handle_sync(self.sock, self.verbose)

        self.ms_label.configure(text=current_ms_text())
        self.frame_count += 1

        now = time.perf_counter()
        elapsed = now - self.last_rate_t
        if elapsed >= 1.0:
            self.refresh_hz = self.frame_count / elapsed
            self.frame_count = 0
            self.last_rate_t = now

            self.info_label.configure(
                text=f"target {self.target_hz} Hz  actual {self.refresh_hz:5.1f} Hz  syncs {self.sync_count}"
            )

        self.next_tick_t += self.interval_s
        delay_s = self.next_tick_t - time.perf_counter()
        if delay_s < 0:
            self.next_tick_t = time.perf_counter()
            delay_ms = 0
        else:
            delay_ms = max(1, int(delay_s * 1000))
        self.root.after(delay_ms, self.tick)


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <port> [--verbose]", file=sys.stderr)
        sys.exit(1)
    port = int(sys.argv[1])
    verbose = "--verbose" in sys.argv

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.setblocking(False)
    s.bind(("0.0.0.0", port))
    print(f"tsync-server listening on port {port}", flush=True)
    winmm = enable_high_resolution_timer()

    try:
        root = tk.Tk()
        Display(root, s, port, verbose)
        root.mainloop()
    except KeyboardInterrupt:
        print("", flush=True)
    finally:
        disable_high_resolution_timer(winmm)


if __name__ == "__main__":
    main()
