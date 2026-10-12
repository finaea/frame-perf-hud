# Read-only walk of SVLServer's std::map<std::string,double> variable registry at server+0xc8.
import struct, sys, time
sys.path.insert(0, r"C:\Users\final\AppData\Local\Temp\claude\C--Users-final-Downloads-Programs\2155ce63-8083-4bbd-ac6c-dcdb12b9eb07\scratchpad")
from vrmem import Proc, pid_of

def read_std_string(p, a):
    raw = p.read(a, 32)
    size, cap = struct.unpack_from("<QQ", raw, 16)
    if size > 4096: return None
    if cap < 16: return raw[:size].decode(errors="replace")
    ptr = struct.unpack_from("<Q", raw, 0)[0]
    b = p.read(ptr, size); return b.decode(errors="replace") if b else None

def walk_map(p, head, limit=5000):
    out = []
    root = p.q(head + 8)
    stack, node = [], root
    while (stack or node != head) and len(out) < limit:
        while node != head and node is not None:
            nil = p.read(node + 0x19, 1)
            if nil is None or nil[0]: break
            stack.append(node); node = p.q(node)
        if not stack: break
        node = stack.pop()
        nd = p.read(node + 0x20, 0x28)
        key = read_std_string(p, node + 0x20)
        val = struct.unpack_from("<d", nd, 0x20)[0]
        out.append((key, val, node))
        node = p.q(node + 0x10)
    return out

if __name__ == "__main__":
    p = Proc(pid_of()); B = p.peb_modules()["driver_vrlink.dll"]
    srv = p.q(B + 0x179d840)
    head = p.q(srv + 0xc8)
    size = p.q(srv + 0xd0)
    t0 = time.perf_counter(); r0 = p.reads
    items = walk_map(p, head)
    dt = time.perf_counter() - t0
    print(f"server {srv:#x} map head {head:#x} size {size}  walked {len(items)} in {dt*1000:.1f} ms, {p.reads-r0} reads")
    for k, v, n in items:
        print(f"  {k:32s} {v:.6g}")
    p.close()
