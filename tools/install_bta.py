"""Install Bluetooth Audio and manage its single GoldHEN AutoRun entry over FTP.

Any remote file it replaces is first copied to install_backups/ on this PC.
Does not launch or reboot the PS4.
"""
import argparse
from datetime import datetime
import ftplib
import io
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = '/data/payloads/BTA-Audio.bin'
INI = '/data/GoldHEN/payloads.ini'
ENTRY = '/user/data/payloads/BTA-Audio.bin'


def retrieve(ftp, path):
    data = bytearray()
    try:
        ftp.retrbinary('RETR ' + path, data.extend)
    except ftplib.error_perm as exc:
        if str(exc).startswith('550'):
            return None
        raise
    return bytes(data)


def edit_autorun(data, enabled):
    # surrogateescape: round-trip any non-UTF-8 bytes in the user's own entries untouched.
    text = (data or b'').decode('utf-8-sig', 'surrogateescape')
    lines = text.splitlines(keepends=True)
    newline = '\r\n' if '\r\n' in text else '\n'
    output, section, found, inserted = [], '', False, False
    entry_line = ENTRY + (' = 1' if enabled else ' = 0') + newline
    for line in lines:
        match = re.match(r'^\s*\[([^]]+)\]', line)
        if match:
            if section == 'AutoRun' and not inserted:
                if output and not output[-1].endswith(('\n', '\r')):
                    output[-1] += newline
                output.append(entry_line)
                inserted = True
            section = match[1]
            if section == 'AutoRun':
                found = True
        if section == 'AutoRun' and line.split('=', 1)[0].strip() in (ENTRY, PAYLOAD):
            if not inserted:
                output.append(entry_line)
                inserted = True
        else:
            output.append(line)
    if not inserted:
        if output and not output[-1].endswith(('\n', '\r')):
            output[-1] += newline
        if not found:
            output.append(newline + '[AutoRun]' + newline)
        output.append(entry_line)
    return ''.join(output).encode('utf-8', 'surrogateescape')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', required=True, help='PS4 LAN IP address')
    p.add_argument('--disable-autorun', action='store_true')
    p.add_argument('--payload', default=str(ROOT / 'build/BTA-Audio.bin'),
                   help='payload to install as the boot payload, default: build/BTA-Audio.bin from make payload')
    args = p.parse_args()
    backup = ROOT / 'install_backups' / datetime.now().strftime('%Y%m%d-%H%M%S')
    with ftplib.FTP() as ftp:
        ftp.connect(args.host, 2121, timeout=8)
        ftp.login()

        def replace(path, data):
            # A console that has never run a payload has no /data/payloads or /data/GoldHEN,
            # and STOR into a missing directory fails with 550. Create the parents instead
            # of making the user mkdir them over FTP first.
            parent = path.rsplit('/', 1)[0]
            try:
                ftp.mkd(parent)
            except ftplib.all_errors:
                pass  # already there, or the server forbids it: STOR reports the real problem
            previous = retrieve(ftp, path)
            if previous == data:
                print('Already current:', path)
                return
            if previous is not None:
                backup.mkdir(parents=True, exist_ok=True)
                (backup / path.strip('/').replace('/', '__')).write_bytes(previous)
                print('Backed up', path, 'to', backup)
            # Upload beside the target and check it landed before renaming it into place, so a
            # dropped connection can't leave a truncated payloads.ini. The .bta-staging suffix
            # keeps the half-written file out of the loader's .bin list.
            staging = path + '.bta-staging'
            ftp.storbinary('STOR ' + staging, io.BytesIO(data))
            if retrieve(ftp, staging) != data:
                raise RuntimeError('Upload did not land intact: ' + staging)
            ftp.rename(staging, path)
            print('Installed:', path)

        if not args.disable_autorun:
            replace(PAYLOAD, Path(args.payload).read_bytes())
        replace(INI, edit_autorun(retrieve(ftp, INI), not args.disable_autorun))
    print('AutoRun disabled.' if args.disable_autorun else 'GoldHEN AutoRun entry enabled.')


if __name__ == '__main__':
    main()
