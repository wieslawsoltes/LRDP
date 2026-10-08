#!/usr/bin/env python3
"""Independent stdlib TLS client plus an independent native Xlib paste application."""
import pathlib
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from integration import Client, clip, data, u16, u32


def html(fragment):
    raw=fragment.encode('utf-8')
    def header(start,end):
        return f'Version:1.0\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:{start:010}\r\nEndFragment:{end:010}\r\n'.encode('ascii')
    offset=len(header(0,0))
    return header(offset,offset+len(raw))+raw+b'\x00'


def image():
    header=struct.pack('<IiiHHIIiiII',124,202,102,1,32,3,202*102*4,0,0,0,0)
    header+=struct.pack('<IIIII',0xff0000,0xff00,0xff,0xff000000,0x73524742)+bytes(48)+u32(4)+bytes(12)
    assert len(header)==124
    pixels=bytes(component for y in reversed(range(102)) for x in range(202) for component in (x,y,90,(x+y)%256))
    return header+pixels


class RichClient(Client):
    def __init__(self,*args):
        super().__init__(*args)
        self.pending_export=None
        self.exported_html=False
        self.exported_image=False
        self.import_requests=[]

    def send_static(self,channel,body):
        for offset in range(0,max(1,len(body)),1600):
            part=body[offset:offset+1600]
            flags=(1 if offset==0 else 0)|(2 if offset+len(part)==len(body) else 0)
            self.stream.sendall(data(channel,u32(len(body))+u32(flags)+part))

    def on_clipboard(self,pdu):
        kind,flags,length=struct.unpack_from('<HHI',pdu)
        assert length==len(pdu)-8
        body=pdu[8:]
        if kind==7:
            self.send_static(1004,clip(7,0,u16(1)+u16(0)+u16(1)+u16(12)+u32(2)+u32(2)))
        elif kind==1:
            assert not body
        elif kind==2:
            formats={}
            offset=0
            while offset<len(body):
                identifier,=struct.unpack_from('<I',body,offset);offset+=4
                end=offset
                while body[end:end+2]!=b'\0\0':end+=2
                formats[identifier]=body[offset:end].decode('utf-16-le');offset=end+2
            self.send_static(1004,clip(3,1))
            html_ids=[identifier for identifier,name in formats.items() if name=='HTML Format']
            assert len(html_ids)==1 and 17 in formats and 8 in formats
            self.pending_export='html'
            self.send_static(1004,clip(4,0,u32(html_ids[0])))
        elif kind==5:
            assert flags==1
            if self.pending_export=='html':
                first=int(re.search(rb'StartFragment:(\d+)',body).group(1))
                last=int(re.search(rb'EndFragment:(\d+)',body).group(1))
                assert body[first:last].decode('utf-8')=='<p>'+'z'*1200000+' 🚀 native</p>'
                self.exported_html=True;self.pending_export='image'
                self.send_static(1004,clip(4,0,u32(17)))
            else:
                assert self.pending_export=='image'
                assert struct.unpack_from('<IiiHH',body)==(124,3,2,1,32)
                assert len(body)==124+24
                for row in range(2):
                    for x in range(3):
                        assert body[124+(row*3+x)*4:124+(row*3+x+1)*4]==bytes([10+row*30+x,70,120,255])
                self.exported_image=True;self.pending_export=None
        elif kind==3:
            assert flags==1
        elif kind==4:
            identifier,=struct.unpack('<I',body);self.import_requests.append(identifier)
            assert identifier in (0xd321,17)
            payload=html('<strong>'+'h'*200007+' 🌍</strong>') if identifier==0xd321 else image()
            self.send_static(1004,clip(5,1,payload))
        else:raise AssertionError(f'unexpected clipboard message {kind}')


def run(binary,app):
    with tempfile.TemporaryDirectory(prefix='lrdp-rich-') as temporary:
        root=pathlib.Path(temporary);cert=root/'cert.pem';key=root/'key.pem';app_log=root/'application.log'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost','-addext','subjectAltName=DNS:localhost','-keyout',str(key),'-out',str(cert)],check=True,capture_output=True)
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        server=subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),'--once','--backend','headless','--desktop-command',app,'--desktop-arg',str(app_log),'--clipboard-rich','--encoder','raw'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        client=None
        try:
            assert 'listening' in server.stdout.readline()
            client=RichClient(socket.create_connection(('127.0.0.1',port)),cert);client.connect()
            client.until(lambda:client.exported_html and client.exported_image)
            client.send_share(35,bytes(4)) # Clipboard must work with graphics suppressed.
            offer=u32(0xd321)+'HTML Format'.encode('utf-16-le')+u16(0)+u32(17)+u16(0)
            client.send_static(1004,clip(2,0,offer))
            client.until(lambda:len(client.import_requests)==2)
            deadline=time.monotonic()+10
            while not app_log.exists() or 'REMOTE_OWNER' not in app_log.read_text():
                assert time.monotonic()<deadline,'remote clipboard not published';time.sleep(.01)
            # Click the native application to perform HTML and bitmap paste.
            client.stream.sendall(bytes([4,9,0x20])+u16(0x9000)+u16(50)+u16(50))
            client.stream.sendall(bytes([4,9,0x20])+u16(0x1000)+u16(50)+u16(50))
            while 'PASTE_OK' not in app_log.read_text():
                assert time.monotonic()<deadline,app_log.read_text();time.sleep(.01)
            client.stream.close();client=None
            stdout,stderr=server.communicate(timeout=10)
            assert server.returncode==0 and 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr,(stdout,stderr)
            print('PASS: real TLS rich clipboard; 1.2 MB native HTML export; DIBV5 pixel oracle; fragmented HTML+alpha bitmap import pasted by native application; graphics suppression')
        except Exception:
            if app_log.exists():print(app_log.read_text())
            if server.poll() is None:server.terminate()
            try: stdout,stderr=server.communicate(timeout=5)
            except subprocess.TimeoutExpired: server.kill();stdout,stderr=server.communicate()
            print('SERVER LOG:',stdout,stderr)
            raise
        finally:
            if client:client.stream.close()
            if server.poll() is None:server.terminate()
            try:stdout,stderr=server.communicate(timeout=5)
            except subprocess.TimeoutExpired:server.kill();stdout,stderr=server.communicate()
            if server.returncode not in (0,-15):print(stdout,stderr)

if __name__=='__main__':run(*map(lambda p:str(pathlib.Path(p).resolve()),sys.argv[1:3]))
