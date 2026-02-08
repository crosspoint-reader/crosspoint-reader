import asyncio
import base64
import json
import os
import re
import shutil
import signal
import sys
from http import HTTPStatus
from pathlib import Path
from typing import Set

import serial
import websockets
from websockets.http11 import Response
from websockets.server import WebSocketServerProtocol
import glob
import time


# Serial port
serial_port: serial.Serial | None = None
# Auto-detect serial device
def find_serial_device():
  """Find the first matching USB modem device."""
  devices = glob.glob("/dev/cu.usbmodem*")
  if devices:
    return devices[0]
  return "/dev/cu.usbmodem1101"  # Fallback default

SERIAL_DEVICE = find_serial_device()
SERIAL_BAUD = 115200

# DFU mode detection - set when we see the bootloader log message
is_in_dfu_mode = False

sdcard_path = os.environ.get("SDCARD_PATH", "./.sdcard")

# SD card emulation directory
if not os.path.exists(sdcard_path):
  os.makedirs(sdcard_path)


class SdCardHandler:
  """Handle SD card filesystem commands from the emulated device."""

  def __init__(self, base_path: str):
    self.base_path = base_path

  def _resolve_path(self, path: str) -> str:
    """Resolve a device path to a host filesystem path."""
    # Remove leading slash and join with base path
    if path.startswith("/"):
      path = path[1:]
    return os.path.join(self.base_path, path)

  def handle_fs_list(self, path: str, max_files: str = "200") -> list[str]:
    """List files in a directory. Returns list of filenames."""
    resolved = self._resolve_path(path)
    max_count = int(max_files) if max_files else 200

    if not os.path.exists(resolved):
      return []

    if not os.path.isdir(resolved):
      return []

    try:
      entries = []
      for entry in os.listdir(resolved):
        if len(entries) >= max_count:
          break
        full_path = os.path.join(resolved, entry)
        # Append "/" for directories to match device behavior
        if os.path.isdir(full_path):
          entries.append(entry + "/")
        else:
          entries.append(entry)
      return entries
    except Exception as e:
      print(f"[SdCard] Error listing {path}: {e}", file=sys.stderr)
      return []

  def handle_fs_read(self, path: str, offset: str = "0", length: str = "-1") -> str:
    """Read file contents. Returns base64-encoded data."""
    resolved = self._resolve_path(path)
    offset_int = int(offset) if offset else 0
    length_int = int(length) if length else -1

    print(f"[SdCard] Reading {path} (offset={offset_int}, length={length_int})")

    if not os.path.exists(resolved) or os.path.isdir(resolved):
      return ""

    try:
      with open(resolved, "rb") as f:
        if offset_int > 0:
          f.seek(offset_int)
        if length_int == -1:
          data = f.read()
        else:
          data = f.read(length_int)
        return base64.b64encode(data).decode("ascii")
    except Exception as e:
      print(f"[SdCard] Error reading {path}: {e}", file=sys.stderr)
      return ""

  def handle_fs_stat(self, path: str) -> int:
    """Get file size. Returns -1 if not found, -2 if directory."""
    resolved = self._resolve_path(path)

    if not os.path.exists(resolved):
      return -1

    if os.path.isdir(resolved):
      return -2

    try:
      return os.path.getsize(resolved)
    except Exception as e:
      print(f"[SdCard] Error stat {path}: {e}", file=sys.stderr)
      return -1

  def handle_fs_write(self, path: str, b64_data: str, offset: str = "0", inplace: str = "0") -> int:
    """Write data to file. Returns bytes written."""
    resolved = self._resolve_path(path)
    offset_int = int(offset) if offset else 0
    is_inplace = inplace == "1"

    try:
      data = base64.b64decode(b64_data)
    except Exception as e:
      print(f"[SdCard] Error decoding base64 for {path}: {e}", file=sys.stderr)
      return 0
    
    print(f"[SdCard] Writing to {path} (inplace={is_inplace}, offset={offset_int}, bytes={len(data)})")

    try:
      # Ensure parent directory exists
      parent = os.path.dirname(resolved)
      if parent and not os.path.exists(parent):
        os.makedirs(parent)

      if is_inplace and os.path.exists(resolved):
        # In-place write at offset
        with open(resolved, "r+b") as f:
          f.seek(offset_int)
          f.write(data)
      else:
        # Overwrite or create new file
        mode = "r+b" if os.path.exists(resolved) and offset_int > 0 else "wb"
        with open(resolved, mode) as f:
          if offset_int > 0:
            f.seek(offset_int)
          f.write(data)

      return len(data)
    except Exception as e:
      print(f"[SdCard] Error writing {path}: {e}", file=sys.stderr)
      return 0

  def handle_fs_mkdir(self, path: str) -> int:
    """Create directory. Returns 0 on success."""
    resolved = self._resolve_path(path)
    print(f"[SdCard] Creating directory {path}")

    try:
      os.makedirs(resolved, exist_ok=True)
      return 0
    except Exception as e:
      print(f"[SdCard] Error mkdir {path}: {e}", file=sys.stderr)
      return -1

  def handle_fs_rm(self, path: str) -> int:
    """Remove file or directory. Returns 0 on success."""
    resolved = self._resolve_path(path)
    print(f"[SdCard] Removing {path}")

    if not os.path.exists(resolved):
      return -1

    try:
      if os.path.isdir(resolved):
        shutil.rmtree(resolved)
      else:
        os.remove(resolved)
      return 0
    except Exception as e:
      print(f"[SdCard] Error removing {path}: {e}", file=sys.stderr)
      return -1
    
  def handle_fs_move(self, src_path: str, dst_path: str) -> int:
    """Move or rename a file or directory. Returns 0 on success."""
    resolved_src = self._resolve_path(src_path)
    resolved_dst = self._resolve_path(dst_path)
    print(f"[SdCard] Moving {src_path} to {dst_path}")

    if not os.path.exists(resolved_src):
      return -1

    try:
      # Ensure destination parent directory exists
      dst_parent = os.path.dirname(resolved_dst)
      if dst_parent and not os.path.exists(dst_parent):
        os.makedirs(dst_parent)

      shutil.move(resolved_src, resolved_dst)
      return 0
    except Exception as e:
      print(f"[SdCard] Error moving {src_path} to {dst_path}: {e}", file=sys.stderr)
      return -1


