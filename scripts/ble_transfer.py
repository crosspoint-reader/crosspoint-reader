#!/usr/bin/env python3
"""Upload Marginalia files over Bluetooth LE."""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import os
import platform
import re
import secrets
import struct
import sys
import uuid
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
DATA_OUT_UUID = "6f9f0a04-9b1d-4d1f-9f53-5b6b8b3d0f10"
DEVICE_NAME = "Marginalia Transfer"
PROGRESS_PRINT_BYTES = 4096
PROGRESS_PRINT_SECONDS = 1.0
DEFAULT_WINDOW_BYTES = 4096
CONFIG_PATH = Path(os.environ.get("MARGINALIA_BLE_CONFIG", "~/.config/marginalia/ble_hosts.json")).expanduser()
PACKAGE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{1,95}$")


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
        os.fchmod(handle.fileno(), 0o600)
    tmp.replace(CONFIG_PATH)
    CONFIG_PATH.chmod(0o600)


def get_host_identity(config: dict[str, Any]) -> tuple[str, str]:
    host_id = config.get("host_id")
    if not isinstance(host_id, str) or not host_id:
        host_id = str(uuid.uuid4())
        config["host_id"] = host_id
    host_name = config.get("host_name")
    if not isinstance(host_name, str) or not host_name:
        host_name = (platform.node() or "Marginalia host")[:48]
        config["host_name"] = host_name
    return host_id, host_name


def trusted_response(secret: str, device_nonce: str, host_id: str) -> str:
    message = f"{device_nonce}|{host_id}|1".encode("utf-8")
    return hmac.new(secret.encode("utf-8"), message, hashlib.sha256).hexdigest()


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
        print(f"Warning: {transfer_label} succeeded, but the device did not confirm trusted-host pairing.", file=sys.stderr)


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


