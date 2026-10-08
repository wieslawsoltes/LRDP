#!/usr/bin/env python3
"""RDP microphone -> real private PipeWire graph -> RDP speakers PCM oracle."""
from __future__ import annotations
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import Client, u16, u32, per, ber, integer, x224
from pipewire_test import connect_loopback


def sound(kind: int, body: bytes = b'') -> bytes:
    return bytes([kind, 0]) + u16(len(body)) + body


def connect_initial() -> bytes:
    core = struct.pack('<IHHHHII', 0x80004, 640, 480, 0xca01, 0xaa03, 0x409, 22631)
    core += 'LRDP-audio'.encode('utf-16-le').ljust(32, b'\x00')
    core += struct.pack('<III', 4, 0, 12) + bytes(64)
    core += struct.pack('<HHIHHH', 0xca04, 1, 0, 24, 0x0f, 0) + bytes(64) + bytes([6, 0]) + u32(1)
    assert len(core) == 212
    channels = u32(3) + b''.join(name.ljust(8, b'\x00') + u32(0x80800000) for name in (b'cliprdr', b'drdynvc', b'rdpsnd'))
    def block(kind: int, body: bytes) -> bytes: return u16(kind) + u16(len(body) + 4) + body
    blocks = block(0xc001, core) + block(0xc002, bytes(8)) + block(0xc003, channels)
    conference = b'\x00\x08\x00\x10\x00\x01\xc0\x00Duca' + per(len(blocks)) + blocks
    gcc = b'\x00\x05\x00\x14\x7c\x00\x01' + per(len(conference)) + conference
    params = ber(b'\x30', b''.join(integer(n) for n in (34, 2, 0, 1, 0, 1, 65535, 2)))
    return x224(ber(b'\x7f\x65', b'\x04\x01\x01\x04\x01\x01\x01\x01\xff' + params * 3 + ber(b'\x04', gcc)))


class AudioClient(Client):
    def __init__(self, *args):
        super().__init__(*args)
        self.playback_ready = False
        self.microphone_ready = False
        self.matched_frames = 0
        self.wave_blocks = 0

    def connect(self) -> None:
        self.stream.sendall(connect_initial())
        assert self.recv_packet()[7:9] == b'\x7f\x66'
        self.stream.sendall(x224(b'\x04\x01\x00\x01\x00') + x224(b'\x28'))
        assert self.recv_packet() == x224(b'\x2e\x00\x00\x00')
        for channel in (1001, 1003, 1004, 1005, 1006):
            self.stream.sendall(x224(b'\x38\x00\x00' + struct.pack('>H', channel)))
            assert self.recv_packet() == x224(b'\x3e\x00\x00\x00' + struct.pack('>HH', channel, channel))
        self.send(1003, u16(0x40) + u16(0) + u32(0) + u32(0x10) + bytes(20))
        self.until(lambda: self.demand is not None)
        self.confirm()

    def process(self) -> None:
        packet = self.recv_packet()
        assert packet[4:8] == b'\x02\xf0\x80\x68'
        payload = packet[7:]
        assert payload[1:3] == b'\x00\x01' and payload[5] == 0x70
        channel, = struct.unpack_from('>H', payload, 3)
        length, offset = payload[6], 7
        if length & 0x80: length, offset = (length & 127) * 256 + payload[7], 8
        body = payload[offset:]
        assert len(body) == length
        if channel == 1003:
            if body[:2] == b'\x80\x00': return
            size, kind, source = struct.unpack_from('<HHH', body)
            assert size == len(body) and source == 1002
            if kind == 0x11: self.demand = body
            elif kind == 0x17:
                assert len(body) >= 18
                if body[14] == 40: self.font_map = True
            return
        total, flags = struct.unpack_from('<II', body)
        assert total <= 1024 * 1024
        if flags & 1:
            assert channel not in self.partials
            self.partials[channel] = (total, bytearray())
        expected, partial = self.partials[channel]
        assert expected == total
        partial.extend(body[8:]); assert len(partial) <= expected
        if not flags & 2: return
        assert len(partial) == expected
        pdu = bytes(partial); del self.partials[channel]
        if channel == 1004: self.on_clipboard(pdu)
        elif channel == 1005: self.on_dvc(pdu)
        elif channel == 1006: self.on_sound(pdu)
        else: raise AssertionError(f'unexpected channel {channel}')

    def on_sound(self, pdu: bytes) -> None:
        kind, pad, length = struct.unpack_from('<BBH', pdu)
        body = pdu[4:]; assert length == len(body)
        if kind == 7:
            assert len(body) == 38 and struct.unpack_from('<H', body, 14)[0] == 1
            pcm = body[20:]
            assert struct.unpack('<HHIIHHH', pcm) == (1, 2, 48000, 192000, 4, 16, 0)
            formats = u32(1) + bytes(10) + u16(1) + bytes([0]) + u16(8) + bytes([0]) + pcm
            self.send_static(1006, sound(7, formats))
            self.send_static(1006, sound(12, u16(2) + u16(0)))
        elif kind == 6:
            assert len(body) == 4 and body[2:] == bytes(2)
            self.send_static(1006, sound(6, body)); self.playback_ready = True
        elif kind == 13:
            assert self.playback_ready and len(body) >= 12
            stamp, index, block = struct.unpack_from('<HHB', body)
            assert index == 0 and (len(body) - 12) % 4 == 0
            for left, right in struct.iter_unpack('<hh', body[12:]):
                if abs(left - 0x1234) <= 1 and abs(right - 0x1234) <= 1: self.matched_frames += 1
            self.wave_blocks += 1
            self.send_static(1006, sound(5, u16(stamp) + bytes([block, 0])))
        elif kind == 1: raise AssertionError('audio closed before the sample oracle completed')
        else: raise AssertionError(f'unexpected RDPSND type {kind}')

    def send_microphone(self, message: bytes) -> None:
        if len(message) <= 1598:
            self.send_static(1005, b'\x30\x03' + message)
            return
        assert len(message) <= 65535
        self.send_static(1005, b'\x24\x03' + u16(len(message)) + message[:1596])
        for offset in range(1596, len(message), 1598): self.send_static(1005, b'\x30\x03' + message[offset:offset + 1598])

    def on_dvc(self, pdu: bytes) -> None:
        if pdu[:2] == b'\x10\x03':
            assert pdu[2:] == b'AUDIO_INPUT\x00'
            self.send_static(1005, b'\x10\x03' + u32(0)); return
        if pdu[:2] == b'\x30\x03':
            message = pdu[2:]
            if message[0] == 1:
                assert message == b'\x01' + u32(2); self.send_microphone(message)
            elif message[0] == 2:
                assert len(message) == 27 and struct.unpack_from('<II', message, 1) == (1, 27)
                assert struct.unpack_from('<HHIIHHH', message, 9) == (1, 1, 48000, 96000, 2, 16, 0)
                self.send_microphone(b'\x05'); self.send_microphone(message)
            elif message[0] == 3:
                assert len(message) == 27 and struct.unpack_from('<II', message, 1) == (960, 0)
                self.send_microphone(b'\x07' + u32(0)); self.send_microphone(b'\x04' + u32(0))
                self.microphone_ready = True
            else: raise AssertionError(f'unknown microphone message {message[0]}')
            return
        super().on_dvc(pdu)


