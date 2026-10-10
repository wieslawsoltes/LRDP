#!/usr/bin/env python3
"""Real TLS/GCC/MCS/DVC fixture with independent GFX wire decoding."""
import pathlib
import struct
import sys
from gfx_integration import GraphicsClient, gfx
from lossless_integration import LosslessClient
from integration import run, u16, u32


class ModernClient(GraphicsClient):
    versions = (0xA0701, 0xA0301, 0xA0600)

    def __init__(self, *args):
        super().__init__(*args)
        self.confirmations = 0
        self.reset_count = 0
        self.completed_resets = 0

    def advertise(self, version):
        self.send_static(1005, b'\x30\x02' + gfx(0x12, u16(1) + u32(version) + u32(4) + u32(0x20)))

    def on_dvc(self, pdu):
        if pdu[:2] == b'\x10\x02':
            assert pdu[2:] == b'Microsoft::Windows::RDS::Graphics\0'
            self.send_static(1005, b'\x10\x02' + u32(0))
            self.advertise(self.versions[0])
        else:
            super().on_dvc(pdu)

    def on_graphics(self, pdu):
        command, flags, length = struct.unpack_from('<HHI', pdu)
        assert not flags and length == len(pdu)
        if command == 0x13:
            assert self.confirmations < len(self.versions)
            version = self.versions[self.confirmations]
            expected_flags = 0x20 | (0 if version == 0xA0301 else 2) | (0x80 if version == 0xA0701 else 0)
            assert pdu[8:] == u32(version) + u32(4) + u32(expected_flags)
            self.confirmations += 1
            self.gfx_confirmed = True
            self.clear_sequence = 0  # A new capability generation resets decoder state.
            return
        if command == 0x0E:
            self.reset_count += 1
        super().on_graphics(pdu)
        if command == 0x0C:
            timestamp = (0xFFFFFFF0 + (self.frame_id - 1) * 32) & 0xFFFFFFFF
            self.send_static(1005, b'\x30\x02' + gfx(0x16, u32(self.frame_id) + u32(timestamp) + u16(7) + u16(9)))
            if self.confirmations < len(self.versions):
                self.advertise(self.versions[self.confirmations])
            else:
                self.completed_resets += 1

    def connect(self):
        super().connect()
        self.until(lambda: self.completed_resets > 0)
        assert self.confirmations == 3 and self.reset_count == 3
        assert self.gfx_frames == 3 and self.frame_id == 3
        assert self.pixels[:3] == bytes((100, 60, 30))


class ModernLosslessClient(ModernClient, LosslessClient):
    """Reuse the independent ClearCodec/SolidFill decoder, not production code."""


if __name__ == '__main__':
    lossless = '--lossless' in sys.argv[2:]
    run(pathlib.Path(sys.argv[1]).resolve(),
        client_class=ModernLosslessClient if lossless else ModernClient,
        server_args=('--encoder', 'lossless') if lossless else ())
    print('PASS: modern GFX capabilities, QoE, two client state resets, pixel oracle, clipboard/input and desktop resize over real TLS')
