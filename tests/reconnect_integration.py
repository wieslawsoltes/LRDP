#!/usr/bin/env python3
"""Real production TLS -> private broker -> same Xorg/application after disconnect.

Wire and HMAC verification use Python stdlib, not the LRDP reconnection codec.
The laboratory profile is loopback-only. NLA remains a separate independent test.
"""
from __future__ import annotations
import contextlib
import hashlib
import hmac
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import Client, client_connect, x224, u16, u32

MAGIC=0x3152534c

def eventually(predicate, seconds=10):
    deadline=time.monotonic()+seconds
    while not predicate():
        assert time.monotonic()<deadline, 'condition timed out'
        time.sleep(0.02)

def string(s):
    b=s.encode('utf-8');return u16(len(b))+b

def request(s, op, body=b'', ok=True):
    s.sendall(u32(MAGIC)+u32(op)+body)
    reply=s.recv(131072)
    assert len(reply)>=4
    assert (int.from_bytes(reply[:4],'little')==0)==ok, 'unexpected broker result'
    return reply[4:]

def broker_socket(path):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET);s.settimeout(10);s.connect(str(path));return s

def summaries(path):
    with broker_socket(path) as s:
        result=request(s,4)
    count,=struct.unpack_from('<I',result);offset=4;entries=[]
    for _ in range(count):
        id,state,size=struct.unpack_from('<IBH',result,offset);offset+=7
        name=result[offset:offset+size].decode('utf-8');offset+=size;entries.append((id,state,name))
    assert offset==len(result);return entries

def proof(cookie):
    assert len(cookie)==28 and cookie[:8]==u32(28)+u32(1)
    return cookie[:12]+hmac.new(cookie[12:],bytes(32),hashlib.md5).digest()

class ReconnectingClient(Client):
    supports_reconnect=True
    def confirm(self):
        if not self.supports_reconnect:
            demand=bytearray(self.demand)
            descriptor,length=struct.unpack_from('<HH',demand,10)
            offset=14+descriptor+4
            for _ in range(struct.unpack_from('<H',demand,14+descriptor)[0]):
                kind,size=struct.unpack_from('<HH',demand,offset)
                if kind==1:
                    flags=struct.unpack_from('<H',demand,offset+14)[0]
                    struct.pack_into('<H',demand,offset+14,flags & ~8)
                offset+=size
            self.demand=bytes(demand)
        super().confirm()
    def connect(self, cookie=None, finish=True):
        self.stream.sendall(client_connect())
        assert self.recv_packet()[7:9]==b'\x7f\x66'
        self.stream.sendall(x224(b'\x04\x01\x00\x01\x00')+x224(b'\x28'))
        assert self.recv_packet()==x224(b'\x2e\x00\x00\x00')
        for channel in (1001,1003,1004,1005):
            self.stream.sendall(x224(b'\x38\x00\x00'+struct.pack('>H',channel)))
            assert self.recv_packet()==x224(b'\x3e\x00\x00\x00'+struct.pack('>HH',channel,channel))
        info=u16(0x40)+u16(0)+u32(0)+u32(0x10)+bytes(20)
        info+=u16(2)+u16(2)+u16(0)+u16(2)+u16(0)+bytes(172)+bytes(8)
        info+=u16(28 if cookie else 0)+(cookie or b'')+bytes(4)
        self.send(1003,info)
        self.until(lambda:self.demand is not None)
        if finish:self.confirm()
    def cookie(self):
        self.until(lambda:any(kind==38 for kind,_ in self.events))
        body=next(body[18:] for kind,body in reversed(self.events) if kind==38)
        assert len(body)==612 and body[:14]==u32(3)+u16(608)+u32(1)+u32(28)
        assert body[42:]==bytes(570)
        return body[14:42]

