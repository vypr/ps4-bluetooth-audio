"""Swap the running audio worker for any payload .bin, in RAM, with no reboot.

  python tools/deploy_variant.py build/BTA-Audio.bin --host <PS4_IP> --connect

A swap cuts live audio for ~6-15 s. It never touches the boot payload on disk: a
reboot returns to whatever tools/install_bta.py last installed. The payload goes
to the GoldHEN payloader (9090) in ONE connection - never probe that port, a bare
connection spends its slot.
"""
import argparse
import ftplib
import io
import json
from pathlib import Path
import socket
import sys
import time


def port_open(host, port, timeout=2):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def http(host, method, path, timeout=3):
    s = socket.create_connection((host, 8097), timeout=timeout)
    s.settimeout(timeout)
    control = 'X-BTA-Control: 1\r\nContent-Length: 0\r\n' if method == 'POST' else ''
    s.sendall(f'{method} {path} HTTP/1.1\r\nHost: {host}:8097\r\n{control}Connection: close\r\n\r\n'.encode())
    buf = b''
    while True:
        try:
            chunk = s.recv(65536)
        except OSError:
            break
        if not chunk:
            break
        buf += chunk
    s.close()
    return buf.split(b'\r\n\r\n', 1)[-1].decode('latin1')


def status(host):
    try:
        return json.loads(http(host, 'GET', '/api/status'))
    except (OSError, ValueError):
        return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('payload')
    p.add_argument('--host', required=True, help='PS4 LAN IP address')
    p.add_argument('--connect', action='store_true', help='press Connect once the new worker is up')
    p.add_argument('--wait', type=int, default=30, help='seconds to keep retrying a refused payloader connect')
    args = p.parse_args()
    data = Path(args.payload).read_bytes()
    print(f'payload {args.payload}: {len(data)} bytes')

    # The new worker cannot bind 8097 while the old one holds it, so ask the old one to
    # stop first. If that fails the payload is sent anyway; the PS4 shows a notification.
    if port_open(args.host, 8097):
        try:
            with ftplib.FTP() as ftp:
                ftp.connect(args.host, 2121, timeout=8)
                ftp.login()
                ftp.storbinary('STOR /data/bta_audio/stop', io.BytesIO(b'stop\n'))
        except ftplib.all_errors as exc:
            print(f'could not write the stop sentinel over FTP ({exc}); sending anyway')
        else:
            print('stop sentinel written; waiting for the old worker to exit...')
            for _ in range(15):
                if not port_open(args.host, 8097):
                    break
                time.sleep(1)
            else:
                print('old worker still up after 15 s; sending anyway')
            time.sleep(1)

    # A refused connect has no side effects, so retry it; the connection that finally
    # opens is the one that carries the payload (never open one without sending).
    for attempt in range(max(1, args.wait // 2)):
        try:
            link = socket.create_connection((args.host, 9090), timeout=6)
            break
        except OSError as exc:
            if not attempt:
                print(f'payloader not accepting ({exc}); retrying for up to {args.wait} s')
            time.sleep(2)
    else:
        sys.exit('Payloader (9090) is not accepting connections. Is GoldHEN loaded? If an old worker '
                 'was stopped, launch BTA-Audio from the GoldHEN payload menu or reboot to let AutoRun start it.')
    link.sendall(data)
    link.shutdown(socket.SHUT_WR)
    try:
        link.recv(64)
    except OSError:
        pass
    link.close()
    print(f'sent {len(data)} bytes to the payloader')

    up = None
    for _ in range(30):
        up = status(args.host)
        if up:
            break
        time.sleep(0.5)
    if not up:
        sys.exit('Nothing answered on 8097 within 15 s. Check the PS4 for a notification, then '
                 '/data/bta_audio/service.log.')
    print(f"up: version {up.get('version')}, message={up.get('message')!r}")
    if args.connect:
        http(args.host, 'POST', '/api/connect')
        print('pressed Connect; reading status once in 8 s')
        time.sleep(8)
        s = status(args.host) or {}
        print({k: s.get(k) for k in ('version', 'message', 'bt_state', 'streaming', 'audio_rc', 'packets', 'errors', 'resyncs')})


if __name__ == '__main__':
    main()