def run(binary: str) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-audio-rdp-') as directory:
        root = pathlib.Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost', '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        with (root / 'server.log').open('w+') as log:
            server = subprocess.Popen([binary, '--lab-no-auth', '--cert', str(cert), '--key', str(key), '--port', str(port),
                                       '--audio', '--microphone', '--encoder', 'raw', '--once'], stdout=log, stderr=subprocess.STDOUT)
            client = None
            try:
                deadline = time.monotonic() + 10
                while True:
                    log.seek(0)
                    if 'listening' in log.read(): break
                    if server.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('audio RDP server failed to start')
                    time.sleep(0.02)
                client = AudioClient(socket.create_connection(('127.0.0.1', port)), cert)
                client.connect(); client.until(lambda: client.playback_ready and client.microphone_ready)
                connect_loopback(server.pid, dict(os.environ))
                pcm = b'\x34\x12' * 960
                deadline = time.monotonic() + 15
                next_samples = time.monotonic()
                suppressed = False
                while client.matched_frames < 3840:
                    now = time.monotonic()
                    if now >= deadline: raise RuntimeError(f'audio roundtrip timed out: {client.matched_frames} matched frames, {client.wave_blocks} wave blocks')
                    if now >= next_samples:
                        client.send_microphone(b'\x05'); client.send_microphone(b'\x06' + pcm)
                        next_samples = now + 0.02
                    if client.matched_frames >= 1920 and not suppressed:
                        client.send_share(35, bytes(4)); suppressed = True
                    if client.stream.pending() or select.select([client.stream], [], [], 0.002)[0]: client.process()
                assert suppressed
                client.stream.close(); client = None
                assert server.wait(timeout=10) == 0
                log.seek(0); text = log.read()
                assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
                print('PASS: verified TLS, AUDIO_INPUT fragmentation, native PipeWire routing, RDPSND sample oracle and audio while graphics suppressed')
            except Exception:
                log.seek(0); print('AUDIO RDP SERVER LOG:\n' + log.read()); raise
            finally:
                if client is not None: client.stream.close()
                if server.poll() is None: server.terminate()
                try: server.wait(timeout=5)
                except subprocess.TimeoutExpired: server.kill(); server.wait()


if __name__ == '__main__': run(str(pathlib.Path(sys.argv[1]).resolve()))
