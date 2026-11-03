import re
frames = []
for line in open('/tmp/nibe_menu.log', errors='replace'):
    m = re.match(r'\+\s*([0-9.]+)s FRAME cmd=(0x[0-9a-f]+) from=(0x[0-9a-f]+) chk=(\w+) data=([0-9a-f ]*)', line)
    if m:
        frames.append((float(m.group(1)), int(m.group(2),16), int(m.group(3),16), m.group(5).strip()))
# group text rows into screens within 120ms windows
screens = []
cur = {}; t0 = None
for t, cmd, frm, data in frames:
    if 0x50 <= cmd <= 0x53 and frm == 0x000a:
        if t0 is None or t - t0 > 0.12:
            if cur: screens.append((t0, dict(cur)))
            cur = {}; t0 = t
        cur[cmd - 0x4F] = bytes.fromhex(data).rstrip(b'\x00').decode('ascii','replace') if data else ''
if cur: screens.append((t0, dict(cur)))
seen = set()
print('--- distinct display screens ---')
for t, r in screens:
    key = (r.get(2,''), r.get(3,''), r.get(4,''))
    if key in seen: continue
    seen.add(key)
    print(f'{t:8.1f} | r2="{r.get(2,"")}" | r3="{r.get(3,"")}" | r4="{r.get(4,"")}"')
print('--- display-originated frames (buttons) ---')
from collections import Counter
c = Counter((hex(cmd), data) for t, cmd, frm, data in frames if frm == 0x00f9)
for k, n in c.most_common(20):
    print(k, 'x', n)
print('--- all frame cmds seen ---')
print(Counter(hex(cmd) for t, cmd, frm, data in frames).most_common())
