# Freshness test per source, read-only. Usage: live2.py <seconds> <hz> <csv_out>
import struct, sys, time, hashlib, statistics as st
sys.path.insert(0, r"C:\Users\final\AppData\Local\Temp\claude\C--Users-final-Downloads-Programs\2155ce63-8083-4bbd-ac6c-dcdb12b9eb07\scratchpad")
from vrmem import Proc, pid_of
from vars import walk_map

secs, hz, outp = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
p = Proc(pid_of()); B = p.peb_modules()["driver_vrlink.dll"]
srv = p.q(B + 0x179d840); aftm = srv + 0xff88; uber = p.q(srv + 0x188)
assert p.rtti_name(srv, B) == ".?AVSVLServer@@" and p.rtti_name(uber, B) == ".?AVSVLDataLinkUber@@"
items = walk_map(p, p.q(srv + 0xc8))
nodes = {k: n for k, v, n in items}
KEYS = ["sPingMS", "cPingAMS", "dbgEncTimeMS", "txTimeMS", "dEstLinkBW", "dCurBW", "rexmitReq", "rexmitSent", "nFramesSkippedPS", "encResets", "sRXpps", "cRXpps", "sSoundSPS", "tsLastWGP"]
links = [l for l in (p.q(uber + 0x388 + 8 * k) for k in range(16)) if l and p.d(l + 0x20)]
ctx = p.q(B + 0x179d838)
px = p.q(ctx + 0x10) if ctx else None; cw, ch = (p.i(ctx + 8), p.i(ctx + 0xc)) if ctx else (0, 0)
bwarr, bwcnt = p.q(srv + 0x1f0), p.i(srv + 0x1e8)
print(f"map keys {len(items)}; live links {len(links)}; bw ring size {bwcnt}")

def aftm_tail(N=144):
    head = struct.unpack("<H", p.read(aftm + 0x88000, 2))[0]
    start = (head - N) & 0xfff
    if start + N <= 4096: raw = p.read(aftm + start * 0x78, N * 0x78)
    else:
        k = 4096 - start; raw = p.read(aftm + start * 0x78, k * 0x78) + p.read(aftm, (N - k) * 0x78)
    recs = [struct.unpack_from("<13I4i", raw, i * 0x78) for i in range(N)]
    return head, recs

rows = []; hdr = None; out = open(outp, "w")
t_end = time.time() + secs
while time.time() < t_end:
    t0 = time.perf_counter()
    r = {"t": round(time.time(), 2)}
    head, recs = aftm_tail()
    done = [x for x in recs if x[10] and x[9] and x[4] and x[7]]
    newest = done[-1][10] if done else 0
    win = [x for x in done if ((newest - x[10]) & 0xffffffff) < 1_000_000]
    d = lambda a, b: st.mean(((x[b] - x[a]) & 0xffffffff) / 1000 for x in win) if win else None
    r.update(aftm_head=head, aftm_newest_illum=newest, aftm_frames_1s=len(win), enc=d(3, 4), net=d(6, 7), dec=d(8, 9), slack=d(9, 10), total=d(1, 10),
             ping=st.mean(x[14] / 1000 for x in win) if win else None)
    for k in KEYS: r[k] = struct.unpack("<d", p.read(nodes[k] + 0x40, 8))[0] if k in nodes else "absent"
    for i, l in enumerate(links):
        c14, _, c1c, a20, a24, a28, a2c = struct.unpack("<7I", p.read(l + 0x14, 0x1c))
        r[f"L{i}_run_cnt"] = c14; r[f"L{i}_pps"] = a24; r[f"L{i}_tot_pps"] = a20; r[f"L{i}_p100_ms"] = a2c / 1000; r[f"L{i}_lost"] = a28
    bi = p.i(srv + 0x1ec)
    last = bwarr + ((bi - 1) % bwcnt) * 0xa8
    now = struct.unpack("<Q", p.read(last + 0x40, 8))[0]
    wire = 0; n = 0
    for k in range(1, 400):
        rec = p.read(bwarr + ((bi - k) % bwcnt) * 0xa8, 0xa8); ts = struct.unpack_from("<Q", rec, 0x40)[0]
        if now - ts >= 1_000_000: break
        wire += struct.unpack_from("<i", rec, 0)[0]; n += 1
    r.update(bw_idx=bi, bw_newest_ts=now, mbps=round(wire * 8 / 1048576, 2), fps=n)
    r["evt_idx"] = "/".join(str(p.d(srv + 0xff00 + 4 * c)) for c in range(4))  # HMD/WGPRX/ENC/GAME rings (draw-code consumers)
    cb = p.read(px, cw * ch * 4) if px and cw > 0 else None
    r["canvas_md5"] = hashlib.md5(cb).hexdigest()[:8] if cb else "no-canvas"
    r["read_ms"] = round((time.perf_counter() - t0) * 1000, 2)
    rows.append(r)
    if hdr is None: hdr = list(r); out.write(",".join(hdr) + "\n")
    out.write(",".join(str(r[k]) for k in hdr) + "\n"); out.flush()
    time.sleep(max(0, 1 / hz - (time.perf_counter() - t0)))

def verdict(key):
    vals = [x[key] for x in rows]
    distinct = len(set(vals))
    return distinct, vals[0], vals[-1]
print(f"ticks {len(rows)} over {rows[-1]['t'] - rows[0]['t']:.1f} s")
for k in hdr:
    if k == "t": continue
    dcount, a, b = verdict(k)
    print(f"  {k:18s} distinct={dcount:3d}  first={a}  last={b}")
p.close()

