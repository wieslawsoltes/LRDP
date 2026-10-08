#!/usr/bin/env python3
"""Real TLS -> DisplayControl -> private Xorg -> surviving native application."""
from __future__ import annotations
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import Client, u16, u32


def size_of_demand(pdu: bytes) -> tuple[int,int]:
    descriptor, combined = struct.unpack_from('<HH',pdu,10)
    caps = memoryview(pdu)[14+descriptor:14+descriptor+combined]
    count, = struct.unpack_from('<H',caps); offset = 4
    for _ in range(count):
        kind, length = struct.unpack_from('<HH',caps,offset)
        assert length >= 4 and offset+length <= len(caps)
        if kind == 2: return struct.unpack_from('<HH',caps,offset+12)
        offset += length
    raise AssertionError('bitmap capability missing')


def run(binary: str, application: str) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-headless-test-') as directory:
        root = pathlib.Path(directory); cert, key = root/'cert.pem', root/'key.pem'; app_log = root/'application.log'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1',
            '-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-out',str(cert),'-keyout',str(key)],check=True,capture_output=True)
        with socket.socket() as probe: probe.bind(('127.0.0.1',0)); port = probe.getsockname()[1]
        with (root/'server.log').open('w+') as log:
            server = subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                '--backend','headless','--desktop-command',application,'--desktop-arg',str(app_log),'--encoder','raw','--once'],stdout=log,stderr=subprocess.STDOUT)
            client = None
            try:
                deadline = time.monotonic()+10
                while True:
                    log.seek(0)
                    if 'listening' in log.read(): break
                    assert server.poll() is None and time.monotonic()<deadline,'headless server did not start'
                    time.sleep(0.01)
                client = Client(socket.create_connection(('127.0.0.1',port)),cert); client.connect()
                client.until(lambda: len(client.pixel_coverage)==40 and client.display_caps is not None and client.remote_text is not None)
                assert size_of_demand(client.demand)==(640,480)
                assert client.remote_text == 'persistent Linux application clipboard'
                pixel = (20*640+20)*3
                client.until(lambda: client.pixels[pixel:pixel+3]==bytes([0x99,0x66,0x33]))
                identity = [line for line in app_log.read_text().splitlines() if line.startswith('START ')]
                assert len(identity)==1
                for monitors, dimensions in [([(1,0,0,800,600)],(800,600)),
                    ([(1,0,0,800,600),(0,-800,0,800,600)],(1600,600)),
                    ([(1,0,0,640,480)],(640,480))]:
                    body = b''.join(struct.pack('<IiiIIIIIII',flags,left,top,width,height,0,0,0,100,100)
                        for flags,left,top,width,height in monitors)
                    layout = u32(2)+u32(16+len(body))+u32(40)+u32(len(monitors))+body
                    client.demand = None; client.send_static(1005,b'\x30\x01'+layout)
                    client.until(lambda: client.demand is not None)
                    assert size_of_demand(client.demand)==dimensions
                    client.confirm(); previous = client.bitmap_count
                    client.until(lambda: client.bitmap_count>previous)
                    assert [line for line in app_log.read_text().splitlines() if line.startswith('START ')] == identity
                    assert f'SIZE {dimensions[0]} {dimensions[1]}' in app_log.read_text()
                # Input still reaches the exact same application after topology changes.
                client.stream.sendall(bytes([4,9,0x20])+u16(0x9000)+u16(20)+u16(20))
                client.until(lambda: client.pixels[pixel:pixel+3]==bytes([0x33,0xaa,0x55]))
                assert 'CLICK 1' in app_log.read_text()
                client.stream.sendall(bytes([4,9,0x20])+u16(0x1000)+u16(20)+u16(20))
                client.stream.close(); client = None
                assert server.wait(timeout=10)==0
                log.seek(0); result = log.read()
                assert 'AddressSanitizer' not in result and 'runtime error:' not in result, result
                print('PASS: native headless RDP pixels; clipboard; 640x480 -> 800x600 -> negative-origin dual monitors -> 640x480; persistent app identity and working pointer after reactivation; cleanup')
            except Exception:
                log.seek(0); print(log.read()); raise
            finally:
                if client: client.stream.close()
                if server.poll() is None: server.terminate()
                try: server.wait(timeout=10)
                except subprocess.TimeoutExpired: server.kill(); server.wait()


if __name__ == '__main__': run(str(pathlib.Path(sys.argv[1]).resolve()), str(pathlib.Path(sys.argv[2]).resolve()))
