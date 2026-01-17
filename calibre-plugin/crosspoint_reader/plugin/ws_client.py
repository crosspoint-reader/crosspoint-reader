import base64
import os
import select
import socket
import struct
import time


class WebSocketError(RuntimeError):
    pass


class WebSocketClient:
    def __init__(self, host, port, timeout=10, debug=False, logger=None):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.debug = debug
        self.logger = logger
        self.sock = None

    def _log(self, *args):
        if self.debug:
            msg = '[CrossPoint WS] ' + ' '.join(str(a) for a in args)
            if self.logger:
                self.logger(msg)
            else:
                print(msg)

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), self.timeout)
        key = base64.b64encode(os.urandom(16)).decode('ascii')
        req = (
            'GET / HTTP/1.1\r\n'
            f'Host: {self.host}:{self.port}\r\n'
            'Upgrade: websocket\r\n'
            'Connection: Upgrade\r\n'
            f'Sec-WebSocket-Key: {key}\r\n'
            'Sec-WebSocket-Version: 13\r\n'
            '\r\n'
        )
        self.sock.sendall(req.encode('ascii'))
        data = self._read_http_response()
        if b' 101 ' not in data.split(b'\r\n', 1)[0]:
            raise WebSocketError('Handshake failed: ' + data.split(b'\r\n', 1)[0].decode('ascii', 'ignore'))
        self._log('Handshake OK')

    def _read_http_response(self):
        self.sock.settimeout(self.timeout)
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.sock.recv(1024)
            if not chunk:
                break
            data += chunk
        return data

    def close(self):
        if not self.sock:
            return
        try:
            self._send_frame(0x8, b'')
        except Exception:
            pass
        try:
            self.sock.close()
        finally:
            self.sock = None

    def send_text(self, text):
        self._send_frame(0x1, text.encode('utf-8'))

    def send_binary(self, payload):
        self._send_frame(0x2, payload)

    def _send_frame(self, opcode, payload):
        if self.sock is None:
            raise WebSocketError('Socket not connected')
        fin = 0x80
        first = fin | (opcode & 0x0F)
        mask_bit = 0x80
        length = len(payload)
        header = bytearray([first])
        if length <= 125:
            header.append(mask_bit | length)
        elif length <= 65535:
            header.append(mask_bit | 126)
            header.extend(struct.pack('!H', length))
        else:
            header.append(mask_bit | 127)
            header.extend(struct.pack('!Q', length))

        mask = os.urandom(4)
        header.extend(mask)
        masked = bytearray(payload)
        for i in range(length):
            masked[i] ^= mask[i % 4]
        self.sock.sendall(header + masked)

    def read_text(self):
        deadline = time.time() + self.timeout
        while True:
            if time.time() > deadline:
                raise WebSocketError('Timed out waiting for text frame')
            opcode, payload = self._read_frame()
            if opcode == 0x8:
                code = None
                reason = ''
                if len(payload) >= 2:
                    code = struct.unpack('!H', payload[:2])[0]
                    reason = payload[2:].decode('utf-8', 'ignore')
                self._log('Server closed connection', code, reason)
                raise WebSocketError('Connection closed')
            if opcode == 0x9:
                # Ping -> respond with Pong
                self._send_frame(0xA, payload)
                continue
            if opcode == 0xA:
                # Pong -> ignore
                continue
            if opcode != 0x1:
                self._log('Ignoring non-text opcode', opcode, len(payload))
                continue
            return payload.decode('utf-8', 'ignore')

    def _read_frame(self):
        if self.sock is None:
            raise WebSocketError('Socket not connected')
        hdr = self._recv_exact(2)
        b1, b2 = hdr[0], hdr[1]
        opcode = b1 & 0x0F
        masked = (b2 & 0x80) != 0
        length = b2 & 0x7F
        if length == 126:
            length = struct.unpack('!H', self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack('!Q', self._recv_exact(8))[0]
        mask = b''
        if masked:
            mask = self._recv_exact(4)
        payload = self._recv_exact(length) if length else b''
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def _recv_exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise WebSocketError('Socket closed')
            data += chunk
        return data

    def drain_messages(self):
        if self.sock is None:
            return []
        messages = []
        while True:
            r, _, _ = select.select([self.sock], [], [], 0)
            if not r:
                break
            opcode, payload = self._read_frame()
            if opcode == 0x1:
                messages.append(payload.decode('utf-8', 'ignore'))
            elif opcode == 0x8:
                raise WebSocketError('Connection closed')
        return messages


def _log(logger, debug, message):
    if not debug:
        return
    if logger:
        logger(message)
    else:
        print(message)


def _broadcast_from_host(host):
    parts = host.split('.')
    if len(parts) != 4:
        return None
    try:
        _ = [int(p) for p in parts]
    except Exception:
        return None
    parts[-1] = '255'
    return '.'.join(parts)


def discover_device(timeout=2.0, debug=False, logger=None, extra_hosts=None):
    ports = [8134, 54982, 48123, 39001, 44044, 59678]
    local_port = 0
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.5)
    try:
        sock.bind(('', local_port))
    except Exception:
        _log(logger, debug, '[CrossPoint WS] discovery bind failed')
        pass

    msg = b'hello'
    try:
        addr, port = sock.getsockname()
        _log(logger, debug, f'[CrossPoint WS] discovery local {addr} {port}')
    except Exception:
        pass

    targets = []
    for port in ports:
        targets.append(('255.255.255.255', port))
    for host in extra_hosts or []:
        if not host:
            continue
        for port in ports:
            targets.append((host, port))
        bcast = _broadcast_from_host(host)
        if bcast:
            for port in ports:
                targets.append((bcast, port))

    for _ in range(3):
        for host, port in targets:
            try:
                sock.sendto(msg, (host, port))
            except Exception as exc:
                _log(logger, debug, f'[CrossPoint WS] discovery send failed {host}:{port} {exc}')
                pass
        start = time.time()
        while time.time() - start < timeout:
            try:
                data, addr = sock.recvfrom(256)
            except Exception:
                break
            _log(logger, debug, f'[CrossPoint WS] discovery {addr} {data}')
            try:
                text = data.decode('utf-8', 'ignore')
            except Exception:
                continue
            semi = text.find(';')
            port = 81
            if semi != -1:
                try:
                    port = int(text[semi + 1:].strip().split(',')[0])
                except Exception:
                    port = 81
            return addr[0], port
    return None, None


def upload_file(host, port, upload_path, filename, filepath, chunk_size=16384, debug=False, progress_cb=None,
                logger=None):
    client = WebSocketClient(host, port, timeout=10, debug=debug, logger=logger)
    try:
        client.connect()
        size = os.path.getsize(filepath)
        start = f'START:{filename}:{size}:{upload_path}'
        client._log('Sending START', start)
        client.send_text(start)

        msg = client.read_text()
        client._log('Received', msg)
        if not msg:
            raise WebSocketError('Unexpected response: <empty>')
        if msg.startswith('ERROR'):
            raise WebSocketError(msg)
        if msg != 'READY':
            raise WebSocketError('Unexpected response: ' + msg)

        sent = 0
        with open(filepath, 'rb') as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                client.send_binary(chunk)
                sent += len(chunk)
                if progress_cb:
                    progress_cb(sent, size)
                client.drain_messages()

        # Wait for DONE or ERROR
        while True:
            msg = client.read_text()
            client._log('Received', msg)
            if msg == 'DONE':
                return
            if msg.startswith('ERROR'):
                raise WebSocketError(msg)
    finally:
        client.close()