async def put_file(
    args: argparse.Namespace,
    *,
    kind: str,
    path_arg: str,
    required_suffix: str,
    success_states: set[str],
) -> int:
    source = Path(path_arg).expanduser().resolve()
    if not source.is_file():
        print(f"File not found: {source}", file=sys.stderr)
        return 2
    if not source.name.lower().endswith(required_suffix):
        print(f"Filename must end with {required_suffix}", file=sys.stderr)
        return 2

    size = source.stat().st_size
    digest = sha256_file(source)
    final_status: dict[str, Any] = {}
    status_event = asyncio.Event()
    done = asyncio.Event()
    transfer_started = False
    pending_pair_device: str | None = None
    pending_pair_secret: str | None = None
    config = load_ble_config()
    host_id, host_name = get_host_identity(config)
    last_print_received = -PROGRESS_PRINT_BYTES
    last_print_time = 0.0

    def on_status(_: Any, data: bytearray) -> None:
        nonlocal final_status, last_print_received, last_print_time
        final_status = decode_status(data)
        state = final_status.get("state", "?")
        received = final_status.get("received")
        total = final_status.get("size")
        if received is not None and total:
            now = asyncio.get_running_loop().time()
            is_final = state in success_states or state == "error" or received == total
            should_print = (
                is_final
                or received == 0
                or received - last_print_received >= PROGRESS_PRINT_BYTES
                or now - last_print_time >= PROGRESS_PRINT_SECONDS
            )
            if should_print:
                print(f"\r{state}: {received}/{total} bytes", end="", flush=True)
                last_print_received = received
                last_print_time = now
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

    async def wait_for_trusted_auth(timeout: float) -> dict[str, Any]:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            if final_status.get("state") == "error":
                return final_status
            if final_status.get("state") == "connected" and final_status.get("trusted_host"):
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
                try:
                    on_status(None, await client.read_gatt_char(STATUS_UUID))
                except (BleakError, OSError):
                    pass

                device_id = final_status.get("device_id")
                device_nonce = final_status.get("device_nonce")
                trusted = config["devices"].get(device_id) if isinstance(device_id, str) else None
                used_trusted_auth = False
                if (
                    trusted
                    and isinstance(trusted, dict)
                    and isinstance(device_nonce, str)
                    and not args.force_code
                ):
                    response = trusted_response(str(trusted.get("secret", "")), device_nonce, str(trusted.get("host_id", host_id)))
                    final_status = {}
                    status_event.clear()
                    await write_json(
                        client,
                        {
                            "op": "hello",
                            "version": 1,
                            "host_id": trusted.get("host_id", host_id),
                            "response": response,
                        },
                    )
                    hello_status = await wait_for_trusted_auth(args.control_timeout)
                    used_trusted_auth = hello_status.get("state") == "connected" and hello_status.get("trusted_host")
                    if not used_trusted_auth:
                        print("\nTrusted-host auth failed; falling back to code.", file=sys.stderr)

                if not used_trusted_auth:
                    if not args.code:
                        print("\nNo trusted host was accepted; pass --code with the six-digit device code.", file=sys.stderr)
                        return 1
                    final_status = {}
                    status_event.clear()
                    hello_payload: dict[str, Any] = {"op": "hello", "version": 1, "code": args.code}
                    if not args.no_remember_host and isinstance(device_id, str):
                        pending_pair_device = device_id
                        pending_pair_secret = secrets.token_hex(32)
                        hello_payload.update(
                            {
                                "pair_host_id": host_id,
                                "pair_host_name": host_name,
                                "pair_secret": pending_pair_secret,
                            }
                        )
                    await write_json(client, hello_payload)
                hello_status = await wait_for_status({"connected", "error"}, args.control_timeout)
                if hello_status.get("state") == "error":
                    print(f"\nDevice rejected session: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if hello_status.get("state") != "connected":
                    print("\nTimed out waiting for device session confirmation.", file=sys.stderr)
                    return 1

                await write_json(
                    client,
                    {"op": "start_put", "kind": kind, "name": source.name, "size": size, "sha256": digest},
                )
                start_status = await wait_for_status({"receiving", "error"}, args.control_timeout)
                if start_status.get("state") == "error":
                    print(f"\nDevice rejected transfer: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if start_status.get("state") != "receiving":
                    print("\nTimed out waiting for device transfer start.", file=sys.stderr)
                    return 1

                transfer_started = True
                sequence = 0
                sent_bytes = 0
                ack_floor = 0
                with source.open("rb") as handle:
                    while True:
                        payload = handle.read(args.chunk_size)
                        if not payload:
                            break
                        frame = struct.pack("<I", sequence) + payload
                        await client.write_gatt_char(DATA_IN_UUID, frame, response=args.transfer_mode == "response")
                        sequence += 1
                        sent_bytes += len(payload)
                        if args.transfer_mode == "windowed" and sent_bytes - ack_floor >= args.window_bytes:
                            ack_floor = sent_bytes
                            if not await wait_for_received(ack_floor, args.control_timeout):
                                error = final_status.get("error") or f"receiver did not acknowledge {ack_floor} bytes"
                                print(f"\nTransfer failed: {error}", file=sys.stderr)
                                return 1
                        if args.chunk_delay > 0:
                            await asyncio.sleep(args.chunk_delay)
                if args.transfer_mode == "windowed" and not await wait_for_received(sent_bytes, args.control_timeout):
                    error = final_status.get("error") or f"receiver did not acknowledge {sent_bytes} bytes"
                    print(f"\nTransfer failed: {error}", file=sys.stderr)
                    return 1

                await write_json(client, {"op": "commit"})
                if pending_pair_device and pending_pair_secret:
                    print("\nApprove Save Host on the device to remember this computer.")
                try:
                    await asyncio.wait_for(done.wait(), timeout=args.install_timeout)
                except asyncio.TimeoutError:
                    print("\nTimed out waiting for transfer result.", file=sys.stderr)
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
        if final_status.get("paired") and pending_pair_device and pending_pair_secret:
            remember_trusted_host(
                config,
                pending_pair_device,
                host_id,
                host_name,
                pending_pair_secret,
                transfer_label="install",
            )
        elif pending_pair_device and pending_pair_secret:
            warn_pairing_not_saved(final_status, transfer_label="install")
        print(f"Installed {final_status.get('name') or final_status.get('package') or source.name}")
        return 0
    if final_status.get("state") == "saved":
        if final_status.get("paired") and pending_pair_device and pending_pair_secret:
            remember_trusted_host(
                config,
                pending_pair_device,
                host_id,
                host_name,
                pending_pair_secret,
                transfer_label="save",
            )
        elif pending_pair_device and pending_pair_secret:
            warn_pairing_not_saved(final_status, transfer_label="save")
        print(f"Saved {final_status.get('path') or final_status.get('name') or source.name}")
        return 0
    print(f"Transfer failed: {final_status.get('error') or final_status}", file=sys.stderr)
    return 1


async def put_package(args: argparse.Namespace) -> int:
    return await put_file(
        args,
        kind="package",
        path_arg=args.archive,
        required_suffix=".mpkg.zip",
        success_states={"installed"},
    )


async def put_book(args: argparse.Namespace) -> int:
    return await put_file(
        args,
        kind="book",
        path_arg=args.book,
        required_suffix=".epub",
        success_states={"saved"},
    )


async def get_ble_download(
    args: argparse.Namespace,
    *,
    output_path: Path,
    start_payload: dict[str, Any],
    timeout_label: str,
) -> int:
    output = output_path.expanduser().resolve()
    if output.exists() and not args.force:
        print(f"Output already exists: {output}", file=sys.stderr)
        return 2
    output.parent.mkdir(parents=True, exist_ok=True)
    part = output.with_name(output.name + ".part")
    if part.exists():
        part.unlink()

    final_status: dict[str, Any] = {}
    status_event = asyncio.Event()
    done = asyncio.Event()
    download_started = False
    received_bytes = 0
    expected_sequence = 0
    data_error: str | None = None
    config = load_ble_config()
    host_id, _ = get_host_identity(config)
    last_print_sent = -PROGRESS_PRINT_BYTES
    last_print_time = 0.0
    pending_ack_tasks: list[asyncio.Task[Any]] = []

    def on_status(_: Any, data: bytearray) -> None:
        nonlocal final_status, last_print_sent, last_print_time
        final_status = decode_status(data)
        state = final_status.get("state", "?")
        sent = final_status.get("sent")
        total = final_status.get("size")
        if sent is not None and total is not None:
            now = asyncio.get_running_loop().time()
            is_final = state in {"sent", "error"} or sent == total
            should_print = (
                is_final
                or sent == 0
                or sent - last_print_sent >= PROGRESS_PRINT_BYTES
                or now - last_print_time >= PROGRESS_PRINT_SECONDS
            )
            if should_print:
                print(f"\r{state}: {sent}/{total} bytes", end="", flush=True)
                last_print_sent = sent
                last_print_time = now
        else:
            print(f"\n{state}: {final_status}")
        status_event.set()
        if download_started and (state == "sent" or state == "error"):
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

    async def wait_for_trusted_auth(timeout: float) -> dict[str, Any]:
        deadline = asyncio.get_running_loop().time() + timeout
        while asyncio.get_running_loop().time() < deadline:
            if final_status.get("state") == "error":
                return final_status
            if final_status.get("state") == "connected" and final_status.get("trusted_host"):
                return final_status
            status_event.clear()
            remaining = deadline - asyncio.get_running_loop().time()
            try:
                await asyncio.wait_for(status_event.wait(), timeout=min(0.25, max(0.0, remaining)))
            except asyncio.TimeoutError:
                pass
        return final_status

    async def collect_ack_errors() -> None:
        nonlocal data_error
        if not pending_ack_tasks:
            return
        results = await asyncio.gather(*pending_ack_tasks, return_exceptions=True)
        pending_ack_tasks.clear()
        for result in results:
            if isinstance(result, Exception) and data_error is None:
                data_error = f"failed to send download ACK: {result}"
                done.set()
                return

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
            handle = None
            data_notify_started = False
            try:
                try:
                    on_status(None, await client.read_gatt_char(STATUS_UUID))
                except (BleakError, OSError):
                    pass

                device_id = final_status.get("device_id")
                device_nonce = final_status.get("device_nonce")
                trusted = config["devices"].get(device_id) if isinstance(device_id, str) else None
                used_trusted_auth = False
                if (
                    trusted
                    and isinstance(trusted, dict)
                    and isinstance(device_nonce, str)
                    and not args.force_code
                ):
                    response = trusted_response(str(trusted.get("secret", "")), device_nonce, str(trusted.get("host_id", host_id)))
                    final_status = {}
                    status_event.clear()
                    await write_json(
                        client,
                        {
                            "op": "hello",
                            "version": 1,
                            "host_id": trusted.get("host_id", host_id),
                            "response": response,
                        },
                    )
                    hello_status = await wait_for_trusted_auth(args.control_timeout)
                    used_trusted_auth = hello_status.get("state") == "connected" and hello_status.get("trusted_host")
                    if not used_trusted_auth:
                        print("\nTrusted-host auth failed; falling back to code.", file=sys.stderr)

                if not used_trusted_auth:
                    if not args.code:
                        print("\nNo trusted host was accepted; pass --code with the six-digit device code.", file=sys.stderr)
                        return 1
                    final_status = {}
                    status_event.clear()
                    await write_json(client, {"op": "hello", "version": 1, "code": args.code})

                hello_status = await wait_for_status({"connected", "error"}, args.control_timeout)
                if hello_status.get("state") == "error":
                    print(f"\nDevice rejected session: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if hello_status.get("state") != "connected":
                    print("\nTimed out waiting for device session confirmation.", file=sys.stderr)
                    return 1

                handle = part.open("wb")

                def on_data(_: Any, data: bytearray) -> None:
                    nonlocal received_bytes, expected_sequence, data_error
                    raw = bytes(data)
                    if len(raw) <= 4:
                        data_error = "invalid data frame"
                        done.set()
                        return
                    sequence = struct.unpack("<I", raw[:4])[0]
                    if sequence != expected_sequence:
                        data_error = f"unexpected data sequence: got {sequence}, expected {expected_sequence}"
                        done.set()
                        return
                    payload = raw[4:]
                    try:
                        handle.write(payload)
                    except OSError as exc:
                        data_error = f"failed to write output file: {exc}"
                        done.set()
                        return
                    received_bytes += len(payload)
                    expected_sequence += 1
                    task = asyncio.create_task(write_json(client, {"op": "get_ack", "sequence": sequence}))
                    pending_ack_tasks.append(task)

                await client.start_notify(DATA_OUT_UUID, on_data)
                data_notify_started = True
                download_started = True
                await write_json(client, start_payload)
                start_status = await wait_for_status({"sending", "sent", "error"}, args.control_timeout)
                if start_status.get("state") == "error":
                    print(f"\nDevice rejected download: {final_status.get('error')}", file=sys.stderr)
                    return 1
                if start_status.get("state") not in {"sending", "sent"}:
                    print("\nTimed out waiting for device download start.", file=sys.stderr)
                    return 1

                try:
                    await asyncio.wait_for(done.wait(), timeout=args.download_timeout)
                    await collect_ack_errors()
                except asyncio.TimeoutError:
                    for task in pending_ack_tasks:
                        task.cancel()
                    await collect_ack_errors()
                    print(f"\nTimed out waiting for {timeout_label}.", file=sys.stderr)
                    return 1
            finally:
                await collect_ack_errors()
                if handle:
                    handle.close()
                if data_notify_started:
                    try:
                        await client.stop_notify(DATA_OUT_UUID)
                    except (BleakError, OSError):
                        pass
                try:
                    await client.stop_notify(STATUS_UUID)
                except (BleakError, OSError) as exc:
                    print(f"\nWarning: failed to stop BLE notifications: {exc}", file=sys.stderr)
    except (BleakError, OSError) as exc:
        print(f"\nBLE transfer failed: {exc}", file=sys.stderr)
        return 1

    print()
    if data_error:
        if part.exists():
            part.unlink()
        print(f"Download failed: {data_error}", file=sys.stderr)
        return 1
    if final_status.get("state") == "error":
        if part.exists():
            part.unlink()
        print(f"Download failed: {final_status.get('error')}", file=sys.stderr)
        return 1
    if final_status.get("state") != "sent":
        if part.exists():
            part.unlink()
        print(f"Download failed: {final_status}", file=sys.stderr)
        return 1
    expected_size = final_status.get("size")
    if not isinstance(expected_size, int) or received_bytes != expected_size:
        if part.exists():
            part.unlink()
        print(f"Download failed: received {received_bytes} bytes, expected {expected_size}", file=sys.stderr)
        return 1
    part.replace(output)
    print(f"Saved {output}")
    return 0


async def get_crash_report(args: argparse.Namespace) -> int:
    return await get_ble_download(
        args,
        output_path=Path(args.output),
        start_payload={"op": "start_get", "kind": "crash_report"},
        timeout_label="crash report",
    )


async def get_package_state(args: argparse.Namespace) -> int:
    if not PACKAGE_ID_RE.fullmatch(args.package_id):
        print("Package id is invalid.", file=sys.stderr)
        return 2
    output = Path(args.output) if args.output else Path(f"{args.package_id}.json")
    return await get_ble_download(
        args,
        output_path=output,
        start_payload={"op": "start_get", "kind": "package_state", "package_id": args.package_id},
        timeout_label="package state",
    )


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
    put.add_argument("--code", type=six_digit_code, help="Six-digit code shown on the device")
    put.add_argument("--force-code", action="store_true", help="Skip trusted-host auth and use --code")
    put.add_argument("--no-remember-host", action="store_true", help="Do not ask the device to save this host")
    put.add_argument("--chunk-size", type=positive_int, default=160, help="Payload bytes per BLE data frame")
    put.add_argument("--chunk-delay", type=float, default=0.0, help="Delay between chunk writes, in seconds")
    put.add_argument(
        "--transfer-mode",
        choices=("windowed", "response", "no-response"),
        default="windowed",
        help="BLE write strategy: windowed is faster with receiver ACKs; response is slowest; no-response is unsafe",
    )
    put.add_argument("--window-bytes", type=positive_int, default=DEFAULT_WINDOW_BYTES, help="Bytes sent before waiting for receiver progress in windowed mode")
    put.add_argument(
        "--write-without-response",
        dest="transfer_mode",
        action="store_const",
        const="no-response",
        help="Deprecated alias for --transfer-mode no-response; may require --chunk-delay",
    )
    put.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    put.add_argument("--control-timeout", type=float, default=5.0, help="Control response timeout, in seconds")
    put.add_argument("--install-timeout", type=float, default=60.0, help="Install result timeout, in seconds")

    put_book_parser = sub.add_parser("put-book", help="Upload an .epub book")
    put_book_parser.add_argument("book", help="Path to the .epub book")
    put_book_parser.add_argument("--code", type=six_digit_code, help="Six-digit code shown on the device")
    put_book_parser.add_argument("--force-code", action="store_true", help="Skip trusted-host auth and use --code")
    put_book_parser.add_argument("--no-remember-host", action="store_true", help="Do not ask the device to save this host")
    put_book_parser.add_argument("--chunk-size", type=positive_int, default=160, help="Payload bytes per BLE data frame")
    put_book_parser.add_argument("--chunk-delay", type=float, default=0.0, help="Delay between chunk writes, in seconds")
    put_book_parser.add_argument(
        "--transfer-mode",
        choices=("windowed", "response", "no-response"),
        default="windowed",
        help="BLE write strategy: windowed is faster with receiver ACKs; response is slowest; no-response is unsafe",
    )
    put_book_parser.add_argument("--window-bytes", type=positive_int, default=DEFAULT_WINDOW_BYTES, help="Bytes sent before waiting for receiver progress in windowed mode")
    put_book_parser.add_argument(
        "--write-without-response",
        dest="transfer_mode",
        action="store_const",
        const="no-response",
        help="Deprecated alias for --transfer-mode no-response; may require --chunk-delay",
    )
    put_book_parser.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    put_book_parser.add_argument("--control-timeout", type=float, default=5.0, help="Control response timeout, in seconds")
    put_book_parser.add_argument("--save-timeout", type=float, default=60.0, help="Save result timeout, in seconds")

    get_crash = sub.add_parser("get-crash-report", help="Download /crash_report.txt")
    get_crash.add_argument("output", nargs="?", default="crash_report.txt", help="Output path")
    get_crash.add_argument("--code", type=six_digit_code, help="Six-digit code shown on the device")
    get_crash.add_argument("--force-code", action="store_true", help="Skip trusted-host auth and use --code")
    get_crash.add_argument("--force", action="store_true", help="Overwrite output path if it already exists")
    get_crash.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    get_crash.add_argument("--control-timeout", type=float, default=5.0, help="Control response timeout, in seconds")
    get_crash.add_argument("--download-timeout", type=float, default=60.0, help="Download timeout, in seconds")

    get_package_state_parser = sub.add_parser(
        "get-package-state",
        help="Download /.marginalia/package-state/<package-id>.json",
    )
    get_package_state_parser.add_argument("package_id", help="Package id to download state for")
    get_package_state_parser.add_argument("output", nargs="?", help="Output path")
    get_package_state_parser.add_argument("--code", type=six_digit_code, help="Six-digit code shown on the device")
    get_package_state_parser.add_argument("--force-code", action="store_true", help="Skip trusted-host auth and use --code")
    get_package_state_parser.add_argument("--force", action="store_true", help="Overwrite output path if it already exists")
    get_package_state_parser.add_argument("--scan-timeout", type=float, default=8.0, help="BLE scan timeout, in seconds")
    get_package_state_parser.add_argument("--control-timeout", type=float, default=5.0, help="Control response timeout, in seconds")
    get_package_state_parser.add_argument("--download-timeout", type=float, default=60.0, help="Download timeout, in seconds")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.command == "put-package":
        return asyncio.run(put_package(args))
    if args.command == "put-book":
        args.install_timeout = args.save_timeout
        return asyncio.run(put_book(args))
    if args.command == "get-crash-report":
        return asyncio.run(get_crash_report(args))
    if args.command == "get-package-state":
        return asyncio.run(get_package_state(args))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
