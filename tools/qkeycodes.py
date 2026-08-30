#!/usr/bin/env python3
"""Print QEMU QKeyCode enum members relevant to typing."""
import json, glob

paths = glob.glob('/usr/share/qemu/qapi-schema.json') + \
        glob.glob('/usr/share/qemu/*.json')
data = None
for p in paths:
    try:
        data = json.load(open(p))
        print('loaded', p)
        break
    except Exception as e:
        print('skip', p, e)

if data:
    for e in data:
        if isinstance(e, dict) and e.get('name') == 'QKeyCode':
            vals = e['values']
            interesting = [v for v in vals if len(v) <= 2 or v in
                           ('spc', 'dot', 'minus', 'slash', 'ret', 'shift')]
            print('short names:', interesting)
            for want in ('spc', 'dot', 'ret', '0', '1', '9', 'minus', 'slash', 'sp'):
                print(want, 'present' if want in vals else 'MISSING')
