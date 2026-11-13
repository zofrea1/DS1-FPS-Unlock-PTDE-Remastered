"""symloop.py LISTING START END: symbolic trace of a straight-line debug-build loop body (addresses START..END).
Frame slots are 4-byte cells at ebp-X; helper calls take pointer arguments and write 16 bytes at their first (out)
pointer. Prints each call with its arguments resolved to sources, the external writes, and the frame cells read
before being written in the body (loop inputs) and the cells written (loop outputs)."""
import re, sys

lines = [l for l in open(sys.argv[1]).read().splitlines()]
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
body = []
for l in lines:
    a, ins = l.split(':', 1)
    if lo <= int(a, 16) < hi:
        body.append((int(a, 16), ins.strip()))

reg = {}
mem = {}          # ebp offset (negative int) -> symbolic value (4 bytes)
stack = []        # pushed args (symbolic)
x87 = []
first_read = {}   # cell -> address of first read before any write
written = set()
calls = []
ext_writes = []
n_call = 0
OUTS = {0x984920: 1, 0x9AF7F0: 3, 0x553700: 1, 0x5537E0: 1, 0x9B18A0: 1, 0x9B4C60: 0}
SIZES = {(0x9AF7F0, 6): 0xC0}


def cell(off):
    return off


def rd(off):
    if off not in written and off not in first_read:
        first_read[off] = True
    return mem.get(off, 'IN[ebp%+#x]' % off)


def wr(off, v):
    written.add(off)
    mem[off] = v


def parse_mem(op):
    m = re.match(r'(?:dword|xmmword|qword) ptr \[(\w+)(?: ([+-]) (0x[0-9a-f]+|\d+))?\]', op)
    if not m:
        return None
    base = m.group(1)
    off = int(m.group(3), 16) if m.group(3) and m.group(3).startswith('0x') else int(m.group(3) or 0)
    if m.group(2) == '-':
        off = -off
    return base, off


def val(op):
    op = op.strip()
    if op in ('eax', 'ecx', 'edx', 'esi', 'edi'):
        return reg.get(op, op + '?')
    if re.match(r'^(0x[0-9a-f]+|\d+)$', op):
        return int(op, 0)
    pm = parse_mem(op)
    if pm:
        base, off = pm
        if base == 'ebp':
            return rd(off)
        if base == 'ebx':
            return 'ARG[ebx%+#x]' % off
        b = reg.get(base, base + '?')
        if isinstance(b, tuple) and b[0] == 'frameptr':
            return rd(b[1] + off)
        if isinstance(b, tuple) and b[0] == 'callout':
            return ('res', b[1], off)
        return ('load', b, off)
    raise ValueError(op)


for a, ins in body:
    m = re.match(r'(\w+) ?(.*)', ins)
    op, rest = m.group(1), m.group(2)
    args = [x.strip() for x in re.split(r',(?![^\[]*\])', rest)] if rest else []
    if op == 'mov':
        dst, src = args
        if dst in ('eax', 'ecx', 'edx', 'esi', 'edi'):
            reg[dst] = val(src)
        else:
            pm = parse_mem(dst)
            v = val(src)
            if pm[0] == 'ebp':
                wr(pm[1], v)
            elif pm[0] == 'esp':
                stack[-1 - pm[1] // 4] = v
            else:
                b = reg.get(pm[0])
                if isinstance(b, tuple) and b[0] == 'frameptr':
                    wr(b[1] + pm[1], v)
                else:
                    ext_writes.append((a, b, pm[1], v))
    elif op == 'lea':
        dst, src = args
        pm = parse_mem(src.replace('[', 'dword ptr ['))
        if pm[0] == 'ebp':
            reg[dst] = ('frameptr', pm[1])
        else:
            reg[dst] = ('lea', reg.get(pm[0]), pm[1])
    elif op == 'push':
        stack.append(val(args[0]) if args[0] not in ('eax', 'ecx', 'edx') else reg.get(args[0], args[0] + '?'))
    elif op == 'fld':
        x87.append(('x87', val(args[0])))
    elif op == 'fstp':
        pm = parse_mem(args[0])
        v = x87.pop()
        if pm[0] == 'esp':
            stack[-1 - pm[1] // 4] = v
        else:
            wr(pm[1], v)
    elif op == 'call':
        t = int(args[0], 16)
        n_call += 1
        cargs = list(reversed(stack))
        nout = OUTS.get(t, 1)
        desc = []
        for j, x in enumerate(cargs):
            if isinstance(x, tuple) and x[0] == 'frameptr' and j >= nout:
                size = SIZES.get((t, j), 16)
                cells = [rd(x[1] + 4 * k) for k in range(size // 4)]
                desc.append(('ptr', x[1], cells))
            elif j < nout and isinstance(x, tuple) and x[0] == 'frameptr':
                desc.append(('out', x[1]))
            else:
                desc.append(x)
        calls.append((a, t, desc, reg.get('ecx')))
        stack = []
        reg['eax'] = ('callout', n_call)
        for j in range(nout):
            x = cargs[j]
            if isinstance(x, tuple) and x[0] == 'frameptr':
                for k in range(4):
                    wr(x[1] + 4 * k, ('res', n_call, 16 * j + 4 * k))
    elif op == 'add' and args[0] == 'esp':
        pass
    elif op == 'imul':
        reg[args[0]] = ('mul', reg.get(args[1]), int(args[2], 0))
    elif op == 'add':
        reg[args[0]] = ('add', reg.get(args[0]), val(args[1]))
    elif op in ('jmp',):
        pass
    else:
        print('?? %08X %s' % (a, ins))


def show(v):
    if isinstance(v, tuple):
        if v[0] == 'res':
            return 'r%d.%d' % (v[1], v[2] // 4)
        if v[0] == 'x87':
            return 'x87(%s)' % show(v[1])
        if v[0] == 'load':
            return '%s[%+#x]' % (show(v[1]), v[2])
        if v[0] == 'ptr':
            cs = [show(c) for c in v[2]]
            if len(v[2]) == 4 and all(isinstance(c, tuple) and c[0] == 'res' and c[1] == v[2][0][1] for c in v[2]) and [c[2] % 16 for c in v[2]] == [0, 4, 8, 12]:
                return '&ebp%+#x=r%d%s' % (v[1], v[2][0][1], '' if v[2][0][2] < 16 else '/%d' % (v[2][0][2] // 16))
            if all(isinstance(c, str) and c.startswith('IN[') for c in cs):
                src = [int(c[6:-1], 16) for c in cs]
                if src == [src[0] + 4 * k for k in range(len(src))]:
                    return '&ebp%+#x=IN@ebp%+#x' % (v[1], src[0])
                return '&ebp%+#x=IN{%s}' % (v[1], ','.join('%+#x' % x for x in src))
            return '&ebp%+#x={%s}' % (v[1], ','.join(cs))
        if v[0] == 'out':
            return 'OUT&ebp%+#x' % v[1]
        if v[0] == 'frameptr':
            return '&ebp%+#x' % v[1]
        return '(' + ' '.join(show(x) for x in v) + ')'
    return str(v)


for i, (a, t, desc, _ecx) in enumerate(calls, 1):
    print('r%-2d %08X call %X(%s)' % (i, a, t, ', '.join(show(d) for d in desc)))
print('external writes:', [(hex(a), show(b), off, show(v)) for a, b, off, v in ext_writes])
ins_cells = sorted(k for k in first_read)
print('frame cells read before written (inputs):', ' '.join('%#x' % -c for c in ins_cells))
print('frame cells written:', len(written))
print('inputs also written:', sorted('%#x' % -c for c in set(first_read) & written))
