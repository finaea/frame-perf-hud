# Re-derive every offset the reader needs from string / RTTI anchors in driver_vrlink.dll (static, file on disk).
# If each search returns exactly one hit with the same value as the hard-coded offset, the reader can self-locate after an update.
import re, struct, pefile, capstone
from capstone import x86
DLL = r"C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\vrlink\bin\win64\driver_vrlink.dll"
pe = pefile.PE(DLL, fast_load=True); data = open(DLL, "rb").read()
text = [s for s in pe.sections if s.Name.rstrip(b"\0") == b".text"][0]
T0, TVA, TLEN = text.PointerToRawData, text.VirtualAddress, text.SizeOfRawData
code = data[T0:T0 + TLEN]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def off2rva(o):
    for s in pe.sections:
        if s.PointerToRawData <= o < s.PointerToRawData + s.SizeOfRawData: return o - s.PointerToRawData + s.VirtualAddress
def str_rva(s):
    hits = [off2rva(m.start()) for m in re.finditer(re.escape(s.encode() + b"\0"), data)]
    return hits
def lea_xrefs(rva):
    out = []
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code):
        i = m.start(); d = struct.unpack_from("<i", code, i + 3)[0]
        if TVA + i + 7 + d == rva: out.append(TVA + i)
    return out
def window(va, before, after):
    s = va - TVA - before
    return list(md.disasm(code[s:s + before + after], TVA + s))
def find_after(va, pred, n=40):
    for ins in md.disasm(code[va - TVA:va - TVA + 400], va):
        if pred(ins): return ins
res = {}
# 1) server global + debug ctx: "mov [rip+G], r15 ; mov [rip+G+0x40], rbx" in the root-UI ctor
sig = [m.start() for m in re.finditer(rb"\x4c\x89\x3d....\x48\x89\x1d", code, re.S)]
cands = []
for i in sig:
    g1 = TVA + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
    g2 = TVA + i + 14 + struct.unpack_from("<i", code, i + 10)[0]
    if g2 - g1 == 0x40: cands.append((hex(TVA + i), hex(g1)))
res["server_global (sig 4C893D..48891D, delta 0x40)"] = cands
# 2) var map offset: GetVar = 'lea rbp,[rcx+X]' ... 'mov rdi,[rdi+Y]' reached from a 'dRelFOV' lookup
x = lea_xrefs(str_rva("cPingAMS")[0])
res["cPingAMS xrefs"] = [hex(v) for v in x]
for v in x:
    call_getvar = find_after(v, lambda ins: ins.mnemonic == "call" and ins.address > v + 0x20)
    gv = call_getvar.operands[0].imm
    ins = list(md.disasm(code[gv - TVA:gv - TVA + 0x60], gv))
    mp = [i for i in ins if i.mnemonic == "mov" and "+ 0x" in i.op_str and i.op_str.startswith("rdi, qword ptr [rdi")]
    res.setdefault("GetVar fn / map offset", []).append((hex(v), hex(gv), mp[0].op_str if mp else None))
    lea_aftm = find_after(v, lambda ins: ins.mnemonic == "lea" and ins.op_str.startswith("rcx, [rsi +"))
    res.setdefault("AFTM offset (lea rcx,[rsi+X] after cPingAMS)", []).append((hex(v), lea_aftm.op_str if lea_aftm else None))
# 3) per-link fields: instructions just before each '%.1f ms' xref in the multilink table
x = lea_xrefs(str_rva("%.1f ms")[0])
res["'%.1f ms' xrefs"] = [hex(v) for v in x]
for v in x:
    prev = [i for i in window(v, 12, 0)][-1]
    res[f"  load before {v:#x}"] = f"{prev.mnemonic} {prev.op_str}"
# 4) AFTM record stride + row format
fmt = str_rva("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%llu,%llu,%u,%u,%u,%u,%u,%d\n")
res["AFTM row format string present"] = [hex(r) for r in fmt]
for v in lea_xrefs(fmt[0]):
    ins = window(v, 0x1a0, 0)
    res["AFTM row writer imul stride"] = [f"{i.mnemonic} {i.op_str}" for i in ins if i.mnemonic == "imul"][:1]
    res["AFTM FILE* offset"] = [i.op_str for i in ins if i.mnemonic == "mov" and "r10, qword ptr [rcx +" in i.op_str][:1]
# 5) RTTI type descriptors present
for nm in (".?AVSVLServer@@", ".?AVSVLDataLinkUber@@", ".?AVSVLDataLinkTransferUDP@@", ".?AVSVLRoot@@"):
    res["RTTI " + nm] = [hex(r) for r in str_rva(nm)]
for k, v in res.items(): print(f"{k}: {v}")

