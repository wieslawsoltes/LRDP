#!/usr/bin/env python3
"""Black-box FreeRDP -> production TLS server -> kernel FUSE -> native file I/O.
No independent-client implementation source is imported. A missing kernel FUSE
facility is a reported skip; once available, mount/protocol failures fail the test.
"""
from __future__ import annotations
import errno
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time

PROBE = r'''
import errno, os, pathlib, sys, time
mount, mode = pathlib.Path(sys.argv[1]), sys.argv[2]
deadline = time.monotonic()+20
while True:
    drives = list(mount.iterdir())
    if drives: break
    assert time.monotonic()<deadline, 'redirected client drive never appeared'
    time.sleep(.02)
assert len(drives)==1, drives
root=drives[0]
capacity=os.statvfs(root)
assert capacity.f_frsize>0 and capacity.f_blocks>0
assert 0<=capacity.f_bavail<=capacity.f_bfree<=capacity.f_blocks
assert bool(capacity.f_flag & os.ST_RDONLY)==(mode=='readonly')
assert os.statvfs(mount).f_blocks==0, 'virtual namespace must not duplicate physical capacity'
annotations=set(os.listxattr(root))
assert 'user.lrdp.volume.filesystem' in annotations, annotations
assert os.getxattr(root,'user.lrdp.volume.filesystem').decode('utf-8')
assert os.getxattr(root,'user.lrdp.volume.serial').startswith(b'0x')
assert 0<int(os.getxattr(root,'user.lrdp.volume.max-component-utf16'))<=255
try: os.getxattr(root,'user.lrdp.volume.not-a-field')
except OSError as e: assert e.errno==errno.ENODATA,e
else: raise AssertionError('unknown volume metadata was fabricated')
try: os.setxattr(root,'user.lrdp.volume.label',b'not-a-remote-rename')
except OSError as e: assert e.errno in (errno.EOPNOTSUPP,errno.EROFS,errno.EACCES),e
else: raise AssertionError('read-only metadata accepted mutation')
assert not os.listxattr(mount), 'synthetic mount root has no real volume metadata'

source=root/'日本語.bin'
expected=bytes((i*17+3)%256 for i in range(180003))
assert source.read_bytes()==expected
assert source.stat().st_size==len(expected)
assert not source.stat().st_mode&0o111
with source.open('rb',buffering=0) as f:
    f.seek(1<<40); assert f.read(1)==b''
try:
    fd=os.open(source,os.O_RDONLY|os.O_APPEND)
except OSError as e: assert e.errno==errno.EOPNOTSUPP, e
else:
    os.close(fd); raise AssertionError('unsupported append was accepted')
if mode=='readonly':
    try: source.open('wb')
    except OSError as e: assert e.errno in (errno.EROFS,errno.EACCES), e
    else: raise AssertionError('read-only drive allowed a write')
else:
    content=bytes((i*13+11)%256 for i in range(200007))
    created=root/'new-世界.bin'
    with created.open('xb') as f: f.write(content)
    assert created.read_bytes()==content
    with created.open('r+b') as f:
        f.truncate(197); f.flush()
        try: os.fsync(f.fileno())
        except OSError as e: assert e.errno==errno.EOPNOTSUPP, e
        else: raise AssertionError('unimplemented fsync claimed durability')
    renamed=root/'renamed-世界.bin'; created.rename(renamed)
    assert renamed.read_bytes()==content[:197]
    renamed.unlink()
    directory=root/'new-directory'; directory.mkdir(); assert directory.is_dir(); directory.rmdir()
assert set(p.name for p in root.iterdir())=={'日本語.bin','folder','hello.txt'}
print('PASS: kernel FUSE + independent client',mode,'Unicode, metadata, reads, EOF, policies and mutations')
'''


def stop(process):
    if process is None: return
    if process.poll() is None: process.terminate()
    try: process.wait(timeout=8)
    except subprocess.TimeoutExpired: process.kill(); process.wait()


