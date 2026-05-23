#!/usr/bin/env python3
"""Transfer CrossPoint Reader files over Bluetooth LE."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import platform
import secrets
import struct
import sys
import uuid
from pathlib import Path
from typing import Any, Callable

try:
    from bleak import BleakClient, BleakScanner
except ImportError as exc:  # pragma: no cover - exercised by users without deps installed.
    raise SystemExit("Missing dependency: install bleak with `python3 -m pip install bleak`.") from exc


SERVICE_UUID = "6f9f0a00-9b1d-4d1f-9f53-5b6b8b3d0f10"
CONTROL_UUID = "6f9f0a01-9b1d-4d1f-9f53-5b6b8b3d0f10"
DATA_IN_UUID = "6f9f0a02-9b1d-4d1f-9f53-5b6b8b3d0f10"
STATUS_UUID = "6f9f0a03-9b1d-4d1f-9f53-5b6b8b3d0f10"
DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10"
DEVICE_NAME = "CrossPoint Transfer"
LEGACY_DEVICE_NAME = "Marginalia Transfer"
CONFIG_PATH = Path(os.environ.get("CROSSPOINT_BLE_CONFIG", "~/.config/crosspoint/ble_hosts.json")).expanduser()
DATA_FRAME_HEADER_BYTES = 4
DEFAULT_UPLOAD_CHUNK_BYTES = 160
DEFAULT_FIRMWARE_UPLOAD_CHUNK_BYTES = 500
DEFAULT_WINDOW_BYTES = 24_000
DEFAULT_FIRMWARE_WINDOW_BYTES = 16 * 1024
DEFAULT_DOWNLOAD_CHUNK_BYTES = 160
PROGRESS_PRINT_BYTES = 4096
PROGRESS_PRINT_SECONDS = 1.0


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


def load_ble_config() -> dict[str, Any]:
    try:
        with CONFIG_PATH.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        data = {}
    if not isinstance(data, dict):
        data = {}
    data.setdefault("devices", {})
    if not isinstance(data["devices"], dict):
        data["devices"] = {}
    return data


def save_ble_config(config: dict[str, Any]) -> None:
    CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG_PATH.with_suffix(CONFIG_PATH.suffix + ".tmp")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        json.dump(config, handle, indent=2, sort_keys=True)
        handle.write("\n")
        if hasattr(os, "fchmod"):
            try:
                os.fchmod(handle.fileno(), 0o600)
            except OSError:
                pass
    tmp.replace(CONFIG_PATH)
    CONFIG_PATH.chmod(0o600)


def get_host_identity(config: dict[str, Any]) -> tuple[str, str]:
    host_id = config.get("host_id")
    if not isinstance(host_id, str) or not host_id:
        host_id = str(uuid.uuid4())
        config["host_id"] = host_id
    host_name = config.get("host_name")
    if not isinstance(host_name, str) or not host_name:
        host_name = (platform.node() or "CrossPoint host")[:48]
        config["host_name"] = host_name
    return host_id, host_name


def trusted_response(secret: str, device_nonce: str, host_id: str) -> str:
    message = f"{device_nonce}|{host_id}|1".encode("utf-8")
    return hmac.new(secret.encode("utf-8"), message, hashlib.sha256).hexdigest()


def resolve_upload_chunk_size(client: BleakClient, requested: int, write_mode: str) -> int:
    if write_mode == "response":
        return requested
    characteristic = client.services.get_characteristic(DATA_IN_UUID)
    max_write = getattr(characteristic, "max_write_without_response_size", None) if characteristic else None
    if not isinstance(max_write, int) or max_write <= DATA_FRAME_HEADER_BYTES:
        return min(requested, 20 - DATA_FRAME_HEADER_BYTES)
    max_payload = max_write - DATA_FRAME_HEADER_BYTES
    if requested > max_payload:
        print(f"Reducing BLE chunk size to {max_payload} bytes for this connection.")
    return min(requested, max_payload)


def remember_trusted_host(
    config: dict[str, Any],
    device_id: str,
    host_id: str,
    host_name: str,
    secret: str,
    *,
    transfer_label: str,
) -> None:
    config["devices"][device_id] = {"host_id": host_id, "host_name": host_name, "secret": secret}
    try:
        save_ble_config(config)
    except OSError as exc:
        print(
            f"Warning: {transfer_label} succeeded, but failed to save trusted host for {device_id}: {exc}",
            file=sys.stderr,
        )
    else:
        print(f"Saved trusted host for {device_id}")


def warn_pairing_not_saved(status: dict[str, Any], *, transfer_label: str) -> None:
    pairing = status.get("pairing")
    if pairing:
        print(f"Warning: {transfer_label} succeeded, but trusted-host pairing was {pairing}.", file=sys.stderr)
    else:
        print(f"Warning: {transfer_label} succeeded, but the reader did not confirm trusted-host pairing.", file=sys.stderr)


async def find_device(timeout: float):
    print(f"Scanning for {DEVICE_NAME}...")
    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    service_uuid = SERVICE_UUID.lower()
    for device, advertisement in devices.values():
        advertised_uuids = {item.lower() for item in advertisement.service_uuids}
        if service_uuid in advertised_uuids:
            return device
    for device, _ in devices.values():
        if device.name in {DEVICE_NAME, LEGACY_DEVICE_NAME}:
            return device
    for device, _ in devices.values():
        if device.name and "Transfer" in device.name:
            return device
    return None


async def write_json(client: BleakClient, payload: dict[str, Any]) -> None:
    await client.write_gatt_char(CONTROL_UUID, json.dumps(payload, separators=(",", ":")).encode("utf-8"), response=True)


def print_progress(state: str, current: int, total: int, last: tuple[int, float]) -> tuple[int, float]:
    loop_time = asyncio.get_running_loop().time()
    last_bytes, last_time = last
    if (
        current == total
        or current == 0
        or current - last_bytes >= PROGRESS_PRINT_BYTES
        or loop_time - last_time >= PROGRESS_PRINT_SECONDS
    ):
        print(f"\r{state}: {current}/{total} bytes", end="", flush=True)
        return current, loop_time
    return last


async def authorize_session(
    client: BleakClient,
    args: argparse.Namespace,
    *,
    get_status: Callable[[], dict[str, Any]],
    get_status_version: Callable[[], int],
    status_event: asyncio.Event,
    config: dict[str, Any],
    host_id: str,
    host_name: str,
) -> tuple[bool, str | None, str | None]:
    async def wait_for_status(predicate, timeout: float, after_version: int = -1) -> dict[str, Any]:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            current_status = get_status()
            if get_status_version() > after_version and predicate(current_status):
                return current_status
            status_event.clear()
            remaining = deadline - asyncio.get_running_loop().time()
            try:
                await asyncio.wait_for(status_event.wait(), timeout=min(0.25, max(0.0, remaining)))
            except asyncio.TimeoutError:
                pass
        return get_status()

    final_status = get_status()
    device_id = final_status.get("device_id")
    device_nonce = final_status.get("device_nonce")
    if not args.force_code and isinstance(device_id, str) and isinstance(device_nonce, str):
        trusted = config["devices"].get(device_id)
        if isinstance(trusted, dict) and isinstance(trusted.get("secret"), str):
            response = trusted_response(str(trusted["secret"]), device_nonce, str(trusted.get("host_id", host_id)))
            trusted_version = get_status_version()
            await write_json(
                client,
                {
                    "op": "hello",
                    "version": 1,
                    "host_id": trusted.get("host_id", host_id),
                    "response": response,
                },
            )
            status = await wait_for_status(
                lambda candidate: bool(candidate.get("trusted_host")) or candidate.get("state") == "error",
                args.control_timeout,
                trusted_version,
            )
            if status.get("trusted_host"):
                print(f"Trusted host accepted: {status['trusted_host']}")
                return True, None, None
            print("Trusted host auth was not accepted; using visible code.", file=sys.stderr)

    if not args.code:
        print("No trusted host was accepted; pass --code with the six-digit device code.", file=sys.stderr)
        return False, None, None
    if not args.code.isdigit() or len(args.code) != 6:
        print("--code must be six digits.", file=sys.stderr)
        return False, None, None

    secret = secrets.token_hex(32)
    code_version = get_status_version()
    await write_json(
        client,
        {
            "op": "hello",
            "version": 1,
            "code": args.code,
            "pair_host_id": host_id,
            "pair_host_name": host_name,
            "pair_secret": secret,
        },
    )
    status = await wait_for_status(
        lambda candidate: candidate.get("state") in {"connected", "error"},
        args.control_timeout,
        code_version,
    )
    if status.get("state") == "error":
        print(f"Authorization failed: {status.get('error')}", file=sys.stderr)
        return False, None, None
    return True, str(status.get("device_id") or device_id or ""), secret


async def put_file(args: argparse.Namespace, *, kind: str, suffix: str, success_states: set[str]) -> int:
    source = Path(args.path).expanduser().resolve()
    if not source.is_file():
        print(f"File not found: {source}", file=sys.stderr)
        return 2
    if not source.name.lower().endswith(suffix):
        print(f"Filename must end with {suffix}", file=sys.stderr)
        return 2

    size = source.stat().st_size
    digest = sha256_file(source)
    final_status: dict[str, Any] = {}
    status_version = 0
    status_event = asyncio.Event()
    done = asyncio.Event()
    transfer_started = False
    pending_pair_device: str | None = None
    pending_pair_secret: str | None = None
    config = load_ble_config()
    host_id, host_name = get_host_identity(config)
    progress_state = (-PROGRESS_PRINT_BYTES, 0.0)

    def on_status(_: Any, data: bytearray) -> None:
        nonlocal final_status, progress_state, status_version
        final_status = decode_status(data)
        status_version += 1
        state = final_status.get("state", "?")
        current = final_status.get("received", final_status.get("written"))
        total = final_status.get("size")
        if isinstance(current, int) and isinstance(total, int) and total > 0:
            progress_state = print_progress(str(state), current, total, progress_state)
        else:
            print(f"\n{state}: {final_status}")
        status_event.set()
        if transfer_started and (state in success_states or state == "error"):
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

    async def wait_for_received(min_received: int, timeout: float) -> bool:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            state = final_status.get("state")
            if state == "error":
                return False
            received = final_status.get("received")
            if isinstance(received, int) and received >= min_received:
                return True
            status_event.clear()
            remaining = deadline - asyncio.get_running_loop().time()
            try:
                await asyncio.wait_for(status_event.wait(), timeout=min(0.5, max(0.0, remaining)))
            except asyncio.TimeoutError:
                pass
        return False

    device = await find_device(args.scan_timeout)
    if not device:
        print("No CrossPoint BLE transfer device found.", file=sys.stderr)
        return 1

    try:
        async with BleakClient(device, timeout=args.connect_timeout) as client:
            await client.start_notify(STATUS_UUID, on_status)
            final_status = decode_status(await client.read_gatt_char(STATUS_UUID))
            status_version += 1
            status_event.set()

            authorized, pending_pair_device, pending_pair_secret = await authorize_session(
                client,
                args,
                get_status=lambda: final_status,
                get_status_version=lambda: status_version,
                status_event=status_event,
                config=config,
                host_id=host_id,
                host_name=host_name,
            )
            if not authorized:
                return 1

            await write_json(
                client,
                {
                    "op": "start_put",
                    "kind": kind,
                    "name": source.name,
                    "size": size,
                    "sha256": digest,
                    "resume": False,
                    "chunk_size": args.chunk_size,
                    "ack_bytes": args.ack_bytes,
                },
            )
            started = await wait_for_status({"receiving", "error"}, args.control_timeout)
            if started.get("state") == "error":
                print(f"Transfer failed: {started.get('error')}", file=sys.stderr)
                return 1

            transfer_started = True
            chunk_size = resolve_upload_chunk_size(client, args.chunk_size, args.write_mode)
            sent = 0
            sequence = 0
            ack_floor = 0
            with source.open("rb") as handle:
                while True:
                    chunk = handle.read(chunk_size)
                    if not chunk:
                        break
                    frame = struct.pack("<I", sequence) + chunk
                    await client.write_gatt_char(DATA_IN_UUID, frame, response=args.write_mode == "response")
                    sent += len(chunk)
                    sequence += 1
                    if sent - ack_floor >= args.ack_bytes:
                        ack_floor = sent
                        if not await wait_for_received(ack_floor, args.data_timeout):
                            raise RuntimeError(f"Timed out waiting for receiver ACK at {ack_floor} bytes")
            if not await wait_for_received(sent, args.data_timeout):
                raise RuntimeError("Timed out waiting for final receiver ACK")

            await write_json(client, {"op": "commit"})
            try:
                await asyncio.wait_for(done.wait(), timeout=args.commit_timeout)
            except asyncio.TimeoutError:
                print("\nTimed out waiting for commit.", file=sys.stderr)
                return 1
    except Exception as exc:
        print(f"\nBLE transfer failed: {exc}", file=sys.stderr)
        return 1

    print()
    if final_status.get("state") not in success_states:
        print(f"Transfer failed: {final_status.get('error') or final_status}", file=sys.stderr)
        return 1

    print(f"Transfer complete: {final_status.get('state')}")
    if pending_pair_device and pending_pair_secret and final_status.get("paired") is True:
        remember_trusted_host(
            config,
            pending_pair_device,
            host_id,
            host_name,
            pending_pair_secret,
            transfer_label=f"{kind} transfer",
        )
    elif pending_pair_device and pending_pair_secret:
        warn_pairing_not_saved(final_status, transfer_label=f"{kind} transfer")
    return 0


async def get_crash_report(args: argparse.Namespace) -> int:
    target = Path(args.output).expanduser().resolve()
    part = target.with_suffix(target.suffix + ".part")
    final_status: dict[str, Any] = {}
    status_version = 0
    status_event = asyncio.Event()
    done = asyncio.Event()
    config = load_ble_config()
    host_id, host_name = get_host_identity(config)
    chunks: list[bytes] = []
    data_error: str | None = None
    expected_sequence = 0
    received = 0

    def on_status(_: Any, data: bytearray) -> None:
        nonlocal final_status, status_version
        final_status = decode_status(data)
        status_version += 1
        state = final_status.get("state", "?")
        sent = final_status.get("sent")
        total = final_status.get("size")
        if isinstance(sent, int) and isinstance(total, int) and total > 0:
            print(f"\r{state}: {sent}/{total} bytes", end="", flush=True)
        else:
            print(f"\n{state}: {final_status}")
        status_event.set()
        if state in {"sent", "error"}:
            done.set()

    async def on_data(_: Any, data: bytearray) -> None:
        nonlocal data_error, expected_sequence, received
        try:
            if len(data) <= DATA_FRAME_HEADER_BYTES:
                raise RuntimeError("Invalid data-out frame")
            sequence = struct.unpack("<I", bytes(data[:DATA_FRAME_HEADER_BYTES]))[0]
            if sequence != expected_sequence:
                raise RuntimeError(f"Unexpected download sequence {sequence}, expected {expected_sequence}")
            payload = bytes(data[DATA_FRAME_HEADER_BYTES:])
            chunks.append(payload)
            received += len(payload)
            expected_sequence += 1
            await write_json(client_ref["client"], {"op": "get_ack", "sequence": sequence})
        except Exception as exc:
            data_error = str(exc)
            done.set()

    pending_data_tasks: set[asyncio.Task[None]] = set()

    def handle_data(char: Any, data: bytearray) -> None:
        task = asyncio.create_task(on_data(char, data))
        pending_data_tasks.add(task)
        task.add_done_callback(pending_data_tasks.discard)

    client_ref: dict[str, BleakClient] = {}
    device = await find_device(args.scan_timeout)
    if not device:
        print("No CrossPoint BLE transfer device found.", file=sys.stderr)
        return 1

    try:
        async with BleakClient(device, timeout=args.connect_timeout) as client:
            client_ref["client"] = client
            await client.start_notify(STATUS_UUID, on_status)
            await client.start_notify(DATA_OUT_UUID, handle_data)
            final_status = decode_status(await client.read_gatt_char(STATUS_UUID))
            status_version += 1
            status_event.set()

            authorized, _, _ = await authorize_session(
                client,
                args,
                get_status=lambda: final_status,
                get_status_version=lambda: status_version,
                status_event=status_event,
                config=config,
                host_id=host_id,
                host_name=host_name,
            )
            if not authorized:
                return 1

            await write_json(client, {"op": "start_get", "kind": "crash_report", "chunk_size": args.chunk_size})
            try:
                await asyncio.wait_for(done.wait(), timeout=args.download_timeout)
                if pending_data_tasks:
                    await asyncio.gather(*pending_data_tasks, return_exceptions=True)
            except asyncio.TimeoutError:
                print("\nTimed out waiting for crash report.", file=sys.stderr)
                return 1
    except Exception as exc:
        print(f"\nBLE transfer failed: {exc}", file=sys.stderr)
        return 1

    print()
    if data_error:
        print(f"Download failed: {data_error}", file=sys.stderr)
        return 1
    if final_status.get("state") != "sent":
        print(f"Download failed: {final_status.get('error') or final_status}", file=sys.stderr)
        return 1
    expected = final_status.get("size")
    if isinstance(expected, int) and received != expected:
        print(f"Download failed: received {received}, expected {expected}", file=sys.stderr)
        return 1

    target.parent.mkdir(parents=True, exist_ok=True)
    with part.open("wb") as handle:
        for chunk in chunks:
            handle.write(chunk)
    part.replace(target)
    print(f"Downloaded crash report to {target}")
    return 0


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--code", help="Six-digit code shown on the device")
    parser.add_argument("--force-code", action="store_true", help="Skip trusted-host auth and use --code")
    parser.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    parser.add_argument("--connect-timeout", type=float, default=20.0, help="BLE connection timeout, in seconds")
    parser.add_argument("--control-timeout", type=float, default=8.0, help="Control operation timeout, in seconds")


def add_upload_common(parser: argparse.ArgumentParser, *, firmware: bool = False) -> None:
    add_common(parser)
    parser.add_argument("--chunk-size", type=int, default=DEFAULT_FIRMWARE_UPLOAD_CHUNK_BYTES if firmware else DEFAULT_UPLOAD_CHUNK_BYTES)
    parser.add_argument("--ack-bytes", type=int, default=DEFAULT_FIRMWARE_WINDOW_BYTES if firmware else DEFAULT_WINDOW_BYTES)
    parser.add_argument("--write-mode", choices=["response", "no-response"], default="no-response")
    parser.add_argument("--data-timeout", type=float, default=20.0)
    parser.add_argument("--commit-timeout", type=float, default=240.0 if firmware else 120.0)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Transfer files to a CrossPoint Reader over BLE")
    sub = parser.add_subparsers(dest="command", required=True)

    put_book = sub.add_parser("put-book", help="Upload an EPUB book")
    put_book.add_argument("path")
    add_upload_common(put_book)

    put_bmp = sub.add_parser("put-bmp", help="Upload a BMP image")
    put_bmp.add_argument("path")
    add_upload_common(put_bmp)

    put_firmware = sub.add_parser("put-firmware", help="Upload and install a firmware .bin")
    put_firmware.add_argument("path")
    add_upload_common(put_firmware, firmware=True)

    get_crash = sub.add_parser("get-crash-report", help="Download /crash_report.txt")
    get_crash.add_argument("output")
    add_common(get_crash)
    get_crash.add_argument("--chunk-size", type=int, default=DEFAULT_DOWNLOAD_CHUNK_BYTES)
    get_crash.add_argument("--download-timeout", type=float, default=90.0)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "put-book":
        return asyncio.run(put_file(args, kind="book", suffix=".epub", success_states={"saved"}))
    if args.command == "put-bmp":
        return asyncio.run(put_file(args, kind="bmp", suffix=".bmp", success_states={"saved"}))
    if args.command == "put-firmware":
        return asyncio.run(put_file(args, kind="firmware", suffix=".bin", success_states={"restarting"}))
    if args.command == "get-crash-report":
        return asyncio.run(get_crash_report(args))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
