#!/usr/bin/env python3
"""Isolated PipeWire graph; no physical devices or user settings touched."""
from __future__ import annotations
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def connect_loopback(pid: int, environment: dict[str, str]) -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        outputs = subprocess.run(['pw-link', '-o'], env=environment, capture_output=True, text=True, timeout=5)
        inputs = subprocess.run(['pw-link', '-i'], env=environment, capture_output=True, text=True, timeout=5)
        source = [p.strip() for p in outputs.stdout.splitlines() if f'lrdp.{pid}.microphone:' in p]
        sinks = [p.strip() for p in inputs.stdout.splitlines() if f'lrdp.{pid}.speakers:' in p]
        if source and len(sinks) >= 2:
            for sink in sinks:
                subprocess.run(['pw-link', source[0], sink], env=environment, check=True, capture_output=True, timeout=5)
            return
        time.sleep(0.05)
    raise RuntimeError(f'virtual ports missing; outputs={outputs.stdout}; inputs={inputs.stdout}; errors={outputs.stderr}{inputs.stderr}')


def run(command: list[str], *, native_fixture: bool = True) -> None:
    with tempfile.TemporaryDirectory(prefix='lrdp-pipewire-') as directory:
        root = pathlib.Path(directory)
        environment = dict(os.environ, XDG_RUNTIME_DIR=directory, PIPEWIRE_RUNTIME_DIR=directory, PIPEWIRE_REMOTE='pipewire-0')
        environment.pop('DBUS_SESSION_BUS_ADDRESS', None)
        with (root / 'pipewire.log').open('w+') as daemon_log, (root / 'fixture.log').open('w+') as test_log:
            daemon = subprocess.Popen(['pipewire'], env=environment, stdout=daemon_log, stderr=subprocess.STDOUT)
            test = None
            try:
                deadline = time.monotonic() + 10
                while not (root / 'pipewire-0').exists():
                    if daemon.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('private PipeWire daemon did not start')
                    time.sleep(0.05)
                test = subprocess.Popen(command, env=environment, stdout=test_log, stderr=subprocess.STDOUT)
                if native_fixture:
                    deadline = time.monotonic() + 10
                    while True:
                        test_log.seek(0); lines = test_log.read().splitlines()
                        ready = next((line for line in lines if line.startswith('READY ')), None)
                        if ready: break
                        if test.poll() is not None or time.monotonic() >= deadline: raise RuntimeError('audio fixture did not create its devices')
                        time.sleep(0.02)
                    connect_loopback(int(ready.split()[1]), environment)
                result = test.wait(timeout=40)
                test_log.seek(0); text = test_log.read(); print(text)
                if result: raise RuntimeError(f'PipeWire fixture exited with {result}')
                if 'AddressSanitizer' in text or 'runtime error:' in text: raise RuntimeError('sanitizer reported a native audio error')
            except Exception:
                daemon_log.seek(0); print('PIPEWIRE LOG:\n' + daemon_log.read())
                test_log.seek(0); print('TEST LOG:\n' + test_log.read()); raise
            finally:
                for process in (test, daemon):
                    if process is None: continue
                    if process.poll() is None: process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()


if __name__ == '__main__':
    if sys.argv[1] == '--rdp': run([sys.executable, str(pathlib.Path(__file__).with_name('audio_integration.py')), sys.argv[2]], native_fixture=False)
    else: run(sys.argv[1:])
