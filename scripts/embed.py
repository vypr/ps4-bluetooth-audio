"""Write a C header holding a file's bytes as a NUL-terminated char array.

  python3 scripts/embed.py <input> <output.h> <symbol>

Line endings are normalised to CRLF so the output does not depend on how git
checked the file out. Releases up to 2.7.5 were built from a Windows checkout
with CRLF endings, so this keeps those payloads reproducible byte for byte.
"""
import sys
from pathlib import Path

src, dst, name = sys.argv[1:4]
data = Path(src).read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
Path(dst).write_text('static const char %s[] = {%s};' % (name, ','.join([str(b) for b in data] + ['0'])))
