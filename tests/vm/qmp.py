"""Talk to a running test VM through QEMU's QMP socket.

qmp.py <socket> shot <out.ppm>        save a screenshot (PPM)
qmp.py <socket> keys <key> [<key>...] press keys, QEMU names: ret, tab, alt-n, spc...
qmp.py <socket> type <text>           type text (US layout: letters, digits, :/.-_ and space)
qmp.py <socket> powerdown             ask Windows to shut down (power button)
qmp.py <socket> quit                  stop the VM immediately
"""
import json
import os
import socket
import sys
import time


def main():
    sock, op, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(30)
    s.connect(sock)
    f = s.makefile('rw')

    def cmd(name, **a):
        f.write(json.dumps({'execute': name, 'arguments': a} if a else {'execute': name}) + '\n')
        f.flush()
        while True:
            r = json.loads(f.readline())
            if 'error' in r:
                raise SystemExit('qmp %s: %s' % (name, r['error'].get('desc')))
            if 'return' in r:
                return r['return']

    json.loads(f.readline())          # greeting
    cmd('qmp_capabilities')
    if op == 'shot':
        cmd('screendump', filename=os.path.abspath(args[0]))
    elif op == 'keys':
        for k in args:
            cmd('human-monitor-command', **{'command-line': 'sendkey ' + k})
            time.sleep(0.4)
    elif op == 'type':
        sym = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus', ':': 'shift-semicolon', '_': 'shift-minus',
               '?': 'shift-slash', '=': 'equal', '&': 'shift-7'}
        for ch in ' '.join(args):
            k = ch if ch.islower() or ch.isdigit() else 'shift-' + ch.lower() if ch.isupper() else sym.get(ch)
            if k is None:
                raise SystemExit('qmp type: no key for %r' % ch)
            cmd('human-monitor-command', **{'command-line': 'sendkey ' + k})
            time.sleep(0.08)
    elif op == 'powerdown':
        cmd('system_powerdown')
    elif op == 'quit':
        cmd('quit')
    else:
        raise SystemExit(__doc__)


main()
