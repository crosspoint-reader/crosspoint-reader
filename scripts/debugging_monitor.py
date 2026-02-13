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

The script will open a matplotlib window showing memory usage over time and provide
an interactive command prompt for sending commands to the device. Press Ctrl-C or
close the graph window to exit gracefully.
"""

from __future__ import annotations

import argparse
import glob
import platform
import re
import signal
import sys
import threading
from collections import deque
from datetime import datetime

# Try to import potentially missing packages
PACKAGE_MAPPING: dict[str, str] = {
    "serial": "pyserial",
    "colorama": "colorama",
    "matplotlib": "matplotlib",
    "PIL": "Pillow",
}

try:
    import matplotlib.pyplot as plt
    import serial
    from colorama import Fore, Style, init
    from matplotlib import animation

    try:
        from PIL import Image
    except ImportError:
        Image = None
except ImportError as e:
    ERROR_MSG = str(e).lower()
    missing_packages = [pkg for mod, pkg in PACKAGE_MAPPING.items() if mod in ERROR_MSG]

    if not missing_packages:
        # Fallback if mapping doesn't cover
        missing_packages = ["pyserial", "colorama", "matplotlib"]

    print("\n" + "!" * 50)
    print(f" Error: Required package(s) not installed: {', '.join(missing_packages)}")
    print("!" * 50)

    print("\nTo fix this, please run the following command in your terminal:\n")
    INSTALL_CMD = "pip install " if sys.platform.startswith("win") else "pip3 install "
    print(f"    {INSTALL_CMD}{' '.join(missing_packages)}")

    print("\nExiting...")
    sys.exit(1)

# --- Global Variables for Data Sharing ---
# Store last 50 data points
MAX_POINTS = 50
time_data: deque[str] = deque(maxlen=MAX_POINTS)
free_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
total_mem_data: deque[float] = deque(maxlen=MAX_POINTS)
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
    plt.close("all")


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


def parse_memory_line(line: str) -> tuple[int | None, int | None]:
    """
    Extracts Free and Total bytes from the specific log line.
    Format: [MEM] Free: 196344 bytes, Total: 226412 bytes, Min Free: 112620 bytes
    """
    # Regex to find 'Free: <digits>' and 'Total: <digits>'
    match = re.search(r"Free:\s*(\d+).*Total:\s*(\d+)", line)
    if match:
        try:
            free_bytes = int(match.group(1))
            total_bytes = int(match.group(2))
            return free_bytes, total_bytes
        except ValueError:
            return None, None
    return None, None


def serial_worker(ser, kwargs: dict[str, str]) -> None:
    """
    Runs in a background thread. Handles reading serial data, printing to console,
    updating memory usage data for graphing, and processing screenshot data.
    Monitors the global shutdown event for graceful termination.
    """
    print(f"{Fore.CYAN}--- Opening serial port ---{Style.RESET_ALL}")
    filter_keyword = kwargs.get("filter", "").lower()
    suppress = kwargs.get("suppress", "").lower()
    if filter_keyword and suppress and filter_keyword == suppress:
        print(
            f"{Fore.YELLOW}Warning: Filter and Suppress keywords are the same. "
            f"This may result in no output.{Style.RESET_ALL}"
        )
    if filter_keyword:
        print(
            f"{Fore.YELLOW}Filtering lines to only show those containing: "
            f"'{filter_keyword}'{Style.RESET_ALL}"
        )
    if suppress:
        print(
            f"{Fore.YELLOW}Suppressing lines containing: '{suppress}'{Style.RESET_ALL}"
        )

    expecting_screenshot = False
    screenshot_size = 0
    screenshot_data = b""

    try:
        while not shutdown_event.is_set():
            if expecting_screenshot:
                data = ser.read(screenshot_size - len(screenshot_data))
                if not data:
                    continue
                screenshot_data += data
                if len(screenshot_data) == screenshot_size:
                    if Image:
                        img = Image.frombytes("1", (800, 480), screenshot_data)
                        # We need to rotate the image because the raw data is in landscape mode
                        img = img.transpose(Image.ROTATE_270)
                        img.save("screenshot.bmp")
                        print(
                            f"{Fore.GREEN}Screenshot saved to screenshot.bmp{Style.RESET_ALL}"
                        )
                    else:
                        with open("screenshot.raw", "wb") as f:
                            f.write(screenshot_data)
                        print(
                            f"{Fore.GREEN}Screenshot saved to screenshot.raw (PIL not available){Style.RESET_ALL}"
                        )
                    expecting_screenshot = False
                    screenshot_data = b""
            else:
                try:
                    raw_data = ser.readline().decode("utf-8", errors="replace")

                    if not raw_data:
                        continue

                    clean_line = raw_data.strip()
                    if not clean_line:
                        continue

                    if clean_line.startswith("SCREENSHOT_START:"):
                        screenshot_size = int(clean_line.split(":")[1])
                        expecting_screenshot = True
                        continue
                    elif clean_line == "SCREENSHOT_END":
                        continue  # ignore

                    # Add PC timestamp
                    pc_time = datetime.now().strftime("%H:%M:%S")
                    formatted_line = re.sub(r"^\[\d+\]", f"[{pc_time}]", clean_line)

                    # Check for Memory Line
                    if "[MEM]" in formatted_line:
                        free_val, total_val = parse_memory_line(formatted_line)
                        if free_val is not None and total_val is not None:
                            with data_lock:
                                time_data.append(pc_time)
                                free_mem_data.append(free_val / 1024)  # Convert to KB
                                total_mem_data.append(total_val / 1024)  # Convert to KB
                    # Apply filters
                    if filter_keyword and filter_keyword not in formatted_line.lower():
                        continue
                    if suppress and suppress in formatted_line.lower():
                        continue
                    # Print to console
                    line_color = get_color_for_line(formatted_line)
                    print(f"{line_color}{formatted_line}")

                except (OSError, UnicodeDecodeError):
                    print(
                        f"{Fore.RED}Device disconnected or data error.{Style.RESET_ALL}"
                    )
                    break
    except KeyboardInterrupt:
        # If thread is killed violently (e.g. main exit), silence errors
        pass
    finally:
        pass  # ser closed in main


def input_worker(ser) -> None:
    """
    Runs in a background thread. Handles user input to send commands to the ESP32 device.
    Monitors the global shutdown event for graceful termination on Ctrl-C.
    """
    while not shutdown_event.is_set():
        try:
            cmd = input("Command: ")
            ser.write(f"CMD:{cmd}\n".encode())
        except (EOFError, KeyboardInterrupt):
            break


def update_graph(frame) -> list:  # pylint: disable=unused-argument
    """
    Called by Matplotlib animation to redraw the memory usage chart.
    Monitors the global shutdown event and closes the plot when shutdown is requested.
    """
    if shutdown_event.is_set():
        plt.close("all")
        return []

    with data_lock:
        if not time_data:
            return []

        # Convert deques to lists for plotting
        x = list(time_data)
        y_free = list(free_mem_data)
        y_total = list(total_mem_data)

    plt.cla()  # Clear axis

    # Plot Total RAM
    plt.plot(x, y_total, label="Total RAM (KB)", color="red", linestyle="--")

    # Plot Free RAM
    plt.plot(x, y_free, label="Free RAM (KB)", color="green", marker="o")

    # Fill area under Free RAM
    plt.fill_between(x, y_free, color="green", alpha=0.1)

    plt.title("ESP32 Memory Monitor")
    plt.ylabel("Memory (KB)")
    plt.xlabel("Time")
    plt.legend(loc="upper left")
    plt.grid(True, linestyle=":", alpha=0.6)

    # Rotate date labels
    plt.xticks(rotation=45, ha="right")
    plt.tight_layout()

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
    """
    Main entry point for the ESP32 monitor application.

    Sets up argument parsing, initializes serial communication, starts background threads
    for serial monitoring and command input, and launches the memory usage graph.
    Implements graceful shutdown handling with signal processing for clean termination.

    Features:
    - Serial port monitoring with color-coded output
    - Real-time memory usage graphing
    - Interactive command interface
    - Screenshot capture capability
    - Graceful shutdown on Ctrl-C or window close
    """
    parser = argparse.ArgumentParser(
        description="ESP32 Serial Monitor with Memory Graph - Real-time monitoring, graphing, and command interface"
    )
    default_baudrate = 115200
    parser.add_argument(
        "port",
        nargs="?",
        default=None,
        help="Serial port (leave empty for autodetection)",
    )
    parser.add_argument(
        "--baud",
        type=int,
        default=default_baudrate,
        help=f"Baud rate (default: {default_baudrate})",
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
    args = parser.parse_args()
    port = args.port
    if port is None:
        port_list = get_auto_detected_port()
        if len(port_list) == 1:
            port = port_list[0]
            print(f"{Fore.CYAN}Auto-detected serial port: {port}{Style.RESET_ALL}")
        elif len(port_list) > 1:
            print(f"{Fore.YELLOW}Multiple serial ports found:{Style.RESET_ALL}")
            for p in port_list:
                print(f"  - {p}")
            print(
                f"{Fore.YELLOW}Please specify the desired port as a command-line argument.{Style.RESET_ALL}"
            )
    if port is None:
        print(f"{Fore.RED}Error: No suitable serial port found.{Style.RESET_ALL}")
        sys.exit(1)

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
        ser.dtr = False
        ser.rts = False
    except serial.SerialException as e:
        print(f"{Fore.RED}Error opening port: {e}{Style.RESET_ALL}")
        return

    # Set up signal handler for graceful shutdown
    signal.signal(signal.SIGINT, signal_handler)

    # 1. Start the Serial Reader in a separate thread
    # Daemon=True means this thread dies when the main program closes
    myargs = vars(args)  # Convert Namespace to dict for easier passing
    t = threading.Thread(target=serial_worker, args=(ser, myargs), daemon=True)
    t.start()

    # Start input thread
    input_thread = threading.Thread(target=input_worker, args=(ser,), daemon=True)
    input_thread.start()

    # 2. Set up the Graph (Main Thread)
    try:
        import matplotlib.style as mplstyle  # pylint: disable=import-outside-toplevel

        default_styles = (
            "light_background",
            "ggplot",
            "seaborn",
            "dark_background",
        )
        styles = list(mplstyle.available)
        for default_style in default_styles:
            if default_style in styles:
                print(
                    f"\n{Fore.CYAN}--- Using Matplotlib style: {default_style} ---{Style.RESET_ALL}"
                )
                mplstyle.use(default_style)
                break
    except (AttributeError, ValueError):
        pass

    fig = plt.figure(figsize=(10, 6))

    # Update graph every 1000ms
    _ = animation.FuncAnimation(
        fig, update_graph, interval=1000, cache_frame_data=False
    )

    try:
        print(
            f"{Fore.YELLOW}Starting Graph Window... (Close window or press Ctrl-C to exit){Style.RESET_ALL}"
        )
        plt.show()
    except KeyboardInterrupt:
        print(f"\n{Fore.YELLOW}Exiting...{Style.RESET_ALL}")
    finally:
        shutdown_event.set()  # Ensure all threads know to stop
        plt.close("all")  # Force close any lingering plot windows


if __name__ == "__main__":
    main()
