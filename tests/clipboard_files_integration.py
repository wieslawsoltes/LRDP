#!/usr/bin/env python3
"""Independent TCP/TLS file clipboard -> private Xorg -> native application paste."""
from __future__ import annotations
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
from integration import Client, clip, u16, u32


def descriptors(files):
    data = u32(len(files))
    for name, directory, size in files:
        encoded = name.replace('/', '\\').encode('utf-16-le') + b'\0\0'
        assert len(encoded) <= 520
        flags = 4 | (0x40 if size is not None and not directory else 0)
        data += u32(flags) + bytes(32) + u32(0x10 if directory else 0x80) + bytes(24)
        data += u32((size or 0) >> 32) + u32((size or 0) & 0xffffffff) + encoded.ljust(520, b'\0')
    return data


def decode_descriptors(data):
    count, = struct.unpack_from('<I', data)
    assert len(data) == 4 + 592 * count
    result = []
    for i in range(count):
        record = data[4+i*592:4+(i+1)*592]
        flags, attrs, high, low = (struct.unpack_from('<I', record, j)[0] for j in (0,36,64,68))
        name = record[72:].decode('utf-16-le').split('\0', 1)[0].replace('\\', '/')
        assert not name.startswith('/') and '..' not in name.split('/') and ':' not in name
        result.append((name, bool(attrs & 0x10), (high << 32 | low) if flags & 0x40 else None))
    return result


class FileClient(Client):
    def __init__(self, *args):
        super().__init__(*args)
        self.file_list = None
        self.native = {}
        self.native_pending = {}
        self.stream_id = 1000
        self.remote_lock = None
        self.remote_unlocks = 0
        self.remote_requests = []
        self.remote_data = bytes((i*7+3) & 255 for i in range(200007))
        self.remote_descriptors = [('Remote folder', True, 0), ('Remote folder/żółć 🚀.bin', False, None), ('zero', False, 0)]
        self.native_done = False

    def send_static(self, channel, body):
        # Independent implementation of static-channel fragmentation.
        for offset in range(0, max(1, len(body)), 1600):
            part = body[offset:offset+1600]
            flags = (1 if offset == 0 else 0) | (2 if offset+len(part) == len(body) else 0)
            self.send(channel, u32(len(body)) + u32(flags) + part)

    def native_request(self, index, kind, offset, count):
        self.stream_id += 1
        self.native_pending[self.stream_id] = (index, kind, offset, count)
        request = struct.pack('<IIIIIII', self.stream_id, index, kind, offset, 0, count, 71)
        self.send_static(1004, clip(8, 0, request))

    def on_clipboard(self, pdu):
        kind, flags, size = struct.unpack_from('<HHI', pdu)
        body = pdu[8:]
        assert size == len(body)
        if kind == 7:
            assert struct.unpack_from('<I', body, 12)[0] == 0x1e
            self.send_static(1004, clip(7, 0, body))
        elif kind == 1:
            assert not body
        elif kind == 2:
            self.send_static(1004, clip(3, 1))
            if not body: return
            identifier, = struct.unpack_from('<I', body)
            assert body[4:].decode('utf-16-le') == 'FileGroupDescriptorW\0'
            self.send_static(1004, clip(10, 0, u32(71)))
            self.send_static(1004, clip(4, 0, u32(identifier)))
        elif kind == 5:
            assert flags == 1
            self.file_list = decode_descriptors(body)
            assert [d[0] for d in self.file_list] == ['Native folder', 'Native folder/empty', 'Native folder/native.bin', 'native-zero']
            for index, (name, directory, length) in enumerate(self.file_list):
                if directory: continue
                assert length is not None
                self.native[name] = bytearray(length)
                self.native_request(index, 1, 0, 8)
                for offset in reversed(range(0, length, 60000)):
                    self.native_request(index, 2, offset, min(60000, length-offset))
        elif kind == 9:
            assert flags == 1
            identifier, = struct.unpack_from('<I', body)
            index, operation, offset, count = self.native_pending.pop(identifier)
            name, _, length = self.file_list[index]
            if operation == 1: assert struct.unpack('<Q', body[4:])[0] == length
            else:
                assert len(body[4:]) == count
                self.native[name][offset:offset+count] = body[4:]
            if not self.native_pending:
                self.native_done = True
                self.send_static(1004, clip(11, 0, u32(71)))
        elif kind == 3:
            assert flags == 1
        elif kind == 10:
            self.remote_lock, = struct.unpack('<I', body)
        elif kind == 11:
            assert struct.unpack('<I', body)[0] == self.remote_lock
            self.remote_unlocks += 1
        elif kind == 4:
            assert self.remote_lock is not None and body == u32(0xc888)
            self.send_static(1004, clip(5, 1, descriptors(self.remote_descriptors)))
        elif kind == 8:
            stream, index, operation, low, high, count, lock = struct.unpack('<IIIIIII', body)
            assert lock == self.remote_lock and high == 0 and index == 1
            if operation == 1:
                assert low == 0 and count == 8
                self.send_static(1004, clip(9, 1, u32(stream) + struct.pack('<Q', len(self.remote_data))))
            else:
                assert operation == 2 and 0 < count <= 65536 and low+count <= len(self.remote_data)
                self.remote_requests.append((stream, self.remote_data[low:low+count]))
                if len(self.remote_requests) == 4:
                    for identifier, data in reversed(self.remote_requests):
                        self.send_static(1004, clip(9, 1, u32(identifier)+data))
                    self.remote_requests.clear()
        else:
            raise AssertionError(f'unexpected clipboard type {kind}')

    def offer_remote(self):
        self.send_static(1004, clip(2, 0, u32(0xc888) + 'FileGroupDescriptorW\0'.encode('utf-16-le')))


