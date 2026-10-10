#!/usr/bin/env python3
"""Real executable preflight: valid/invalid config without bind, exec or IPC.
An occupied TCP port and a non-accepting private broker socket prove that
preflight is metadata validation, not listener/desktop/broker activation.
"""
from __future__ import annotations
import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile


def run(binary: pathlib.Path) -> None:
    checks = 0
    def command(*arguments: str, success: bool = True, interactive: bool = False):
        nonlocal checks
        # Keep stdin open to catch accidental OpenSSL terminal-password reads.
        process = subprocess.Popen([str(binary), *arguments], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            if interactive:
                process.wait(timeout=3)
                stdout, stderr = process.communicate(timeout=1)
            else:
                stdout, stderr = process.communicate(timeout=5)
            assert (process.returncode == 0) == success, (arguments, stdout, stderr)
            assert 'AddressSanitizer' not in stderr and 'runtime error:' not in stderr, stderr
            checks += 1
            return stdout, stderr
        finally:
            if process.poll() is None: process.kill(); process.communicate()
    caps = json.loads(command('--capabilities')[0])
    assert caps['schema'] == 1 and not caps['hardware_probe_performed']
    assert caps['backends']['demo'] and caps['integrations']['lossless_graphics']
    assert not caps['integrations']['zero_copy_capture'] and not caps['integrations']['pam_login_broker']
    assert all(type(value) is bool for value in [*caps['backends'].values(), *caps['integrations'].values()])
    assert command('--version')[0] == f"LRDP {caps['version']}\n"
    command('--capabilities', '--unknown', success=False)
    command('--check-config', success=False)
    with tempfile.TemporaryDirectory(prefix='lrdp-check-') as tmp:
        root = pathlib.Path(tmp); cert, key = root/'cert.pem', root/'key.pem'
        def openssl(*args):
            subprocess.run(['openssl', *map(str,args)], check=True, capture_output=True, timeout=10)
        openssl('req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=localhost',
                '-addext','subjectAltName=DNS:localhost','-keyout',key,'-out',cert)
        private = root/'private'; private.mkdir(mode=0o700)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0)); listener.listen()
            port = listener.getsockname()[1]
            options = ['--check-config','--lab-no-auth','--cert',str(cert),'--key',str(key),
                       '--port',str(port),'--backend','demo','--encoder','lossless']
            def snapshot():
                return sorted((str(p.relative_to(root)), p.stat().st_mode, p.stat().st_size)
                              for p in root.rglob('*'))
            before = snapshot()
            report = json.loads(command(*options)[0])
            assert report['valid'] and not report['listener_opened'] and not report['desktop_opened']
            assert report['capabilities'] == caps and len(report['unchecked']) >= 5
            assert snapshot() == before, 'preflight modified deployment files'
            listener.setblocking(False)
            try: listener.accept(); raise AssertionError('preflight connected to configured listener')
            except BlockingIOError: pass
            command(*options,'--listen','localhost',success=False)
            command(*options,'--listen','0.0.0.0',success=False)
            command(*options,'--port','0',success=False)
            command(*options,'--gfx','off',success=False)
            command(*options,'--key',str(root/'missing'),success=False)
            command(*options,'--key',str(private),success=False)
            fifo = root/'key-fifo'; os.mkfifo(fifo)
            command(*options,'--key',str(fifo),success=False,interactive=True)
            encrypted = root/'encrypted.pem'
            openssl('pkey','-in',key,'-aes256','-passout','pass:test-only','-out',encrypted)
            _, stderr = command(*options,'--key',str(encrypted),success=False,interactive=True)
            assert 'Enter' not in stderr and 'password' not in stderr.lower(), stderr
            # Ordinary unattended startup must also reject encryption without prompting.
            ordinary = [x for x in options if x != '--check-config']
            command(*ordinary,'--key',str(encrypted),success=False,interactive=True)
            other = root/'other.pem'; openssl('genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:2048','-out',other)
            command(*options,'--key',str(other),success=False)
            empty = root/'empty.pem'; empty.touch()
            command(*options,'--cert',str(empty),success=False)
            large = root/'large.pem'
            with large.open('wb') as stream: stream.truncate(4*1024*1024+1)
            command(*options,'--cert',str(large),success=False)
            csr, expired = root/'request.pem', root/'expired.pem'
            openssl('req','-new','-key',key,'-subj','/CN=localhost','-out',csr)
            # Explicit historical/future dates are supported by openssl ca on
            # the oldest CI OpenSSL; negative x509 -days is not portable.
            database, serial, ca_config = root/'index.txt', root/'serial', root/'ca.cnf'
            database.touch(); serial.write_text('01\n')
            ca_config.write_text(f"[ca]\ndefault_ca=test\n[test]\ndatabase={database}\nserial={serial}\n"
                                 f"new_certs_dir={root}\nprivate_key={key}\ncertificate={cert}\n"
                                 "default_md=sha256\ndefault_days=1\nunique_subject=no\npolicy=policy\n"
                                 "[policy]\ncommonName=supplied\n")
            openssl('ca','-config',ca_config,'-batch','-selfsign','-notext','-in',csr,'-out',expired,
                    '-startdate','20000101000000Z','-enddate','20010101000000Z')
            command(*options,'--cert',str(expired),success=False)
            future = root/'future.pem'
            openssl('ca','-config',ca_config,'-batch','-selfsign','-notext','-in',csr,'-out',future,
                    '-startdate','21000101000000Z','-enddate','21010101000000Z')
            command(*options,'--cert',str(future),success=False)
            # Managed certificate symlinks are permitted and never rewritten.
            certlink, keylink = root/'managed-cert.pem', root/'managed-key.pem'
            certlink.symlink_to(cert); keylink.symlink_to(key)
            command(*options,'--cert',str(certlink),'--key',str(keylink))
            for enabled, option in [('raw_printers','--printers-directory'),('fuse_drives','--drives-directory')]:
                supported = caps['integrations'][enabled]
                command(*options,option,str(private),success=supported)
                command(*options,option,str(root/'missing-directory'),success=False)
                if supported:
                    private.chmod(0o755);command(*options,option,str(private),success=False);private.chmod(0o700)
                    link = root/(enabled+'-link');link.symlink_to(private,target_is_directory=True)
                    command(*options,option,str(link),success=False)
            if caps['backends']['headless']:
                marker = root/'must-not-execute'; program = root/'desktop'
                program.write_text(f'#!/bin/sh\ntouch "{marker}"\n'); program.chmod(0o700)
                before = snapshot()
                command(*options,'--backend','headless','--desktop-command',str(program))
                assert snapshot() == before and not marker.exists()
                command(*options,'--backend','headless','--desktop-command',str(root/'absent'),success=False)
                broker_path = private/'broker.sock'
                with socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET) as broker:
                    broker.bind(str(broker_path));broker_path.chmod(0o600)
                    # Not listening: proves no broker connect or lease request occurs.
                    command(*options,'--backend','headless','--session-broker',str(broker_path),
                            '--desktop-command',str(root/'not-used-with-broker'))
                    broker_path.chmod(0o666)
                    command(*options,'--backend','headless','--session-broker',str(broker_path),success=False)
            for integration, extra in [('gssapi_nla',['--auth','nla','--service','TERMSRV@host','--allow-principal','SECRET@REALM']),
                                       ('ffmpeg',['--encoder','software']),('pipewire_audio',['--audio'])]:
                base = [x for x in options if x != '--lab-no-auth'] if integration == 'gssapi_nla' else options
                output, _ = command(*base,*extra,success=caps['integrations'][integration])
                assert 'SECRET@REALM' not in output and str(key) not in output
        print(f'PASS: {checks} deployment commands; compiled capability JSON, nonbinding preflight, current TLS validity, mismatched/encrypted/special-file keys, private endpoints, no exec/IPC and optional-feature gates')


if __name__ == '__main__': run(pathlib.Path(sys.argv[1]).resolve())
