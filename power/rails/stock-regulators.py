#!/usr/bin/env python3
"""List every regulator in a stock device tree and who uses it.

    python3 -I camera/iris/stock-dt.py stock/BOOT.img OUT   # on research/iris
    python3 -I power/rails/stock-regulators.py OUT/dtb11_2441800.txt

dtb11 is the rev26 (hw_rev 0x1a) tree that matches this phone.
Input is the text that stock-dt.py writes (phandles as numbers).
For each node with a regulator-name it prints the path, the voltage range,
the flags (always-on, boot-on, initial-mode, suspend states) and the
consumers: any `*-supply` property that points at its phandle, and any
string property elsewhere in the tree that names it (Samsung's drivers
often call regulator_get() with a name read from a string property).
Reads a text file only; touches no hardware.
"""
import re
import sys


def parse(txt):
    nodes, stack = [], []
    for line in txt.splitlines():
        s = line.strip()
        if s.endswith(' {'):
            node = {'path': '/'.join([n['name'] for n in stack] + [s[:-2]]),
                    'name': s[:-2], 'props': {}, 'kids': []}
            if stack:
                stack[-1]['kids'].append(node)
            stack.append(node)
            nodes.append(node)
        elif s == '};':
            stack.pop()
        else:
            m = re.match(r'([^ =;]+)(?: = (.*))?;$', s)
            if m and stack:
                stack[-1]['props'][m.group(1)] = m.group(2) or ''
    return nodes


def uv(v):
    m = re.match(r'<(0x[0-9a-f]+)>', v or '')
    return int(m.group(1), 16) / 1e6 if m else None


def main():
    nodes = parse(open(sys.argv[1]).read())
    ph = {}
    for n in nodes:
        p = n['props'].get('phandle')
        if p:
            ph[int(p.strip('<>'), 16)] = n
    regs = [n for n in nodes if 'regulator-name' in n['props']]
    for r in regs:
        pr = r['props']
        name = pr['regulator-name'].strip('"')
        lo, hi = uv(pr.get('regulator-min-microvolt')), uv(pr.get('regulator-max-microvolt'))
        flags = [k.replace('regulator-', '') + ('=' + pr[k] if pr[k] else '')
                 for k in pr if k.startswith('regulator-')
                 and k not in ('regulator-name', 'regulator-min-microvolt',
                               'regulator-max-microvolt')]
        for k in r['kids']:
            flags.append(k['name'] + '{' + ','.join(
                a + ('=' + b if b else '') for a, b in k['props'].items()) + '}')
        for k in pr:
            if not k.startswith('regulator-') and k not in ('phandle', 'linux,phandle'):
                flags.append(k + ('=' + pr[k] if pr[k] else ''))
        myph = int(pr['phandle'].strip('<>'), 16) if 'phandle' in pr else None
        users = []
        for n in nodes:
            if n is r:
                continue
            for k, v in n['props'].items():
                if k.endswith('-supply') and myph is not None and \
                        re.fullmatch(r'<0x%x>' % myph, v):
                    users.append('%s:%s' % (n['path'], k))
                elif v.startswith('"') and k != 'regulator-name' and \
                        re.search(r'(^|[",])%s([",]|$)' % re.escape(name), v):
                    users.append('%s:%s' % (n['path'], k))
        rng = '-' if lo is None else ('%.3f' % lo if lo == hi else '%.3f-%.3f' % (lo, hi))
        print('%s\t%s\t%s\t%s\t%s' % (r['path'], name, rng, ' '.join(flags),
                                      ' | '.join(users) or '-'))


if __name__ == '__main__':
    main()
