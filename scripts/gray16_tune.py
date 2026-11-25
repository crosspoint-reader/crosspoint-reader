#!/usr/bin/env python3
"""Host side of the 16-level gray tuning commands (src/util/Gray16Tuning.h).

  gray16_tune.py bridge PORT DIR      hold PORT open: log what the device prints
                                      to DIR/out.log, send lines added to
                                      DIR/in.txt; stops when DIR/stop exists
  gray16_tune.py send DIR [-w SEC] CMD...
                                      queue commands for the bridge, wait, and
                                      print what the device answered
  gray16_tune.py measure PHOTO X0 X1 Y0 Y1 [-n 16]
                                      lightness and grain of N equal stripes
                                      down the strip X0..X1, Y0..Y1, in the
                                      photo's own pixels (Y0 = top of the first
                                      stripe, Y1 = bottom of the last)

Opening the port resets the device, so run one bridge for a whole session and
talk to it with `send`. bridge needs pyserial (PlatformIO's Python has it);
measure needs Pillow and numpy. See docs/gray16-tuning.md.
"""
import argparse
import os
import sys
import time


def bridge(port, directory):
    import serial

    os.makedirs(directory, exist_ok=True)
    for name in ("in.txt", "out.log"):
        open(os.path.join(directory, name), "w").close()
    stop = os.path.join(directory, "stop")
    if os.path.exists(stop):
        os.remove(stop)
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = port, 115200, 0.2
    link.dtr = link.rts = False
    link.open()
    sent = 0
    with open(os.path.join(directory, "out.log"), "ab", buffering=0) as log:
        while not os.path.exists(stop):
            data = link.read(4096)
            if data:
                log.write(data)
            with open(os.path.join(directory, "in.txt")) as queue:
                lines = queue.read().split("\n")[:-1]
            for line in lines[sent:]:
                link.write((line + "\n").encode())
                link.flush()
                log.write(("\n>>> " + line + "\n").encode())
                time.sleep(0.3)
            sent = len(lines)
    link.close()


def send(directory, commands, wait):
    log = os.path.join(directory, "out.log")
    start = os.path.getsize(log)
    with open(os.path.join(directory, "in.txt"), "a") as queue:
        for command in commands:
            queue.write(command + "\n")
    time.sleep(len(commands) * 0.35 + wait)
    with open(log, "rb") as handle:
        handle.seek(start)
        print(handle.read().decode("utf-8", "replace"))


def measure(photo, x0, x1, y0, y1, stripes):
    import numpy as np
    from PIL import Image

    image = np.asarray(Image.open(photo).convert("L")).astype(float)
    strip = image[y0:y1, x0:x1]
    # The strip spans exactly the stripes, top of the first to bottom of the
    # last; the middle half of each is measured, so small errors do not matter.
    height = (y1 - y0) / stripes
    bounds = [int(round(index * height)) for index in range(stripes + 1)]
    levels, grain = [], []
    for top, bottom in zip(bounds, bounds[1:]):
        margin = (bottom - top) // 4
        patch = strip[top + margin : bottom - margin]
        levels.append(float(np.median(patch)))
        # Fine texture: what is left after removing shading across the patch.
        grain.append(float(np.std(patch - patch.mean(axis=0, keepdims=True) - patch.mean(axis=1, keepdims=True))))
    darkest, lightest = min(levels), max(levels)
    print("stripe  rows        level  lightness  grain")
    for index, (top, bottom) in enumerate(zip(bounds, bounds[1:])):
        lightness = 100 * (levels[index] - darkest) / (lightest - darkest)
        print(f"{index + 1:6d}  {y0 + top:4d}-{y0 + bottom:4d}  {levels[index]:5.0f}  {lightness:9.0f}  {grain[index]:5.2f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    bridge_parser = commands.add_parser("bridge")
    bridge_parser.add_argument("port")
    bridge_parser.add_argument("directory")
    send_parser = commands.add_parser("send")
    send_parser.add_argument("directory")
    send_parser.add_argument("-w", "--wait", type=float, default=3.0)
    send_parser.add_argument("commands", nargs="+")
    measure_parser = commands.add_parser("measure")
    measure_parser.add_argument("photo")
    for name in ("x0", "x1", "y0", "y1"):
        measure_parser.add_argument(name, type=int)
    measure_parser.add_argument("-n", "--stripes", type=int, default=16)
    args = parser.parse_args()
    if args.command == "bridge":
        bridge(args.port, args.directory)
    elif args.command == "send":
        send(args.directory, args.commands, args.wait)
    else:
        measure(args.photo, args.x0, args.x1, args.y0, args.y1, args.stripes)


if __name__ == "__main__":
    sys.exit(main())
