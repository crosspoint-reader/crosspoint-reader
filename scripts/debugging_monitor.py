#!/usr/bin/env python3
"""
ESP32 Serial Monitor with Memory Graph

This script provides a comprehensive real-time serial monitor for ESP32 devices with
integrated memory usage graphing capabilities. It reads serial output, parses memory
information, and displays it in both console and graphical form.

Features:
- Real-time serial output monitoring with color-coded log levels
- Interactive memory usage graphing with matplotlib
- Command input interface for sending commands to the ESP32 device
- Screenshot capture and processing (1-bit black/white format)
- Graceful shutdown handling with Ctrl-C signal processing
- Configurable filtering and suppression of log messages
- Thread-safe operation with coordinated shutdown events

Usage:
    python debugging_monitor.py [port] [options]

Use --serve for a localhost HTTP API, with --headless to omit the graph and stdin.
See docs/debugging-monitor.md for the API and benchmark control workflow.
Press Ctrl-C or close the graph window to exit gracefully.
"""

from __future__ import annotations

import argparse
import glob
import importlib
import platform
import re
import signal
import tempfile
import threading
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

from monitor_session import DeviceSession, SessionError

DEFAULT_BAUDRATE = 115200


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="ESP32 Serial Monitor with Memory Graph - Real-time monitoring, graphing, and command interface"
    )
    parser.add_argument(
        "port",
        nargs="?",
        default=None,
        help="Serial port (leave empty for autodetection)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=DEFAULT_BAUDRATE,
        help=f"Baud rate (default: {DEFAULT_BAUDRATE})",
    )
    parser.add_argument(
        "--filter",
        type=str,
        default="",
        help="Only display lines containing this keyword (case-insensitive)",
    )
    parser.add_argument(
        "--suppress",
        type=str,
        default="",
        help="Suppress lines containing this keyword (case-insensitive)",
    )
    parser.add_argument(
        "--serve", action="store_true", help="Expose the local device API"
    )
    parser.add_argument(
        "--headless", action="store_true", help="Run without graph or interactive stdin"
    )
    parser.add_argument(
        "--http-port", type=int, default=8765, help="Local API port (default: 8765)"
    )
    parser.add_argument(
        "--output-dir",
        default="debugging-monitor-output",
        help="Parent directory for serial logs and screenshots",
    )
    return parser


# Colors are optional; headless mode requires only pyserial.
try:
    from colorama import Fore, Style, init
except ImportError:

    class PlainColors:
        def __getattr__(self, _name):
            return ""

    Fore = Style = PlainColors()

    def init(**_kwargs):
        pass


plt = None

# --- Global Variables for Data Sharing ---
# Store last 50 data points
MAX_POINTS = 50
time_data: deque[str] = deque(maxlen=MAX_POINTS)
free_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
total_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
max_alloc_data: deque[float] = deque(maxlen=MAX_POINTS)
psram_time_data: deque[str] = deque(maxlen=MAX_POINTS)
psram_free_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
psram_total_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
psram_max_alloc_data: deque[float] = deque(maxlen=MAX_POINTS)
data_lock: threading.Lock = threading.Lock()  # Prevent reading while writing

# Global shutdown flag
shutdown_event = threading.Event()

# Initialize colors
init(autoreset=True)

# Color mapping for log lines
COLOR_KEYWORDS: dict[str, list[str]] = {
    Fore.RED: ["ERROR", "[ERR]", "[SCT]", "FAILED", "WARNING"],
    Fore.CYAN: ["[MEM]", "FREE:"],
    Fore.MAGENTA: [
        "[GFX]",
        "[ERS]",
        "DISPLAY",
        "RAM WRITE",
        "RAM COMPLETE",
        "REFRESH",
        "POWERING ON",
        "FRAME BUFFER",
        "LUT",
    ],
    Fore.GREEN: [
        "[EBP]",
        "[BMC]",
        "[ZIP]",
        "[PARSER]",
        "[EHP]",
        "LOADING EPUB",
        "CACHE",
        "DECOMPRESSED",
        "PARSING",
    ],
    Fore.YELLOW: ["[ACT]", "ENTERING ACTIVITY", "EXITING ACTIVITY"],
    Fore.BLUE: ["RENDERED PAGE", "[LOOP]", "DURATION", "WAIT COMPLETE"],
    Fore.LIGHTYELLOW_EX: [
        "[CPS]",
        "SETTINGS",
        "[CLEAR_CACHE]",
        "[CHAP]",
        "[OPDS]",
        "[COF]",
    ],
    Fore.LIGHTBLACK_EX: [
        "ESP-ROM",
        "BUILD:",
        "RST:",
        "BOOT:",
        "SPIWP:",
        "MODE:",
        "LOAD:",
        "ENTRY",
        "[SD]",
        "STARTING CROSSPOINT",
        "VERSION",
    ],
    Fore.LIGHTCYAN_EX: ["[RBS]"],
    Fore.LIGHTMAGENTA_EX: [
        "[KRS]",
        "EINKDISPLAY:",
        "STATIC FRAME",
        "INITIALIZING",
        "SPI INITIALIZED",
        "GPIO PINS",
        "RESETTING",
        "SSD1677",
        "E-INK",
    ],
    Fore.LIGHTGREEN_EX: ["[FNS]", "FOOTNOTE"],
}


