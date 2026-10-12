# helper/

`fph-camgrab`, the camera helper, and its wire protocol. It's the only part of frame-perf-hud that needs
sudo, once, and only for the camera widgets.

- `fph-camgrab.c`: hands the agent the dmabufs behind the camera frames, plus "newest frame" reports. It
  reads XRService's buffers with `pidfd_getfd`, which `kernel.yama.ptrace_scope=1` blocks for a normal
  user, so it's installed root-owned with `cap_sys_ptrace=ep`.
- `fph_camgrab_proto.h`: the messages between the agent (`headset/camera_helper.*`) and the helper.

## Install

```sh
sudo ~/.local/share/frame-perf-hud/install-camera-helper.sh            # install.sh stages it there
sudo ~/.local/share/frame-perf-hud/install-camera-helper.sh --uninstall
```

It goes to `/home/.frame-perf-hud/fph-camgrab` (root:root 0755), because `/home` survives SteamOS updates.
Replacing the file clears the capability, so a new helper version means running the installer again. That's
why the helper stays tiny and all the logic lives in the agent.

## What it does and doesn't do

- Read-only: `VIDIOC_G_FMT` once per discovery and `VIDIOC_QUERYBUF`. Never queues, dequeues, streams off
  or changes the format. XRService's V4L2 fd is borrowed per use, closed right away, and never handed on.
  Only dmabuf fds go to the agent.
- Fixed targets, found by `/proc/<pid>/exe` and owned by the user who started it: XRService (outward cams on
  `/dev/video0, 3, 9, 13`) and the eye tracker (its `udmabuf` buffers). The agent can only send "outward
  on/off", "eye on/off" and "recheck": no paths, pids or fds.
- Exits on socket EOF, when the agent dies, and when XRService or the eye tracker exits. The agent respawns
  it, at most once per 5 s.

## Check it

```sh
frame-perf-hud --camera-helper-dump [--seconds S] [--camera-eye] [--camera-no-outward]
```

Prints the install state, the fds and formats the helper hands over, per-second frame reports and recheck
results, and respawns it the way the camera widgets do.
