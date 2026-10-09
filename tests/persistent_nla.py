#!/usr/bin/env python3
"""Independent FreeRDP/system GSS authentication before native broker allocation.

Uses temporary NTLM test identities, not host credentials. Cookie reconnection is
separately verified by the stdlib TLS fixture; this tests the actual NLA boundary.
"""
import pathlib
import subprocess
import sys
import tempfile
from nla_interop import scenario
from reconnect_integration import eventually, summaries


def main(binary, client, sessiond, app):
    with tempfile.TemporaryDirectory(prefix='lrdp-persistent-nla-') as temporary:
        root = pathlib.Path(temporary)
        broker, app_log = root / 'broker.sock', root / 'application.log'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', str(root / 'key.pem'), '-out', str(root / 'cert.pem')],
                       check=True, capture_output=True)
        with (root / 'broker.log').open('w+') as log:
            daemon = subprocess.Popen([sessiond, '--directory', str(root), '--desktop-command', app,
                                       '--desktop-arg', str(app_log)], stdout=log, stderr=subprocess.STDOUT)
            try:
                eventually(lambda: broker.exists())
                options = ('--backend', 'headless', '--session-broker', str(broker))
                for allowed, password in ((True, False), (False, True)):
                    scenario(binary, client, root, allowed, password, options)
                    assert summaries(broker) == [] and not app_log.exists(), 'unauthorized desktop allocated'
                def authorized():
                    entries = summaries(broker)
                    assert len(entries) == 1 and entries[0][1:] == (1, 'nla:LRDPTEST\\alice'), entries
                    assert len([line for line in app_log.read_text().splitlines() if line.startswith('START ')]) == 1
                scenario(binary, client, root, True, True, options, authorized)
                print('PASS: independent FreeRDP NLA authorizes exact broker principal; wrong password and unauthorized identity allocate no Xorg/application')
            except Exception:
                log.seek(0); print(log.read()); raise
            finally:
                if daemon.poll() is None: daemon.terminate()
                try: daemon.wait(timeout=10)
                except subprocess.TimeoutExpired: daemon.kill(); daemon.wait()
            assert not broker.exists()
            log.seek(0); output = log.read()
            assert 'AddressSanitizer' not in output and 'runtime error:' not in output, output


if __name__ == '__main__': main(*sys.argv[1:5])
