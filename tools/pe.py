"""PE loading + x86 control-flow discovery for the static recompiler."""
import pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X

class Image:
    def __init__(self, path):
        self.pe = pe = pefile.PE(path)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.mem = bytearray(self.size)
        self.mem[:len(pe.header)] = pe.header
        self.sections = []
        for s in pe.sections:
            d = s.get_data()
            va = s.VirtualAddress
            n = min(len(d), s.Misc_VirtualSize or len(d))
            self.mem[va:va + n] = d[:n]
            self.sections.append((s.Name.rstrip(b'\0').decode(), self.base + va,
                                  max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        t = [s for s in self.sections if s[0] == '.text'][0]
        self.text_lo, self.text_hi = t[1], t[1] + t[2]
        self.imports = {}  # iat addr -> (dll, name)
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                nm = i.name.decode() if i.name else 'ord%d' % i.ordinal
                self.imports[i.address] = (d.dll.decode().upper(), nm)
        self.relocs = set()
        if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC'):
            for b in pe.DIRECTORY_ENTRY_BASERELOC:
                for e in b.entries:
                    if e.type == 3:
                        self.relocs.add(e.rva + self.base)
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.cache = {}

    def u32(self, a):
        o = a - self.base
        return struct.unpack_from('<I', self.mem, o)[0]

    def in_text(self, a):
        return self.text_lo <= a < self.text_hi

    def insn(self, a):
        if a in self.cache:
            return self.cache[a]
        o = a - self.base
        r = None
        for i in self.md.disasm(bytes(self.mem[o:o + 16]), a, 1):
            r = i
        self.cache[a] = r
        return r


def is_jcc(i):
    if i.mnemonic in ('loop', 'loope', 'loopne', 'loopz', 'loopnz', 'jecxz', 'jcxz'):
        return True
    return X.X86_GRP_JUMP in i.groups and i.mnemonic not in ('jmp', 'ljmp')


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.blocks = {}     # start -> list of insns
        self.succ = {}       # start -> list of successor starts
        self.jtables = {}    # insn addr -> list of targets
        self.bad = None


def mem_op(i):
    for op in i.operands:
        if op.type == X.X86_OP_MEM:
            return op.mem
    return None


class Analyzer:
    def __init__(self, img):
        self.img = img
        self.funcs = {}
        self.queue = []
        self.extra_entries = set()

    def add_func(self, a):
        if a not in self.funcs and self.img.in_text(a):
            self.funcs[a] = None
            self.queue.append(a)

    def jump_table(self, i, insns_before):
        """Detect MSVC switch: jmp dword ptr [reg*4 + table]."""
        m = mem_op(i)
        if m is None or m.scale != 4 or m.base != 0 or m.index == 0:
            return None
        table = m.disp & 0xffffffff
        # bound: look back for cmp reg, N ; ja
        n = None
        idx = m.index
        for p in reversed(insns_before[-8:]):
            if p.mnemonic == 'cmp' and len(p.operands) == 2 and p.operands[1].type == X.X86_OP_IMM:
                if p.operands[0].type == X.X86_OP_REG:
                    n = p.operands[1].imm + 1
                    break
        if n is None or n > 1024:
            # walk table while entries look like text addresses
            n = 0
            while n < 512:
                try:
                    v = self.img.u32(table + 4 * n)
                except Exception:
                    break
                if not self.img.in_text(v):
                    break
                n += 1
        tg = []
        for k in range(n):
            v = self.img.u32(table + 4 * k)
            if not self.img.in_text(v):
                return None
            tg.append(v)
        return tg, table, n

    def analyze_func(self, entry):
        img = self.img
        f = Func(entry)
        work = [entry]
        starts = set([entry])
        seen = {}
        # first pass: find block leaders
        while work:
            a = work.pop()
            if a in seen:
                continue
            cur = a
            hist = []
            while True:
                if cur in seen and cur != a:
                    break
                i = img.insn(cur)
                if i is None:
                    f.bad = cur
                    return f
                seen[cur] = i
                hist.append(i)
                nxt = cur + i.size
                mn = i.mnemonic
                if mn in ('ret', 'retf', 'hlt', 'ud2'):
                    break
                if mn == 'int3':
                    break
                if mn == 'jmp':
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm
                        if t in self.funcs and t != entry:
                            break   # tail call
                        if not img.in_text(t):
                            break
                        starts.add(t); work.append(t)
                    else:
                        jt = self.jump_table(i, hist)
                        if jt:
                            f.jtables[cur] = jt
                            for t in jt[0]:
                                starts.add(t); work.append(t)
                    break
                if is_jcc(i):
                    t = i.operands[0].imm
                    starts.add(t); work.append(t)
                    starts.add(nxt)
                    work.append(nxt)
                    break
                if mn == 'call':
                    op = i.operands[0]
                    if op.type == X.X86_OP_IMM:
                        self.add_func(op.imm)
                        # noreturn calls are handled conservatively: continue
                cur = nxt
                if cur in starts:
                    break
        # build blocks
        order = sorted(seen)
        for s in sorted(starts):
            if s not in seen:
                continue
            blk = []
            cur = s
            while True:
                i = seen[cur]
                blk.append(i)
                nxt = cur + i.size
                mn = i.mnemonic
                if mn in ('ret', 'retf', 'hlt', 'ud2', 'int3') or mn == 'jmp' or is_jcc(i):
                    break
                if nxt in starts or nxt not in seen:
                    break
                cur = nxt
            f.blocks[s] = blk
        return f

    def scan_pointers(self):
        """Find code pointers in data and in immediate operands."""
        img = self.img
        cands = set()
        for name, va, size, ch in img.sections:
            if name in ('.rdata', '.data'):
                o = va - img.base
                end = min(o + size, len(img.mem)) - 3
                if img.relocs:
                    for r in img.relocs:
                        if va <= r < va + size:
                            v = img.u32(r)
                            if img.in_text(v):
                                cands.add(v)
                else:
                    for p in range(o, end, 4):
                        v = struct.unpack_from('<I', img.mem, p)[0]
                        if img.in_text(v):
                            cands.add(v)
        return cands

    WEIRD = set('in out ins insb insd insw outs outsb outsd outsw int into bound ljmp lcall retf hlt daa das aaa aas aam aad arpl lds les lock sti cli iretd iret enter'.split())

    def clean(self, f):
        if f is None or f.bad:
            return False
        for b in f.blocks.values():
            for i in b:
                m = i.mnemonic
                pre = set(i.prefix) - {0}
                if m in self.WEIRD or m.startswith('lock') or (pre & {0x26, 0x2e, 0x36, 0x3e, 0x65}):                    return False
                if any(op.type == X.X86_OP_REG and i.reg_name(op.reg) in ('es', 'cs', 'ss', 'ds', 'fs', 'gs') for op in i.operands):
                    return False
                if any(g in (X.X86_GRP_SSE1, X.X86_GRP_SSE2, X.X86_GRP_MMX) for g in i.groups):
                    return False
        return True

    def run(self, extra=()):
        self.discover([self.img.entry])
        for a in sorted(extra):
            if a in self.funcs:
                continue
            snap = dict(self.funcs)
            self.discover([a])
            newf = [k for k in self.funcs if k not in snap]
            if not all(self.clean(self.funcs[k]) for k in newf):
                self.funcs = snap
        # gap filling: try to decode functions in uncovered parts of .text
        for rnd in range(4):
            cov = set()
            for f in self.funcs.values():
                if f:
                    for b in f.blocks.values():
                        for i in b:
                            cov.update(range(i.address, i.address + i.size))
            a = self.img.text_lo
            added = 0
            while a < self.img.text_hi:
                if a in cov:
                    a += 1; continue
                b = self.img.mem[a - self.img.base]
                if b in (0xcc, 0x90, 0x00):
                    a += 1; continue
                # gap start
                if a not in self.funcs:
                    snap = dict(self.funcs)
                    self.discover([a])
                    newf = [k for k in self.funcs if k not in snap]
                    if not all(self.clean(self.funcs[k]) for k in newf):
                        self.funcs = snap
                    else:
                        added += 1
                        for k in newf:
                            for bb in self.funcs[k].blocks.values():
                                for i in bb:
                                    cov.update(range(i.address, i.address + i.size))
                # skip to end of this gap
                while a < self.img.text_hi and a not in cov:
                    a += 1
            if not added:
                break
        return self.funcs

    def discover(self, entries):
        for a in entries:
            self.add_func(a)
        while True:
            while self.queue:
                a = self.queue.pop()
                f = self.analyze_func(a)
                self.funcs[a] = f
            # collect immediates referencing text (push offset func / mov reg, offset func)
            new = False
            for f in list(self.funcs.values()):
                if f is None or f.bad:
                    continue
                for blk in f.blocks.values():
                    for i in blk:
                        if i.mnemonic in ('push', 'mov', 'lea'):
                            for op in i.operands:
                                if op.type == X.X86_OP_IMM and self.img.in_text(op.imm) and op.imm not in self.funcs:
                                    self.add_func(op.imm); new = True
            if not new:
                break
        return self.funcs
