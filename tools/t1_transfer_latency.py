"""Read-only public SteamVR ping proof. Background app, compositor FnTable slot 11, Windows ABI 192 B.

Uses SteamVR's installed openvr_api.dll. Polls VREvent_Quit and shuts down immediately.
No Steam settings or process memory access. Run: python tools/t1_transfer_latency.py --seconds 15
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import re
import struct
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=15)
    args = parser.parse_args()
    paths_file = Path(os.environ["LOCALAPPDATA"]) / "openvr/openvrpaths.vrpath"
    paths = json.loads(paths_file.read_text())
    dll = c.CDLL(str(Path(paths["runtime"][0]) / "bin/win64/openvr_api.dll"))
    init = dll.VR_InitInternal2
    init.argtypes = [c.POINTER(c.c_int), c.c_int, c.c_char_p]
    init.restype = c.c_uint32
    interface = dll.VR_GetGenericInterface
    interface.argtypes = [c.c_char_p, c.POINTER(c.c_int)]
    interface.restype = c.POINTER(c.c_void_p)
    shutdown = dll.VR_ShutdownInternal
    shutdown.argtypes = []
    shutdown.restype = None
    # Derive PollNextEvent's table index from the vendored interface declaration.
    header = (Path(__file__).resolve().parents[1] / "third_party/openvr/openvr.h").read_text()
    body = header.split("class IVRSystem", 1)[1].split("IVRSystem_Version", 1)[0]
    names = re.findall(r"virtual\s+[^;{]*?\b(\w+)\s*\([^;]*?=\s*0", body)
    poll_slot = names.index("PollNextEvent")
    error = c.c_int()
    init(c.byref(error), 3, None)  # VRApplication_Background never launches SteamVR.
    if error.value:
        print(f"SteamVR not running / init error {error.value}")
        return 1
    connected = True
    try:
        table = interface(b"FnTable:IVRCompositor_029", c.byref(error))
        if error.value or not table:
            raise RuntimeError("compositor interface unavailable")
        get_timings = c.WINFUNCTYPE(c.c_uint32, c.c_void_p, c.c_uint32)(table[11])
        system = interface(b"FnTable:IVRSystem_026", c.byref(error))
        if error.value or not system:
            raise RuntimeError("system interface unavailable")
        poll = c.WINFUNCTYPE(c.c_bool, c.c_void_p, c.c_uint32)(system[poll_slot])
        timing = c.create_string_buffer(192)
        # VREvent_t Windows x64: header 12 B + union aligned to 8 B, union 48 B -> 64 B.
        event = c.create_string_buffer(64)
        end = time.monotonic() + args.seconds
        next_sample = 0
        while time.monotonic() < end:
            while poll(event, len(event)):
                if struct.unpack_from("<I", event.raw)[0] == 700:  # VREvent_Quit
                    shutdown()
                    connected = False
                    print("SteamVR quit: VR_ShutdownInternal called immediately")
                    return 0
            if time.monotonic() >= next_sample:
                next_sample = time.monotonic() + 0.5
                struct.pack_into("<I", timing, 0, 192)
                if get_timings(timing, 1):
                    index = struct.unpack_from("<I", timing.raw, 4)[0]
                    ping = struct.unpack_from("<f", timing.raw, 184)[0]
                    print(f"frame={index} public_ping_ms={ping:.3f}", flush=True)
                else:
                    print("no frames", flush=True)
            time.sleep(0.05)
    finally:
        if connected:
            shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
