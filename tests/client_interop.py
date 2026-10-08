#!/usr/bin/env python3
"""Black-box client interoperability; no client implementation source is used."""
import pathlib
import socket
import subprocess
import sys
import tempfile
import time

server_binary, client_binary = map(str, sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='lrdp-interop-') as directory:
    root = pathlib.Path(directory)
    cert, key = root / 'cert.pem', root / 'key.pem'
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
    with (root / 'server.log').open('w+') as server_log, (root / 'client.log').open('w+') as client_log:
        server = subprocess.Popen([server_binary, '--lab-no-auth', '--cert', str(cert), '--key', str(key), '--port', str(port), '--once'],
                                  stdout=server_log, stderr=subprocess.STDOUT)
        client = None
        try:
            deadline = time.monotonic() + 10
            while True:
                server_log.seek(0)
                if 'listening' in server_log.read(): break
                if server.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('server did not start')
                time.sleep(0.05)
            client = subprocess.Popen([client_binary, f'/v:127.0.0.1:{port}', '/u:lrdp-fixture', '/p:unused',
                                       '/cert:ignore', '/sec:tls', '/size:640x480', '/bpp:24', '+clipboard', '/log-level:DEBUG'],
                                      stdout=client_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            while True:
                server_log.seek(0); text = server_log.read()
                if 'Session active' in text:
                    time.sleep(1)
                    if client.poll() is not None: raise RuntimeError('client disconnected immediately after activation')
                    print('PASS: independent FreeRDP client reached and remained in an active TLS RDP desktop session')
                    break
                if server.poll() is not None or client.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('independent client did not reach session activation')
                time.sleep(0.05)
        except Exception:
            server_log.seek(0); print('SERVER LOG:\n' + server_log.read())
            client_log.seek(0); print('CLIENT LOG:\n' + client_log.read())
            raise
        finally:
            for process in (client, server):
                if process is None: continue
                if process.poll() is None: process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
