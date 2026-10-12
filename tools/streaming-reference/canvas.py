# Read the debug window's own canvas (32bpp buffer at ctx+0x10) straight from vrserver memory and save it as PNG.
# Read-only; does not send anything to the window. Usage: canvas.py out.png [crop x0 y0 x1 y1]
import sys, struct, time
sys.path.insert(0, r"C:\Users\final\AppData\Local\Temp\claude\C--Users-final-Downloads-Programs\2155ce63-8083-4bbd-ac6c-dcdb12b9eb07\scratchpad")
from vrmem import Proc, pid_of
from PIL import Image
p = Proc(pid_of()); B = p.peb_modules()["driver_vrlink.dll"]
ctx = p.q(B + 0x179d838)
w, h = p.i(ctx + 8), p.i(ctx + 0xc); buf = p.q(ctx + 0x10)
t0 = time.perf_counter()
raw = p.read(buf, w * h * 4)
dt = (time.perf_counter() - t0) * 1000
img = Image.frombuffer("RGBA", (w, h), raw, "raw", "BGRA", 0, 1).convert("RGB")
if len(sys.argv) >= 6:
    img = img.crop(tuple(int(v) for v in sys.argv[2:6]))
img.save(sys.argv[1])
print(f"canvas {w}x{h} read {len(raw)} bytes in {dt:.1f} ms; mode={p.d(B + 0x179d878)} label={p.cstr(B + 0x179df90)!r}")
p.close()
