import asyncio
import json
import os
import signal
import sys
from http import HTTPStatus
from pathlib import Path
from typing import Set

import websockets
from websockets.http11 import Response
from websockets.server import WebSocketServerProtocol


"""
This script spawns a QEMU process, then forward its stdout and stderr to WebSocket clients.
"""


# WebSocket clients
connected_clients: Set[WebSocketServerProtocol] = set()

# QEMU process
qemu_process: asyncio.subprocess.Process | None = None

def process_message(message: str, for_ui: bool) -> str:
  if message.startswith("$$DATA:DISPLAY:"):
    if for_ui:
      return message
    else:
      return f"[DISPLAY DATA]"
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


async def read_stream(stream: asyncio.StreamReader, stream_type: str):
  """Read from a stream line by line and broadcast to clients."""
  buffer = b""

  while True:
    try:
      chunk = await stream.read(1024)
      if not chunk:
        break

      buffer += chunk

      # Process complete lines
      while b"\n" in buffer:
        line, buffer = buffer.split(b"\n", 1)
        try:
          decoded_line = line.decode("utf-8", errors="replace")
        except Exception:
          decoded_line = line.decode("latin-1", errors="replace")

        # Forward to parent process
        if stream_type == "stdout":
          print(process_message(decoded_line, for_ui=False), flush=True)
        else:
          print(process_message(decoded_line, for_ui=False), file=sys.stderr, flush=True)

        # Broadcast to WebSocket clients
        await broadcast_message(stream_type, process_message(decoded_line, for_ui=True))

    except Exception as e:
      print(f"Error reading {stream_type}: {e}", file=sys.stderr)
      break

  # Process remaining buffer
  if buffer:
    try:
      decoded_line = buffer.decode("utf-8", errors="replace")
    except Exception:
      decoded_line = buffer.decode("latin-1", errors="replace")

    if stream_type == "stdout":
      print(decoded_line, flush=True)
    else:
      print(decoded_line, file=sys.stderr, flush=True)

    await broadcast_message(stream_type, decoded_line)


async def spawn_qemu():
  """Spawn the QEMU process and capture its output."""
  global qemu_process

  # Build the command
  cmd = [
    "qemu-system-riscv32",
    "-nographic",
    "-M", "esp32c3",
    "-drive", "file=flash.bin,if=mtd,format=raw"
  ]

  # Get working directory from environment or use /tmp
  work_dir = os.getcwd()

  print(f"Starting QEMU in {work_dir}...", flush=True)
  print(f"Command: {' '.join(cmd)}", flush=True)

  try:
    qemu_process = await asyncio.create_subprocess_exec(
      *cmd,
      stdout=asyncio.subprocess.PIPE,
      stderr=asyncio.subprocess.PIPE,
      stdin=asyncio.subprocess.PIPE,
      cwd=work_dir,
      env=os.environ.copy()
    )

    # Read stdout and stderr concurrently
    await asyncio.gather(
      read_stream(qemu_process.stdout, "stdout"),
      read_stream(qemu_process.stderr, "stderr")
    )

    # Wait for process to complete
    await qemu_process.wait()
    print(f"QEMU process exited with code {qemu_process.returncode}", flush=True)

  except FileNotFoundError:
    print("Error: qemu-system-riscv32 not found. Make sure it's in PATH.", file=sys.stderr)
    await broadcast_message("stderr", "Error: qemu-system-riscv32 not found")
  except Exception as e:
    print(f"Error spawning QEMU: {e}", file=sys.stderr)
    await broadcast_message("stderr", f"Error spawning QEMU: {e}")


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

    # Handle incoming messages (for stdin forwarding)
    async for message in websocket:
      try:
        data = json.loads(message)
        if data.get("type") == "stdin" and qemu_process and qemu_process.stdin:
          input_data = data.get("data", "")
          qemu_process.stdin.write((input_data + "\n").encode())
          await qemu_process.stdin.drain()
      except json.JSONDecodeError:
        pass
      except Exception as e:
        print(f"Error handling client message: {e}", file=sys.stderr)

  except websockets.exceptions.ConnectionClosed:
    pass
  finally:
    connected_clients.discard(websocket)
    print(f"Client disconnected. Total clients: {len(connected_clients)}", flush=True)


async def main():
  """Main entry point."""
  host = os.environ.get("HOST", "0.0.0.0")
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

    # Spawn QEMU process
    await spawn_qemu()


def signal_handler(signum, frame):
  """Handle shutdown signals."""
  print("\nShutting down...", flush=True)
  if qemu_process:
    qemu_process.terminate()
  sys.exit(0)


def process_request(connection, request):
  """Handle HTTP requests for serving static files."""
  path = request.path

  if path == "/" or path == "/web_ui.html":
    # Serve the web_ui.html file
    html_path = Path(__file__).parent / "web_ui.html"
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
      return Response(HTTPStatus.NOT_FOUND, "Not Found", websockets.Headers(), b"web_ui.html not found")

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
