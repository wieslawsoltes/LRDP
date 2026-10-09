#!/usr/bin/env python3
"""Real TLS/RDP AVC444/v2 fixture with a separately linked native H.264 decoder.

No LRDP encoder, plane-packing or graphics parser is imported into the oracle.
Windows-client or hardware interoperability is not implied by this test.
"""
from __future__ import annotations
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
from gfx_integration import GraphicsClient, gfx
from integration import u16, u32


class AvcClient(GraphicsClient):
    def __init__(self, raw: socket.socket, certificate: pathlib.Path, version: int):
        self.version = version
        self.generations: list[dict] = []
        self.current_frame: tuple[bytes, bytes] | None = None
        self.next_identifier = 0
        super().__init__(raw, certificate)

    def on_dvc(self, pdu: bytes) -> None:
        if pdu[:2] == b'\x10\x02':
            assert pdu[2:] == b'Microsoft::Windows::RDS::Graphics\0'
            self.send_static(1005, b'\x10\x02' + u32(0))
            data = bytes(16) if self.version == 0xa0100 else u32(0)
            self.send_static(1005, b'\x30\x02' + gfx(0x12, u16(1) + u32(self.version) + u32(len(data)) + data))
        else:
            super().on_dvc(pdu)

    def on_graphics(self, pdu: bytes) -> None:
        command, flags, length = struct.unpack_from('<HHI', pdu)
        assert flags == 0 and length == len(pdu)
        body = pdu[8:]
        if command == 0x13:
            data = bytes(16) if self.version == 0xa0100 else u32(2)
            assert body == u32(self.version) + u32(len(data)) + data
            self.gfx_confirmed = True
        elif command == 0x0e:
            assert self.gfx_confirmed and len(pdu) == 340 and self.current_frame is None
            self.gfx_width, self.gfx_height, count = struct.unpack_from('<III', body)
            assert count == 1
            self.generations.append({'width': self.gfx_width, 'height': self.gfx_height, 'frames': []})
        elif command == 9:
            assert body == u16(0) + u16(self.gfx_width) + u16(self.gfx_height) + b'\x20'
        elif command == 0xa: assert body == u16(0)
        elif command == 0xf: assert body == bytes(12)
        elif command == 0xb:
            timestamp, identifier = struct.unpack('<II', body)
            assert identifier > self.frame_id and self.next_identifier == 0
            self.next_identifier = identifier
        elif command == 1:
            surface, codec, pixel, left, top, right, bottom, size = struct.unpack_from('<HHBHHHHI', body)
            assert self.next_identifier and self.current_frame is None
            assert surface == 0 and codec == (15 if self.version == 0xa0100 else 14) and pixel == 32
            assert (left, top, right, bottom) == (0, 0, self.gfx_width, self.gfx_height)
            assert size == len(body) - 17
            info, = struct.unpack_from('<I', body, 17)
            assert info >> 30 == 0 and 14 < info < size - 18
            first = body[21:21 + info]
            second = body[21 + info:]
            for picture in (first, second):
                assert len(picture) > 18
                assert struct.unpack_from('<IHHHH', picture) == (1, 0, 0, self.gfx_width, self.gfx_height)
                assert picture[12] <= 51 and picture[13] == 75
                assert picture[14:17] == b'\0\0\1' or picture[14:18] == b'\0\0\0\1'
            self.current_frame = (first[14:], second[14:])
        elif command == 0xc:
            assert body == u32(self.next_identifier) and self.current_frame is not None
            self.frame_id = self.next_identifier
            self.next_identifier = 0
            self.generations[-1]['frames'].append(self.current_frame)
            self.current_frame = None
            self.gfx_frames += 1
            self.send_static(1005, b'\x30\x02' + gfx(13, u32(0) + u32(self.frame_id) + u32(self.gfx_frames)))
            if self.version != 0xa0100:
                self.send_static(1005, b'\x30\x02' + gfx(0x16, u32(self.frame_id) + u32((0xfffffff0 + self.gfx_frames*16) & 0xffffffff) + u16(8) + u16(3)))
        else: raise AssertionError(f'unexpected AVC graphics command {command:#x}')

    def next_frame(self) -> None:
        previous = self.gfx_frames
        self.until(lambda: self.gfx_frames > previous)

    def move(self, x: int, y: int) -> None:
        self.stream.sendall(bytes([4, 9, 0x20]) + u16(0x0800) + u16(x) + u16(y))