# Global SD card handler instance
sdcard_handler = SdCardHandler(sdcard_path)


# WebSocket clients
connected_clients: Set[WebSocketServerProtocol] = set()

# Command pattern: $$CMD_(COMMAND):(ARG0)[:(ARG1)][:(ARG2)][:(ARG3)]$$
CMD_PATTERN = re.compile(r'^\$\$CMD_([A-Z_]+):(.+)\$\$$')


def parse_command(message: str) -> tuple[str, list[str]] | None:
  """Parse a command message. Returns (command, args) or None if not a command."""
  match = CMD_PATTERN.match(message)
  if not match:
    return None
  command = match.group(1)
  args_str = match.group(2)
  # Split args by ':' but the last arg might contain ':'
  args = args_str.split(':')
  return (command, args)


LAST_RESPONSE_TIME = 0
async def send_response(response: str):
  """Send a response back to the device via serial port."""
  global serial_port, LAST_RESPONSE_TIME
  now = time.time()
  if now - LAST_RESPONSE_TIME < 0.01:  # 10ms minimum between responses
    await asyncio.sleep(0.005)  # 5ms delay
  LAST_RESPONSE_TIME = time.time()
  if serial_port and serial_port.is_open:
    data = (response + "\n").encode()
    chunk_size = 64  # Smaller chunks to avoid buffer overflow
    for i in range(0, len(data), chunk_size):
      chunk = data[i:i + chunk_size]
      serial_port.write(chunk)
      serial_port.flush()
      if i + chunk_size < len(data):
        await asyncio.sleep(0.002)  # 2ms delay between chunks


LAST_DISPLAY_BUFFER = None
BUTTON_STATE = 0

