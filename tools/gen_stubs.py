import sys, re, os
def cpp_argbytes(nm):
    # ?Name@Class@@<access><cc><ret><params>@Z  ; returns (bytes, is_thiscall)
    m = re.match(r'\?+[0-9]?[^@]*@[^@]*@@(.*)', nm)
    if not m: return 0, False
    s = m.group(1)
    acc = s[0]
    thiscall = acc in 'ABCDEFGHIJKLMNOPQRSTUVW' and acc not in 'SKT' and not acc in 'Y'
    # access char then (for members) const char 'A'/'B', then cc 'E' (thiscall) / 'A' cdecl / 'G' stdcall
    if acc in 'QAIMEU' or acc in 'ABCDEFGHIJKLMNOPQRSTUVW':
        i = 1
        if acc not in 'SKTCLD': i = 2   # skip cv qualifier for non-static members
        cc = s[i]; i += 1
    else:
        cc = s[1]; i = 2
    # return type
    def skip_type(i):
        c = s[i]
        if c == '@': return i + 1, 0
        if c in 'PQRA':
            i += 2
            j, _ = skip_type(i)
            return j, 4
        if c == 'U' or c == 'V':
            j = s.index('@@', i) + 2
            return j, 4
        if c == '_':
            return i + 2, 8
        if c.isdigit():
            return i + 1, 4
        if c == 'X': return i + 1, 0
        if c in 'NO': return i + 1, 8
        return i + 1, 4
    if s[i] == '@': i += 1   # ctor/dtor no return
    else: i, _ = skip_type(i)
    total = 0
    while i < len(s) and s[i] not in '@Z':
        if s[i] == 'X': i += 1; break
        i, b = skip_type(i)
        total += b
    return total, cc == 'E'

impl = set()
import glob
for f in glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','rt','*.c')):
    for m in re.finditer(r'SHIM\((\w+),\s*(\w+),', open(f).read()):
        impl.add('imp_%s_%s' % (m.group(1), m.group(2)))
    for m in re.finditer(r'void (imp_\w+)\(void\)', open(f).read()):
        impl.add(m.group(1))
out = ['#include "rt.h"']
seen=set()
for fn in sys.argv[1:]:
    for line in open(fn):
        dll, nm, sym = line.split()
        if sym in seen or sym in impl: continue
        seen.add(sym)
        if dll == 'IFC22':
            b, th = cpp_argbytes(nm)
            out.append('void %s(void) { /* %s */ R.eax = 0; R.esp += 4 + %d; }' % (sym, nm, b))
        else:
            out.append('void %s(void) { guest_fault(0, "unimplemented import %s!%s"); }' % (sym, dll, nm))
open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','gen','stubs.c'),'w').write('\n'.join(out)+'\n')
print(len(seen), 'stubs')