def signal_handler(signum, frame):
    """Handle SIGINT (Ctrl-C) by setting the shutdown event."""
    # frame parameter is required by signal handler signature but not used
    del frame  # Explicitly mark as unused to satisfy linters
    print(f"\n{Fore.YELLOW}Received signal {signum}. Shutting down...{Style.RESET_ALL}")
    shutdown_event.set()


# pylint: disable=R0912
def get_color_for_line(line: str) -> str:
    """
    Classify log lines by type and assign appropriate colors.
    """
    line_upper = line.upper()
    for color, keywords in COLOR_KEYWORDS.items():
        if any(keyword in line_upper for keyword in keywords):
            return color
    return Fore.WHITE


def parse_memory_line(line: str) -> tuple[int | None, int | None, int | None]:
    """
    Extracts memory stats from MEM log lines.
    Format: Free: N bytes, Total: N bytes, Min Free: N bytes, MaxAlloc: N bytes
    Returns: (free_bytes, total_bytes, max_alloc_bytes)
    """

    def _find(pattern: str) -> int | None:
        m = re.search(pattern, line)
        if m:
            try:
                return int(m.group(1))
            except ValueError:
                pass
        return None

    return (
        _find(r"\bFree:\s*(\d+)"),
        _find(r"\bTotal:\s*(\d+)"),
        _find(r"\bMaxAlloc:\s*(\d+)"),
    )


def presentation_worker(session, args):
    """Consume session events without blocking serial reads on terminal output."""
    cursor = 0
    while not shutdown_event.is_set():
        batch = session.read_events(cursor, wait=0.2)
        cursor = batch["cursor"]
        if batch["dropped"]:
            print(
                "Monitor display fell behind; full output remains in the session logs."
            )
        for event in batch["events"]:
            if event["type"] == "screenshot":
                print(
                    "Screenshot saved to " + event.get("image_path", event["raw_path"])
                )
                continue
            if event["type"] != "log":
                if event["type"] in ("connection", "screenshot_error"):
                    print(event)
                continue
            line = event["line"]
            pc_time = (
                datetime.fromtimestamp(event["time"], timezone.utc)
                .astimezone()
                .strftime("%H:%M:%S")
            )
            if "[MEM]" in line:
                free_val, total_val, max_alloc_val = parse_memory_line(line)
                if free_val is not None and total_val is not None:
                    with data_lock:
                        series = (
                            (
                                psram_time_data,
                                psram_free_mem_data,
                                psram_total_mem_data,
                                psram_max_alloc_data,
                            )
                            if "PSRAM:" in line
                            else (
                                time_data,
                                free_mem_data,
                                total_mem_data,
                                max_alloc_data,
                            )
                        )
                        for target, value in zip(
                            series,
                            (
                                pc_time,
                                free_val / 1024,
                                total_val / 1024,
                                (max_alloc_val or 0) / 1024,
                            ),
                        ):
                            target.append(value)
            if args.filter and args.filter.lower() not in line.lower():
                continue
            if args.suppress and args.suppress.lower() in line.lower():
                continue
            formatted = re.sub(r"^\[\d+\]", f"[{pc_time}]", line)
            print(f"{get_color_for_line(line)}{formatted}")
        if session.stopped.is_set():
            shutdown_event.set()


def input_worker(session):
    while not shutdown_event.is_set():
        try:
            command = input("Command: ")
            session.request("command", command)
        except SessionError as exc:
            print(f"Command rejected: {exc}")
        except (EOFError, KeyboardInterrupt):
            break


