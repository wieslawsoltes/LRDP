#!/usr/bin/env python3
"""Independent RDPDR wire peer drives the production Session and native facade.
Uses no FUSE kernel access, Windows client, or other RDP implementation source.
"""
from __future__ import annotations
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
from integration import Client, client_connect, x224, u16, u32


def u64(n: int) -> bytes: return struct.pack('<Q', n)
def packet(kind: int, body: bytes = b'') -> bytes: return b'\x72\x44' + u16(kind) + body
def pattern(n: int) -> bytearray: return bytearray((i*17+3) % 256 for i in range(n))


class DriveClient(Client):
    def __init__(self, *args):
        super().__init__(*args)
        self.nodes = {'': None, 'folder': None, 'hello.txt': bytearray(b'hello'), '日本語.bin': pattern(180003)}
        self.handles: dict[int, str] = {}
        self.enumerated: set[int] = set()
        self.deletes: set[int] = set()
        self.next_file = 1
        self.writes = 0
        self.operations: set[int] = set()

    def connect(self):
        self.stream.sendall(client_connect().replace(b'drdynvc\0', b'rdpdr\0\0\0'))
        assert self.recv_packet()[7:9] == b'\x7f\x66'
        self.stream.sendall(x224(b'\x04\x01\x00\x01\x00') + x224(b'\x28'))
        assert self.recv_packet() == x224(b'\x2e\x00\x00\x00')
        for channel in (1001, 1003, 1004, 1005):
            self.stream.sendall(x224(b'\x38\0\0' + struct.pack('>H', channel)))
            assert self.recv_packet() == x224(b'\x3e\0\0\0' + struct.pack('>HH', channel, channel))
        self.send(1003, u16(0x40) + u16(0) + u32(0) + u32(0x10) + bytes(20))
        self.until(lambda: self.demand is not None)
        self.confirm()
        self.send_share(35, bytes(4))  # Drive I/O must continue while graphics are suppressed.

    def send_drive(self, kind: int, body: bytes = b''):
        message = packet(kind, body)
        # Fragments deliberately split response headers as well as large data.
        for offset in range(0, len(message), 733):
            fragment = message[offset:offset+733]
            flags = (1 if offset == 0 else 0) | (2 if offset+len(fragment) == len(message) else 0)
            self.send(1005, u32(len(message)) + u32(flags) + fragment)

    def on_dvc(self, message: bytes):
        # The fixture substitutes rdpdr for the second static channel. This hook
        # receives reassembled static bytes, not dynamic-channel headers.
        assert message[:2] == b'\x72\x44'
        kind, = struct.unpack_from('<H', message, 2)
        body = message[4:]
        if kind == 0x496e:
            major, minor, client_id = struct.unpack('<HHI', body)
            assert (major, minor) == (1, 13)
            self.send_drive(0x4343, body)
            name = 'independent-peer\0'.encode('utf-16-le')
            self.send_drive(0x434e, u32(1)+u32(0)+u32(len(name))+name)
        elif kind == 0x5350:
            cap = u16(1)+u16(44)+u32(2)+u32(0)+u32(0)+u16(1)+u16(13)+u32(0x3fff)+u32(0)+u32(7)+u32(0)+u32(0)+u32(0)
            self.send_drive(0x4350, u16(2)+u16(0)+cap+u16(4)+u16(8)+u32(2))
        elif kind == 0x4343: assert len(body) == 8
        elif kind == 0x554c:
            self.send_drive(0x4441, u32(1)+u32(8)+u32(7)+b'DATA\0\0\0\0'+u32(0))
        elif kind == 0x6472: assert body == u32(7)+u32(0)
        elif kind == 0x4952: self.request(body)
        else: raise AssertionError(f'unexpected RDPDR packet {kind:x}')

    def request(self, body: bytes):
        device, file_id, completion, major, minor = struct.unpack_from('<5I', body)
        assert device == 7 and completion != 0
        body = body[20:]
        self.operations.add(major)
        status, response = 0, b''
        if major == 0:
            access, allocation, attrs, share, disposition, options, length = struct.unpack_from('<IQ5I', body)
            assert allocation == 0 and share == 7 and options & 0x200000
            assert len(body) == 32+length
            name = body[32:].decode('utf-16-le')
            assert name.startswith('\\') and name.endswith('\0')
            name = name[1:-1].replace('\\', '/')
            exists = name in self.nodes
            if not exists and disposition in (1, 4): status = 0xc0000034
            elif exists and disposition == 2: status = 0xc0000035
            elif exists and bool(options & 1) and self.nodes[name] is not None: status = 0xc0000103
            elif exists and bool(options & 0x40) and self.nodes[name] is None: status = 0xc00000ba
            else:
                if not exists: self.nodes[name] = None if options & 1 else bytearray()
                if disposition in (4, 5): self.nodes[name] = bytearray()
                file_id = self.next_file; self.next_file += 1
                self.handles[file_id] = name
                response = u32(file_id)+b'\x01'
        elif file_id not in self.handles: status = 0xc0000008
        else:
            name = self.handles[file_id]
            content = self.nodes[name]
            size = len(content) if content is not None else 0
            attrs = 0x10 if content is None else 0x80
            if major == 2:
                del self.handles[file_id]
                if file_id in self.deletes: del self.nodes[name]; self.deletes.remove(file_id)
                response = bytes(5)
            elif major == 5:
                info, = struct.unpack_from('<I', body)
                assert len(body) == 32
                if info == 4: payload = u64(116444756000000000)*4+u32(attrs)
                elif info == 5: payload = u64((size+511)//512*512)+u64(size)+u32(1)+bytes([0, int(content is None)])
                else: raise AssertionError(f'unexpected information class {info}')
                response = u32(len(payload))+payload
            elif major == 3:
                length, offset = struct.unpack_from('<IQ', body)
                assert length <= 65536 and len(body) == 32 and content is not None
                data = content[offset:offset+length]
                response = u32(len(data))+data
            elif major == 4:
                length, offset = struct.unpack_from('<IQ', body)
                assert len(body) == 32+length and content is not None and offset < 1024*1024
                length = min(length, 4093)  # Legal short writes must not lose data.
                if offset+length > len(content): content.extend(bytes(offset+length-len(content)))
                content[offset:offset+length] = body[32:32+length]
                self.writes += 1; response = u32(length)+b'\0'
            elif major == 12:
                assert minor == 1 and content is None
                if file_id in self.enumerated: status = 0x80000006
                else:
                    self.enumerated.add(file_id)
                    prefix = name+'/' if name else ''
                    records = []
                    for entry, data in sorted(self.nodes.items()):
                        if not entry.startswith(prefix) or not entry or '/' in entry[len(prefix):]: continue
                        encoded = entry[len(prefix):].encode('utf-16-le')
                        n = len(data) if data is not None else 0
                        records.append(u32(0)+u32(0)+u64(116444756000000000)*4+u64(n)+u64(n)+u32(0x10 if data is None else 0x80)+u32(len(encoded))+encoded)
                    payload = bytearray()
                    for i, record in enumerate(records):
                        if i+1 != len(records):
                            next_entry = (len(record)+7)&~7
                            record = u32(next_entry)+record[4:]+bytes(next_entry-len(record))
                        payload.extend(record)
                    response = u32(len(payload))+payload
            elif major == 6:
                info, length = struct.unpack_from('<II', body)
                data = body[32:]; assert len(data) == length
                if info == 20:
                    new_size, = struct.unpack('<Q', data)
                    assert content is not None and new_size < 1024*1024
                    self.nodes[name] = content[:new_size] + bytes(max(0, new_size-len(content)))
                elif info == 13:
                    assert not data
                    self.deletes.add(file_id)
                elif info == 10:
                    replace, root, n = struct.unpack_from('<BBI', data)
                    assert root == 0 and n == len(data)-6
                    destination = data[6:].decode('utf-16-le')[1:].replace('\\', '/')
                    if destination in self.nodes and not replace: status = 0xc0000035
                    else:
                        self.nodes[destination] = self.nodes.pop(name)
                        self.handles[file_id] = destination
                else: raise AssertionError(f'unexpected set class {info}')
                response = u32(length)
            else: raise AssertionError(f'unexpected major function {major}')
        self.send_drive(0x4943, u32(device)+u32(completion)+u32(status)+(response if status == 0 else b''))


def run(binary: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix='lrdp-drive-session-') as temp:
        root = pathlib.Path(temp); cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)], check=True, capture_output=True)
        for mode in ('readonly', 'writable'):
            server = subprocess.Popen([str(binary),str(cert),str(key),mode],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            peer = None
            try:
                line = server.stdout.readline(); assert line.startswith('READY '), line
                peer = DriveClient(socket.create_connection(('127.0.0.1',int(line.split()[1]))),cert)
                peer.connect()
                try:
                    while True: peer.process()
                except EOFError: pass
                stdout, stderr = server.communicate(timeout=5)
                assert server.returncode == 0 and 'PASS:' in stdout, (stdout,stderr)
                assert 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr, stderr
                assert not peer.handles, peer.handles
                assert peer.operations >= {0,2,3,5,12}
                assert peer.writes > 40 if mode == 'writable' else peer.writes == 0
                assert set(peer.nodes) == {'','folder','hello.txt','日本語.bin'}
                print(stdout.strip())
            finally:
                if peer: peer.stream.close()
                if server.poll() is None: server.terminate()
                try: server.communicate(timeout=5)
                except subprocess.TimeoutExpired: server.kill(); server.communicate()

if __name__ == '__main__': run(pathlib.Path(sys.argv[1]).resolve())