async def handle_command(command: str, args: list[str]) -> bool:
  """Handle a command from the device. Returns True if handled."""
  global sdcard_handler, LAST_DISPLAY_BUFFER, BUTTON_STATE

  try:
    if command == "PING":
      await send_response(str("123456"))
      print(f"[Emulator] PING command responded", flush=True)
      return True

    elif command == "DISPLAY":
      # Display command - no response needed
      # arg0: base64-encoded buffer
      print(f"[Emulator] DISPLAY command received (buffer size: {len(args[0]) if args else 0})", flush=True)
      LAST_DISPLAY_BUFFER = args[0] if args else None
      await send_response(str("0"))  # Acknowledge
      return True

    elif command == "FS_LIST":
      # arg0: path, arg1: max files (optional)
      path = args[0] if len(args) > 0 else "/"
      max_files = args[1] if len(args) > 1 else "200"
      entries = sdcard_handler.handle_fs_list(path, max_files)
      print(f"[Emulator] FS_LIST {path} -> {len(entries)} entries", flush=True)
      # Send each entry as a line, then empty line to terminate
      for entry in entries:
        await send_response(entry)
      await send_response("")  # Empty line terminates the list
      return True

    elif command == "FS_READ":
      # arg0: path, arg1: offset, arg2: length
      path = args[0] if len(args) > 0 else ""
      offset = args[1] if len(args) > 1 else "0"
      length = args[2] if len(args) > 2 else "-1"
      result = sdcard_handler.handle_fs_read(path, offset, length)
      print(f"[Emulator] FS_READ {path} offset={offset} len={length} -> {len(result)} bytes (b64)", flush=True)
      await send_response(result)
      return True

    elif command == "FS_STAT":
      # arg0: path
      path = args[0] if len(args) > 0 else ""
      result = sdcard_handler.handle_fs_stat(path)
      print(f"[Emulator] FS_STAT {path} -> {result}", flush=True)
      await send_response(str(result))
      return True

    elif command == "FS_WRITE":
      # arg0: path, arg1: base64-encoded data, arg2: offset, arg3: is inplace
      path = args[0] if len(args) > 0 else ""
      b64_data = args[1] if len(args) > 1 else ""
      offset = args[2] if len(args) > 2 else "0"
      inplace = args[3] if len(args) > 3 else "0"
      result = sdcard_handler.handle_fs_write(path, b64_data, offset, inplace)
      print(f"[Emulator] FS_WRITE {path} offset={offset} inplace={inplace} -> {result} bytes", flush=True)
      await send_response(str(result))
      return True

    elif command == "FS_MKDIR":
      # arg0: path
      path = args[0] if len(args) > 0 else ""
      result = sdcard_handler.handle_fs_mkdir(path)
      print(f"[Emulator] FS_MKDIR {path} -> {result}", flush=True)
      await send_response(str(result))
      return True

    elif command == "FS_RM":
      # arg0: path
      path = args[0] if len(args) > 0 else ""
      result = sdcard_handler.handle_fs_rm(path)
      print(f"[Emulator] FS_RM {path} -> {result}", flush=True)
      await send_response(str(result))
      return True

    elif command == "FS_MOVE":
      # arg0: src path, arg1: dst path
      src_path = args[0] if len(args) > 0 else ""
      dst_path = args[1] if len(args) > 1 else ""
      result = sdcard_handler.handle_fs_move(src_path, dst_path)
      print(f"[Emulator] FS_MOVE {src_path} -> {dst_path} to {result}", flush=True)
      await send_response(str(result))
      return True

    else:
      print(f"[Emulator] Unknown command: {command}", flush=True)
      return False

  except Exception as e:
    print(f"[Emulator] Error handling command {command}: {e}", file=sys.stderr)
    return False


def process_message(message: str, for_ui: bool) -> str | None:
  if message.startswith("$$CMD_BUTTON:"):
    return None # Too frequent, ignore
  if message.startswith("$$CMD_"):
    if for_ui:
      return message
    else:
      # $$CMD_(COMMAND)[:(ARG0)][:(ARG1)][:(ARG2)]$$
      command = message[2:].split(':')[0]
      return "[Emulator] Received command: " + command
  return message


async def broadcast_message(msg_type: str, data: str):
  """Broadcast a message to all connected WebSocket clients."""
  if not connected_clients:
    return

  message = json.dumps({"type": msg_type, "data": data})

  # Send to all clients, remove disconnected ones
  disconnected = set()
  for client in connected_clients:
    try:
      await client.send(message)
    except websockets.exceptions.ConnectionClosed:
      disconnected.add(client)

  connected_clients.difference_update(disconnected)


