#!/usr/bin/env python3
"""Independent Python wire peer through production MCS/Session/TLS, no C++ parser reuse."""
from __future__ import annotations
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import Client, ber, integer, per, tpkt, x224, u16, u32


def connect_initial(netchar: bool, request_channel: bool, count: int = 2) -> bytes:
    core = bytearray(212)
    struct.pack_into('<IHHHHII', core, 0, 0x80004, 640, 480, 0xca01, 0xaa03, 0x409, 22631)
    struct.pack_into('<HHIHHH', core, 128, 0xca04, 1, 0, 24, 15, 0x80 if netchar else 0)
    struct.pack_into('<I', core, 208, 1)
    def block(kind: int, body: bytes) -> bytes:
        return u16(kind) + u16(len(body) + 4) + body
    names = ['cliprdr', 'drdynvc'] + [f'test{i}' for i in range(count - 2)]
    channels = u32(count) + b''.join(n.encode().ljust(8, b'\0') + u32(0x80800000) for n in names[:count])
    # Test order independence: extended block precedes both Core and Network.
    blocks = block(0xc006, u32(0)) if request_channel else b''
    blocks += block(0xc002, bytes(8)) + block(0xc003, channels) + block(0xc001, core)
    conference = b'\x00\x08\x00\x10\x00\x01\xc0\x00Duca' + per(len(blocks)) + blocks
    gcc = b'\x00\x05\x00\x14\x7c\x00\x01' + per(len(conference)) + conference
    domain = ber(b'\x30', b''.join(integer(x) for x in (35, 3, 0, 1, 0, 1, 65535, 2)))
    return x224(ber(b'\x7f\x65', b'\x04\x01\x01\x04\x01\x01\x01\x01\xff' + domain * 3 + ber(b'\x04', gcc)))


def mcs_body(packet: bytes) -> tuple[int, bytes] | None:
    if len(packet) < 14 or packet[7] != 0x68:
        return None
    channel, = struct.unpack_from('>H', packet, 10)
    n = packet[13]
    offset = 14
    if n & 128:
        n = (n & 127) * 256 + packet[14]
        offset += 1
    assert offset + n == len(packet)
    return channel, packet[offset:]


def response_channels(packet: bytes, expected_count: int) -> tuple[list[int], int]:
    # Parse complete response block boundaries, not a presumed channel offset.
    marker = packet.index(b'McDn') + 4
    n = packet[marker]; marker += 1
    if n & 128:
        n = (n & 127) * 256 + packet[marker]; marker += 1
    assert marker + n == len(packet)
    static, message, seen = [], 0, set()
    while marker < len(packet):
        kind, size = struct.unpack_from('<HH', packet, marker)
        assert size >= 4 and kind not in seen and marker + size <= len(packet)
        seen.add(kind)
        body = packet[marker + 4:marker + size]
        if kind == 0x0c03:
            global_id, count = struct.unpack_from('<HH', body)
            assert global_id == 1003 and count == expected_count
            static = list(struct.unpack_from(f'<{count}H', body, 4))
            assert len(body) == 4 + count * 2 + (count % 2) * 2
        elif kind == 0x0c04:
            assert size == 6
            message, = struct.unpack('<H', body)
        marker += size
    assert {0xc01, 0xc02, 0xc03}.issubset(seen)
    assert not message or message not in static + [1001, 1002, 1003]
    return static, message


class NetworkClient(Client):
    def __init__(self, *args):
        self.message_channel = 0
        self.rtt_requests = self.bandwidth_requests = 0
        self.started = None
        self.counted = 0
        self.cached_packet = None
        self.reply = True
        super().__init__(*args)

    def recv_packet(self) -> bytes:
        if self.cached_packet is not None:
            packet, self.cached_packet = self.cached_packet, None
            return packet
        return super().recv_packet()

    def process(self) -> None:
        packet = super().recv_packet()
        decoded = mcs_body(packet)
        if decoded and self.message_channel and decoded[0] == self.message_channel:
            body = decoded[1]
            assert self.font_map, 'network probe overtook activation completion'
            assert len(body) == 10
            flags, high, size, direction, seq, kind = struct.unpack('<HHBBHH', body)
            assert (flags, high, size, direction) == (0x1000, 0, 6, 0)
            if kind == 1:
                self.rtt_requests += 1
                # Ensure a measurable peer delay without relying on wall-clock synchronization.
                time.sleep(0.012)
                if self.reply:
                    self.send(self.message_channel, u16(0x2030) + u16(0xbeef) + bytes([6,1]) + u16(seq) + u16(0))
            elif kind == 0x14:
                assert self.started is None
                self.started = time.monotonic(); self.counted = 0
                # Request an ordinary desktop repaint, not an artificial bandwidth
                # payload. Initial activation may already have drained before START.
                self.send_share(33, bytes([1,0,0,0]) + struct.pack('<HHHH',0,0,639,479))
            else:
                assert kind == 0x429 and self.started is not None, 'only continuous probes are supported'
                elapsed = max(1, int((time.monotonic() - self.started) * 1000))
                if self.reply:
                    self.send(self.message_channel, u16(0x2000) + u16(0) + bytes([14,1]) + u16(seq) + u16(11) + u32(elapsed) + u32(self.counted))
                self.bandwidth_requests += 1; self.started = None
            return
        if self.started is not None:
            self.counted += len(decoded[1]) if decoded else len(packet)
        self.cached_packet = packet
        super().process()

    def connect(self, netchar=True, request_channel=True, expected_channel=True, count=2, before_activation=False):
        self.stream.sendall(connect_initial(netchar, request_channel, count))
        channels, self.message_channel = response_channels(self.recv_packet(), count)
        assert bool(self.message_channel) == expected_channel
        if self.message_channel:
            assert self.message_channel == 1004 + count
        self.stream.sendall(x224(b'\x04\x01\x00\x01\x00') + x224(b'\x28'))
        assert self.recv_packet() == x224(b'\x2e\x00\x00\x00')
        # Dedicated channel may be joined first, not presumed last.
        for channel in ([self.message_channel] if self.message_channel else []) + [1001, 1003] + channels:
            self.stream.sendall(x224(b'\x38\x00\x00' + struct.pack('>H', channel)))
            assert self.recv_packet() == x224(b'\x3e\x00\x00\x00' + struct.pack('>HH', channel, channel))
        if before_activation:
            self.send(self.message_channel, u16(0x2000) + u16(0) + bytes([6,1]) + u16(1) + u16(0))
            return
        self.send(1003, u16(0x40) + u16(0) + u32(0) + u32(0x10) + bytes(20))
        self.until(lambda: self.demand is not None)
        self.confirm()


