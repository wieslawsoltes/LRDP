#!/usr/bin/env python3
"""Production TLS/RDPDR + native CLI + independently encoded printer responses.
No printer hardware, CUPS, Windows client or LRDP wire library is used by this peer.
"""
from __future__ import annotations
import array
import fcntl
import os
import pathlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from drive_session import DriveClient, u16, u32, u64


class PrinterClient(DriveClient):
    def __init__(self, *args):
        super().__init__(*args)
        self.accepted = False
        self.print_handles = {}
        self.completed = []
        self.creates = self.closes = 0
        self.hold_create = False
        self.held = None
        self.zero_write = self.close_error = False

    def on_dvc(self, message):
        assert message[:2] == b'\x72\x44'
        kind, = struct.unpack_from('<H', message, 2)
        body = message[4:]
        if kind == 0x5350:
            count, = struct.unpack_from('<H', body)
            types = []
            offset = 4
            for _ in range(count):
                cap, length = struct.unpack_from('<HH', body, offset)
                types.append(cap)
                offset += length
            assert offset == len(body) and 2 in types and 4 not in types, types
            general = u16(1)+u16(44)+u32(2)+u32(0)+u32(0)+u16(1)+u16(13)+u32(0x3fff)+u32(0)+u32(7)+u32(0)+u32(0)+u32(0)
            self.send_drive(0x4350, u16(2)+u16(0)+general+u16(2)+u16(8)+u32(1))
        elif kind == 0x554c:
            driver = 'PostScript\0'.encode('utf-16-le')
            name = 'Printer 日本語\0'.encode('utf-16-le')
            metadata = struct.pack('<6I', 0, 0, 0, len(driver), len(name), 0)+driver+name
            self.send_drive(0x4441, u32(1)+u32(4)+u32(9)+b'PRN1\0\0\0\0'+u32(len(metadata))+metadata)
        elif kind == 0x6472:
            assert body == u32(9)+u32(0), body.hex()
            self.accepted = True
        elif kind == 0x4952:
            self.print_request(body)
        else:
            super().on_dvc(message)

    def reply(self, completion, status=0, data=b''):
        self.send_drive(0x4943, u32(9)+u32(completion)+u32(status)+data)

    def print_request(self, body):
        device, file_id, completion, major, minor = struct.unpack_from('<5I', body)
        assert device == 9 and minor == 0
        data = body[20:]
        if major == 0:
            assert data == bytes(32) and file_id == 0
            self.creates += 1
            handle = self.creates + 100
            self.print_handles[handle] = bytearray()
            if self.hold_create:
                assert self.held is None
                self.held = (completion, handle)
            else:
                self.reply(completion, data=u32(handle))
        elif major == 4:
            length, offset = struct.unpack_from('<IQ', data)
            assert offset == 0 and data[12:32] == bytes(20) and len(data) == 32+length and length <= 65536
            count = 0 if self.zero_write else min(length, 7333)
            self.print_handles[file_id].extend(data[32:32+count])
            self.reply(completion, data=u32(count)+b'\0')
        else:
            assert major == 2 and data == bytes(32)
            self.closes += 1
            self.completed.append(bytes(self.print_handles.pop(file_id)))
            self.reply(completion, status=0xc0000185 if self.close_error else 0, data=bytes(4))


