#!/usr/bin/env python3
"""folder_to_dsk.py <archive or folder> <out.dsk> [--label NAME]
Unpacks a classic Mac archive (.sit/.hqx/.cpt/.zip... via unar, keeping resource forks) or takes a
folder with native forks, and builds a raw HFS disk image with the same files, folders, types,
creators and resource forks (for Mini vMac / the website's Insert Disk)."""
import os, sys, subprocess, tempfile, struct, shutil
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "mactools")); import rsrc
src, out = sys.argv[1], sys.argv[2]
label = sys.argv[sys.argv.index('--label') + 1] if '--label' in sys.argv else None
work = tempfile.mkdtemp(prefix='f2d_')
if os.path.isdir(src): root = src
else:
    subprocess.run(['unar', '-q', '-k', 'fork', '-f', '-o', work, src], check=True)
    ents = [e for e in os.listdir(work) if not e.startswith('.')]
    root = os.path.join(work, ents[0]) if len(ents) == 1 and os.path.isdir(os.path.join(work, ents[0])) else work
label = (label or os.path.basename(root.rstrip('/')) or 'Disk')[:27]
files = []; total = 0
for dp, dns, fns in os.walk(root):
    dns[:] = [d for d in dns if not d.startswith('.')]
    for fn in fns:
        if fn.startswith('.'): continue
        p = os.path.join(dp, fn)
        rf = open(p + '/..namedfork/rsrc', 'rb').read() if os.path.exists(p + '/..namedfork/rsrc') else b''
        data = open(p, 'rb').read()
        fi = subprocess.run(['xattr', '-px', 'com.apple.FinderInfo', p], capture_output=True, text=True).stdout
        fi = bytes.fromhex(fi.replace(' ', '').replace('\n', '')) if fi.strip() else b'\0' * 32
        files.append((os.path.relpath(p, root), data, rf, fi)); total += len(data) + len(rf)
kb = max(1440, int(total * 1.35 / 1024) + 200); kb = (kb + 1) // 2 * 2
os.environ['HOME'] = tempfile.mkdtemp()
with open(out, 'wb') as f: f.truncate(kb * 1024)
subprocess.run(['hformat', '-l', label, out], check=True, capture_output=True)
subprocess.run(['hmount', out], check=True, capture_output=True)
made = set()
for rel, data, rf, fi in files:
    parts = rel.split('/')
    for i in range(1, len(parts)):
        d = ':' + ':'.join(parts[:i])
        if d not in made: subprocess.run(['hmkdir', d], capture_output=True); made.add(d)
    h = bytearray(128); nm = parts[-1].encode('mac_roman', 'replace')[:63]; h[1] = len(nm); h[2:2 + len(nm)] = nm
    h[65:73] = fi[0:8]; h[73] = fi[8] & ~1; h[101] = fi[9]
    mb = os.path.join(work, 'x.bin'); rsrc.write_macbinary(mb, bytes(h), data, rf)
    r = subprocess.run(['hcopy', '-m', mb, ':' + ':'.join(parts)], capture_output=True, text=True)
    if r.returncode: print('copy failed:', rel, r.stderr.strip())
print(subprocess.run(['hls', '-lR', ':'], capture_output=True, text=True).stdout.count('\n'), 'entries;',
      [l for l in subprocess.run(['hvol'], capture_output=True, text=True).stdout.splitlines() if 'free' in l][0].strip())
subprocess.run(['humount'], capture_output=True); shutil.rmtree(work, ignore_errors=True)
print(f'{out}  ({kb} KB HFS, label "{label}", {len(files)} files)')