def run(binary: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-network-') as directory:
        root = pathlib.Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)], check=True, capture_output=True)
        # CLI opt-in and explicit peer support are independently required.
        scenarios = [(True,True,True,2,False), (False,True,True,2,False), (True,False,True,2,False),
                     (True,True,False,31,False), (True,True,True,2,True)]
        for index, (enabled, netchar, request_channel, count, early) in enumerate(scenarios):
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1',0)); port = reservation.getsockname()[1]
            with (root / f'server-{index}.log').open('w+') as log:
                args = [str(binary),'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),'--once','--encoder','raw']
                if enabled: args += ['--network-metrics']
                server = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, text=True)
                client = None
                try:
                    deadline = time.monotonic() + 10
                    while True:
                        log.seek(0)
                        if 'listening' in log.read(): break
                        assert server.poll() is None and time.monotonic() < deadline, 'server startup failed'
                        time.sleep(0.02)
                    client = NetworkClient(socket.create_connection(('127.0.0.1',port)),cert)
                    client.connect(netchar,request_channel,enabled and netchar,count,early)
                    if early:
                        try:
                            client.recv_packet()
                            raise AssertionError('network data before activation was accepted')
                        except (EOFError, ConnectionResetError): pass
                    else:
                        client.until(lambda: len(client.pixel_coverage) == 40)
                        assert client.pixels[:3] == bytes([100,60,30])
                        if enabled and netchar:
                            client.until(lambda: client.rtt_requests >= 1 and client.bandwidth_requests >= 1)
                            if index == 0:
                                client.send_share(35, bytes(4))
                                old = client.rtt_requests
                                client.until(lambda: client.rtt_requests >= old + 3)
                                # Unknown/duplicate valid replies never release a pending measurement.
                                client.send(client.message_channel, u16(0x2000)+u16(0)+bytes([6,1])+u16(65535)+u16(0))
                                client.send_share(35, b'\x01\x00\x00\x00' + struct.pack('<HHHH',0,0,639,479))
                                before = client.bitmap_count
                                client.until(lambda: client.bitmap_count > before)
                                log.seek(0); evidence = log.read()
                                assert re.search(r'rtt_us=\d+ .*rtt_samples=[1-9]', evidence), evidence
                                assert re.search(r'peer_receive_kbps=\d+ .*bandwidth_samples=[1-9]', evidence), evidence
                        else:
                            assert not client.rtt_requests and not client.message_channel
                    client.stream.close(); client = None
                    server.wait(timeout=10); assert server.returncode == 0
                    log.seek(0); evidence = log.read()
                    assert 'AddressSanitizer' not in evidence and 'runtime error:' not in evidence, evidence
                    if not early: assert 'Session active' in evidence, evidence
                    else: assert 'network measurement before RDP activation' in evidence, evidence
                except Exception:
                    log.seek(0); print(log.read()); raise
                finally:
                    if client: client.stream.close()
                    if server.poll() is None: server.terminate()
                    try: server.wait(timeout=5)
                    except subprocess.TimeoutExpired: server.kill(); server.wait()
        print('PASS: MCS message negotiation/join, 31 static channels, opt-in/capability refusal, early-PDU rejection, '
              'real TLS egress RTT, passive bandwidth, stale replies, graphics suppression/resume and pixels')


if __name__ == '__main__':
    run(pathlib.Path(sys.argv[1]).resolve())
