#!/usr/bin/env python3
"""Independent TCP/TLS graphics fixture; only Python's standard library is used."""
import pathlib
import struct
import sys
from integration import Client, run, u16, u32


def gfx(command: int, body: bytes) -> bytes:
    return u16(command) + u16(0) + u32(len(body) + 8) + body


class GraphicsClient(Client):
    use_gfx = True

    def __init__(self, *args):
        super().__init__(*args)
        self.gfx_partial = bytearray()
        self.gfx_expected = 0
        self.gfx_frames = 0
        self.gfx_width = self.gfx_height = 0
        self.surface = bytearray()
        self.frame_id = 0
        self.gfx_confirmed = False

    def on_dvc(self, pdu: bytes) -> None:
        if pdu[0] == 0x10 and pdu[1] == 2:
            assert pdu[2:] == b'Microsoft::Windows::RDS::Graphics\x00'
            self.send_static(1005, b'\x10\x02' + u32(0))
            self.send_static(1005, b'\x30\x02' + gfx(0x12, u16(1) + u32(0x80105) + u32(4) + u32(0)))
        elif pdu[1] == 2 and pdu[0] >> 4 in (2, 3):
            assert pdu[0] & 3 == 0
            offset = 2
            if pdu[0] >> 4 == 2:
                width = 1 << ((pdu[0] >> 2) & 3)
                assert self.gfx_expected == 0 and width <= 4
                self.gfx_expected = int.from_bytes(pdu[offset:offset + width], 'little')
                assert self.gfx_expected <= 16 * 1024 * 1024
                offset += width
            if self.gfx_expected:
                self.gfx_partial.extend(pdu[offset:])
                assert len(self.gfx_partial) <= self.gfx_expected
                if len(self.gfx_partial) != self.gfx_expected: return
                body = bytes(self.gfx_partial)
                self.gfx_partial.clear(); self.gfx_expected = 0
            else: body = pdu[offset:]
            self.on_graphics(self.unwrap(body))
        else: super().on_dvc(pdu)

    @staticmethod
    def unwrap(body: bytes) -> bytes:
        if body[0] == 0xe0:
            assert body[1] == 4
            return body[2:]
        assert body[0] == 0xe1
        count, total = struct.unpack_from('<HI', body, 1)
        offset, result = 7, bytearray()
        for _ in range(count):
            size, = struct.unpack_from('<I', body, offset); offset += 4
            assert body[offset] == 4 and size > 0
            result.extend(body[offset + 1:offset + size]); offset += size
        assert offset == len(body) and len(result) == total
        return bytes(result)

    def on_graphics(self, pdu: bytes) -> None:
        command, flags, size = struct.unpack_from('<HHI', pdu)
        assert flags == 0 and size == len(pdu)
        body = pdu[8:]
        if command == 0x13:
            assert body == u32(0x80105) + u32(4) + u32(2)
            self.gfx_confirmed = True
        elif command == 0x0e:
            assert self.gfx_confirmed and size == 340
            self.gfx_width, self.gfx_height, count = struct.unpack_from('<III', body)
            assert count == 1 and 200 <= self.gfx_width <= 8192 and 200 <= self.gfx_height <= 8192
            self.surface = bytearray(self.gfx_width * self.gfx_height * 4)
        elif command == 9:
            assert body == u16(0) + u16(self.gfx_width) + u16(self.gfx_height) + b'\x20'
        elif command == 0x0f: assert body == bytes(12)
        elif command == 0x0a: assert body == u16(0)
        elif command == 0x0b:
            _, identifier = struct.unpack('<II', body)
            assert identifier > self.frame_id
            self.frame_id = identifier
        elif command == 1:
            surface, codec, pixel, left, top, right, bottom, length = struct.unpack_from('<HHBHHHHI', body)
            assert surface == 0 and codec == 0 and pixel == 0x20
            assert left == 0 and right == self.gfx_width and 0 <= top < bottom <= self.gfx_height
            assert length == (bottom - top) * self.gfx_width * 4 and len(body) == 17 + length
            start = top * self.gfx_width * 4
            self.surface[start:start + length] = body[17:]
        elif command == 0x0c:
            assert body == u32(self.frame_id)
            self.gfx_frames += 1
            if self.gfx_width == 640 and self.gfx_height == 480:
                self.pixels = bytearray(component for index, component in enumerate(self.surface) if index % 4 != 3)
            else: assert (self.gfx_width, self.gfx_height) == (800, 600)
            self.send_static(1005, b'\x30\x02' + gfx(13, u32(0) + u32(self.frame_id) + u32(self.gfx_frames)))
        else: raise AssertionError(f'unknown graphics command {command}')


if __name__ == '__main__':
    run(pathlib.Path(sys.argv[1]).resolve(), client_class=GraphicsClient)
    print('PASS: real TLS GFX capability exchange, segmented DVC frames, top-down BGRA pixel oracle, frame ACKs, graphics reset after resize')
