#!/usr/bin/env python3
"""x86-32 (MSVC Win32) -> portable C static recompiler.

usage: recomp.py <exe> <prefix> <outdir>
"""
import sys, os, struct, collections
from pe import Image, Analyzer, is_jcc, mem_op
from capstone import x86_const as X

FLAGS = 'CZSOPA'

REG32 = {'eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'}
REG16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi', 'di': 'edi'}
REG8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
REG8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}

CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SB = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}

CC_FLAGS = {
    'o': 'O', 'no': 'O', 'b': 'C', 'ae': 'C', 'e': 'Z', 'ne': 'Z', 'be': 'CZ', 'a': 'CZ',
    's': 'S', 'ns': 'S', 'p': 'P', 'np': 'P', 'l': 'SO', 'ge': 'SO', 'le': 'ZSO', 'g': 'ZSO',
}
CC_ALIAS = {'c': 'b', 'nae': 'b', 'nc': 'ae', 'nb': 'ae', 'z': 'e', 'nz': 'ne', 'na': 'be', 'nbe': 'a',
            'pe': 'p', 'po': 'np', 'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g'}


def norm_cc(c):
    return CC_ALIAS.get(c, c)


class Unsupported(Exception):
    pass


def reg_read(name):
    if name in REG32:
        return name
    if name in REG16:
        return '((uint16_t)%s)' % REG16[name]
    if name in REG8L:
        return '((uint8_t)%s)' % REG8L[name]
    if name in REG8H:
        return '((uint8_t)(%s>>8))' % REG8H[name]
    raise Unsupported('reg ' + name)


def reg_write(name, v):
    if name in REG32:
        return '%s = (uint32_t)(%s);' % (name, v)
    if name in REG16:
        r = REG16[name]
        return '%s = (%s & 0xffff0000u) | (uint16_t)(%s);' % (r, r, v)
    if name in REG8L:
        r = REG8L[name]
        return '%s = (%s & 0xffffff00u) | (uint8_t)(%s);' % (r, r, v)
    if name in REG8H:
        r = REG8H[name]
        return '%s = (%s & 0xffff00ffu) | ((uint32_t)(uint8_t)(%s) << 8);' % (r, r, v)
    raise Unsupported('reg ' + name)


