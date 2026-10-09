#!/usr/bin/env python3
"""Black-box FreeRDP CLI test: real presentation and continuous RTT replies."""
from __future__ import annotations
import pathlib
import re
import socket
import subprocess
import sys
import tempfile
import time
from client_interop import pixel_oracle


def main() -> int:
    binary, client_binary = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='lrdp-network-client-') as directory:
        root = pathlib.Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                        '-keyout',str(key),'-out',str(cert)], check=True, capture_output=True)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1',0)); port = reservation.getsockname()[1]
        with (root/'server.log').open('w+') as server_log, (root/'client.log').open('w+') as client_log:
            server = subprocess.Popen([binary,'--lab-no-auth','--cert',str(cert),'--key',str(key),'--port',str(port),
                                       '--once','--encoder','raw','--network-metrics'], stdout=server_log,stderr=subprocess.STDOUT)
            client = None
            try:
                deadline = time.monotonic() + 10
                while True:
                    server_log.seek(0)
                    if 'listening' in server_log.read(): break
                    if server.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('server startup failed')
                    time.sleep(0.02)
                client = subprocess.Popen([client_binary,f'/v:127.0.0.1:{port}','/u:lrdp-network-fixture','/p:unused',
                                           '/cert:ignore','/sec:tls','/network:auto','/size:640x480','/bpp:24',
                                           '+clipboard','/log-level:DEBUG'],stdout=client_log,stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 20
                while True:
                    server_log.seek(0); evidence = server_log.read()
                    samples = [int(n) for n in re.findall(r'\brtt_samples=(\d+)', evidence)]
                    if 'Session active' in evidence and samples and max(samples) >= 2:
                        if server.poll() is not None or client.poll() is not None: raise RuntimeError('peer disconnected')
                        pixel_oracle()
                        client_log.seek(0)
                        errors = [line for line in client_log.read().splitlines() if '[ERROR]' in line and
                                  any(word in line.lower() for word in ('autodetect','rdpgfx','codec','update','mcs'))]
                        if errors: raise RuntimeError('\n'.join(errors))
                        if 'AddressSanitizer' in evidence or 'runtime error:' in evidence: raise RuntimeError(evidence)
                        print('PASS: independent FreeRDP /network:auto, dedicated message channel, repeated RTT replies and native rendered pixels')
                        return 0
                    if server.poll() is not None or client.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('independent client did not complete continuous RTT exchange')
                    time.sleep(0.05)
            except Exception:
                server_log.seek(0); print('SERVER LOG:\n' + server_log.read())
                client_log.seek(0); print('CLIENT LOG:\n' + client_log.read())
                raise
            finally:
                for process in (client,server):
                    if process is None: continue
                    if process.poll() is None: process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()


if __name__ == '__main__':
    sys.exit(main())