def decoded_pixel(main, auxiliary, w: int, h: int, x: int, y: int, v2: bool) -> tuple[int, int, int]:
    """Inverse source-coordinate equations, independently written for the fixture."""
    def chroma(c: int, px: int, py: int) -> int:
        if not v2:
            plane, xx, yy = (0, px, py//16*16 + py%16//2 + (c-1)*8) if py & 1 else (c, px//2, py//2)
        else:
            plane, xx, yy = (0, px//2 + (c-1)*(w//2), py) if px & 1 else (1 + (px%4)//2, px//4 + (c-1)*(w//4), py//2)
        return auxiliary[plane][yy*(w//2 if plane else w) + xx]
    values = [main[0][y*w+x]]
    for c in (1, 2):
        if x%2 == 0 and y%2 == 0:
            value = 4*main[c][(y//2)*(w//2)+x//2] - chroma(c,x+1,y) - chroma(c,x,y+1) - chroma(c,x+1,y+1)
            values.append(max(0, min(255, value)))
        else: values.append(chroma(c,x,y))
    yy, u, v = values[0], values[1]-128, values[2]-128
    return tuple(max(0, min(255, round(value))) for value in (yy+1.5748*v, yy-0.187324*u-0.468124*v, yy+1.8556*u))


def verify_decode(root: pathlib.Path, decoder: str, client: AvcClient) -> None:
    assert len(client.generations) == 2
    for index, generation in enumerate(client.generations):
        frames = generation['frames']
        assert frames
        w, h = (generation['width']+15)&~15, (generation['height']+15)&~15
        capture = root / f'{index}.h264'
        capture.write_bytes(b''.join(primary + auxiliary for primary, auxiliary in frames))
        run = subprocess.run([decoder, str(capture)], capture_output=True, timeout=15)
        assert run.returncode == 0, run.stderr.decode(errors='replace')
        result = memoryview(run.stdout)
        pairs, offset = [], 0
        while offset < len(result):
            assert struct.unpack_from('<II', result, offset) == (w, h)
            offset += 8
            planes = []
            for size in (w*h, w*h//4, w*h//4):
                assert size <= len(result)-offset
                planes.append(result[offset:offset+size]); offset += size
            pairs.append(planes)
        assert len(pairs) == len(frames)*2
        for i in range(0, len(pairs), 2):
            for x, y in ((20,20),(201,201),(202,202),(205,203),(350,270)):
                expected = (30,60,100) if y < 48 else (22,28+y*60//generation['height'],32+x*72//generation['width'])
                actual = decoded_pixel(pairs[i], pairs[i+1], w, h, x, y, client.version == 0xa0100)
                assert max(abs(a-b) for a,b in zip(actual,expected)) <= 16, (index,i,x,y,actual,expected)


def run(binary: str, decoder: str, version: int) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-avc-wire-') as directory:
        root = pathlib.Path(directory); cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)],check=True,capture_output=True)
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0)); port=reserve.getsockname()[1]
        with (root/'server.log').open('w+') as log:
            server = subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                                       '--encoder','software','--once'],stdout=subprocess.PIPE,stderr=log,text=True)
            client = None
            try:
                assert server.stdout is not None and 'listening' in server.stdout.readline()
                client = AvcClient(socket.create_connection(('127.0.0.1',port)),cert,version)
                client.connect(); client.until(lambda: client.gfx_frames >= 1 and client.display_caps is not None)
                client.move(450,340); client.next_frame()
                # Same-size refresh discards codec references and emits a new primary IDR.
                client.send_share(33, bytes([1,0,0,0]) + u16(0)+u16(0)+u16(639)+u16(479)); client.next_frame()
                # Suppress/resume must refresh even if the original capture is unchanged.
                client.send_share(35, bytes(4))
                client.move(500,380)
                client.send_share(35, bytes([1,0,0,0]) + u16(0)+u16(0)+u16(639)+u16(479)); client.next_frame()
                monitor = struct.pack('<IiiIIIIIII',1,0,0,800,600,0,0,0,100,100)
                client.demand = None
                client.send_static(1005,b'\x30\x01'+u32(2)+u32(56)+u32(40)+u32(1)+monitor)
                client.until(lambda: client.demand is not None); client.confirm()
                client.until(lambda: len(client.generations)==2 and len(client.generations[-1]['frames'])>=1)
                client.stream.close()
                stdout, _ = server.communicate(timeout=15)
                log.seek(0); text=log.read()
                assert server.returncode==0 and 'AddressSanitizer' not in text and 'runtime error:' not in text, text
                verify_decode(root,decoder,client)
                print(f'PASS: {version:#x} real TLS AVC444 pairs, DVC segmentation, separate H.264 decoder, pixel oracle, refresh, suppression and resize')
            except Exception:
                log.seek(0); print('SERVER LOG:\n'+log.read()); raise
            finally:
                if client:
                    client.stream.close()
                if server.poll() is None: server.terminate()
                try: server.communicate(timeout=5)
                except subprocess.TimeoutExpired: server.kill(); server.communicate()


if __name__ == '__main__':
    run(str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve()), 0xa0002)
    run(str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve()), 0xa0100)
