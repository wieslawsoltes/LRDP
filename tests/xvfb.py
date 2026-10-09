#!/usr/bin/env python3
"""Run a native test on an isolated X server without opening a network listener."""
import os
import select
import subprocess
import sys

read_fd, write_fd = os.pipe()
server = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '640x480x24', '-nolisten', 'tcp'],
                          pass_fds=(write_fd,), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
os.close(write_fd)
try:
    with os.fdopen(read_fd) as stream:
        if not select.select([stream], [], [], 10)[0]: raise RuntimeError('Xvfb startup deadline exceeded')
        number = stream.readline().strip()
    if not number.isdecimal(): raise RuntimeError('Xvfb did not create a display')
    timeout = max(1, min(120, int(os.environ.get('LRDP_FIXTURE_TIMEOUT', '80'))))
    result = subprocess.run(sys.argv[1:], env=dict(os.environ, DISPLAY=':' + number), timeout=timeout)
    sys.exit(result.returncode)
finally:
    server.terminate()
    try: server.communicate(timeout=5)
    except subprocess.TimeoutExpired: server.kill(); server.communicate()
