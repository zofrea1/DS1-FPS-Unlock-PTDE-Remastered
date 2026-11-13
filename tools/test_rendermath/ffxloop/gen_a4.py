"""gen_a4.py: C++ for one iteration of 0xA4D420's particle loop, per branch path, from the symbolic trace."""
import io, contextlib, runpy, sys

NOUT = {0x9AF7F0: 3}


def trace(listing):
    sys.argv = ['symloop.py', listing, '0', 'FFFFFFFF']
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        g = runpy.run_path('symloop.py')
    return g['calls']


def scalar(v):
    if isinstance(v, int):
        return '0x%Xu' % (v & 0xFFFFFFFF)
    if isinstance(v, str):
        if v.startswith('ARG[ebx+'):
            return '*reinterpret_cast<const uint32_t*>(ebx + %s)' % v[8:-1]
        if v == 'IN[ebp-0x730]':
            return 'off'
        raise ValueError(v)
    if v[0] == 'x87':
        inner = v[1]
        if inner[0] == 'load' and isinstance(inner[1], str) and inner[1].startswith('0x'):
            return 'x87_pass(*reinterpret_cast<const uint32_t*>(%s))' % inner[1].rstrip('?')
    raise ValueError(repr(v))


def cell(c):
    if isinstance(c, tuple) and c[0] == 'res':
        return 'r%d[%d]' % (c[1], c[2] // 4)
    if isinstance(c, str) and c.startswith('IN[ebp'):
        return '*reinterpret_cast<const uint32_t*>(ebp - %s)' % c[7:-1]
    raise ValueError(repr(c))


def pointer(d, temps, k, j):
    if d[0] == 'out':
        raise ValueError('out as input')
    cells = d[2]
    if all(isinstance(c, tuple) and c[0] == 'res' for c in cells):
        m = cells[0][1]
        base = cells[0][2]
        if all(c[1] == m and c[2] == base + 4 * i for i, c in enumerate(cells)) and base % 16 == 0:
            return 'r%d + %d' % (m, base // 4) if base else 'r%d' % m
    if all(isinstance(c, str) and c.startswith('IN[ebp') for c in cells):
        src = [int(c[6:-1], 16) for c in cells]
        if src == [src[0] + 4 * i for i in range(len(src))]:
            return 'reinterpret_cast<const uint32_t*>(ebp - %#x)' % -src[0]
    name = 't%d_%d' % (k, j)
    temps.append('    const uint32_t %s[%d] = {%s};' % (name, len(cells), ', '.join(cell(c) for c in cells)))
    return name


def gen(listing, fname):
    calls = trace(listing)
    out = ['void %s(uint8_t* ebp, const uint8_t* ebx, uint32_t off) {' % fname]
    for k, (a, t, desc, ecx) in enumerate(calls, 1):
        temps = []
        if t == 0x5538B0:   # out = *(src + off)
            assert desc[0][0] == 'out'
            out.append('    alignas(16) uint32_t r%d[4];  // %08X' % (k, a))
            src = desc[1]
            if isinstance(src, tuple) and src[0] == 'ptr':   # a frame array indexed by the offset
                base = 'ebp - %#x' % -src[1]
            else:
                base = 'reinterpret_cast<const uint8_t*>(%s)' % scalar(src)
            out.append('    std::memcpy(r%d, %s + %s, 16);' % (k, base, scalar(desc[2])))
            continue
        if t == 0x553880:   # *(dst + off) = *src
            src = pointer(desc[1], temps, k, 1)
            out.extend(temps)
            out.append('    std::memcpy(reinterpret_cast<uint8_t*>(%s) + %s, %s, 16);  // %08X' % (scalar(desc[0]), scalar(desc[2]), src, a))
            continue
        nout = NOUT.get(t, 1)
        out.append('    alignas(16) uint32_t r%d[%d];  // %08X' % (k, 4 * nout, a))
        args = []
        for j, d in enumerate(desc):
            if j < nout:
                assert d[0] == 'out', (hex(a), d)
                args.append('r%d + %d' % (k, 4 * j) if j else 'r%d' % k)
            elif isinstance(d, tuple) and d[0] == 'ptr':
                args.append(pointer(d, temps, k, j))
            else:
                args.append(scalar(d))
        out.extend(temps)
        if t == 0x9B4650:
            self_ = scalar(ecx)
            out.append('    reinterpret_cast<void*(__fastcall*)(uint32_t, uint32_t, void*, const void*, uint32_t, const void*)>(0x%X)(%s, 0, %s);'
                       % (t, self_, ', '.join(args)))
        else:
            types = ', '.join(['void*'] + ['uint32_t' if not a_.startswith(('r', 't', 'reinterpret')) else 'const void*' for a_ in args[1:]])
            out.append('    reinterpret_cast<void*(__cdecl*)(%s)>(0x%X)(%s);' % (types, t, ', '.join(args)))
    out.append('}')
    return '\n'.join(out), len(calls)


code_t, nt = gen('a4_taken.txt', 'a4_iter_skip')
code_f, nf = gen('a4_fall.txt', 'a4_iter_full')
open('a4_gen.cpp', 'w').write(code_t + '\n\n' + code_f + '\n')
print('calls', nt, nf)
