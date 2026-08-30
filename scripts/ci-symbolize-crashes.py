import json, os, re, subprocess, sys, bisect

LOG = "build_debug/meson-logs/testlog.json"
if not os.path.exists(LOG):
    print("no testlog.json; nothing to symbolize"); sys.exit(0)

def symtab(path):
    try:
        out = subprocess.run(["nm", "-n", path], capture_output=True, text=True, timeout=120).stdout
    except Exception as e:
        print(f"    (nm failed: {e})"); return []
    syms = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 3 and p[1] in ("t", "T"):
            try: syms.append((int(p[0], 16), " ".join(p[2:])))
            except ValueError: pass
    syms.sort()
    return syms

found = False
for line in open(LOG):
    line = line.strip()
    if not line: continue
    try: rec = json.loads(line)
    except Exception: continue
    if rec.get("result") not in ("FAIL", "ERROR", "TIMEOUT"): continue
    err = rec.get("stderr") or ""
    if "crash sig=" not in err: continue

    name = str(rec.get("name", ""))
    short = name.split(":")[-1]
    binary = os.path.join("build_debug", "test_" + short)
    if not os.path.exists(binary):
        binary = os.path.join("build_debug", short)
    found = True
    print(f"\n=== {name}  (rc={rec.get('returncode')}) ===")
    sig = re.search(r"crash sig=(\d+)", err)
    fa  = re.search(r"fault_addr=0x([0-9a-fA-F]+)", err)
    print(f"    sig={sig.group(1) if sig else '?'}  fault_addr=0x{fa.group(1) if fa else '?'}")
    if fa:
        v = int(fa.group(1), 16)
        print(f"    fault_addr mod 16384 = {v % 16384}   (16384-8 = 16376 means 8 bytes below a page start)")
    if not os.path.exists(binary):
        print(f"    (binary not found: {binary})"); continue

    # __TEXT vmaddr, so a file offset maps to vmaddr + off.
    vm = 0x100000000
    m = re.search(r"vmaddr=0x([0-9a-fA-F]+)", err)
    if m: vm = int(m.group(1), 16)
    syms = symtab(binary)
    if not syms:
        print("    (no text symbols)"); continue
    for label, pat in (("pc", r"pc_off=0x([0-9a-fA-F]+)"), ("lr", r"lr_off=0x([0-9a-fA-F]+)")):
        mm = re.search(pat, err)
        if not mm: continue
        a = vm + int(mm.group(1), 16)
        i = bisect.bisect_right(syms, (a, chr(0x10FFFF))) - 1
        if 0 <= i < len(syms):
            print(f"    {label}_off=0x{mm.group(1)}  ->  {syms[i][1]} +0x{a - syms[i][0]:x}")
    for fm in re.finditer(r"frame\[(\d+)\][^\n]*ret_off=0x([0-9a-fA-F]+)", err):
        a = vm + int(fm.group(2), 16)
        i = bisect.bisect_right(syms, (a, chr(0x10FFFF))) - 1
        if 0 <= i < len(syms):
            print(f"    frame[{fm.group(1)}]  ->  {syms[i][1]} +0x{a - syms[i][0]:x}")

if not found:
    print("no crashing test found in testlog.json")
