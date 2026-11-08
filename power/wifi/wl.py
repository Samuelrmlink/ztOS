#!/usr/bin/python3
# wl.py: tiny wl over brcmfmac's vendor dcmd (via iw vendor recv)
#  wl.py get NAME [LEN]          GET_VAR, prints u32s
#  wl.py set NAME U32...         SET_VAR with u32 payload
#  wl.py setb NAME HEX           SET_VAR with raw hex payload
#  wl.py cget CMD [LEN]          plain ioctl get
#  wl.py cset CMD U32            plain ioctl set
#  wl.py str NAME [LEN]          GET_VAR printed as a string
import struct, subprocess, sys, tempfile, re
IF = 'wlp1s0'
def dcmd(cmd, data, ln, set_):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(struct.pack('<IiIII', cmd, ln, 20, set_, 0) + data)
        p = f.name
    out = subprocess.run(['iw', 'dev', IF, 'vendor', 'recv', '0x001018', '0x1', p],
                         capture_output=True, text=True)
    if out.returncode:
        print('ERR', out.returncode, out.stderr.strip(), out.stdout.strip()); sys.exit(1)
    b = bytes(int(x, 16) for x in re.findall(r'\b[0-9a-f]{2}\b', out.stdout.replace('vendor response:', '')))
    # attributes: len u16, type u16, data, padded to 4
    res = b''; i = 0
    while i + 4 <= len(b):
        l, t = struct.unpack_from('<HH', b, i)
        if l < 4: break
        if t == 2: res += b[i + 4:i + l]
        i += (l + 3) & ~3
    return res
a = sys.argv
if a[1] in ('get', 'str'):
    n = a[2].encode() + b'\0'; ln = int(a[3]) if len(a) > 3 else 64
    r = dcmd(262, n + b'\0' * max(0, ln - len(n)), max(ln, len(n)), 0)
    if a[1] == 'str': print(r.split(b'\0')[0].decode(errors='replace'))
    else: print(a[2], *struct.unpack_from('<%dI' % min(int(a[4]) if len(a) > 4 else 4, len(r) // 4), r))
elif a[1] == 'getp':	# getp NAME U32 [LEN]: GET_VAR with a u32 parameter
    n = a[2].encode() + b'\0' + struct.pack('<I', int(a[3], 0)); ln = int(a[4]) if len(a) > 4 else 64
    r = dcmd(262, n + b'\0' * max(0, ln - len(n)), max(ln, len(n)), 0)
    print(a[2], a[3], *struct.unpack_from('<%dI' % (len(r) // 4), r))
elif a[1] == 'set':
    d = a[2].encode() + b'\0' + b''.join(struct.pack('<I', int(x, 0)) for x in a[3:])
    dcmd(263, d, len(d), 1); print('ok')
elif a[1] == 'setb':
    d = a[2].encode() + b'\0' + bytes.fromhex(a[3])
    dcmd(263, d, len(d), 1); print('ok')
elif a[1] == 'cget':
    ln = int(a[3]) if len(a) > 3 else 4
    r = dcmd(int(a[2]), b'\0' * ln, ln, 0)
    print('cmd', a[2], *struct.unpack_from('<%dI' % (len(r) // 4), r))
elif a[1] == 'cset':
    d = struct.pack('<I', int(a[3], 0))
    dcmd(int(a[2]), d, 4, 1); print('ok')