def run(binary,sessiond,sessionctl,application):
    with tempfile.TemporaryDirectory(prefix='lrdp-reconnect-') as directory:
        root=pathlib.Path(directory);broker=root/'broker.sock';app_log=root/'application.log'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
            '-addext','subjectAltName=DNS:localhost','-keyout',str(root/'key.pem'),'-out',str(root/'cert.pem')],check=True,capture_output=True)
        logs=[];processes=[];clients=[]
        def launch(command,name):
            log=(root/name).open('w+');logs.append(log);p=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT);processes.append(p);return p,log
        def start(cookie=None,finish=True,reject=False,retain=True):
            with socket.socket() as temp:temp.bind(('127.0.0.1',0));port=temp.getsockname()[1]
            p,log=launch([binary,'--lab-no-auth','--cert',str(root/'cert.pem'),'--key',str(root/'key.pem'),
                '--port',str(port),'--backend','headless','--session-broker',str(broker),'--encoder','raw','--once'],f'server-{len(processes)}.log')
            def ready():
                log.seek(0);return 'listening' in log.read()
            eventually(ready)
            c=ReconnectingClient(socket.create_connection(('127.0.0.1',port)),root/'cert.pem');clients.append(c)
            c.supports_reconnect=retain
            if reject:
                try:c.connect(cookie,finish)
                except (EOFError,ConnectionError):pass
                else:raise AssertionError('invalid resume unexpectedly succeeded')
                c.stream.close();assert p.wait(timeout=10)==0;return None,None,None
            c.connect(cookie,finish)
            return c,p,(c.cookie() if finish and retain else None)
        try:
            daemon,_=launch([sessiond,'--directory',str(root),'--retain-seconds','5','--max-desktops','2',
                '--desktop-command',application,'--desktop-arg',str(app_log)],'broker.log')
            eventually(lambda:broker.exists())
            assert broker.stat().st_mode & 0o777 == 0o700
            # Malformed local IPC closes its own connection without harming the broker.
            with broker_socket(broker) as s: request(s,999,ok=False)
            with broker_socket(broker) as s: request(s,1,string('bad')+u16(27)+bytes(27),ok=False)
            assert summaries(broker)==[]
            # Refuse ambiguous permissions and a second daemon without removing its socket.
            root.chmod(0o755)
            denied=subprocess.run([sessionctl,'--socket',str(broker),'list'],capture_output=True,timeout=5)
            assert denied.returncode!=0
            root.chmod(0o700)
            denied=subprocess.run([sessiond,'--directory',str(root)],capture_output=True,timeout=5)
            assert denied.returncode!=0 and broker.exists()
            c,p,cookie=start();id=int.from_bytes(cookie[8:12],'little')
            assert summaries(broker)==[(id,1,'lrdp:loopback-laboratory')]
            c.until(lambda:len(c.pixel_coverage)==40 and c.remote_text is not None and c.display_caps is not None)
            assert c.remote_text=='persistent Linux application clipboard'
            pixel=(20*640+20)*3
            c.stream.sendall(bytes([4,9,0x20])+u16(0x9000)+u16(20)+u16(20))
            c.until(lambda:c.pixels[pixel:pixel+3]==bytes([0x33,0xaa,0x55]))
            c.stream.sendall(bytes([4,9,0x20])+u16(0x1000)+u16(20)+u16(20))
            identity=[line for line in app_log.read_text().splitlines() if line.startswith('START ')]
            assert len(identity)==1
            # A correct cookie cannot steal a currently attached desktop.
            start(proof(cookie),reject=True)
            # Leave Control down and kill the process without input teardown.
            c.stream.sendall(bytes([4,4,0,0x1d]))
            eventually(lambda:'KEY_DOWN 37' in app_log.read_text())
            # Kill the actual RDP process, not just its socket; broker/application survive.
            p.kill();p.wait(timeout=10);c.stream.close()
            eventually(lambda:summaries(broker)==[(id,0,'lrdp:loopback-laboratory')])
            # Correct verifier with a different authenticated principal is denied locally.
            with broker_socket(broker) as s:request(s,1,string('other-principal')+u16(28)+proof(cookie),ok=False)
            wrong=bytearray(proof(cookie));wrong[-1]^=1;start(bytes(wrong),reject=True)
            # A resume abandoned before Confirm Active leaves the old cookie usable.
            pending,pending_server,_=start(proof(cookie),finish=False)
            pending.stream.close();assert pending_server.wait(timeout=10)==0
            eventually(lambda:summaries(broker)==[(id,0,'lrdp:loopback-laboratory')])
            c2,p2,new_cookie=start(proof(cookie))
            assert new_cookie[:12]==cookie[:12] and new_cookie[12:]!=cookie[12:]
            c2.until(lambda:len(c2.pixel_coverage)==40 and c2.remote_text is not None and c2.display_caps is not None)
            assert c2.pixels[pixel:pixel+3]==bytes([0x33,0xaa,0x55])
            assert c2.remote_text=='persistent Linux application clipboard'
            eventually(lambda:'KEY_UP 37' in app_log.read_text())
            assert [line for line in app_log.read_text().splitlines() if line.startswith('START ')]==identity
            # Re-established input operates on application state from the old connection.
            c2.stream.sendall(bytes([4,9,0x20])+u16(0x9000)+u16(20)+u16(20))
            c2.until(lambda:c2.pixels[pixel:pixel+3]==bytes([0x99,0x66,0x33]))
            assert 'CLICK 2' in app_log.read_text()
            assert not (int([line.split()[1] for line in app_log.read_text().splitlines() if line.startswith('MODIFIERS ')][-1]) & 4), 'crashed Control key stayed held'
            c2.stream.sendall(bytes([4,9,0x20])+u16(0x1000)+u16(20)+u16(20))
            # DisplayControl on a resumed connection retains the cookie and application.
            layout=u32(2)+u32(56)+u32(40)+u32(1)+struct.pack('<IiiIIIIIII',1,0,0,800,600,0,0,0,100,100)
            c2.demand=None;c2.send_static(1005,b'\x30\x01'+layout);c2.until(lambda:c2.demand is not None);c2.confirm()
            previous=c2.bitmap_count;c2.until(lambda:c2.bitmap_count>previous)
            assert len([event for event in c2.events if event[0]==38])==1,'resize must not rotate reconnection identity'
            assert [line for line in app_log.read_text().splitlines() if line.startswith('START ')]==identity
            c2.stream.close();assert p2.wait(timeout=10)==0
            eventually(lambda:summaries(broker)==[(id,0,'lrdp:loopback-laboratory')])
            start(proof(cookie),reject=True) # Consumed cookie cannot be replayed.
            listing=subprocess.run([sessionctl,'--socket',str(broker),'list'],check=True,capture_output=True,text=True).stdout
            assert f'{id}\tdetached\tlrdp:loopback-laboratory' in listing
            eventually(lambda:not summaries(broker),seconds=10)
            start(proof(new_cookie),reject=True) # Expired session cannot be resurrected.
            assert len([line for line in app_log.read_text().splitlines() if line.startswith('START ')])==1
            # Clients that omit AUTORECONNECT_SUPPORTED receive no cookie and
            # their private desktop is destroyed at disconnect, not retained.
            ordinary,ordinary_server,_=start(retain=False)
            ordinary.until(lambda:len(ordinary.pixel_coverage)==40)
            assert not any(kind==38 for kind,_ in ordinary.events)
            ordinary.stream.close();assert ordinary_server.wait(timeout=10)==0
            eventually(lambda:not summaries(broker))
            # Explicit owner-admin termination revokes an active lease and its app.
            terminated,active_server,last_cookie=start()
            last_id=int.from_bytes(last_cookie[8:12],'little')
            subprocess.run([sessionctl,'--socket',str(broker),'terminate',str(last_id)],check=True,capture_output=True,timeout=10)
            assert active_server.wait(timeout=10)==0
            eventually(lambda:not summaries(broker))
            start(proof(last_cookie),reject=True)
            daemon.terminate();assert daemon.wait(timeout=10)==0;assert not broker.exists()
            for log in logs:
                log.seek(0);text=log.read();assert 'AddressSanitizer' not in text and 'runtime error:' not in text,text
            print('PASS: real TLS reconnect, killed worker recovery, stable Xorg/application pixels and clipboard, fresh input, resize, wrong principal/proof, exclusivity, cookie rotation/replay, abandoned resume, expiration and cleanup')
        except Exception:
            for log in logs:
                log.seek(0);print(pathlib.Path(log.name).name+':\n'+log.read())
            raise
        finally:
            for c in clients:
                with contextlib.suppress(Exception):c.stream.close()
            for p in reversed(processes):
                if p.poll() is None:p.terminate()
                try:p.wait(timeout=10)
                except subprocess.TimeoutExpired:p.kill();p.wait()
            for log in logs:log.close()

if __name__=='__main__':run(*(str(pathlib.Path(a).resolve()) for a in sys.argv[1:5]))