def run(binary, cli):
    with tempfile.TemporaryDirectory(prefix='lpp-') as temp:
        root = pathlib.Path(temp)
        private = root/'spool'; private.mkdir(mode=0o700)
        cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)], check=True, capture_output=True)
        with socket.socket() as address:
            address.bind(('127.0.0.1',0)); port = address.getsockname()[1]
        log = (root/'server.log').open('w+')
        server = subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                                   '--printers-directory',str(private),'--encoder','raw','--once'],stdout=log,stderr=subprocess.STDOUT)
        peer = None
        processes = []
        def server_log():
            log.flush();log.seek(0);return log.read()
        def until(condition, timeout=10):
            deadline = time.monotonic()+timeout
            while not condition():
                assert time.monotonic()<deadline, ('condition timed out', server_log())
                assert server.poll() is None, server_log()
                if peer is not None and (peer.stream.pending() or select.select([peer.stream],[],[],0.01)[0]):
                    peer.process()
                else: time.sleep(0.002)
        def command(*args, success=True):
            process=subprocess.Popen([cli,*map(str,args)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            processes.append(process);until(lambda:process.poll() is not None)
            stdout,stderr=process.communicate(timeout=1)
            assert (process.returncode==0)==success,(stdout,stderr,server_log())
            return stdout,stderr
        try:
            until(lambda:'listening' in server_log())
            peer=PrinterClient(socket.create_connection(('127.0.0.1',port)),cert)
            peer.connect();until(lambda:peer.accepted)
            endpoint=next(line.split(': ',1)[1] for line in server_log().splitlines() if line.startswith('Printers endpoint: '))
            stdout,_=command('list',endpoint)
            device=stdout.split('\t')[0];assert device.startswith('9:') and 'Printer 日本語' in stdout
            generation=int(device.split(':')[1])
            payload=bytes((i*17+3)%256 for i in range(180003))
            source=root/'原稿.prn';source.write_bytes(payload)
            stdout,_=command('submit',endpoint,device,source)
            assert 'acknowledged_bytes=180003' in stdout and peer.completed[-1]==payload
            assert peer.creates==peer.closes==1
            command('submit',endpoint,'9:999',source,success=False)
            assert peer.creates==1
            # Invalid ancillary sources must not leak descriptors or reach RDP.
            baseline=len(list(pathlib.Path(f'/proc/{server.pid}/fd').iterdir()))
            for iteration in range(24):
                fd=os.memfd_create('untrusted',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING)
                os.write(fd,b'bad')
                native=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
                try:
                    native.connect(endpoint)
                    packet=struct.pack('<IIIQ',0x3150524c,2,9,generation)
                    mode=iteration%4
                    descriptors=[fd]*(10 if mode==2 else 1+mode%2)
                    if mode==3: packet+=bytes(32768)
                    native.sendmsg([packet],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',descriptors))])
                    until(lambda:bool(select.select([native],[],[],0)[0]))
                    try: result=native.recv(32768)
                    except ConnectionResetError: result=b''
                    if mode<2: assert len(result)==20 and struct.unpack_from('<I',result,8)[0]!=0
                    else: assert result==b'', 'truncated data/ancillary packet must close without accepting it'
                finally:native.close();os.close(fd)
            until(lambda:len(list(pathlib.Path(f'/proc/{server.pid}/fd').iterdir()))==baseline)
            assert peer.creates==1
            # Client disappearance during CREATE closes the remote handle, without writing.
            native=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);native.connect(endpoint)
            fd=os.memfd_create('cancel',os.MFD_CLOEXEC|os.MFD_ALLOW_SEALING);os.write(fd,b'never print this')
            fcntl.fcntl(fd,fcntl.F_ADD_SEALS,fcntl.F_SEAL_SEAL|fcntl.F_SEAL_SHRINK|fcntl.F_SEAL_GROW|fcntl.F_SEAL_WRITE)
            peer.hold_create=True
            native.sendmsg([struct.pack('<IIIQ',0x3150524c,2,9,generation)],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[fd]))])
            os.close(fd);until(lambda:peer.held is not None);native.close()
            # Allow one ordinary event-loop turn to observe native EOF before completing CREATE.
            time.sleep(0.1)
            completion,handle=peer.held;peer.held=None;peer.hold_create=False;peer.reply(completion,data=u32(handle))
            until(lambda:peer.closes==2);assert peer.completed[-1]==b''
            peer.zero_write=True
            _,stderr=command('submit',endpoint,device,source,success=False)
            assert 'acknowledged_bytes=0' in stderr and peer.completed[-1]==b''
            peer.zero_write=False;peer.close_error=True
            _,stderr=command('submit',endpoint,device,source,success=False)
            assert 'acknowledged_bytes=180003' in stderr and peer.completed[-1]==payload
            assert peer.closes==4 and not peer.print_handles
            peer.stream.close();peer=None
            server.wait(timeout=5)
            text=server_log();assert 'AddressSanitizer' not in text and 'runtime error:' not in text,text
            assert not list(private.iterdir()),'endpoint directory was not cleaned'
            print('PASS: production TLS/RDPDR and native CLI; Unicode printer selection, 180003 byte short-write job, stale generations, sealed descriptor enforcement/leak checks, cancellation during CREATE, zero-progress/close failures, graphics suppression and cleanup')
        except BaseException:
            print('SERVER LOG:\n'+server_log());raise
        finally:
            if peer:peer.stream.close()
            for process in [*processes,server]:
                if process.poll() is None:process.terminate()
                try:process.communicate(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.communicate()
            log.close()

if __name__=='__main__':run(*(str(pathlib.Path(a).resolve()) for a in sys.argv[1:3]))
