# perf-hud-research

These are the Python scripts from the Steam Link stats spike (2026-10-11). They're kept as the reference
for frame-perf-hud's C++ vrserver memory reader, copied unchanged from the spike's temporary scratchpad.

All of them only read: they open `vrserver.exe` with query + read access. Nothing is injected or written.

| File | What it does |
| --- | --- |
| `vrmem.py` | Read-only process memory helper: finds the vrserver PID, walks the PEB for module bases, reads RTTI names. The other scripts import it |
| `anchors.py` | Re-derives every offset from `driver_vrlink.dll` on disk (signature scan, string xrefs, RTTI). Needs `capstone` |
| `vars.py` | Walks the named-stats map (`std::map<std::string,double>` at server+0xc8) |
| `aftm.py` | Reads the per-frame timing ring (server+0xff88, 4096 × 0x78) |
| `live2.py` | 2 Hz live reader, writes a CSV, e.g. `python live2.py 30 2 out.csv`. Copes with the debug graph being off and with missing keys |
| `canvas.py` | Dumps the debug window's canvas from memory to a PNG. Needs `Pillow` |

The full write-up is the Traycer artifact `frame-hud/streaming-stats-spike` (epic `1824a50e`): offsets,
anchors, the debug-graph-off tests, and the reliability history. Offsets were checked on the
`driver_vrlink.dll` build with PE timestamp 1791581198 (2026-10-10). Run `anchors.py` again after any
SteamVR update.
