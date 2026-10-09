#!/usr/bin/env python3
"""Actual FreeRDP -> TLS -> CredSSP -> system GSS-NTLMSSP interoperability.

The credentials below are generated for this temporary, loopback-only test.
No host users, keytabs, PAM configuration, or production passwords are changed.
"""
from __future__ import annotations
import os
import pathlib
import secrets
import socket
import subprocess
import sys
import tempfile
import time
from client_interop import pixel_oracle


def scenario(binary: str, client_binary: str, root: pathlib.Path, allowed: bool, valid_password: bool,
             server_options: tuple[str, ...] = (), activated_check=None) -> None:
    password = secrets.token_hex(24)
    users = root / 'ntlm-users'
    users.write_text(f'LRDPTEST:alice:{password}\n'); users.chmod(0o600)
    env = dict(os.environ, NTLM_USER_FILE=str(users), NETBIOS_COMPUTER_NAME='LRDPHOST',
               NETBIOS_DOMAIN_NAME='LRDPTEST', KRB5_KTNAME=str(root / 'absent.keytab'))
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
    with (root / 'server.log').open('w+') as server_log, (root / 'client.log').open('w+') as client_log:
        args = [binary, '--cert', str(root / 'cert.pem'), '--key', str(root / 'key.pem'),
                '--port', str(port), '--auth', 'nla', '--service', 'TERMSRV@localhost', '--allow-ntlm',
                '--allow-principal', 'LRDPTEST\\alice' if allowed else 'LRDPTEST\\bob', '--encoder', 'raw', '--once']
        server = subprocess.Popen(args + list(server_options), env=env, stdout=server_log, stderr=subprocess.STDOUT)
        client = None
        try:
            deadline = time.monotonic() + 10
            while True:
                server_log.seek(0)
                if 'listening' in server_log.read(): break
                if server.poll() is not None or time.monotonic() > deadline: raise RuntimeError('NLA server startup failed')
                time.sleep(0.02)
            client = subprocess.Popen([client_binary, f'/v:localhost:{port}', '/u:alice', '/d:LRDPTEST',
                                       '/p:' + (password if valid_password else secrets.token_hex(24)),
                                       '/cert:ignore', '/sec:nla', '/size:640x480', '/bpp:24', '/log-level:DEBUG'],
                                      stdout=client_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 20
            expected = allowed and valid_password
            while True:
                server_log.seek(0); text = server_log.read()
                if expected and 'Session active' in text:
                    assert 'NLA principal authorized: LRDPTEST\\alice' in text
                    time.sleep(1); (activated_check or pixel_oracle)()
                    print('PASS: independent client NLA, sealed TLS binding, authorized principal and rendered desktop')
                    break
                if not expected and server.poll() is not None:
                    assert 'Session active' not in text and 'NLA principal authorized:' not in text, text
                    print('PASS: invalid credentials/unauthorized principal rejected before desktop creation')
                    break
                if (expected and (server.poll() is not None or client.poll() is not None)) or time.monotonic() > deadline:
                    raise RuntimeError('NLA scenario did not reach its required outcome')
                time.sleep(0.05)
        except Exception:
            server_log.seek(0); print('SERVER LOG:\n' + server_log.read().replace(password, '<test-password>'))
            client_log.seek(0); print('CLIENT LOG:\n' + client_log.read().replace(password, '<test-password>'))
            raise
        finally:
            for p in (client, server):
                if p is None: continue
                if p.poll() is None: p.terminate()
                try: p.wait(timeout=5)
                except subprocess.TimeoutExpired: p.kill(); p.wait()


def main() -> int:
    binary, client = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix='lrdp-nla-') as directory:
        root = pathlib.Path(directory)
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem')],
                       check=True, capture_output=True)
        scenario(binary, client, root, True, True)
        scenario(binary, client, root, True, False)
        scenario(binary, client, root, False, True)
    return 0


if __name__ == '__main__': sys.exit(main())
