#!/usr/bin/env python3
"""Append raw serial output to a file, reconnecting if the port vanishes (board reset).
usage: serial_log.py <port> <seconds> <outfile>"""
import sys, time, termios, os, select
port = sys.argv[1]; secs = float(sys.argv[2]); out = open(sys.argv[3], 'ab', buffering=0)
end = time.time() + secs
def openport():
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attr = termios.tcgetattr(fd)
    attr[0] = 0; attr[1] = 0; attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL; attr[3] = 0
    attr[4] = attr[5] = termios.B115200
    attr[6][termios.VMIN] = 0; attr[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attr)
    return fd
fd = None
while time.time() < end:
    if fd is None:
        try: fd = openport(); out.write(b'\n--- port opened ---\n')
        except OSError: time.sleep(1); continue
    try:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            d = os.read(fd, 4096)
            if d: out.write(d)
    except OSError as e:
        out.write(('\n--- port error: %s ---\n' % e).encode())
        try: os.close(fd)
        except Exception: pass
        fd = None; time.sleep(1)
