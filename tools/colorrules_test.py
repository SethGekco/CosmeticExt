"""Install or remove the ColorRules test snippet in the game's rulesmd.ini.

The snippet is read from docs/colorrules.md ("## Test snippet"), so the doc
stays the one source. Sections the file already has get the snippet's lines
added after their last key; sections it lacks are added as one block above
the file's final comment, which the engine ignores (the EOF guard).

Every added line is marked, so `remove` takes out exactly what `install` put
in and leaves any other edit made in between alone. rulesmd.ini is backed up
first (*.bak-cosmeticext-<time>), and the file is compared again just before
writing, so an edit by someone else in the meantime aborts the install.

    python3 tools/colorrules_test.py status
    python3 tools/colorrules_test.py install
    python3 tools/colorrules_test.py remove
"""

import argparse
import datetime
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DOC = os.path.join(HERE, '..', 'docs', 'colorrules.md')
GAME = os.path.expanduser('~/snap/cncra2yr/common/.wine/drive_c/Westwood/RA2')
MARK = 'cosmeticext-colortest'
BEGIN = f'; ===== BEGIN {MARK}: remove with CosmeticExt tools/colorrules_test.py remove ====='
END = f'; ===== END {MARK} ====='


def snippet(doc=DOC):
    """[(section, [line, ...]), ...] from the doc's test snippet."""
    text = open(doc, encoding='utf-8').read()
    m = re.search(r'^## Test snippet\n.*?^```ini\n(.*?)^```', text, re.M | re.S)
    if not m:
        raise SystemExit(f'{doc}: no ini block under "## Test snippet"')
    out = []
    for line in m.group(1).splitlines():
        if line.startswith('['):
            out.append((line[1:line.index(']')], []))
        elif line.strip() and not line.lstrip().startswith(';'):
            if not out:
                raise SystemExit(f'{doc}: snippet line before any section: {line}')
            out[-1][1].append(line)
    return out


def key(line):
    return line.split(';', 1)[0].split('=', 1)[0].strip().lower()


def marked(line):
    """The line with the marker leading its comment, so the comment still reads."""
    value, sep, comment = line.partition(';')
    return f'{value.rstrip()} ; {MARK}' + (f': {comment.strip()}' if sep else '')


def headers(lines):
    """{lowercase section name: [line index of its header, ...]}"""
    found = {}
    for i, line in enumerate(lines):
        m = re.match(r'\[([^\]]+)\]', line)
        if m:
            found.setdefault(m.group(1).strip().lower(), []).append(i)
    return found


def install_text(text, sections):
    if MARK in text:
        raise SystemExit(f'already installed ({MARK} lines found); run remove first')
    lines = text.split('\n')
    at = headers(lines)
    inserts, block = [], []
    for name, body in sections:
        where = at.get(name.lower(), [])
        if len(where) > 1:
            raise SystemExit(f'[{name}] appears {len(where)} times; not guessing which one the engine uses')
        if not where:
            block += [f'[{name}]'] + body + ['']
            continue
        start = where[0]
        end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith('[')), len(lines))
        have = {key(l) for l in lines[start + 1:end] if '=' in l.split(';', 1)[0]}
        clash = [key(l) for l in body if key(l) in have]
        if clash:
            raise SystemExit(f'[{name}] already sets {", ".join(clash)}; not overwriting the mod')
        last = max((i for i in range(start + 1, end) if '=' in lines[i].split(';', 1)[0]), default=start)
        inserts.append((last + 1, [marked(l) for l in body]))
    if block:
        # above the final comment: the engine ignores an INI's last line
        tail = len(lines) - 1 if lines[-1] == '' else len(lines)
        guard = tail - 1 if tail and lines[tail - 1].lstrip().startswith(';') else tail
        inserts.append((guard, [BEGIN] + block[:-1] + [END, '']))
    for pos, new in sorted(inserts, reverse=True):
        lines[pos:pos] = new
    return '\n'.join(lines)


def remove_text(text):
    lines = text.split('\n')
    out, skipping = [], False
    for line in lines:
        if line == BEGIN:
            skipping = True
        elif skipping and line == END:
            skipping = 'end'
        elif skipping == 'end':
            skipping = False
            if line != '':
                out.append(line)
        elif not skipping and MARK not in line:
            out.append(line)
    if skipping is True:
        raise SystemExit('BEGIN marker without END; fix rulesmd.ini by hand')
    return '\n'.join(out)


def read(path):
    raw = open(path, 'rb').read()
    try:
        return raw, raw.decode('utf-8'), 'utf-8'
    except UnicodeDecodeError:
        return raw, raw.decode('latin-1'), 'latin-1'


def write_checked(path, before, text, enc):
    """Write in place (same inode) only if nobody changed the file since we read it."""
    if open(path, 'rb').read() != before:
        raise SystemExit(f'{path} changed while this ran; nothing written, run it again')
    with open(path, 'r+b') as fh:
        fh.write(text.encode(enc))
        fh.truncate()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('action', choices=('status', 'install', 'remove'))
    ap.add_argument('--game', default=GAME, help='game folder holding rulesmd.ini')
    ap.add_argument('--doc', default=DOC, help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    path = os.path.join(args.game, 'rulesmd.ini')
    raw, text, enc = read(path)
    count = sum(MARK in l for l in text.split('\n'))
    if args.action == 'status':
        print(f'{path}: {"installed" if count else "not installed"} ({count} marked lines)')
        return 0
    if args.action == 'install':
        new = install_text(text, snippet(args.doc))
    else:
        if not count:
            raise SystemExit('not installed')
        new = remove_text(text)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup, n = f'{path}.bak-cosmeticext-{stamp}', 1
    while os.path.exists(backup):  # two runs in one second must not share a backup
        backup, n = f'{path}.bak-cosmeticext-{stamp}-{n}', n + 1
    shutil.copy2(path, backup)
    write_checked(path, raw, new, enc)
    added = sum(MARK in l for l in new.split('\n'))
    print(f'{args.action}: {path} ({added} marked lines now); backup {os.path.basename(backup)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
