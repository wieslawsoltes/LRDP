#!/usr/bin/env python3
"""Independent ClearCodec/SolidFill decoder over the actual TLS RDP session."""
import pathlib
import struct
import sys
from gfx_integration import GraphicsClient
from integration import run


class LosslessClient(GraphicsClient):
    def __init__(self, *args):
        super().__init__(*args)
        self.clear_sequence = 0
        self.clear_count = 0
        self.solid_count = 0
        self.surface_generation = 0

    def region(self, left, top, right, bottom, pixels):
        assert 0 <= left < right <= self.gfx_width and 0 <= top < bottom <= self.gfx_height
        stride = (right - left)*4
        assert len(pixels) == stride*(bottom-top)
        for row in range(top, bottom):
            offset = (row*self.gfx_width+left)*4
            source = (row-top)*stride
            self.surface[offset:offset+stride] = pixels[source:source+stride]

    def on_graphics(self, pdu: bytes) -> None:
        command, flags, size = struct.unpack_from('<HHI', pdu)
        assert flags == 0 and size == len(pdu)
        body = pdu[8:]
        if command == 4:
            surface, color, count = struct.unpack_from('<H4sH', body)
            assert surface == 0 and count > 0 and len(body) == 8+count*8
            for i in range(count):
                left, top, right, bottom = struct.unpack_from('<4H', body, 8+i*8)
                self.region(left, top, right, bottom, color*((right-left)*(bottom-top)))
            self.solid_count += count
        elif command == 1:
            surface, codec, pixel, left, top, right, bottom, count = struct.unpack_from('<HHBHHHHI', body)
            assert surface == 0 and pixel == 32 and len(body) == count+17 and codec in (0,8)
            content = body[17:]
            if codec == 0:
                pixels = content
            else:
                control, sequence, residual, bands, subcodecs = struct.unpack_from('<BBIII', content)
                assert control == 0 and sequence == self.clear_sequence and bands == subcodecs == 0
                assert len(content) == residual+14
                self.clear_sequence = (sequence+1)&255
                pixels = bytearray()
                offset = 14
                maximum = (right-left)*(bottom-top)*4
                while offset < len(content):
                    blue, green, red, length = struct.unpack_from('<4B', content, offset); offset += 4
                    if length == 255:
                        length, = struct.unpack_from('<H', content, offset); offset += 2
                        if length == 65535:
                            length, = struct.unpack_from('<I', content, offset); offset += 4
                    assert 0 < length <= (maximum-len(pixels))//4
                    pixels.extend(bytes((blue,green,red,255))*length)
                assert offset == len(content) and len(pixels) == maximum
                self.clear_count += 1
            self.region(left,top,right,bottom,pixels)
        else:
            if command == 0xe:
                if self.surface_generation:
                    assert self.clear_count > 0 and self.solid_count > 0
                self.surface_generation += 1
            if command == 0xc:
                assert self.clear_count > 0 and self.solid_count > 0
            super().on_graphics(pdu)


if __name__ == '__main__':
    run(pathlib.Path(sys.argv[1]).resolve(), client_class=LosslessClient, server_args=('--encoder','lossless'))
    print('PASS: ClearCodec and SolidFill session, independent decoded pixels, uninterrupted sequence on resize and graphics suppression')