class FuncGen:
    def __init__(self, rc, f):
        self.rc = rc
        self.f = f
        self.img = rc.img
        self.lines = []
        self.saved = set()        # producers that need saved operands
        self.explicit = set()     # (producer, flag)
        self.direct = {}          # consumer addr -> {flag: producer}
        self.kinds = {}           # producer addr -> kind
        self.insns = {}
        self.unsupported = []

    # ---------------- flag modeling -----------------
    def flag_info(self, i):
        """returns (uses, defs, kind)"""
        m = i.mnemonic
        base = m.split()[-1]
        if m.startswith('rep') and base.startswith(('movs', 'stos', 'lods')):
            return set(), set(), None
        if base in ('cmp', 'sub'):
            return set(), set(FLAGS), 'sub'
        if base in ('sbb',):
            return set('C'), set(FLAGS), 'sbb'
        if base in ('add',):
            return set(), set(FLAGS), 'add'
        if base in ('adc',):
            return set('C'), set(FLAGS), 'adc'
        if base in ('and', 'or', 'xor', 'test'):
            return set(), set(FLAGS), 'logic'
        if base == 'inc':
            return set(), set('ZSOPA'), 'inc'
        if base == 'dec':
            return set(), set('ZSOPA'), 'dec'
        if base == 'neg':
            return set(), set(FLAGS), 'neg'
        if base in ('shl', 'sal', 'shr', 'sar'):
            return set(), set(FLAGS), base if base != 'sal' else 'shl'
        if base in ('rol', 'ror'):
            return set(), set('CO'), base
        if base in ('rcl', 'rcr'):
            return set('C'), set('CO'), base
        if base in ('shld', 'shrd'):
            return set(), set(FLAGS), base
        if base in ('mul', 'imul'):
            return set(), set(FLAGS), base
        if base in ('div', 'idiv'):
            return set(), set(FLAGS), 'undef'
        if base in ('scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd'):
            return set(), set(FLAGS), 'strcmp'
        if base in ('bt', 'bts', 'btr', 'btc'):
            return set(), set('C'), 'bt'
        if base in ('bsf', 'bsr'):
            return set(), set('Z'), base
        if base == 'sahf':
            return set(), set('CZSPA'), 'sahf'
        if base == 'popfd':
            return set(), set(FLAGS), 'popf'
        if base == 'clc' or base == 'stc':
            return set(), set('C'), base
        if base == 'cmc':
            return set('C'), set('C'), 'cmc'
        if base == 'pushfd' or base == 'lahf':
            return set(FLAGS), set(), None
        if base.startswith('set'):
            return set(CC_FLAGS[norm_cc(base[3:])]), set(), None
        if base.startswith('cmov'):
            return set(CC_FLAGS[norm_cc(base[4:])]), set(), None
        if is_jcc(i) and base not in ('jecxz', 'jcxz', 'loop'):
            if base in ('loope', 'loopz', 'loopne', 'loopnz'):
                return set('Z'), set(), None
            return set(CC_FLAGS[norm_cc(base[1:])]), set(), None
        if base == 'call':
            return set(), set(FLAGS), 'undef'
        if base in ('daa', 'das'):
            return set('CA'), set(FLAGS), 'undef'
        return set(), set(), None

    def analyze_flags(self):
        f = self.f
        succ = {}
        for s, blk in f.blocks.items():
            last = blk[-1]
            nxt = last.address + last.size
            ss = []
            m = last.mnemonic
            if is_jcc(last):
                ss = [last.operands[0].imm, nxt]
            elif m == 'jmp':
                if last.operands[0].type == X.X86_OP_IMM:
                    t = last.operands[0].imm
                    if t in f.blocks and not (t in self.rc.funcs and t != f.entry):
                        ss = [t]
                elif last.address in f.jtables:
                    ss = list(set(f.jtables[last.address][0]))
            elif m in ('ret', 'retf', 'hlt', 'int3', 'ud2'):
                ss = []
            else:
                ss = [nxt]
            succ[s] = [t for t in ss if t in f.blocks]
        self.succ = succ
        for blk in f.blocks.values():
            for i in blk:
                u, d, k = self.flag_info(i)
                if d:
                    self.kinds[i.address] = k
        # reaching producers per flag
        U = 'U'
        inn = {s: None for s in f.blocks}
        inn[f.entry] = {F: frozenset([U]) for F in FLAGS}
        work = [f.entry]
        out = {}
        while work:
            s = work.pop()
            cur = dict(inn[s])
            for i in f.blocks[s]:
                uses, defs, kind = self.flag_info(i)
                for F in defs:
                    cur[F] = frozenset([i.address])
                    self.kinds[i.address] = kind
            out[s] = cur
            for t in succ[s]:
                if inn[t] is None:
                    inn[t] = dict(cur); work.append(t)
                else:
                    ch = False
                    for F in FLAGS:
                        u = inn[t][F] | cur[F]
                        if u != inn[t][F]:
                            inn[t][F] = u; ch = True
                    if ch:
                        work.append(t)
        # unreachable blocks (shouldn't happen) get unknown
        for s in f.blocks:
            if inn[s] is None:
                inn[s] = {F: frozenset([U]) for F in FLAGS}
        # consumers
        for s, blk in f.blocks.items():
            cur = dict(inn[s])
            for i in blk:
                uses, defs, kind = self.flag_info(i)
                if uses:
                    d = {}
                    for F in uses:
                        ps = cur[F]
                        if len(ps) == 1 and U not in ps:
                            p = next(iter(ps))
                            if self.can_direct(self.kinds[p], F):
                                d[F] = p
                                self.saved.add(p)
                                continue
                        for p in ps:
                            if p != U:
                                self.explicit.add((p, F))
                    self.direct[i.address] = d
                for F in defs:
                    cur[F] = frozenset([i.address])

    @staticmethod
    def can_direct(kind, F):
        if kind in ('sub', 'add', 'logic', 'neg'):
            return F in 'CZSO'
        if kind in ('inc', 'dec'):
            return F in 'ZSO'
        if kind in ('shl', 'shr', 'sar'):
            return F in 'ZS'
        if kind in ('clc', 'stc'):
            return F == 'C'
        return False

    def fexpr(self, p, F):
        """direct expression for flag F produced by producer p (uses saved operands)."""
        k = self.kinds[p]
        n = self.psize[p]
        a, b, r = 'fa_%x' % p, 'fb_%x' % p, 'fr_%x' % p
        s = ST[n]
        if F == 'Z':
            return '(%s == 0)' % r
        if F == 'S':
            return '((%s)%s < 0)' % (s, r)
        if k == 'sub':
            if F == 'C':
                return '(%s < %s)' % (a, b)
            if F == 'O':
                return '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, b, a, r, SB[n])
        if k == 'add':
            if F == 'C':
                return '(%s < %s)' % (r, a)
            if F == 'O':
                return '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, r, b, r, SB[n])
        if k == 'neg':
            if F == 'C':
                return '(%s != 0)' % b
            if F == 'O':
                return '(%s == %s)' % (b, SB[n])
        if k == 'logic':
            return '0'
        if k == 'inc' and F == 'O':
            return '(%s == %s)' % (r, SB[n])
        if k == 'dec' and F == 'O':
            return '(%s == (%s - 1))' % (r, SB[n])
        if k == 'clc':
            return '0'
        if k == 'stc':
            return '1'
        raise Unsupported('fexpr %s %s' % (k, F))

    def flagval(self, i, F):
        d = self.direct.get(i.address, {})
        if F in d:
            return self.fexpr(d[F], F)
        return {'C': 'cf', 'Z': 'zf', 'S': 'sf', 'O': 'of', 'P': 'pf', 'A': 'af'}[F]

    def cond(self, i, cc):
        cc = norm_cc(cc)
        need = CC_FLAGS[cc]
        d = self.direct.get(i.address, {})
        ps = set(d.get(F) for F in need)
        if len(ps) == 1 and None not in ps:
            p = ps.pop()
            k = self.kinds[p]
            n = self.psize[p]
            a, b, r = 'fa_%x' % p, 'fb_%x' % p, 'fr_%x' % p
            s = ST[n]
            if k == 'sub':
                tbl = {'e': '%s == %s' % (a, b), 'ne': '%s != %s' % (a, b),
                       'b': '%s < %s' % (a, b), 'ae': '%s >= %s' % (a, b),
                       'be': '%s <= %s' % (a, b), 'a': '%s > %s' % (a, b),
                       'l': '(%s)%s < (%s)%s' % (s, a, s, b), 'ge': '(%s)%s >= (%s)%s' % (s, a, s, b),
                       'le': '(%s)%s <= (%s)%s' % (s, a, s, b), 'g': '(%s)%s > (%s)%s' % (s, a, s, b)}
                if cc in tbl:
                    return '(' + tbl[cc] + ')'
            if k == 'logic':
                tbl = {'e': '%s == 0' % r, 'ne': '%s != 0' % r, 's': '(%s)%s < 0' % (s, r),
                       'ns': '(%s)%s >= 0' % (s, r), 'l': '(%s)%s < 0' % (s, r),
                       'ge': '(%s)%s >= 0' % (s, r), 'le': '(%s)%s <= 0' % (s, r),
                       'g': '(%s)%s > 0' % (s, r), 'b': '0', 'ae': '1', 'be': '%s == 0' % r,
                       'a': '%s != 0' % r, 'o': '0', 'no': '1'}
                if cc in tbl:
                    return '(' + tbl[cc] + ')'
        fv = lambda F: self.flagval(i, F)
        t = {
            'o': fv('O'), 'no': '!' + fv('O'), 'b': fv('C'), 'ae': '!' + fv('C'),
            'e': fv('Z'), 'ne': '!' + fv('Z'), 'be': '(%s || %s)' % (fv('C'), fv('Z')),
            'a': '(!%s && !%s)' % (fv('C'), fv('Z')), 's': fv('S'), 'ns': '!' + fv('S'),
            'p': fv('P'), 'np': '!' + fv('P'), 'l': '(%s != %s)' % (fv('S'), fv('O')),
            'ge': '(%s == %s)' % (fv('S'), fv('O')),
            'le': '(%s || (%s != %s))' % (fv('Z'), fv('S'), fv('O')),
            'g': '(!%s && (%s == %s))' % (fv('Z'), fv('S'), fv('O')),
        }
        return '(' + t[cc] + ')'

    # ---------------- operands -----------------
    def addr(self, i, m):
        parts = []
        if m.base != 0:
            parts.append(reg_read(i.reg_name(m.base)))
        if m.index != 0:
            ix = reg_read(i.reg_name(m.index))
            parts.append(ix if m.scale == 1 else '%s*%d' % (ix, m.scale))
        if m.disp or not parts:
            parts.append('0x%xu' % (m.disp & 0xffffffff))
        e = ' + '.join(parts)
        if m.segment != 0:
            sg = i.reg_name(m.segment)
            if sg == 'fs':
                e = 'FS_BASE + ' + e
            elif sg in ('ds', 'ss', 'es', 'cs'):
                pass
            else:
                raise Unsupported('segment ' + sg)
        return '(uint32_t)(%s)' % e

    def rd(self, i, op, size=None):
        n = size or op.size
        if op.type == X.X86_OP_REG:
            return reg_read(i.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return '0x%xu' % (op.imm & ((1 << (8 * n)) - 1))
        if op.type == X.X86_OP_MEM:
            return 'rd%d(%s)' % (8 * n, self.addr(i, op.mem))
        raise Unsupported('op')

    def wr(self, i, op, v, size=None):
        n = size or op.size
        if op.type == X.X86_OP_REG:
            return reg_write(i.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return 'wr%d(%s, %s);' % (8 * n, self.addr(i, op.mem), v)
        raise Unsupported('wr op')

    def wr_addr_cached(self, i, op):
        """for RMW: returns (prelude, readexpr, writefn)"""
        if op.type == X.X86_OP_MEM:
            pre = 'uint32_t ea = %s;' % self.addr(i, op.mem)
            n = op.size
            return pre, 'rd%d(ea)' % (8 * n), lambda v: 'wr%d(ea, %s);' % (8 * n, v)
        return '', self.rd(i, op), lambda v: self.wr(i, op, v)

    # ---------------- flags emission -----------------
    def emit_flags(self, i, kind, n, a, b, r, extra=None):
        """a,b,r are C variable names (already masked to n bytes). returns lines."""
        p = i.address
        L = []
        if p in self.saved:
            L.append('fa_%x = %s; fb_%x = %s; fr_%x = %s;' % (p, a, p, b, p, r))
        ex = set(F for (q, F) in self.explicit if q == p)
        if not ex:
            return L
        sb = SB[n]
        s = ST[n]
        for F in sorted(ex):
            if F == 'Z':
                L.append('zf = (%s == 0);' % r)
            elif F == 'S':
                L.append('sf = ((%s)%s < 0);' % (s, r))
            elif F == 'P':
                L.append('pf = parity8((uint8_t)%s);' % r)
            elif F == 'A':
                L.append('af = (((%s ^ %s ^ %s) & 0x10) != 0);' % (a, b, r))
            elif F == 'C':
                L.append('cf = %s;' % {
                    'add': '(%s < %s)' % (r, a),
                    'adc': '(cin ? (%s <= %s) : (%s < %s))' % (r, a, r, a),
                    'sub': '(%s < %s)' % (a, b),
                    'sbb': '(cin ? (%s <= %s) : (%s < %s))' % (a, b, a, b),
                    'logic': '0', 'neg': '(%s != 0)' % b,
                }.get(kind, extra.get('C') if extra else None))
            elif F == 'O':
                L.append('of = %s;' % {
                    'add': '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, r, b, r, sb),
                    'adc': '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, r, b, r, sb),
                    'sub': '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, b, a, r, sb),
                    'sbb': '((((%s ^ %s) & (%s ^ %s)) & %s) != 0)' % (a, b, a, r, sb),
                    'logic': '0', 'neg': '(%s == %s)' % (b, sb),
                    'inc': '(%s == %s)' % (r, sb), 'dec': '(%s == (%s - 1))' % (r, sb),
                }.get(kind, extra.get('O') if extra else None))
        for l in L:
            if 'None' in l:
                raise Unsupported('flag expr %s' % kind)
        return L

    # ---------------- instruction emission -----------------
    def sync_out(self):
        return 'SYNC_OUT;'

    def sync_in(self):
        return 'SYNC_IN;'

    def emit_call_target(self, t, ret):
        rc = self.rc
        L = []
        L.append('PUSH32(0x%xu);' % ret)
        L.append('SYNC_OUT;')
        if t in rc.funcs:
            L.append('%s_f_%x();' % (rc.prefix, t))
        else:
            L.append('call_ind(0x%xu);' % t)
        L.append('SYNC_IN;')
        return L

    def emit_insn(self, i, blk_next):
        m = i.mnemonic
        ops = i.operands
        L = []
        nxt = i.address + i.size
        parts = m.split()
        prefix = parts[0] if len(parts) > 1 else None
        base = parts[-1]

        # x87
        b0 = i.bytes[0]
        raw = bytes(i.bytes)
        k = 0
        while raw[k] in (0x9b, 0x66, 0x2e, 0x3e, 0x26, 0x36, 0x64, 0x65, 0xf2, 0xf3) and k < len(raw) - 1:
            k += 1
        if 0xd8 <= raw[k] <= 0xdf:
            return self.emit_fpu(i, raw[k:])
        if base in ('wait', 'nop', 'fwait'):
            return []

        if base == 'mov':
            return [self.wr(i, ops[0], self.rd(i, ops[1]))]
        if base == 'movzx':
            return [self.wr(i, ops[0], '(uint32_t)' + self.rd(i, ops[1]))]
        if base == 'movsx':
            return [self.wr(i, ops[0], '(uint32_t)(int32_t)(%s)%s' % (ST[ops[1].size], self.rd(i, ops[1])))]
        if base == 'lea':
            return [self.wr(i, ops[0], self.addr(i, ops[1].mem))]
        if base == 'push':
            v = self.rd(i, ops[0])
            if ops[0].type == X.X86_OP_IMM:
                v = '0x%xu' % (ops[0].imm & 0xffffffff)
            if ops[0].size == 2 and ops[0].type != X.X86_OP_IMM:
                return ['{ uint16_t t = %s; esp -= 2; wr16(esp, t); }' % v]
            return ['{ uint32_t t = %s; PUSH32(t); }' % v]
        if base == 'pop':
            if ops[0].size == 2:
                return ['{ uint16_t t = rd16(esp); esp += 2; %s }' % self.wr(i, ops[0], 't')]
            if ops[0].type == X.X86_OP_MEM:
                # address computed after esp increment (x86 semantics)
                return ['{ uint32_t t = rd32(esp); esp += 4; %s }' % self.wr(i, ops[0], 't')]
            return ['{ uint32_t t = rd32(esp); esp += 4; %s }' % self.wr(i, ops[0], 't')]
        if base == 'xchg':
            if ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return []
            pre, r0, w0 = self.wr_addr_cached(i, ops[0])
            return ['{ %s %s t0 = %s; %s t1 = %s; %s %s }' % (pre, CT[ops[0].size], r0, CT[ops[1].size],
                                                              self.rd(i, ops[1]), w0('t1'), self.wr(i, ops[1], 't0'))]
        if base == 'cdq':
            return ['edx = (uint32_t)((int32_t)eax >> 31);']
        if base == 'cwde':
            return ['eax = (uint32_t)(int32_t)(int16_t)eax;']
        if base == 'cbw':
            return [reg_write('ax', '(uint16_t)(int16_t)(int8_t)eax')]
        if base == 'cwd':
            return [reg_write('dx', '((int16_t)eax < 0) ? 0xffff : 0')]
        if base == 'bswap':
            r = i.reg_name(ops[0].reg)
            return ['%s = __builtin_bswap32(%s);' % (r, r)]
        if base == 'leave':
            return ['esp = ebp; ebp = rd32(esp); esp += 4;']
        if base == 'enter':
            sz = ops[0].imm
            if ops[1].imm != 0:
                raise Unsupported('enter nesting')
            return ['PUSH32(ebp); ebp = esp; esp -= %du;' % sz]
        if base == 'pushal':
            return ['{ uint32_t t = esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); PUSH32(t); PUSH32(ebp); PUSH32(esi); PUSH32(edi); }']
        if base == 'popal':
            return ['edi = rd32(esp); esi = rd32(esp+4); ebp = rd32(esp+8); ebx = rd32(esp+16); edx = rd32(esp+20); ecx = rd32(esp+24); eax = rd32(esp+28); esp += 32;']
        if base == 'pushfd':
            fv = lambda F: self.flagval(i, F)
            return ['{ uint32_t t = 0x202u | (%s ? 1u : 0) | (%s ? 4u : 0) | (%s ? 0x10u : 0) | (%s ? 0x40u : 0) | (%s ? 0x80u : 0) | (%s ? 0x800u : 0) | (R.df ? 0x400u : 0); PUSH32(t); }'
                    % (fv('C'), fv('P'), fv('A'), fv('Z'), fv('S'), fv('O'))]
        if base == 'popfd':
            L = ['{ uint32_t t = rd32(esp); esp += 4; R.df = (t >> 10) & 1;']
            ex = set(F for (q, F) in self.explicit if q == i.address)
            bits = {'C': 0, 'P': 2, 'A': 4, 'Z': 6, 'S': 7, 'O': 11}
            for F in ex:
                L.append('%sf = (t >> %d) & 1;' % (F.lower(), bits[F]))
            L.append('}')
            return [' '.join(L)]
        if base == 'lahf':
            fv = lambda F: self.flagval(i, F)
            return [reg_write('ah', '2u | (%s ? 1u : 0) | (%s ? 4u : 0) | (%s ? 0x10u : 0) | (%s ? 0x40u : 0) | (%s ? 0x80u : 0)'
                              % (fv('C'), fv('P'), fv('A'), fv('Z'), fv('S')))]
        if base == 'sahf':
            ex = set(F for (q, F) in self.explicit if q == i.address)
            bits = {'C': 0, 'P': 2, 'A': 4, 'Z': 6, 'S': 7}
            return ['{ uint32_t t = (eax >> 8) & 0xff; %s }' % ' '.join('%sf = (t >> %d) & 1;' % (F.lower(), bits[F]) for F in sorted(ex))]
        if base in ('cld', 'std'):
            return ['R.df = %d;' % (base == 'std')]
        if base in ('clc', 'stc'):
            return ['cf = %d;' % (base == 'stc')] if (i.address, 'C') in self.explicit else []
        if base == 'cmc':
            if (i.address, 'C') in self.explicit:
                return ['cf = !%s;' % self.flagval(i, 'C')]
            return []
        if base.startswith('set'):
            return [self.wr(i, ops[0], self.cond(i, base[3:]) + ' ? 1 : 0')]
        if base.startswith('cmov'):
            return ['if (%s) { %s }' % (self.cond(i, base[4:]), self.wr(i, ops[0], self.rd(i, ops[1])))]

        # ALU
        if base in ('add', 'sub', 'cmp', 'and', 'or', 'xor', 'test', 'adc', 'sbb'):
            n = ops[0].size
            t = CT[n]
            pre, r0, w0 = self.wr_addr_cached(i, ops[0])
            bexpr = self.rd(i, ops[1], n)
            if ops[1].type == X.X86_OP_IMM:
                bexpr = '(%s)0x%xu' % (t, ops[1].imm & ((1 << (8 * n)) - 1))
            if base == 'xor' and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                L = ['{ %s a = 0, b = 0, r = 0;' % t]
                L += self.emit_flags(i, 'logic', n, 'a', 'b', 'r')
                L.append(w0('0') + ' }')
                return [' '.join(L)]
            op = {'add': '+', 'sub': '-', 'cmp': '-', 'and': '&', 'or': '|', 'xor': '^', 'test': '&', 'adc': '+', 'sbb': '-'}[base]
            kind = {'add': 'add', 'sub': 'sub', 'cmp': 'sub', 'and': 'logic', 'or': 'logic', 'xor': 'logic', 'test': 'logic', 'adc': 'adc', 'sbb': 'sbb'}[base]
            L = ['{', pre, '%s a = %s; %s b = %s;' % (t, r0, t, bexpr)]
            if base in ('adc', 'sbb'):
                L.append('uint32_t cin = %s ? 1 : 0;' % self.flagval(i, 'C'))
                L.append('%s r = (%s)(a %s b %s cin);' % (t, t, op, op))
            else:
                L.append('%s r = (%s)(a %s b);' % (t, t, op))
            L += self.emit_flags(i, kind, n, 'a', 'b', 'r')
            if base not in ('cmp', 'test'):
                L.append(w0('r'))
            L.append('}')
            return [' '.join(x for x in L if x)]
        if base in ('inc', 'dec', 'neg', 'not'):
            n = ops[0].size
            t = CT[n]
            pre, r0, w0 = self.wr_addr_cached(i, ops[0])
            if base == 'not':
                return ['{ %s %s }' % (pre, w0('(%s)~%s' % (t, r0)))]
            if base == 'neg':
                L = ['{', pre, '%s a = 0; %s b = %s; %s r = (%s)(0 - b);' % (t, t, r0, t, t)]
                L += self.emit_flags(i, 'neg', n, 'a', 'b', 'r')
            else:
                L = ['{', pre, '%s a = %s; %s b = 1; %s r = (%s)(a %s 1);' % (t, r0, t, t, t, '+' if base == 'inc' else '-')]
                L += self.emit_flags(i, base, n, 'a', 'b', 'r')
            L.append(w0('r'))
            L.append('}')
            return [' '.join(x for x in L if x)]
        if base in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr'):
            n = ops[0].size
            t = CT[n]
            bits = 8 * n
            pre, r0, w0 = self.wr_addr_cached(i, ops[0])
            if len(ops) == 1:
                cnt = '1'
            else:
                cnt = '(%s & 31)' % self.rd(i, ops[1])
            kind = 'shl' if base == 'sal' else base
            L = ['{', pre, '%s a = %s; uint32_t c = %s;' % (t, r0, cnt), 'if (c) {']
            ex = set(F for (q, F) in self.explicit if q == i.address)
            if kind == 'shl':
                L.append('%s r = (%s)((uint32_t)a << c);' % (t, t))
                cf = '((c <= %d) ? (((uint32_t)a >> (%d - c)) & 1) : 0)' % (bits, bits)
                of = '((((uint32_t)r >> %d) & 1) ^ cf)' % (bits - 1)
            elif kind == 'shr':
                L.append('%s r = (%s)((c >= %d) ? 0 : ((uint32_t)a >> c));' % (t, t, bits))
                cf = '((c <= %d) ? (((uint32_t)a >> (c - 1)) & 1) : 0)' % bits
                of = '(((uint32_t)a >> %d) & 1)' % (bits - 1)
            elif kind == 'sar':
                L.append('%s r = (%s)((int32_t)(%s)a >> (c >= %d ? %d : c));' % (t, t, ST[n], bits, bits - 1))
                cf = '(((int32_t)(%s)a >> ((c - 1) >= %d ? %d : (c - 1))) & 1)' % (ST[n], bits, bits - 1)
                of = '0'
            elif kind == 'rol':
                L.append('uint32_t cc = c %% %d; %s r = (%s)(cc ? (((uint32_t)a << cc) | ((uint32_t)a >> (%d - cc))) : a);' % (bits, t, t, bits))
                cf = '(r & 1)'
                of = '((((uint32_t)r >> %d) & 1) ^ (r & 1))' % (bits - 1)
            elif kind == 'ror':
                L.append('uint32_t cc = c %% %d; %s r = (%s)(cc ? (((uint32_t)a >> cc) | ((uint32_t)a << (%d - cc))) : a);' % (bits, t, t, bits))
                cf = '(((uint32_t)r >> %d) & 1)' % (bits - 1)
                of = '((((uint32_t)r >> %d) ^ ((uint32_t)r >> %d)) & 1)' % (bits - 1, bits - 2)
            elif kind in ('rcl', 'rcr'):
                L.append('uint64_t w = (uint64_t)a | ((uint64_t)(%s ? 1 : 0) << %d); uint32_t cc = c %% %d;' % (self.flagval(i, 'C'), bits, bits + 1))
                if kind == 'rcl':
                    L.append('w = cc ? ((w << cc) | (w >> (%d - cc))) : w;' % (bits + 1))
                else:
                    L.append('w = cc ? ((w >> cc) | (w << (%d - cc))) : w;' % (bits + 1))
                L.append('w &= ((uint64_t)1 << %d) - 1; %s r = (%s)w;' % (bits + 1, t, t))
                cf = '((uint32_t)(w >> %d) & 1)' % bits
                of = '((((uint32_t)r >> %d) & 1) ^ cf)' % (bits - 1) if kind == 'rcl' else '((((uint32_t)r >> %d) ^ ((uint32_t)r >> %d)) & 1)' % (bits - 1, bits - 2)
            if 'C' in ex or 'O' in ex:
                L.append('cf = %s;' % cf)
            if 'O' in ex:
                L.append('of = %s;' % of)
            if 'Z' in ex:
                L.append('zf = (r == 0);')
            if 'S' in ex:
                L.append('sf = ((%s)r < 0);' % ST[n])
            if 'P' in ex:
                L.append('pf = parity8((uint8_t)r);')
            if 'A' in ex:
                L.append('af = 0;')
            if i.address in self.saved:
                L.append('fa_%x = a; fb_%x = 0; fr_%x = r;' % (i.address, i.address, i.address))
            L.append(w0('r'))
            L.append('}}')
            return [' '.join(x for x in L if x)]
        if base in ('shld', 'shrd'):
            n = ops[0].size
            if n != 4:
                raise Unsupported('shld16')
            pre, r0, w0 = self.wr_addr_cached(i, ops[0])
            src = self.rd(i, ops[1])
            cnt = '(%s & 31)' % self.rd(i, ops[2])
            ex = set(F for (q, F) in self.explicit if q == i.address)
            L = ['{', pre, 'uint32_t a = %s; uint32_t s = %s; uint32_t c = %s; if (c) {' % (r0, src, cnt)]
            if base == 'shld':
                L.append('uint32_t r = (a << c) | (s >> (32 - c)); cf = (a >> (32 - c)) & 1;')
            else:
                L.append('uint32_t r = (a >> c) | (s << (32 - c)); cf = (a >> (c - 1)) & 1;')
            if 'Z' in ex: L.append('zf = (r == 0);')
            if 'S' in ex: L.append('sf = ((int32_t)r < 0);')
            if 'O' in ex: L.append('of = 0;')
            if 'P' in ex: L.append('pf = parity8((uint8_t)r);')
            L.append(w0('r'))
            L.append('}}')
            return [' '.join(x for x in L if x)]
        if base in ('mul', 'imul') and len(ops) == 1:
            n = ops[0].size
            v = self.rd(i, ops[0])
            ex = set(F for (q, F) in self.explicit if q == i.address)
            if n == 4:
                if base == 'mul':
                    L = ['{ uint64_t r = (uint64_t)eax * (uint64_t)%s; eax = (uint32_t)r; edx = (uint32_t)(r >> 32); int ov = edx != 0;' % v]
                else:
                    L = ['{ int64_t r = (int64_t)(int32_t)eax * (int64_t)(int32_t)%s; eax = (uint32_t)r; edx = (uint32_t)((uint64_t)r >> 32); int ov = r != (int64_t)(int32_t)r;' % v]
            elif n == 2:
                if base == 'mul':
                    L = ['{ uint32_t r = (uint32_t)(uint16_t)eax * (uint32_t)%s; %s %s int ov = (r >> 16) != 0;' % (v, reg_write('ax', 'r'), reg_write('dx', 'r >> 16'))]
                else:
                    L = ['{ int32_t r = (int32_t)(int16_t)eax * (int32_t)(int16_t)%s; %s %s int ov = r != (int16_t)r;' % (v, reg_write('ax', 'r'), reg_write('dx', '(uint32_t)r >> 16'))]
            else:
                if base == 'mul':
                    L = ['{ uint32_t r = (uint32_t)(uint8_t)eax * (uint32_t)%s; %s int ov = (r >> 8) != 0;' % (v, reg_write('ax', 'r'))]
                else:
                    L = ['{ int32_t r = (int32_t)(int8_t)eax * (int32_t)(int8_t)%s; %s int ov = r != (int8_t)r;' % (v, reg_write('ax', 'r'))]
            for F in ex:
                L.append({'C': 'cf = ov;', 'O': 'of = ov;', 'Z': 'zf = (eax == 0);', 'S': 'sf = ((int32_t)eax < 0);',
                          'P': 'pf = parity8((uint8_t)eax);', 'A': 'af = 0;'}[F])
            L.append('}')
            return [' '.join(L)]
        if base == 'imul':
            n = ops[0].size
            if len(ops) == 2:
                a, b = self.rd(i, ops[0]), self.rd(i, ops[1])
            else:
                a, b = self.rd(i, ops[1]), '0x%xu' % (ops[2].imm & 0xffffffff)
            ex = set(F for (q, F) in self.explicit if q == i.address)
            L = ['{ int64_t r = (int64_t)(%s)%s * (int64_t)(%s)%s; int ov = r != (int64_t)(%s)r;' % (ST[n], a, ST[n], b, ST[n])]
            L.append(self.wr(i, ops[0], '(%s)r' % CT[n]))
            for F in ex:
                L.append({'C': 'cf = ov;', 'O': 'of = ov;', 'Z': 'zf = ((%s)r == 0);' % CT[n], 'S': 'sf = ((%s)r < 0);' % ST[n],
                          'P': 'pf = parity8((uint8_t)r);', 'A': 'af = 0;'}[F])
            L.append('}')
            return [' '.join(L)]
        if base in ('div', 'idiv'):
            n = ops[0].size
            v = self.rd(i, ops[0])
            if n == 4:
                if base == 'div':
                    return ['{ uint32_t d = %s; uint64_t x = ((uint64_t)edx << 32) | eax; if (!d) guest_fault(0x%x, "div0"); eax = (uint32_t)(x / d); edx = (uint32_t)(x %% d); }' % (v, i.address)]
                return ['{ int32_t d = (int32_t)%s; int64_t x = (int64_t)(((uint64_t)edx << 32) | eax); if (!d) guest_fault(0x%x, "div0"); eax = (uint32_t)(int32_t)(x / d); edx = (uint32_t)(int32_t)(x %% d); }' % (v, i.address)]
            if n == 2:
                if base == 'div':
                    return ['{ uint32_t d = %s; uint32_t x = ((edx & 0xffff) << 16) | (eax & 0xffff); if (!d) guest_fault(0x%x, "div0"); %s %s }' % (v, i.address, reg_write('ax', 'x / d'), reg_write('dx', 'x % d'))]
                return ['{ int32_t d = (int16_t)%s; int32_t x = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); if (!d) guest_fault(0x%x, "div0"); %s %s }' % (v, i.address, reg_write('ax', 'x / d'), reg_write('dx', 'x % d'))]
            if base == 'div':
                return ['{ uint32_t d = %s; uint32_t x = eax & 0xffff; if (!d) guest_fault(0x%x, "div0"); %s %s }' % (v, i.address, reg_write('al', 'x / d'), reg_write('ah', 'x % d'))]
            return ['{ int32_t d = (int8_t)%s; int32_t x = (int16_t)eax; if (!d) guest_fault(0x%x, "div0"); %s %s }' % (v, i.address, reg_write('al', 'x / d'), reg_write('ah', 'x % d'))]
        if base in ('bt', 'bts', 'btr', 'btc'):
            if ops[0].type != X.X86_OP_REG:
                raise Unsupported('bt mem')
            r = self.rd(i, ops[0])
            L = ['{ uint32_t bit = %s & 31; cf = (%s >> bit) & 1;' % (self.rd(i, ops[1]), r)]
            if base == 'bts': L.append(self.wr(i, ops[0], '%s | (1u << bit)' % r))
            if base == 'btr': L.append(self.wr(i, ops[0], '%s & ~(1u << bit)' % r))
            if base == 'btc': L.append(self.wr(i, ops[0], '%s ^ (1u << bit)' % r))
            L.append('}')
            return [' '.join(L)]
        if base in ('bsf', 'bsr'):
            v = self.rd(i, ops[1])
            fn = '__builtin_ctz(t)' if base == 'bsf' else '(31 - __builtin_clz(t))'
            return ['{ uint32_t t = %s; zf = (t == 0); if (t) { %s } }' % (v, self.wr(i, ops[0], fn))]

        # string ops
        if base[:-1] in ('movs', 'stos', 'lods', 'scas', 'cmps') and base[-1] in 'bwd' and (not ops or ops[0].type == X.X86_OP_MEM or base[:-1] in ('stos', 'lods', 'scas')):
            sz = {'b': 1, 'w': 2, 'd': 4}[base[-1]]
            kind = base[:-1]
            rep = {'rep': 1, 'repe': 1, 'repz': 1, 'repne': 2, 'repnz': 2}.get(prefix, 0)
            if kind in ('movs', 'stos', 'lods'):
                return ['SYNC_OUT; str_%s%d(%d); SYNC_IN;' % (kind, sz, 1 if rep else 0)]
            # scas / cmps: result in R via helper, returns last a,b
            ex = set(F for (q, F) in self.explicit if q == i.address)
            t = CT[sz]
            L = ['{ uint32_t sa, sb; SYNC_OUT; int ran = str_%s%d(%d, &sa, &sb); SYNC_IN;' % (kind, sz, rep)]
            L.append('if (ran) { %s a = (%s)sa, b = (%s)sb, r = (%s)(a - b);' % (t, t, t, t))
            L += self.emit_flags(i, 'sub', sz, 'a', 'b', 'r')
            L.append('} else { %s }' % ' '.join({'Z': 'zf = 1;', 'C': 'cf = 0;', 'S': 'sf = 0;', 'O': 'of = 0;', 'P': 'pf = 1;', 'A': 'af = 0;'}[F] for F in ex))
            L.append('}')
            if i.address in self.saved:
                raise Unsupported('strcmp saved')
            return [' '.join(L)]

        raise Unsupported(m + ' ' + i.op_str)

    # ---------------- FPU -----------------
    def emit_fpu(self, i, raw):
        op = raw[0]
        modrm = raw[1]
        mod = modrm >> 6
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        a = None
        if mod != 3:
            mm = mem_op(i)
            if mm is None:
                raise Unsupported('fpu mem')
            a = self.addr(i, mm)
        ST0 = 'FST(0)'
        STI = 'FST(%d)' % rm
        ar = ['+', '*', None, None, '-', '-r', '/', '/r']

        def arith(dst, x, y, o):
            # dst = x o y
            if o == '-r':
                return '%s = %s - %s;' % (dst, y, x)
            if o == '/r':
                return '%s = %s / %s;' % (dst, y, x)
            return '%s = %s %s %s;' % (dst, x, o, y)

        if op in (0xd8, 0xdc, 0xda, 0xde) and mod != 3:
            ld = {0xd8: 'rdf32(%s)', 0xdc: 'rdf64(%s)', 0xda: '(double)(int32_t)rd32(%s)', 0xde: '(double)(int16_t)rd16(%s)'}[op] % a
            if reg in (2, 3):
                return ['fpu_com(%s, %s); %s' % (ST0, ld, 'FPOP();' if reg == 3 else '')]
            return [arith(ST0, ST0, ld, ar[reg])]
        if op == 0xd8:
            if reg in (2, 3):
                return ['fpu_com(%s, %s); %s' % (ST0, STI, 'FPOP();' if reg == 3 else '')]
            return [arith(ST0, ST0, STI, ar[reg])]
        if op in (0xdc, 0xde):
            if op == 0xde and modrm == 0xd9:
                return ['fpu_com(FST(0), FST(1)); FPOP(); FPOP();']
            if reg in (2, 3):
                return ['fpu_com(%s, %s); %s' % (ST0, STI, 'FPOP();' if (reg == 3 or op == 0xde) else '')]
            o = {0: '+', 1: '*', 4: '-r', 5: '-', 6: '/r', 7: '/'}[reg]
            # dst = st(i) ; for reg4 (fsubr st(i),st0): st(i) = st0 - st(i)  => arith(sti, sti, st0, '-r')
            L = [arith(STI, STI, ST0, o)]
            if op == 0xde:
                L.append('FPOP();')
            return [' '.join(L)]
        if op == 0xd9:
            if mod != 3:
                if reg == 0: return ['FPUSH(rdf32(%s));' % a]
                if reg == 2: return ['wrf32(%s, FST(0));' % a]
                if reg == 3: return ['wrf32(%s, FST(0)); FPOP();' % a]
                if reg == 5: return ['F.cw = rd16(%s);' % a]
                if reg == 7: return ['wr16(%s, F.cw);' % a]
                if reg == 4: return ['fpu_ldenv(%s);' % a]
                if reg == 6: return ['fpu_stenv(%s);' % a]
                raise Unsupported('d9 mem %d' % reg)
            if reg == 0: return ['{ double t = FST(%d); FPUSH(t); }' % rm]
            if reg == 1: return ['{ double t = FST(0); FST(0) = FST(%d); FST(%d) = t; }' % (rm, rm)]
            tbl = {0xd0: '', 0xe0: 'FST(0) = -FST(0);', 0xe1: 'FST(0) = fabs(FST(0));', 0xe4: 'fpu_com(FST(0), 0.0);',
                   0xe5: 'fpu_xam();', 0xe8: 'FPUSH(1.0);', 0xe9: 'FPUSH(3.321928094887362347870319429489390175864831393);',
                   0xea: 'FPUSH(1.442695040888963407359924681001892137426645954);', 0xeb: 'FPUSH(3.141592653589793238462643383279502884197169399);',
                   0xec: 'FPUSH(0.301029995663981195213738894724493026768189881);', 0xed: 'FPUSH(0.693147180559945309417232121458176568075500134);',
                   0xee: 'FPUSH(0.0);', 0xf0: 'FST(0) = exp2(FST(0)) - 1.0;', 0xf1: 'FST(1) = FST(1) * log2(FST(0)); FPOP();',
                   0xf2: 'FST(0) = tan(FST(0)); FPUSH(1.0); F.sw &= ~0x400;', 0xf3: 'FST(1) = atan2(FST(1), FST(0)); FPOP();',
                   0xf5: 'FST(0) = remainder(FST(0), FST(1)); F.sw &= ~0x400;', 0xf6: 'F.top = (F.top - 1) & 7;', 0xf7: 'F.top = (F.top + 1) & 7;',
                   0xf8: 'FST(0) = fmod(FST(0), FST(1)); F.sw &= ~0x400;', 0xf9: 'FST(1) = FST(1) * log2(FST(0) + 1.0); FPOP();',
                   0xfa: 'FST(0) = sqrt(FST(0));', 0xfb: '{ double t = FST(0); FST(0) = sin(t); FPUSH(cos(t)); F.sw &= ~0x400; }',
                   0xfc: 'FST(0) = fpu_round(FST(0));', 0xfd: 'FST(0) = ldexp(FST(0), (int)trunc(FST(1)));',
                   0xfe: 'FST(0) = sin(FST(0)); F.sw &= ~0x400;', 0xff: 'FST(0) = cos(FST(0)); F.sw &= ~0x400;',
                   0xf4: '{ double t = FST(0); int e; double m = frexp(t, &e); FST(0) = (double)(e - 1); FPUSH(m * 2.0); }'}
            if modrm in tbl:
                return [tbl[modrm]]
            raise Unsupported('d9 %02x' % modrm)
        if op == 0xda:
            if modrm == 0xe9:
                return ['fpu_com(FST(0), FST(1)); FPOP(); FPOP();']
            raise Unsupported('da %02x' % modrm)
        if op == 0xdb:
            if mod != 3:
                if reg == 0: return ['FPUSH((double)(int32_t)rd32(%s));' % a]
                if reg == 2: return ['wr32(%s, (uint32_t)fpu_toint32(FST(0)));' % a]
                if reg == 3: return ['wr32(%s, (uint32_t)fpu_toint32(FST(0))); FPOP();' % a]
                if reg == 1: return ['wr32(%s, (uint32_t)fpu_trunc32(FST(0))); FPOP();' % a]
                if reg == 5: return ['FPUSH(rdf80(%s));' % a]
                if reg == 7: return ['wrf80(%s, FST(0)); FPOP();' % a]
                raise Unsupported('db mem')
            if modrm == 0xe2: return ['F.sw &= 0x7f00;']
            if modrm == 0xe3: return ['fpu_init();']
            if modrm in (0xe0, 0xe1, 0xe4): return []
            raise Unsupported('db %02x' % modrm)
        if op == 0xdd:
            if mod != 3:
                if reg == 0: return ['FPUSH(rdf64(%s));' % a]
                if reg == 2: return ['wrf64(%s, FST(0));' % a]
                if reg == 3: return ['wrf64(%s, FST(0)); FPOP();' % a]
                if reg == 7: return ['wr16(%s, fpu_sw());' % a]
                if reg == 4: return ['fpu_frstor(%s);' % a]
                if reg == 6: return ['fpu_fsave(%s);' % a]
                if reg == 1: return ['wr64(%s, (uint64_t)(int64_t)trunc(FST(0))); FPOP();' % a]
                raise Unsupported('dd mem')
            if reg == 0: return []  # ffree
            if reg == 2: return ['FST(%d) = FST(0);' % rm]
            if reg == 3: return ['FST(%d) = FST(0); FPOP();' % rm]
            if reg == 4: return ['fpu_com(FST(0), FST(%d));' % rm]
            if reg == 5: return ['fpu_com(FST(0), FST(%d)); FPOP();' % rm]
            raise Unsupported('dd %02x' % modrm)
        if op == 0xdf:
            if mod != 3:
                if reg == 0: return ['FPUSH((double)(int16_t)rd16(%s));' % a]
                if reg == 2: return ['wr16(%s, (uint16_t)fpu_toint16(FST(0)));' % a]
                if reg == 3: return ['wr16(%s, (uint16_t)fpu_toint16(FST(0))); FPOP();' % a]
                if reg == 5: return ['FPUSH((double)(int64_t)rd64(%s));' % a]
                if reg == 7: return ['wr64(%s, (uint64_t)fpu_toint64(FST(0))); FPOP();' % a]
                raise Unsupported('df mem %d' % reg)
            if modrm == 0xe0:
                return [reg_write('ax', 'fpu_sw()')]
            raise Unsupported('df %02x' % modrm)
        raise Unsupported('fpu %02x %02x' % (op, modrm))

    # ---------------- function -----------------
    def gen(self):
        f = self.f
        rc = self.rc
        self.psize = {}
        for blk in f.blocks.values():
            for i in blk:
                self.insns[i.address] = i
                n = 4
                ops = i.operands
                if ops:
                    n = ops[0].size or 4
                b = i.mnemonic.split()[-1]
                if b[:-1] in ('scas', 'cmps'):
                    n = {'b': 1, 'w': 2, 'd': 4}[b[-1]]
                if n not in (1, 2, 4):
                    n = 4
                self.psize[i.address] = n
        self.analyze_flags()
        name = '%s_f_%x' % (rc.prefix, f.entry)
        body = []
        blocks = sorted(f.blocks)
        if blocks[0] != f.entry:
            body.append('goto L_%x;' % f.entry)
        for bi, s in enumerate(blocks):
            blk = f.blocks[s]
            body.append('L_%x:' % s)
            for i in blk:
                body.append('/* %08x: %s %s */' % (i.address, i.mnemonic, i.op_str))
                try:
                    body.extend(self.emit_ctl(i, f, blocks, bi))
                except Unsupported as e:
                    self.unsupported.append((i.address, str(e)))
                    body.append('guest_fault(0x%xu, "unsupported: %s");' % (i.address, str(e).replace('"', "'")))
            # fallthrough
            last = blk[-1]
            nxt = last.address + last.size
            lm = last.mnemonic
            if lm in ('ret', 'retf', 'jmp', 'hlt', 'int3', 'ud2', 'int') or lm.startswith('ret'):
                continue
            if bi + 1 < len(blocks) and blocks[bi + 1] == nxt:
                continue
            if nxt in f.blocks:
                body.append('goto L_%x;' % nxt)
            else:
                # falls off: treat as call of following code (e.g., after noreturn call)
                if nxt in rc.funcs:
                    body.append('%s_f_%x(); return;' % (rc.prefix, nxt))
                else:
                    body.append('guest_fault(0x%xu, "fell off function"); return;' % nxt)
        decl = ['void %s(void) {' % name,
                '  uint8_t *const M = MEM;',
                '  uint32_t eax = R.eax, ecx = R.ecx, edx = R.edx, ebx = R.ebx, esp = R.esp, ebp = R.ebp, esi = R.esi, edi = R.edi;',
                '  bool cf = 0, zf = 0, sf = 0, of = 0, pf = 0, af = 0; (void)cf; (void)zf; (void)sf; (void)of; (void)pf; (void)af;']
        for p in sorted(self.saved):
            t = CT[self.psize[p]]
            decl.append('  %s fa_%x = 0, fb_%x = 0, fr_%x = 0;' % (t, p, p, p))
        out = decl + ['  ' + l for l in body] + ['}', '']
        return name, '\n'.join(out)

    def spin_check(self, i, t):
        """backward branch to t forming a busy-wait loop (only memory loads,
        compares and branches) -> poll so timer callbacks can run"""
        if t > i.address:
            return ''
        f = self.f
        n = 0
        for s, blk in f.blocks.items():
            if s < t or s > i.address:
                continue
            for x in blk:
                n += 1
                b = x.mnemonic.split()[-1]
                ops = x.operands
                if b in ('cmp', 'test') or b.startswith('j') or b == 'nop':
                    continue
                if b in ('mov', 'movzx', 'movsx') and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_MEM:
                    continue
                return ''
        if n > 16:
            return ''
        return 'SYNC_OUT; rt_spin_poll(); '

    def emit_ctl(self, i, f, blocks, bi):
        rc = self.rc
        m = i.mnemonic
        base = m.split()[-1]
        nxt = i.address + i.size
        ops = i.operands
        if base == 'call':
            o = ops[0]
            if o.type == X.X86_OP_IMM:
                return self.emit_call_target(o.imm, nxt)
            if o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 and (o.mem.disp & 0xffffffff) in rc.img.imports:
                dll, nm = rc.img.imports[o.mem.disp & 0xffffffff]
                return ['PUSH32(0x%xu); SYNC_OUT; %s(); SYNC_IN;' % (nxt, rc.shim_name(dll, nm))]
            return ['{ uint32_t t = %s; PUSH32(0x%xu); SYNC_OUT; call_ind(t); SYNC_IN; }' % (self.rd(i, o), nxt)]
        if base in ('ret', 'retn'):
            n = ops[0].imm if ops else 0
            return ['esp += %d; SYNC_OUT; return;' % (4 + n)]
        if base == 'jmp':
            o = ops[0]
            if o.type == X.X86_OP_IMM:
                t = o.imm
                if t in f.blocks and not (t in rc.funcs and t != f.entry):
                    return [self.spin_check(i, t) + 'goto L_%x;' % t]
                if t in rc.funcs:
                    return ['SYNC_OUT; %s_f_%x(); return;' % (rc.prefix, t)]
                return ['SYNC_OUT; call_ind(0x%xu); return;' % t]
            if i.address in f.jtables:
                tg, table, n = f.jtables[i.address]
                L = ['{ uint32_t t = %s; switch (t) {' % self.rd(i, o)]
                for t in sorted(set(tg)):
                    L.append('case 0x%xu: goto L_%x;' % (t, t))
                L.append('default: SYNC_OUT; call_ind(t); return; } }')
                return [' '.join(L)]
            if o.type == X.X86_OP_MEM and o.mem.base == 0 and o.mem.index == 0 and (o.mem.disp & 0xffffffff) in rc.img.imports:
                dll, nm = rc.img.imports[o.mem.disp & 0xffffffff]
                return ['SYNC_OUT; %s(); return;' % rc.shim_name(dll, nm)]
            return ['{ uint32_t t = %s; SYNC_OUT; call_ind(t); return; }' % self.rd(i, o)]
        if is_jcc(i):
            t = ops[0].imm
            tgt = 'goto L_%x;' % t if t in f.blocks else 'guest_fault(0x%xu, "jcc out");' % t
            if base in ('jecxz', 'jcxz'):
                return ['if (ecx == 0) %s' % tgt]
            if base == 'loop':
                return ['ecx--; if (ecx != 0) %s' % tgt]
            if base in ('loope', 'loopz'):
                return ['ecx--; if (ecx != 0 && %s) %s' % (self.flagval(i, 'Z'), tgt)]
            if base in ('loopne', 'loopnz'):
                return ['ecx--; if (ecx != 0 && !%s) %s' % (self.flagval(i, 'Z'), tgt)]
            sp = self.spin_check(i, t) if t in f.blocks else ''
            if sp:
                return ['if %s { %s%s }' % (self.cond(i, base[1:]), sp, tgt)]
            return ['if %s %s' % (self.cond(i, base[1:]), tgt)]
        if base in ('int3', 'hlt', 'ud2', 'int', 'into'):
            return ['guest_fault(0x%xu, "%s"); return;' % (i.address, base)]
        return self.emit_insn(i, None)


class Recomp:
    def __init__(self, exe, prefix):
        self.img = Image(exe)
        self.prefix = prefix
        an = Analyzer(self.img)
        self.funcs = an.run(an.scan_pointers())
        self.funcs = {k: v for k, v in self.funcs.items() if v is not None and not v.bad}
        self.shims = {}

    def shim_name(self, dll, nm):
        dll = dll.replace('.DLL', '').replace('.', '_')
        orig = nm
        if nm.startswith('?'):
            import zlib; nm = 'cpp_%08x' % zlib.crc32(nm.encode())
        s = 'imp_%s_%s' % (dll, nm)
        self.shims[(dll, nm)] = s
        self.orig = getattr(self, 'orig', {}); self.orig[s] = orig
        return s

    def run(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        chunks = []
        cur = []
        size = 0
        unsup = []
        names = {}
        for a in sorted(self.funcs):
            g = FuncGen(self, self.funcs[a])
            nm, code = g.gen()
            names[a] = nm
            unsup += g.unsupported
            cur.append(code)
            size += len(code)
            if size > 1500000:
                chunks.append(cur); cur = []; size = 0
        if cur:
            chunks.append(cur)
        hdr = '#define GENCODE 1\n#include "rt.h"\n'
        for k, c in enumerate(chunks):
            with open(os.path.join(outdir, '%s_code%d.c' % (self.prefix, k)), 'w') as fh:
                fh.write(hdr)
                fh.write('#include "%s_decl.h"\n' % self.prefix)
                fh.write('\n'.join(c))
        with open(os.path.join(outdir, '%s_decl.h' % self.prefix), 'w') as fh:
            for a in sorted(names):
                fh.write('void %s(void);\n' % names[a])
            for (dll, nm), s in sorted(self.shims.items()):
                fh.write('void %s(void);\n' % s)
        # function table + import table
        with open(os.path.join(outdir, '%s_tables.c' % self.prefix), 'w') as fh:
            fh.write(hdr + '#include "%s_decl.h"\n' % self.prefix)
            fh.write('const guest_func_entry %s_funcs[] = {\n' % self.prefix)
            for a in sorted(names):
                fh.write('  {0x%xu, %s},\n' % (a, names[a]))
            fh.write('  {0, 0}};\n')
            fh.write('const guest_import_entry %s_imports[] = {\n' % self.prefix)
            for iat, (dll, nm) in sorted(self.img.imports.items()):
                s = self.shim_name(dll, nm)
                fh.write('  {0x%xu, "%s", "%s", %s},\n' % (iat, dll, nm, s))
            fh.write('  {0, 0, 0, 0}};\n')
            fh.write('const uint32_t %s_entry = 0x%xu;\n' % (self.prefix, self.img.entry))
        # rewrite decl with all shims (imports table may have added)
        with open(os.path.join(outdir, '%s_decl.h' % self.prefix), 'w') as fh:
            for a in sorted(names):
                fh.write('void %s(void);\n' % names[a])
            for (dll, nm), s in sorted(self.shims.items()):
                fh.write('void %s(void);\n' % s)
        with open(os.path.join(outdir, '%s_shims.txt' % self.prefix), 'w') as fh:
            for (dll, nm), s in sorted(self.shims.items()):
                fh.write('%s %s %s\n' % (dll, self.orig.get(s, nm), s))
        print('functions:', len(names), 'chunks:', len(chunks), 'unsupported:', len(unsup))
        for u in unsup[:80]:
            print('  %x %s' % u)


if __name__ == '__main__':
    Recomp(sys.argv[1], sys.argv[2]).run(sys.argv[3])
