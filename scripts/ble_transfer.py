#!/usr/bin/env python3
"""Upload Marginalia package archives over Bluetooth LE."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import struct
import sys
from pathlib import Path
from typing import Any

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError as exc:  # pragma: no cover - exercised by users without deps installed.
    raise SystemExit("Missing dependency: install bleak with `python3 -m pip install bleak`.") from exc


SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10"
CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10"
DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10"
STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10"
DEVICE_NAME = "Marginalia Transfer"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def decode_status(data: bytearray | bytes) -> dict[str, Any]:
    try:
        return json.loads(bytes(data).decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {"state": "unknown", "raw": bytes(data).decode("utf-8", errors="replace")}


async def find_device(timeout: float):
    print(f"Scanning for {DEVICE_NAME}...")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    service_uuid = SERVICE_UUID.lower()
    for device, advertisement in devices.values():
        advertised_uuids = {uuid.lower() for uuid in advertisement.service_uuids}
        if service_uuid in advertised_uuids:
            return device
    devices = [device for device, _ in devices.values()]
    for device in devices:
        if device.name == DEVICE_NAME:
            return device
    for device in devices:
        if device.name and "Marginalia" in device.name:
            return device
    return None


async def write_json(client: BleakClient, payload: dict[str, Any]) -> None:
    await client.write_gatt_char(CONTROL_UUID, json.dumps(payload, separators=(",", ":")).encode("utf-8"), response=True)


async def put_package(args: argparse.Namespace) -> int:
    archive = Path(args.archive).expanduser().resolve()
    if not archive.is_file():
        print(f"Archive not found: {archive}", file=sys.stderr)
        return 2
    if not archive.name.endswith(".mpkg.zip"):
        print("Archive filename must end with .mpkg.zip", file=sys.stderr)
        return 2

    size = archive.stat().st_size
    digest = sha256_file(archive)
    final_status: dict[str, Any] = {}
    status_event = asyncio.Event()
    done = asyncio.Event()

    def on_status(_: Any, data: bytearray) -> None:
        nonlocal final_status
        final_status = decode_status(data)
        state = final_status.get("state", "?")
        received = final_status.get("received")
        total = final_status.get("size")
        if received is not None and total:
            print(f"\r{state}: {received}/{total} bytes", end="", flush=True)
        else:
            print(f"\n{state}: {final_status}")
        status_event.set()
        if state in {"installed", "error"}:
            done.set()

    async def wait_for_status(states: set[str], timeout: float) -> dict[str, Any]:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            if final_status.get("state") in states:
                return final_status
            status_event.clear()
            remaining = deadline - asyncio.get_running_loop().time()
            try:
                await asyncio.wait_for(status_event.wait(), timeout=min(0.25, max(0.0, remaining)))
            except asyncio.TimeoutError:
                pass
        return final_status

    try:
        device = await find_device(args.scan_timeout)
    except (BleakError, OSError) as exc:
        print(f"BLE scan failed: {exc}", file=sys.stderr)
        return 1
    if device is None:
        print("No Marginalia BLE transfer device found.", file=sys.stderr)
        return 1

    print(f"Connecting to {device.name or device.address}...")
    try:
        async with BleakClient(device) as client:
            await client.start_notify(STATUS_UUID, on_status)
            try:
                await write_json(client, {"op": "hello", "version": 1, "code": args.code})
                hello_status = await wait_for_status({"connected", "error"}, args.control_timeout)
                if hello_status.get("state") == "error":
                    print(f"\nDevice rejected session: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if hello_status.get("state") != "connected":
                    print("\nTimed out waiting for device session confirmation.", file=sys.stderr)
                    return 1

                await write_json(
                    client,
                    {"op": "start_put", "kind": "package", "name": archive.name, "size": size, "sha256": digest},
                )
                start_status = await wait_for_status({"receiving", "error"}, args.control_timeout)
                if start_status.get("state") == "error":
                    print(f"\nDevice rejected transfer: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if start_status.get("state") != "receiving":
                    print("\nTimed out waiting for device transfer start.", file=sys.stderr)
                    return 1

                sequence = 0
                with archive.open("rb") as handle:
                    while True:
                        payload = handle.read(args.chunk_size)
                        if not payload:
                            break
                        frame = struct.pack("<I", sequence) + payload
                        await client.write_gatt_char(DATA_IN_UUID, frame, response=False)
                        sequence += 1
                        if args.chunk_delay > 0:
                            await asyncio.sleep(args.chunk_delay)

                await write_json(client, {"op": "commit"})
                try:
                    await asyncio.wait_for(done.wait(), timeout=args.install_timeout)
                except asyncio.TimeoutError:
                    print("\nTimed out waiting for install result.", file=sys.stderr)
                    return 1
            finally:
                try:
                    await client.stop_notify(STATUS_UUID)
                except (BleakError, OSError) as exc:
                    print(f"\nWarning: failed to stop BLE notifications: {exc}", file=sys.stderr)
    except (BleakError, OSError) as exc:
        print(f"\nBLE transfer failed: {exc}", file=sys.stderr)
        return 1

    print()
    if final_status.get("state") == "installed":
        print(f"Installed {final_status.get('name') or final_status.get('package') or archive.name}")
        return 0
    print(f"Transfer failed: {final_status.get('error') or final_status}", file=sys.stderr)
    return 1


def six_digit_code(value: str) -> str:
    if len(value) != 6 or not value.isdigit():
        raise argparse.ArgumentTypeError("must be exactly 6 digits")
    return value


def positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than 0")
    return parsed


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    put = sub.add_parser("put-package", help="Upload and install a .mpkg.zip archive")
    put.add_argument("archive", help="Path to the .mpkg.zip archive")
    put.add_argument("--code", required=True, type=six_digit_code, help="Six-digit code shown on the device")
    put.add_argument("--chunk-size", type=positive_int, default=160, help="Payload bytes per BLE data frame")
    put.add_argument("--chunk-delay", type=float, default=0.0, help="Delay between chunk writes, in seconds")
    put.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    put.add_argument("--control-timeout", type=float, default=5.0, help="Control response timeout, in seconds")
    put.add_argument("--install-timeout", type=float, default=60.0, help="Install result timeout, in seconds")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "put-package":
        return asyncio.run(put_package(args))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
