#!/usr/bin/env python3
"""Black-box tests: only the installed client's public CLI is used.

An absent client codec is a CTest skip (77), never a server test success.
"""
from __future__ import annotations
import ctypes as C
import ctypes.util
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def pixel_oracle() -> None:
    """Read the independent client's actual X11 presentation, not LRDP's buffers."""
    x = C.CDLL(ctypes.util.find_library('X11'))
    x.XOpenDisplay.argtypes = [C.c_char_p]; x.XOpenDisplay.restype = C.c_void_p
    x.XDefaultRootWindow.argtypes = [C.c_void_p]; x.XDefaultRootWindow.restype = C.c_ulong
    x.XGetImage.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_ulong, C.c_int]
    x.XGetImage.restype = C.c_void_p
    x.XGetPixel.argtypes = [C.c_void_p, C.c_int, C.c_int]; x.XGetPixel.restype = C.c_ulong
    x.XDestroyImage.argtypes = [C.c_void_p]
    x.XCloseDisplay.argtypes = [C.c_void_p]
    display = x.XOpenDisplay(None)
    if not display: raise RuntimeError('cannot inspect independent client display')
    image = None
    try:
        image = x.XGetImage(display, x.XDefaultRootWindow(display), 0, 0, 640, 480, C.c_ulong(-1).value, 2)
        if not image: raise RuntimeError('cannot capture independent client presentation')
        # No window manager is running in tests/xvfb.py; the client is at 0,0.
        for px, py, expected in [(20, 20, (30, 60, 100)), (200, 200, (22, 53, 54))]:
            value = x.XGetPixel(image, px, py)
            rgb = ((value >> 16) & 255, (value >> 8) & 255, value & 255)
            if any(abs(a - b) > 10 for a, b in zip(rgb, expected)):
                raise RuntimeError(f'client presentation pixel {px},{py}: {rgb}, expected {expected}')
    finally:
        if image: x.XDestroyImage(image)
        x.XCloseDisplay(display)


def main() -> int:
    server_binary, client_binary = sys.argv[1:3]
    mode = 'avc420' if '--gfx' in sys.argv[3:] else 'raw' if '--gfx-raw' in sys.argv[3:] else 'bitmap'
    help_result = subprocess.run([client_binary, '/help'], capture_output=True, text=True, timeout=10)
    help_text = help_result.stdout + help_result.stderr
    graphics_args = []
    if mode == 'avc420':
        if 'AVC420' not in help_text:
            print('SKIP: installed independent client was built without AVC420; server codec tests remain required')
            return 77
        graphics_args = ['/gfx-h264:AVC420'] if '/gfx-h264' in help_text else ['/gfx:AVC420']
    elif mode == 'raw': graphics_args = ['/gfx']
    with tempfile.TemporaryDirectory(prefix='lrdp-interop-') as directory:
        root = pathlib.Path(directory)
        cert, key = root / 'cert.pem', root / 'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(cert)], check=True, capture_output=True)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        with (root / 'server.log').open('w+') as server_log, (root / 'client.log').open('w+') as client_log:
            server = subprocess.Popen([server_binary, '--lab-no-auth', '--cert', str(cert), '--key', str(key),
                                       '--port', str(port), '--encoder', 'software' if mode == 'avc420' else 'raw', '--once'],
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
                                           '/cert:ignore', '/sec:tls', '/size:640x480',
                                           '/bpp:24' if mode == 'bitmap' else '/bpp:32', '+clipboard', '/log-level:DEBUG'] + graphics_args,
                                          stdout=client_log, stderr=subprocess.STDOUT)
                required = {'bitmap': 'Session active', 'raw': 'GFX uncompressed BGRA', 'avc420': 'libx264 (software encode)'}[mode]
                deadline = time.monotonic() + 20
                while True:
                    server_log.seek(0); text = server_log.read()
                    if required in text:
                        time.sleep(1)
                        if client.poll() is not None or server.poll() is not None:
                            raise RuntimeError('peer disconnected immediately after graphics negotiation')
                        pixel_oracle()
                        client_log.seek(0)
                        errors = [line for line in client_log.read().splitlines() if '[ERROR]' in line and
                                  ('rdpgfx' in line or 'codec' in line or 'update' in line)]
                        if errors: raise RuntimeError('\n'.join(errors))
                        print(f'PASS: independent client {mode} TLS session and rendered-pixel oracle')
                        return 0
                    if server.poll() is not None or client.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(f'independent client did not reach {required}')
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


if __name__ == '__main__': sys.exit(main())