async def read_serial():
  """Read from the serial port line by line and broadcast to clients."""
  global serial_port, is_in_dfu_mode
  buffer = b""
  loop = asyncio.get_event_loop()

  while True:
    try:
      # Read available data from serial port in a thread to avoid blocking
      chunk = await loop.run_in_executor(None, lambda: serial_port.read(serial_port.in_waiting or 1))
      if not chunk:
        await asyncio.sleep(0.01)
        continue

      buffer += chunk

      # Process complete lines
      while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        try:
          decoded_line = line.decode("utf-8", errors="replace").rstrip('\r')
        except Exception:
          decoded_line = line.decode("latin-1", errors="replace").rstrip('\r')

        # Check for DFU/bootloader mode log message
        if "waiting for download" in decoded_line or "DOWNLOAD(USB/UART" in decoded_line:
          is_in_dfu_mode = True
          print(f"[DFU] Detected bootloader mode, releasing serial port for firmware upload...", flush=True)
          await broadcast_message("info", "Device entering DFU mode, releasing port for firmware upload...")
          # Close the serial port immediately to let esptool take over
          if serial_port and serial_port.is_open:
            serial_port.close()
          return  # Exit read_serial to release the port

        # Check if this is a command that needs handling
        parsed = parse_command(decoded_line)
        if parsed:
          command, args = parsed
          await handle_command(command, args)
          if command == "DISPLAY":
            await broadcast_message("stdout", decoded_line)
          continue

        to_stdio = process_message(decoded_line, for_ui=False)
        to_ws = process_message(decoded_line, for_ui=True)

        # Forward to parent process
        if to_stdio is not None:
          print(to_stdio, flush=True)

        # Broadcast to WebSocket clients
        if to_ws is not None:
          await broadcast_message("stdout", to_ws)

    except serial.SerialException as e:
      print(f"Serial port error: {e}", file=sys.stderr)
      break
    except Exception as e:
      print(f"Error reading serial: {e}", file=sys.stderr)
      await asyncio.sleep(0.1)

  # Process remaining buffer
  if buffer:
    try:
      decoded_line = buffer.decode("utf-8", errors="replace").rstrip('\r')
    except Exception:
      decoded_line = buffer.decode("latin-1", errors="replace").rstrip('\r')

    print(decoded_line, flush=True)
    await broadcast_message("stdout", decoded_line)


RETRY_INTERVAL = 2  # seconds
DFU_WAIT_INTERVAL = 1  # seconds - interval to wait during DFU mode

async def wait_for_dfu_complete():
  """Wait for device to finish firmware upload and reconnect."""
  global is_in_dfu_mode
  
  print("[DFU] Waiting for firmware upload to complete...", flush=True)
  await broadcast_message("info", "Waiting for firmware upload to complete...")
  
  # Wait for the device to disconnect (esptool resets it after upload)
  # and then reconnect
  max_wait = 60  # seconds - firmware upload can take a while
  waited = 0
  device_was_gone = False
  
  while waited < max_wait:
    await asyncio.sleep(DFU_WAIT_INTERVAL)
    waited += DFU_WAIT_INTERVAL
    
    devices = glob.glob("/dev/cu.usbmodem*")
    
    if not devices:
      device_was_gone = True
      # Device disconnected, this is expected during/after upload
      continue
    
    if device_was_gone and devices:
      # Device came back after being gone - upload is complete
      print(f"[DFU] Device reconnected after firmware upload", flush=True)
      await broadcast_message("info", "Device reconnected after firmware upload")
      # Give it time to boot up
      await asyncio.sleep(2)
      is_in_dfu_mode = False
      return
  
  print("[DFU] Timeout waiting for device, will retry anyway...", flush=True)
  is_in_dfu_mode = False

async def open_serial():
  """Open the serial port and read from it. Retries every RETRY_INTERVAL seconds if disconnected."""
  global serial_port, SERIAL_DEVICE, is_in_dfu_mode

  while True:
    # If we detected DFU mode, wait for upload to complete
    if is_in_dfu_mode:
      await wait_for_dfu_complete()
      continue
    
    # Re-detect serial device on each attempt
    SERIAL_DEVICE = find_serial_device()
    
    # Check if device exists
    if not SERIAL_DEVICE or not glob.glob("/dev/cu.usbmodem*"):
      print("No serial device found, waiting...", flush=True)
      await asyncio.sleep(RETRY_INTERVAL)
      continue
    
    print(f"Opening serial port {SERIAL_DEVICE} at {SERIAL_BAUD} baud...", flush=True)

    try:
      serial_port = serial.Serial(SERIAL_DEVICE, SERIAL_BAUD, timeout=0.1)
      print(f"Serial port opened successfully", flush=True)

      # Read from serial port (this blocks until disconnected or DFU detected)
      await read_serial()

      # If we exited due to DFU mode, don't print connection lost message
      if is_in_dfu_mode:
        continue
      
      print(f"Serial connection lost, will retry in {RETRY_INTERVAL} seconds...", flush=True)
      await broadcast_message("stderr", f"Serial connection lost, retrying in {RETRY_INTERVAL} seconds...")

    except serial.SerialException as e:
      if is_in_dfu_mode:
        continue
      print(f"Error opening serial port: {e}", file=sys.stderr)
      await broadcast_message("stderr", f"Error opening serial port: {e}. Retrying in {RETRY_INTERVAL} seconds...")
    except Exception as e:
      print(f"Error: {e}", file=sys.stderr)
      await broadcast_message("stderr", f"Error: {e}. Retrying in {RETRY_INTERVAL} seconds...")
    finally:
      # Close the port if it's open (unless already closed for DFU)
      if serial_port and serial_port.is_open:
        serial_port.close()
        serial_port = None

    if not is_in_dfu_mode:
      await asyncio.sleep(RETRY_INTERVAL)


