#!/usr/bin/env python3
"""Independent wire peer -> production Session/Bridge -> native volume facade.
Records below use struct and Python Unicode, not any LRDP encoder/decoder.
"""
from __future__ import annotations
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
from drive_session import DriveClient, u32, u64


class VolumeClient(DriveClient):
    def __init__(self, stream, cert, mode):
        super().__init__(stream,cert)
        self.mode=mode
        self.queries=[]

    def request(self, body: bytes):
        device,file_id,completion,major,minor=struct.unpack_from('<5I',body)
        if major!=10:
            # File opens/closes/metadata remain independently decoded by the
            # ordinary drive fixture; no backing writes are expected here.
            return super().request(body)
        assert device==7 and file_id in self.handles and minor==0
        kind,length=struct.unpack_from('<II',body,20)
        assert length==0 and len(body)==52 and body[28:]==bytes(24)
        self.queries.append(kind)
        status=0
        name=self.handles[file_id]
        if kind==7 and name=='hello.txt': status=0xc0000003
        if self.mode=='unsupported' and kind in (1,4,5): status=0xc00000bb
        if self.mode=='denied' and kind==5: status=0xc0000022
        if status:
            self.send_drive(0x4943,u32(device)+u32(completion)+u32(status)); return
        if kind in (3,7):
            payload=(u64(1<<32)+u64(123456)+u64(1<<30) if kind==7 else u64(1<<33)+u64(777))+u32(8)+u32(512)
        elif kind==1:
            label='Client 日本語 🚀 \0'.encode('utf-16-le')
            # RDP excludes FSCC's reserved byte; Boolean 0xFF must be accepted.
            payload=struct.pack('<QIIB',116444736000000001,0x89abcdef,len(label),255)+label
        elif kind==5:
            name='NTFS'.encode('utf-16-le')
            flags=7 | (0x80000 if self.mode=='readonly' else 0)
            payload=struct.pack('<III',flags,255,len(name))+name
            if self.mode=='malformed': payload=payload[:-1]
        elif kind==4:
            payload=struct.pack('<II',7,2 if self.mode=='device-readonly' else 0)
        else: raise AssertionError(f'unexpected volume class {kind}')
        # Explicit protocol-level optional padding is outside Length, not a
        # field silently inserted into the variable class-1 structure.
        self.send_drive(0x4943,u32(device)+u32(completion)+u32(0)+u32(len(payload))+payload+b'\xA5')


def run(binary: pathlib.Path):
    with tempfile.TemporaryDirectory(prefix='lrdp-volume-session-') as temp:
        root=pathlib.Path(temp);cert,key=root/'cert.pem',root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
            '-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)],check=True,capture_output=True)
        for mode in ('readonly','writable','device-readonly','unsupported','denied','malformed'):
            server=subprocess.Popen([str(binary),str(cert),str(key),mode],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            peer=None
            try:
                line=server.stdout.readline();assert line.startswith('READY '),line
                peer=VolumeClient(socket.create_connection(('127.0.0.1',int(line.split()[1]))),cert,mode)
                peer.connect()  # Includes graphics suppression before volume work.
                try:
                    while True: peer.process()
                except (EOFError,ConnectionResetError): pass
                stdout,stderr=server.communicate(timeout=5)
                assert 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr,stderr
                if mode=='malformed':
                    assert server.returncode==1 and 'truncated wire data' in stderr,(stdout,stderr)
                    print('PASS: malformed successful metadata rejected by production network parser')
                else:
                    assert server.returncode==0 and 'PASS:' in stdout,(stdout,stderr)
                    assert not peer.handles,peer.handles
                    assert peer.writes==0,'volume introspection must not mutate files'
                    assert set(peer.nodes)=={'','folder','hello.txt','日本語.bin'}
                    assert 7 in peer.queries and 5 in peer.queries
                    if mode=='denied': assert 3 not in peer.queries,'denial must not trigger class fallback'
                    else: assert 3 in peer.queries and 4 in peer.queries and 1 in peer.queries
                    print(stdout.strip())
            finally:
                if peer: peer.stream.close()
                if server.poll() is None: server.terminate()
                try: server.communicate(timeout=5)
                except subprocess.TimeoutExpired: server.kill();server.communicate()


if __name__=='__main__': run(pathlib.Path(sys.argv[1]).resolve())
