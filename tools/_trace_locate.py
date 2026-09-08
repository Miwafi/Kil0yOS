import gdb
gdb.execute("set architecture i386:x86-64")
gdb.execute("target remote :1234")
gdb.execute("break *0x110f4c")
gdb.execute("continue")
rip = int(gdb.parse_and_eval("$rip"))
print("AT CALL SITE: %#x" % rip, flush=True)
gdb.execute("delete")
log = []
for i in range(80000):
    try:
        gdb.execute("stepi", to_string=True)
    except gdb.error as e:
        print("STEP ERROR:", e, flush=True)
        break
    rip = int(gdb.parse_and_eval("$rip"))
    log.append(rip)
    if rip == 0x1E80D1EB:
        print("REACHED POOL SITE after %d steps" % (i + 1), flush=True)
        break
print("TOTAL STEPS: %d" % len(log), flush=True)
prev_in = None
for j, r in enumerate(log):
    cur_in = r < 0x600000
    if prev_in is None or cur_in != prev_in:
        tag = "start" if prev_in is None else ("KERNEL->FW" if prev_in and not cur_in else "FW->KERNEL")
        print("step %6d: %-11s 0x%x" % (j, tag, r), flush=True)
    prev_in = cur_in
print("=== last 80 RIPs ===", flush=True)
for r in log[-80:]:
    print("  0x%x" % r, flush=True)