async def websocket_handler(websocket: WebSocketServerProtocol):
  """Handle a WebSocket connection."""
  connected_clients.add(websocket)
  print(f"Client connected. Total clients: {len(connected_clients)}", flush=True)

  try:
    # Send a welcome message
    await websocket.send(json.dumps({
      "type": "info",
      "data": "Connected to CrossPoint emulator"
    }))

    # Send the last display buffer if available
    if LAST_DISPLAY_BUFFER is not None:
      await websocket.send(json.dumps({
        "type": "stdout",
        "data": f"$$CMD_DISPLAY:{LAST_DISPLAY_BUFFER}$$"
      }))

    # Handle incoming messages (for stdin forwarding and events)
    async for message in websocket:
      try:
        data = json.loads(message)
        msg_type = data.get("type")
        
        if msg_type == "stdin" and serial_port and serial_port.is_open:
          input_data = data.get("data", "")
          serial_port.write((input_data + "\n").encode())
          serial_port.flush()
        elif msg_type == "button_state":
          global BUTTON_STATE
          BUTTON_STATE = int(data.get("state", "0"))
          print(f"[WebUI] Button state updated to {BUTTON_STATE}", flush=True)
        else:
          # Print any other messages for debugging
          print(f"[WebUI Message] {message}", flush=True)
      except json.JSONDecodeError:
        print(f"[WebUI] Invalid JSON received: {message}", file=sys.stderr)
      except Exception as e:
        print(f"Error handling client message: {e}", file=sys.stderr)

  except websockets.exceptions.ConnectionClosed:
    pass
  finally:
    connected_clients.discard(websocket)
    print(f"Client disconnected. Total clients: {len(connected_clients)}", flush=True)


async def main():
  """Main entry point."""
  host = os.environ.get("HOST", "127.0.0.1")
  port = int(os.environ.get("PORT", "8090"))

  print(f"Starting WebSocket server on {host}:{port}", flush=True)

  # Start WebSocket server
  async with websockets.serve(
    websocket_handler, host, port,
    process_request=process_request,
    origins=None,
  ):
    print(f"WebSocket server running on ws://{host}:{port}/ws", flush=True)
    print(f"Web UI available at http://{host}:{port}/", flush=True)

    # Open serial port and read from it
    await open_serial()


def signal_handler(signum, frame):
  """Handle shutdown signals."""
  print("\nShutting down...", flush=True)
  if serial_port and serial_port.is_open:
    serial_port.close()
  sys.exit(0)


def process_request(connection, request):
  """Handle HTTP requests for serving static files."""
  path = request.path

  if path == "/" or path == "/dev_server_ui.html":
    # Serve the dev_server_ui.html file
    html_path = Path(__file__).parent / "dev_server_ui.html"
    try:
      content = html_path.read_bytes()
      return Response(
        HTTPStatus.OK,
        "OK",
        websockets.Headers([
          ("Content-Type", "text/html; charset=utf-8"),
          ("Content-Length", str(len(content))),
        ]),
        content
      )
    except FileNotFoundError:
      return Response(HTTPStatus.NOT_FOUND, "Not Found", websockets.Headers(), b"dev_server_ui.html not found")

  if path == "/ws":
    # Return None to continue with WebSocket handshake
    return None

  # Return 404 for other paths
  return Response(HTTPStatus.NOT_FOUND, "Not Found", websockets.Headers(), b"Not found")


if __name__ == "__main__":
  # Set up signal handlers
  signal.signal(signal.SIGINT, signal_handler)
  signal.signal(signal.SIGTERM, signal_handler)

  # Run the main loop
  asyncio.run(main())
