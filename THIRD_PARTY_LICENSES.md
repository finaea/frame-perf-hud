# Third-party licenses

frame-perf-hud is a fork of [frame-perf-overlay](https://github.com/sasaken1102r/frame-perf-overlay) by
sasaken1102r (MIT). Both are released under the MIT License (see [LICENSE](LICENSE), which keeps the original
copyright line). This file lists the third-party code it contains and the system software it uses at run time.

## Bundled in this repository

### Update helper (`vendor/frame-updater/`)

- A copy of frame-perf-overlay's author's shared update code (frame-updater, a private repository), unchanged from upstream (version and commit in `vendor/frame-updater/UPSTREAM`) and checked by `vendor/frame-updater/verify.sh`.
- It is by sasaken1102r and is released under the same MIT License as frame-perf-overlay (see [LICENSE](LICENSE)). frame-perf-hud only changes the repository it checks (`finaea/frame-perf-hud`, set in `headset/main.cpp`), not these files.

### Noto Sans font (`assets/fonts/`)

- Project: Noto Sans by the Noto Project Authors, https://github.com/notofonts/latin-greek-cyrillic
- Version: NotoSans-v2.015 (release asset `NotoSans-v2.015.zip`, hinted TTFs)
- Downloaded from: https://github.com/notofonts/latin-greek-cyrillic/releases/download/NotoSans-v2.015/NotoSans-v2.015.zip
  (SHA-256 of the zip: `0c34df072a3fa7efbb7cbf34950e1f971a4447cffe365d3a359e2d4089b958f5`)
- Files and their SHA-256:
  - `NotoSans-Regular.ttf` (from `NotoSans/hinted/ttf/`): `478c558ea716033cd60c03438f628dfa75694dcf6b5f6d505a2f05fd2b4f3823`
  - `NotoSans-Bold.ttf` (from `NotoSans/hinted/ttf/`): `1df075a380fc7cb898acf64c1f7b3b4dd780de3caa860178bf929de35817a913`
  - `OFL.txt` (from the zip's root): `cee9892f9f0cc8fe882c9e9537ee6a89621d86ee7ceaf70b02e2b2b1c25c061a`
- License: SIL Open Font License 1.1, full text in [`assets/fonts/OFL.txt`](assets/fonts/OFL.txt)
- How it is used: unmodified font files, installed to `~/.local/share/frame-perf-hud/fonts/` and loaded at run time. Bundled so the headset and the (later) Windows app draw with the same font.

### OpenVR SDK header (`third_party/openvr/openvr.h`)

- Project: OpenVR SDK by Valve Corporation, https://github.com/ValveSoftware/openvr
- Version: v2.15.6 (the file is identical to `headers/openvr.h` at that tag)
- License: BSD-3-Clause
- How it is used: compiled into the program as a header. Only this one header is bundled.

The full license text, as published in the OpenVR repository
(https://github.com/ValveSoftware/openvr/blob/master/LICENSE):

```
Copyright (c) 2015, Valve Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### framestream eye-ring discovery (`headset/camera_eye_ring.*`)

- Project: FrameEyeCameraFeed's `framestream.c` by Curtis English, https://github.com/Curtis-VL/FrameEyeCameraFeed
- Version: commit 309c41d (2026-10-02)
- License: MIT
- How it is used: the eye cameras' ring discovery, "ring is live" check and slot reading were ported from C to
  C++ (`headset/camera_eye_ring.cpp`), with a tear guard added. The cache-maintained copy in
  `headset/camera_cache.cpp` follows the same technique.

```
MIT License

Copyright (c) 2026 Curtis English

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Used at run time from the Steam Frame, not bundled

The release tarball contains only the frame-perf-hud program, its install script, the files in
`contrib/`, the bundled font and the documentation. The libraries below are **not** included. The program
links to or reads the copies that are already on the Steam Frame (SteamOS and SteamVR).

The direct library dependencies below were checked with `readelf -d` and `ldd` on the release binary
on a Steam Frame. Versions are the SteamOS packages on the headset at the time of checking
(2026-09-27) and will change with SteamOS updates.

| Component | How it is used | License | Source |
|---|---|---|---|
| `libopenvr_api.so` (OpenVR API) | Dynamically linked; the copy that comes with SteamVR (`/opt/steamvr/bin/linuxarm64`) | BSD-3-Clause | https://github.com/ValveSoftware/openvr |
| cairo (`libcairo.so.2`, 1.18.0) | Dynamically linked; draws the panels | LGPL-2.1 or MPL-1.1 | https://gitlab.freedesktop.org/cairo/cairo/-/blob/master/COPYING |
| FreeType (`libfreetype.so.6`, 2.13.2) | Dynamically linked; loads the font | FreeType License (FTL) or GPL-2.0 | https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/LICENSE.TXT |
| libnl (`libnl-3.so.200`, `libnl-genl-3.so.200`, 3.9.0) | Dynamically linked; reads the Wi-Fi link status over nl80211 | LGPL-2.1 | https://github.com/thom311/libnl/blob/main/COPYING |
| libjpeg-turbo (`libturbojpeg.so.0`) | Dynamically linked; encodes camera frames sent to the PC app (ticket 10) | BSD-3-Clause, IJG, zlib | https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md |
| Vulkan loader (`libvulkan.so.1`, 1.4.309) | Dynamically linked; hands the panel image to SteamVR | Apache-2.0 | https://github.com/KhronosGroup/Vulkan-Loader/blob/main/LICENSE.txt |
| GNU C Library (`libc.so.6`, `libm.so.6`, 2.39) | Dynamically linked | LGPL-2.1-or-later | https://www.gnu.org/software/libc/ |
| GCC runtime libraries (`libstdc++.so.6`, `libgcc_s.so.1`) | Dynamically linked | GPL-3.0 with the GCC Runtime Library Exception 3.1 | https://www.gnu.org/licenses/gcc-exception-3.1.html |

cairo and FreeType in turn load other SteamOS libraries (for example pixman, libpng, zlib,
fontconfig, HarfBuzz and the X11/XCB client libraries). Those also come from SteamOS and are not
bundled either.

These libraries are used only as shared libraries loaded at run time from the Steam Frame, so they
can be replaced with any compatible version. The text of the GNU Lesser General Public License 2.1,
which covers cairo, libnl and the GNU C Library, is included in [`licenses/LGPL-2.1.txt`](licenses/LGPL-2.1.txt).

Portions of this software are based in part on the work of the FreeType Team (https://freetype.org).

## Icons and images

The launcher icons in `contrib/icons/` and the screenshots in `docs/images/` were drawn by this
program itself with cairo. They are original work of this project and fall under its MIT License.
