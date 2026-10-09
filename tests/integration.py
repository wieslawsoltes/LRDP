#!/usr/bin/env python3
"""Independent stdlib wire fixture: real TCP/TLS, not a Windows interoperability claim."""
from __future__ import annotations
import argparse
import pathlib
import socket
import ssl
import struct
import subprocess
import tempfile
import time


def u16(n: int) -> bytes: return struct.pack('<H', n)
def u32(n: int) -> bytes: return struct.pack('<I', n)
def per(n: int) -> bytes: return bytes([n]) if n < 128 else struct.pack('>H', n | 0x8000)
def ber(tag: bytes, body: bytes) -> bytes:
    n = len(body)
    length = bytes([n]) if n < 128 else bytes([0x82]) + struct.pack('>H', n)
    return tag + length + body

def integer(n: int) -> bytes:
    data = n.to_bytes(max(1, (n.bit_length() + 7) // 8), 'big')
    if data[0] & 128: data = b'\x00' + data
    return ber(b'\x02', data)

def tpkt(body: bytes) -> bytes: return b'\x03\x00' + struct.pack('>H', len(body) + 4) + body
def x224(body: bytes) -> bytes: return tpkt(b'\x02\xf0\x80' + body)
def data(channel: int, body: bytes) -> bytes:
    return x224(b'\x64\x00\x00' + struct.pack('>H', channel) + b'\x70' + per(len(body)) + body)

def share(kind: int, body: bytes) -> bytes:
    return u16(len(body) + 6) + u16(0x10 | kind) + u16(1001) + body

def share_data(kind: int, body: bytes) -> bytes:
    return share(7, u32(0x103ea) + b'\x00\x02' + u16(0xda3b if kind == 39 else len(body) + 4) + bytes([kind, 0]) + b'\x00\x00' + body)

def clip(kind: int, flags: int, body: bytes = b'') -> bytes:
    return u16(kind) + u16(flags) + u32(len(body)) + body

def static(body: bytes) -> bytes: return u32(len(body)) + u32(3) + body

def client_connect(width: int = 640, height: int = 480, gfx: bool = False, monitors=(), monitor_attributes=()) -> bytes:
    core = struct.pack('<IHHHHII', 0x80004, width, height, 0xca01, 0xaa03, 0x409, 22631)
    core += 'LRDP-fixture'.encode('utf-16-le').ljust(32, b'\x00')
    core += struct.pack('<III', 4, 0, 12) + bytes(64)
    core += struct.pack('<HHIHHH', 0xca04, 1, 0, 24, 0x0f, (0x100 if gfx else 0) | (0x40 if monitors else 0)) + bytes(64) + bytes([6, 0]) + u32(1)
    assert len(core) == 212
    def block(kind: int, body: bytes) -> bytes: return u16(kind) + u16(len(body) + 4) + body
    channels = u32(2) + b'cliprdr\x00' + u32(0x80800000) + b'drdynvc\x00' + u32(0x80800000)
    blocks = block(0xc001, core) + block(0xc002, u32(0) + u32(0)) + block(0xc003, channels)
    if monitor_attributes:
        # Extended blocks may precede the definitions they augment.
        attrs = b''.join(struct.pack('<5I',*a) for a in monitor_attributes)
        blocks += block(0xc008,u32(0)+u32(20)+u32(len(monitor_attributes))+attrs)
    if monitors:
        defs = b''.join(struct.pack('<iiiiI',*m) for m in monitors)
        blocks += block(0xc005,u32(0)+u32(len(monitors))+defs)
    conference = b'\x00\x08\x00\x10\x00\x01\xc0\x00Duca' + per(len(blocks)) + blocks
    gcc = b'\x00\x05\x00\x14\x7c\x00\x01' + per(len(conference)) + conference
    params = ber(b'\x30', b''.join(integer(x) for x in (34, 2, 0, 1, 0, 1, 65535, 2)))
    body = b'\x04\x01\x01\x04\x01\x01\x01\x01\xff' + params * 3 + ber(b'\x04', gcc)
    return x224(ber(b'\x7f\x65', body))


class Client:
    def __init__(self, raw: socket.socket, cert: pathlib.Path):
        raw.settimeout(8)
        # Deliberately fragmented initial transport header.
        request = tpkt(b'\x0e\xe0\x00\x00\x00\x00\x00\x01\x00\x08\x00' + u32(1))
        raw.sendall(request[:2]); raw.sendall(request[2:])
        self.stream = raw
        assert self.recv_packet() == tpkt(b'\x0e\xd0\x00\x00\x00\x00\x00\x02\x01\x08\x00' + u32(1))
        context = ssl.create_default_context(cafile=str(cert))
        self.stream = context.wrap_socket(raw, server_hostname='localhost')
        self.partials: dict[int, tuple[int, bytes]] = {}
        self.events: list[tuple[int, bytes]] = []
        self.bitmap_count = 0
        self.pixels = bytearray(640 * 480 * 3)
        self.pixel_coverage: set[tuple[int, int]] = set()
        self.remote_text: str | None = None
        self.display_open = False
        self.display_caps: bytes | None = None
        self.dvc_ready = False
        self.formats_acked = False
        self.font_map = False
        self.demand: bytes | None = None

    def exact(self, n: int) -> bytes:
        result = bytearray()
        while len(result) < n:
            part = self.stream.recv(n - len(result))
            if not part: raise EOFError('server disconnected')
            result.extend(part)
        return bytes(result)

    def recv_packet(self) -> bytes:
        header = self.exact(4)
        assert header[:2] == b'\x03\x00'
        n, = struct.unpack('>H', header[2:])
        assert 4 <= n <= 65535
        return header + self.exact(n - 4)

    def send(self, channel: int, body: bytes) -> None: self.stream.sendall(data(channel, body))
    def send_static(self, channel: int, body: bytes) -> None: self.send(channel, static(body))
    def send_share(self, kind: int, body: bytes) -> None: self.send(1003, share_data(kind, body))

    def process(self) -> None:
        packet = self.recv_packet()
        assert packet[4:7] == b'\x02\xf0\x80'
        payload = packet[7:]
        assert payload[0] == 0x68, payload[:8].hex()
        assert payload[1:3] == b'\x00\x01'
        channel, = struct.unpack('>H', payload[3:5])
        assert payload[5] == 0x70
        length = payload[6]
        offset = 7
        if length & 128:
            length = (length & 127) * 256 + payload[7]; offset = 8
        body = payload[offset:]
        assert len(body) == length
        if channel == 1003:
            if body[:2] == b'\x80\x00':
                assert body == bytes.fromhex('80000000ff031000070000000200000004000000')
                self.events.append((0x80, body)); return
            size, kind, source = struct.unpack('<HHH', body[:6])
            assert size == len(body) and source == (0 if kind == 0x17 and body[14] == 55 else 1002)
            if kind == 0x11: self.demand = body; return
            if kind == 0x16: self.events.append((6, body)); return
            assert kind == 0x17 and struct.unpack_from('<I', body, 6)[0] == 0x103ea
            assert struct.unpack_from('<H', body, 12)[0] == len(body)
            subtype = body[14]
            if subtype == 40:
                assert body[18:] == bytes.fromhex('0000000003000400'); self.font_map = True
            elif subtype == 2:
                update, rectangles = struct.unpack_from('<HH', body, 18)
                assert update == 1 and rectangles == 1
                left, top, right, bottom, width, height, depth, flags, byte_count = struct.unpack_from('<9H', body, 22)
                assert right == left + width - 1 and bottom == top + height - 1 and flags == 0 and depth == 24
                stride = (width * 3 + 3) & ~3
                assert byte_count == stride * height and len(body) == 40 + byte_count
                if self.demand and struct.unpack_from('<H', self.demand, 0)[0] > 0:
                    if right < 640 and bottom < 480:
                        for row in range(height):
                            source_offset = 40 + (height - row - 1) * stride
                            destination = ((top + row) * 640 + left) * 3
                            self.pixels[destination:destination + width * 3] = body[source_offset:source_offset + width * 3]
                        self.pixel_coverage.add((left, top))
                self.bitmap_count += 1
            self.events.append((subtype, body))
        else:
            total, flags = struct.unpack_from('<II', body)
            if flags & 1: self.partials[channel] = (total, bytearray())
            expected, partial = self.partials[channel]
            assert expected == total
            partial.extend(body[8:]); assert len(partial) <= total
            if not flags & 2: return
            assert len(partial) == total
            pdu = bytes(partial); del self.partials[channel]
            if channel == 1004: self.on_clipboard(pdu)
            elif channel == 1005: self.on_dvc(pdu)

    def on_clipboard(self, pdu: bytes) -> None:
        kind, flags, length = struct.unpack('<HHI', pdu[:8]); assert length == len(pdu) - 8
        body = pdu[8:]
        if kind == 7:
            assert body == bytes.fromhex('0100000001000c000200000002000000')
            self.send_static(1004, clip(7, 0, body))
        elif kind == 1: assert not body
        elif kind == 2:
            self.send_static(1004, clip(3, 1))
            assert body == u32(13) + u16(0)
            self.send_static(1004, clip(4, 0, u32(13)))
        elif kind == 3: self.formats_acked = flags == 1
        elif kind == 4:
            assert body == u32(13)
            self.send_static(1004, clip(5, 1, 'client → server 🚀\r\n'.encode('utf-16-le') + u16(0)))
        elif kind == 5:
            assert flags == 1
            self.remote_text = body.decode('utf-16-le').removesuffix('\x00')

    def on_dvc(self, pdu: bytes) -> None:
        if pdu[0] == 0x50:
            assert pdu == b'\x50\x00\x01\x00'; self.send_static(1005, pdu); self.dvc_ready = True
        elif pdu[0] == 0x10:
            assert pdu[1] == 1 and pdu[2:] == b'Microsoft::Windows::RDS::DisplayControl\x00'
            self.send_static(1005, b'\x10\x01' + u32(0)); self.display_open = True
        elif pdu[:2] == b'\x30\x01': self.display_caps = pdu[2:]
        else: raise AssertionError(f'unexpected DVC: {pdu[:16].hex()}')

    def until(self, predicate) -> None:
        deadline = time.monotonic() + 15
        while not predicate():
            assert time.monotonic() < deadline, 'condition timed out'
            self.process()

    def confirm(self) -> None:
        assert self.demand is not None
        descriptor, length = struct.unpack_from('<HH', self.demand, 10)
        caps = self.demand[14 + descriptor:14 + descriptor + length]
        self.send(1003, share(3, u32(0x103ea) + u16(1002) + u16(4) + u16(len(caps)) + b'TEST' + caps))
        self.send_share(31, u16(1) + u16(1002))
        self.send_share(20, u16(4) + bytes(6))
        self.send_share(20, u16(1) + bytes(6))
        self.send_share(39, bytes(4) + u16(3) + u16(50))
        self.font_map = False
        self.until(lambda: self.font_map)

    def connect(self) -> None:
        self.stream.sendall(client_connect(gfx=getattr(self, 'use_gfx', False), monitors=getattr(self, 'initial_monitors', ()), monitor_attributes=getattr(self, 'initial_attributes', ())))
        response = self.recv_packet()
        assert response[7:9] == b'\x7f\x66' and b'McDn' in response
        self.stream.sendall(x224(b'\x04\x01\x00\x01\x00') + x224(b'\x28'))
        assert self.recv_packet() == x224(b'\x2e\x00\x00\x00')
        for channel in (1001, 1003, 1004, 1005):
            self.stream.sendall(x224(b'\x38\x00\x00' + struct.pack('>H', channel)))
            assert self.recv_packet() == x224(b'\x3e\x00\x00\x00' + struct.pack('>HH', channel, channel))
        self.send(1003, u16(0x40) + u16(0) + u32(0) + u32(0x10) + bytes(10) + bytes(10))
        self.until(lambda: self.demand is not None)
        assert any(kind == 0x80 for kind, _ in self.events)
        self.confirm()


def run(binary: pathlib.Path, output: pathlib.Path | None = None, client_class=Client, server_args=()) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-test-') as directory:
        root = pathlib.Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
                        '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        server = subprocess.Popen([str(binary), '--lab-no-auth', '--cert', str(cert), '--key', str(key), '--port', str(port), '--once', *server_args],
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert server.stdout is not None
            line = server.stdout.readline(); assert 'listening' in line, line
            client = client_class(socket.create_connection(('127.0.0.1', port)), cert)
            client.connect()
            client.until(lambda: (getattr(client, 'gfx_frames', 0) > 0 if getattr(client, 'use_gfx', False) else len(client.pixel_coverage) == 40) and client.remote_text is not None and client.display_caps is not None)
            assert client.remote_text.startswith('LRDP native protocol laboratory')
            assert client.display_caps == u32(5) + u32(20) + u32(16) + u32(1024) + u32(1024)
            # Golden pixel colors prove BGR ordering and bottom-up rectangle row handling.
            assert client.pixels[:3] == bytes([100, 60, 30])
            index = (200 * 640 + 200) * 3
            assert client.pixels[index:index+3] == bytes([32 + 200 * 72 // 640, 28 + 200 * 60 // 480, 22])
            if output:
                rgb = bytearray(client.pixels)
                for i in range(0, len(rgb), 3): rgb[i], rgb[i + 2] = rgb[i + 2], rgb[i]
                output.write_bytes(b'P6\n640 480\n255\n' + rgb)
            client.send_static(1004, clip(2, 0, u32(13) + u16(0)))
            client.until(lambda: client.formats_acked)
            # Fast-path pointer and F9 key input trigger a new server-side clipboard offer.
            client.remote_text = None
            client.stream.sendall(bytes([8, 11, 0x20]) + u16(0x0800) + u16(300) + u16(240) + bytes([0, 0x43]))
            client.until(lambda: client.remote_text is not None)
            assert client.remote_text.startswith('LRDP server clipboard sample')
            # Exercise actual deactivate/reactivate for a client-requested resize.
            monitor = struct.pack('<IiiIIIIIII', 1, 0, 0, 800, 600, 0, 0, 0, 100, 100)
            layout = u32(2) + u32(56) + u32(40) + u32(1) + monitor
            client.demand = None
            client.send_static(1005, b'\x30\x01' + layout)
            client.until(lambda: client.demand is not None)
            assert any(kind == 6 for kind, _ in client.events)
            client.confirm()
            old_count = getattr(client, 'gfx_frames', client.bitmap_count)
            client.until(lambda: getattr(client, 'gfx_frames', client.bitmap_count) > old_count)
            client.send_share(35, bytes(4)) # Suppress output.
            client.stream.close()
            stdout, stderr = server.communicate(timeout=15)
            assert server.returncode == 0, (stdout, stderr)
            assert 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr, stderr
            print('PASS: verified TLS; GCC/MCS; activation; 40 bitmap tiles; pixel oracle; bidirectional clipboard; fast-path input; display resize/reactivation; suppression; teardown')
        finally:
            if server.poll() is None: server.terminate()
            try: server.communicate(timeout=5)
            except subprocess.TimeoutExpired: server.kill(); server.communicate()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('binary', type=pathlib.Path)
    parser.add_argument('--capture', type=pathlib.Path)
    options = parser.parse_args()
    run(options.binary.resolve(), options.capture)