def main(binary: str, client: str) -> int:
    if os.getuid()==0 or not shutil.which('fusermount3'):
        print('SKIP: FUSE requires an ordinary user and fusermount3'); return 77
    try: fd=os.open('/dev/fuse',os.O_RDWR|os.O_CLOEXEC)
    except OSError as e:
        if e.errno not in (errno.ENOENT,errno.EACCES,errno.EPERM,errno.ENODEV): raise
        print('SKIP: kernel FUSE unavailable:',e); return 77
    else: os.close(fd)
    with tempfile.TemporaryDirectory(prefix='lrdp-fuse-interop-') as temp:
        root=pathlib.Path(temp); cert,key=root/'cert.pem',root/'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-keyout',str(key),'-out',str(cert)],check=True,capture_output=True)
        exported=root/'export'; exported.mkdir(mode=0o700)
        (exported/'日本語.bin').write_bytes(bytes((i*17+3)%256 for i in range(180003)))
        (exported/'hello.txt').write_text('hello');(exported/'folder').mkdir()
        for mode in ('readonly','writable'):
            mounts=root/mode;mounts.mkdir(mode=0o700)
            with socket.socket() as reserve:
                reserve.bind(('127.0.0.1',0));port=reserve.getsockname()[1]
            with (root/'server.log').open('w+') as server_log,(root/'client.log').open('w+') as client_log:
                server=subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                    '--backend','demo','--encoder','raw','--gfx','off','--drives-directory',str(mounts),'--once']+
                    (['--drives-writable'] if mode=='writable' else []),stdout=server_log,stderr=subprocess.STDOUT)
                peer=probe=None
                try:
                    deadline=time.monotonic()+10
                    while True:
                        server_log.seek(0);text=server_log.read()
                        if 'listening' in text:break
                        assert server.poll() is None and time.monotonic()<deadline, text
                        time.sleep(.02)
                    peer=subprocess.Popen([client,f'/v:127.0.0.1:{port}','/u:lrdp-fixture','/p:unused','/sec:tls','/cert:ignore',
                        '/size:640x480','/bpp:24',f'/drive:DATA,{exported}','/log-level:WARN'],stdout=client_log,stderr=subprocess.STDOUT)
                    deadline=time.monotonic()+20
                    while True:
                        server_log.seek(0);text=server_log.read()
                        line=next((line for line in text.splitlines() if line.startswith('Drives mounted: ')),None)
                        if line:
                            mount=pathlib.Path(line[len('Drives mounted: '):].rsplit(' (',1)[0])
                            assert mount.parent==mounts and mount.name.startswith('lrdp-'), mount
                            break
                        assert server.poll() is None and peer.poll() is None and time.monotonic()<deadline,text
                        time.sleep(.02)
                    probe=subprocess.Popen([sys.executable,'-c',PROBE,str(mount),mode],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                    stdout,stderr=probe.communicate(timeout=35)
                    assert probe.returncode==0,(stdout,stderr)
                    print(stdout.strip())
                    stop(peer);server.wait(timeout=10)
                    server_log.seek(0);text=server_log.read()
                    assert server.returncode==0 and 'AddressSanitizer' not in text and 'runtime error:' not in text,text
                    assert not list(mounts.iterdir()),'session teardown left a mount directory'
                    assert set(p.name for p in exported.iterdir())=={'日本語.bin','folder','hello.txt'}
                except Exception:
                    server_log.seek(0); print('SERVER LOG:\n'+server_log.read())
                    client_log.seek(0); print('CLIENT LOG:\n'+client_log.read());raise
                finally:
                    stop(probe);stop(peer);stop(server)
                    # Best-effort recovery is limited to random mount children in
                    # this test's private root, never any user-configured path.
                    for mount in mounts.glob('lrdp-*'):
                        if os.path.ismount(mount): subprocess.run(['fusermount3','-u',str(mount)],capture_output=True,timeout=5)
    return 0

if __name__=='__main__':sys.exit(main(*sys.argv[1:3]))
