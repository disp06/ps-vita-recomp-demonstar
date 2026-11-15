import sys, collections
from pe import *
img=Image(sys.argv[1]); an=Analyzer(img)
c=an.scan_pointers()
print("ptr cands",len(c))
an.run(c)
good=[f for f in an.funcs.values() if f and not f.bad]
bad=[f for f in an.funcs.values() if f and f.bad]
print("funcs",len(good),"bad",len(bad))
cnt=collections.Counter(); cov=set()
for f in good:
  for b in f.blocks.values():
    for i in b: cnt[i.mnemonic]+=1; cov.add(i.address)
print("coverage bytes", sum(img.insn(a).size for a in cov), "of", img.text_hi-img.text_lo)
print(sorted(cnt.items(), key=lambda x:-x[1]))
print("jtables", sum(len(f.jtables) for f in good))
ind=collections.Counter()
for f in good:
  for b in f.blocks.values():
    i=b[-1]
    if i.mnemonic=='jmp' and i.operands[0].type!=X.X86_OP_IMM and i.address not in f.jtables: ind[i.op_str]+=1
print("unresolved ind jmps", ind)
