# Read-only look at the AdvancedFrameTimeMonitoring ring inside SVLServer (server+0xff88). Record = 0x78 bytes.
import struct, sys, time
sys.path.insert(0, r"C:\Users\final\AppData\Local\Temp\claude\C--Users-final-Downloads-Programs\2155ce63-8083-4bbd-ac6c-dcdb12b9eb07\scratchpad")
from vrmem import Proc, pid_of
COLS = ["PresentID", "WGPRelease", "AppSubmit", "EncStart", "EncEnd", "SendStart", "SendEnd", "Receive", "DecStart", "DecEnd",
        "SubmitIllum", "PredDisplay", "DecQuery", "RunStartAtWGPDegate", "PingAtSend", "FiltPingAtSend", "UpPingAtSend"]
def rec(raw, o):
    v = list(struct.unpack_from("<13I4i", raw, o))
    v += list(struct.unpack_from("<QQ", raw, o + 0x48))
    v += list(struct.unpack_from("<5Ii", raw, o + 0x58))
    return v
if __name__ == "__main__":
    p = Proc(pid_of()); B = p.peb_modules()["driver_vrlink.dll"]
    srv = p.q(B + 0x179d840); aftm = srv + 0xff88
    t0 = time.perf_counter()
    raw = p.read(aftm, 0x88100)
    dt = (time.perf_counter() - t0) * 1000
    print(f"aftm {aftm:#x}: read {len(raw)} bytes in {dt:.2f} ms; FILE*@+0x88030 = {struct.unpack_from('<Q', raw, 0x88030)[0]:#x}")
    n = 0x88030 // 0x78
    recs = [rec(raw, i * 0x78) for i in range(n)]
    ids = [r[0] for r in recs]
    valid = [i for i, r in enumerate(recs) if r[0] != 0]
    print(f"slots {n}, non-empty {len(valid)}, PresentID min {min(ids)} max {max(ids)}")
    top = max(range(n), key=lambda i: ids[i])
    print("newest slot", top)
    # tail bytes after the array (indices / counters)
    tail = raw[n * 0x78: 0x88100]
    print("tail dwords:", struct.unpack_from(f"<{len(tail)//4}i", tail))
    for i in range(top - 6, top + 1):
        r = recs[i % n]
        base = r[1]
        d = lambda k: (r[k] - base) / 1000 if r[k] else None
        print(f"[{i % n}] id={r[0]} rel-to-WGP ms: submit {d(2)} encS {d(3)} encE {d(4)} sendS {d(5)} sendE {d(6)} recv {d(7)} decS {d(8)} decE {d(9)} illum {d(10)} pred {d(11)} | ping {r[14]} filt {r[15]} up {r[16]} | txid {r[24]}")
    p.close()
