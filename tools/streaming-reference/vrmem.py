# Read-only peek into vrserver.exe. Access: PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ only.
# No toolhelp, no debug APIs, no writes, no threads. Module base found by walking the PEB loader list.
import ctypes, ctypes.wintypes as wt, struct, subprocess

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_VM_READ = 0x0010
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
k32.OpenProcess.restype = wt.HANDLE
k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.ReadProcessMemory.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]

class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t), ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t

def pid_of(name="vrserver.exe"):
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == name.lower():
            return int(parts[1])
    raise RuntimeError(name + " not running")

class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
        if not self.h:
            raise OSError(ctypes.get_last_error(), "OpenProcess failed")
        self.reads = 0; self.bytes = 0
    def close(self):
        k32.CloseHandle(self.h)
    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t(0)
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
        self.reads += 1; self.bytes += n
        if not ok:
            return None
        return buf.raw[:got.value]
    def q(self, a):
        b = self.read(a, 8); return struct.unpack("<Q", b)[0] if b else None
    def d(self, a):
        b = self.read(a, 4); return struct.unpack("<I", b)[0] if b else None
    def i(self, a):
        b = self.read(a, 4); return struct.unpack("<i", b)[0] if b else None
    def cstr(self, a, n=128):
        b = self.read(a, n); return b.split(b"\0")[0].decode(errors="replace") if b else None
    def peb_modules(self):
        class PBI(ctypes.Structure):
            _fields_ = [("ExitStatus", ctypes.c_void_p), ("PebBaseAddress", ctypes.c_void_p), ("AffinityMask", ctypes.c_void_p),
                        ("BasePriority", ctypes.c_void_p), ("UniqueProcessId", ctypes.c_void_p), ("InheritedFrom", ctypes.c_void_p)]
        pbi = PBI(); rl = wt.ULONG(0)
        st = ntdll.NtQueryInformationProcess(self.h, 0, ctypes.byref(pbi), ctypes.sizeof(pbi), ctypes.byref(rl))
        if st != 0:
            raise OSError(st, "NtQueryInformationProcess")
        ldr = self.q(pbi.PebBaseAddress + 0x18)
        head = ldr + 0x20  # InMemoryOrderModuleList
        mods = {}
        cur = self.q(head)
        for _ in range(2000):
            if cur == head or cur is None: break
            ent = cur - 0x10
            base = self.q(ent + 0x30)
            ln = struct.unpack("<H", self.read(ent + 0x58, 2))[0]
            name = self.read(self.q(ent + 0x60), ln).decode("utf-16-le", errors="replace")
            mods[name.lower()] = base
            cur = self.q(cur)
        return mods
    def rtti_name(self, obj, modbase):
        vt = self.q(obj)
        if not vt: return None
        col = self.q(vt - 8)
        if not col: return None
        td_rva = self.d(col + 12)
        if td_rva is None: return None
        return self.cstr(modbase + td_rva + 0x10, 96)
    def regions(self):
        a = 0; m = MBI()
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(a), ctypes.byref(m), ctypes.sizeof(m)):
            yield (m.BaseAddress or 0, m.RegionSize, m.State, m.Protect, m.Type)
            a = (m.BaseAddress or 0) + m.RegionSize
            if a >= 0x7FFFFFFF0000: break