def update_graph(frame) -> list:  # pylint: disable=unused-argument
    """
    Redraw the memory usage chart unless shutdown is requested.
    Shows DRAM metrics (free, total, max contiguous alloc) and an optional PSRAM subplot.
    """
    if shutdown_event.is_set():
        return []

    with data_lock:
        if not time_data and not psram_time_data:
            return []

        x = list(time_data)
        y_free = list(free_mem_data)
        y_total = list(total_mem_data)
        y_max_alloc = list(max_alloc_data)
        px = list(psram_time_data)
        py_free = list(psram_free_mem_data)
        py_total = list(psram_total_mem_data)
        py_max_alloc = list(psram_max_alloc_data)

    fig = plt.gcf()
    fig.clf()
    ax1 = fig.add_subplot(211 if px else 111)

    ax1.plot(x, y_total, label="Total RAM (KB)", color="red", linestyle="--")
    ax1.plot(x, y_free, label="Free RAM (KB)", color="green", marker="o", markersize=3)
    if any(v > 0 for v in y_max_alloc):
        ax1.plot(x, y_max_alloc, label="Max Alloc (KB)", color="orange", linestyle="-.")
    ax1.fill_between(x, y_free, color="green", alpha=0.1)
    ax1.set_title("ESP32 Memory Monitor")
    ax1.set_ylabel("Memory (KB)")
    ax1.set_xlabel("Time")
    ax1.legend(loc="upper left")
    ax1.grid(True, linestyle=":", alpha=0.6)
    plt.setp(ax1.get_xticklabels(), rotation=45, ha="right")

    if px:
        ax2 = fig.add_subplot(212)
        ax2.plot(px, py_total, label="Total PSRAM (KB)", color="red", linestyle="--")
        ax2.plot(
            px,
            py_free,
            label="Free PSRAM (KB)",
            color="green",
            marker="o",
            markersize=3,
        )
        if any(v > 0 for v in py_max_alloc):
            ax2.plot(
                px, py_max_alloc, label="Max Alloc (KB)", color="orange", linestyle="-."
            )
        ax2.fill_between(px, py_free, color="green", alpha=0.1)
        ax2.set_title("ESP32 PSRAM Monitor")
        ax2.set_ylabel("Memory (KB)")
        ax2.set_xlabel("Time")
        ax2.legend(loc="upper left")
        ax2.grid(True, linestyle=":", alpha=0.6)
        plt.setp(ax2.get_xticklabels(), rotation=45, ha="right")

    fig.tight_layout()
    return []


def get_auto_detected_port() -> list[str]:
    """
    Attempts to auto-detect the serial port for the ESP32 device.
    Returns a list of all detected ports.
    If no suitable port is found, the list will be empty.
    Darwin/Linux logic by jonasdiemer
    """
    port_list = []
    system = platform.system()
    # Code for darwin (macOS), linux, and windows
    if system in ("Darwin", "Linux"):
        pattern = "/dev/tty.usbmodem*" if system == "Darwin" else "/dev/ttyACM*"
        port_list = sorted(glob.glob(pattern))
    elif system == "Windows":
        from serial.tools import list_ports

        # Be careful with this pattern list - it should be specific
        # enough to avoid picking up unrelated devices, but broad enough
        # to catch all common USB-serial adapters used with ESP32
        # Caveat: localized versions of Windows may have different descriptions,
        # so we also check for specific VID:PID (but that may not cover all clones)
        pattern_list = ["CP210x", "CH340", "USB Serial"]
        found_ports = list_ports.comports()
        port_list = [
            port.device
            for port in found_ports
            if any(pat in port.description for pat in pattern_list)
            or port.hwid.startswith(
                "USB VID:PID=303A:1001"
            )  # Add specific VID:PID for XTEINK X4
        ]

    return port_list


def main() -> None:
    global plt
    parser = build_arg_parser()
    args = parser.parse_args()
    if not 0 <= args.http_port <= 65535:
        parser.error("--http-port must be between 0 and 65535")
    try:
        importlib.import_module("serial")
        if not args.headless:
            from matplotlib import pyplot

            plt = pyplot
    except ImportError as exc:
        parser.error(
            f"Missing dependency: {exc}. Install pyserial; graph mode also needs matplotlib."
        )

    port = args.port
    if port is None:
        ports = get_auto_detected_port()
        if len(ports) != 1:
            parser.error(
                "Specify a serial port; detected: " + (", ".join(ports) or "none")
            )
        port = ports[0]
    output_parent = Path(args.output_dir)
    output_parent.mkdir(parents=True, exist_ok=True)
    output_dir = tempfile.mkdtemp(
        prefix=datetime.now(timezone.utc).astimezone().strftime("%Y%m%d-%H%M%S-"),
        dir=output_parent,
    )
    session = DeviceSession(port, args.baud, output_dir)
    server = None
    server_thread = None
    if args.serve:
        from monitor_server import create_server

        try:
            server = create_server(session, args.http_port)
        except OSError as exc:
            parser.error(f"Cannot start local API: {exc}")
    shutdown_event.clear()
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)
    try:
        session.start()
        if server:
            server_thread = threading.Thread(
                target=server.serve_forever, name="monitor-api", daemon=True
            )
            server_thread.start()
            print(f"Device API: http://127.0.0.1:{server.server_port}")
        print(f"Session output: {session.output_dir}")
        threading.Thread(
            target=presentation_worker, args=(session, args), daemon=True
        ).start()
        if args.headless:
            while not shutdown_event.wait(0.2):
                if session.stopped.is_set():
                    break
        else:
            threading.Thread(target=input_worker, args=(session,), daemon=True).start()
            fig = plt.figure(figsize=(10, 6))

            plt.show(block=False)
            while not shutdown_event.is_set() and plt.fignum_exists(fig.number):
                update_graph(0)
                fig.canvas.draw_idle()
                # Pump GUI events with a deadline so shutdown returns to cleanup.
                fig.canvas.start_event_loop(1.0)
    finally:
        shutdown_event.set()
        session.close()
        if server:
            if server_thread:
                server.shutdown()
                server_thread.join()
            server.server_close()
        if plt is not None:
            plt.close("all")


if __name__ == "__main__":
    main()
