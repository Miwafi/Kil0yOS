import re, subprocess
out = subprocess.check_output(['objdump','-drl','build/kernel.bin']).decode(errors='replace')
lines = out.splitlines()
# call format: " 109574:  e8 d7 00 00 00        call   100950 <somefunc+0x10>"
pat = re.compile(r'^\s*([0-9a-f]+):\s+.*\bcall\b\s+([0-9a-f]+)\s+<(.+?)>')
res=[]
for l in lines:
    m=pat.match(l)
    if m:
        ins_addr=int(m.group(1),16)
        tgt=int(m.group(2),16)
        sym=m.group(3)
        if 0x109570<=tgt<=0x1095d0 and 'strcmp' in sym:
            al=subprocess.run(['addr2line','-e','build/kernel.bin',hex(ins_addr)],capture_output=True,text=True).stdout.strip()
            res.append((hex(ins_addr), sym, al))
seen=set()
for a,s,al in res:
    if al in seen: continue
    seen.add(al)
    print(f"call strcmp at {a}  ({s})  caller: {al}")
print("total:", len(res))