def await_log(path, marker, server, timeout=8):
    deadline = time.monotonic()+timeout
    while True:
        text = path.read_text() if path.exists() else ''
        if marker in text: return text
        assert 'FAIL' not in text and server.poll() is None and time.monotonic() < deadline, text
        time.sleep(0.01)


def run(binary, application):
    with tempfile.TemporaryDirectory(prefix='lrdp-file-session-') as directory:
        root = pathlib.Path(directory)
        shared = root/'shared'; shared.mkdir(mode=0o700)
        native = shared/'Native folder'; (native/'empty').mkdir(parents=True)
        original = bytes((i*13+9) & 255 for i in range(180003))
        (native/'native.bin').write_bytes(original); (shared/'native-zero').write_bytes(b'')
        (shared/'offer.uris').write_text(native.as_uri()+'\r\n'+(shared/'native-zero').as_uri()+'\r\n')
        cert, key, app_log = root/'cert.pem', root/'key.pem', root/'app.log'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
            '-addext','subjectAltName=DNS:localhost','-out',str(cert),'-keyout',str(key)],check=True,capture_output=True)
        with socket.socket() as s: s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
        with (root/'server.log').open('w+') as log:
            server = subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                '--backend','headless','--desktop-command',application,'--desktop-arg',str(shared),'--desktop-arg',str(app_log),
                '--clipboard-files',str(shared),'--encoder','raw','--once'],stdout=log,stderr=subprocess.STDOUT)
            client = None
            try:
                await_log(root/'server.log', 'listening', server)
                client = FileClient(socket.create_connection(('127.0.0.1',port)),cert); client.connect()
                client.until(lambda: client.native_done and len(client.pixel_coverage) == 40)
                assert client.native['Native folder/native.bin'] == original and not client.native['native-zero']
                # Public pointer PDUs must contain the native shape, not a diagnostic default arrow.
                def cursor_ready():
                    return any(kind == 27 and body[18:20] == u16(8) and struct.unpack_from('<HH',body,30) == (3,3) for kind,body in client.events)
                client.until(cursor_ready)
                cursors = [body for kind,body in client.events if kind == 27 and body[18:20] == u16(8)]
                assert any(struct.unpack_from('<HH',body,26) == (1,2) for body in cursors)
                client.send_share(35, bytes(4)) # Clipboard must continue with graphics suppressed.
                client.offer_remote(); client.until(lambda: client.remote_unlocks == 1)
                await_log(app_log,'REMOTE_OWNER',server)
                client.stream.sendall(bytes([4,9,0x20])+u16(0x9000)+u16(50)+u16(50))
                pasted = await_log(app_log,'PASTE_OK 200007',server)
                paths = [pathlib.Path(urllib.parse.unquote(line[11:])) for line in pasted.splitlines() if line.startswith('URI file://')]
                assert len(paths) == 2 and all(p.exists() for p in paths)
                assert all(str(p).startswith(str(shared)+'/') for p in paths)
                # Malicious names fail without publication and without breaking the session.
                client.remote_descriptors = [('../escape',False,1)]
                client.offer_remote(); client.until(lambda: client.remote_unlocks == 2)
                assert not (root/'escape').exists() and len(list(shared.glob('.lrdp-clipboard-*'))) == 1
                client.stream.close(); client=None
                assert server.wait(timeout=10) == 0
                assert all(not p.exists() for p in paths) and (native/'native.bin').read_bytes() == original
                log.seek(0); text=log.read()
                assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
                print('PASS: native cursor/hotspot; TLS file descriptors/locks/ranges; 180003-byte export; 200007-byte out-of-order import with native application paste; suppressed graphics; traversal rejection; staging cleanup')
            except Exception:
                log.seek(0); print(log.read()); print(app_log.read_text() if app_log.exists() else 'no native log'); raise
            finally:
                if client: client.stream.close()
                if server.poll() is None: server.terminate()
                try: server.wait(timeout=10)
                except subprocess.TimeoutExpired: server.kill(); server.wait()

if __name__ == '__main__': run(str(pathlib.Path(sys.argv[1]).resolve()),str(pathlib.Path(sys.argv[2]).resolve()))
