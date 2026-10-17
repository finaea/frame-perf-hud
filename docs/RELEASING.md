# Releasing

A release is one GitHub release on `finaea/frame-perf-hud` with three files. The headset's updater and the PC
app's updater both read it, so one release updates both sides.

| File | Built by | Used by |
| --- | --- | --- |
| `frame-perf-hud-<version>.tar.gz` | `scripts/package.sh`, on the Steam Frame | the headset's dashboard updater (`frame-update.sh`) |
| `FramePerfHud-<version>.exe` | `pc\package.cmd`, on Windows | the PC app's updater (`pc/updater.cpp`) |
| `SHA256SUMS` | both, one line each | both updaters check their file against it; a missing or wrong line means no install |

Both updaters only take the **latest** release, only when it isn't a draft or a prerelease, and only when its tag
`v<version>` is newer than the running version. The examples below use 0.4.0.

Nothing here runs by itself: no CI publishes anything, and the scripts print what to do next but don't upload.

## 1. Set the version

1. In `CMakeLists.txt`, set `project(frame-perf-hud VERSION 0.4.0 ...)`. The headset binary, the PC exe and both
   updaters all take their version from it.
2. In `CHANGELOG.md`, give the section its number and date.
3. Commit, and push `main`.

## 2. Build the headset tarball (on the Frame)

The Frame has no git checkout. Copy the committed tree over from the PC (Git Bash, in the repo folder), then build
there:

```sh
git archive HEAD | ssh frame "rm -rf ~/projects/fph-release && mkdir -p ~/projects/fph-release && tar -xf - -C ~/projects/fph-release"
ssh frame "cd ~/projects/fph-release && scripts/package.sh"
```

It writes `dist/frame-perf-hud-0.4.0.tar.gz` and `dist/SHA256SUMS` with the tarball's line.

## 3. Copy both to the PC

From the repo folder on the PC:

```sh
mkdir -p dist
scp frame:projects/fph-release/dist/frame-perf-hud-0.4.0.tar.gz frame:projects/fph-release/dist/SHA256SUMS dist/
```

## 4. Build the PC exe

```bat
pc\package.cmd
```

It makes a clean Release build in `out\pc\build-release`, which leaves the everyday `out\pc\build` and any copy
running from it alone. It copies the exe to `dist\FramePerfHud-0.4.0.exe` and prints its line for `SHA256SUMS`:

```
<64 hex chars>  FramePerfHud-0.4.0.exe
```

## 5. Add the exe's line to SHA256SUMS

Append the printed line in Git Bash, so the file keeps Unix line ends, then check both lines:

```sh
printf '%s\n' '<64 hex chars>  FramePerfHud-0.4.0.exe' >> dist/SHA256SUMS
(cd dist && sha256sum -c SHA256SUMS)
```

Both files should say `OK`. `SHA256SUMS` now has two lines.

## 6. Scan the exe on VirusTotal

Only the PC exe gets scanned. The headset tarball targets Linux, not Windows' virus scanners.

1. **Upload** `dist\FramePerfHud-0.4.0.exe` one of two ways:
   - **On the web:** go to https://www.virustotal.com/gui/home/upload. No account is needed.
   - **With the API, if `VT_API_KEY` is set:** the files endpoint takes files up to 32 MB, and the exe is about 4.7 MB
     ([docs](https://docs.virustotal.com/reference/files-scan)):

     ```sh
     curl -s --request POST --url https://www.virustotal.com/api/v3/files \
       --header "x-apikey: $VT_API_KEY" --form file=@dist/FramePerfHud-0.4.0.exe
     ```
2. **Wait** for the analysis to finish. The report stays at `https://www.virustotal.com/gui/file/<sha256>/detection`
   ([docs](https://docs.virustotal.com/docs/most-recent-report)), where `<sha256>` is the hash `pc\package.cmd`
   printed, so the link can be written before the scan is done.
3. **Judge the result:**
   - A few heuristic hits are common for a new exe that isn't code-signed, reads another process's memory and
     replaces itself. Say so in the release notes.
   - Many engines flagging it, or a well-known engine naming a real malware family, is a reason to stop and
     investigate before publishing. This is a rule of thumb, not a fixed number.
4. **Add a line to the release notes:**
   `VirusTotal: https://www.virustotal.com/gui/file/<sha256>/detection (<detections>/<engines>)`.

VirusTotal shares uploaded files with its partners. Only upload a release exe that's about to be public anyway, never
a dev build or anything with a secret in it.

## 7. Publish

Write the release notes to `dist/release-notes.md` first: what changed (from the CHANGELOG) and the VirusTotal line.

`gh` doesn't follow the repo's account on its own. Switch to finaea first, then switch back afterwards:

```sh
gh auth switch --user finaea
gh release create v0.4.0 dist/frame-perf-hud-0.4.0.tar.gz dist/FramePerfHud-0.4.0.exe dist/SHA256SUMS \
  --repo finaea/frame-perf-hud --target main --title v0.4.0 --notes-file dist/release-notes.md
gh auth switch --user jacklim0921
```

- Don't mark it as a draft or a prerelease: both updaters skip those.
- Use the exact file names. The updaters look for `frame-perf-hud-<version>.tar.gz` and `FramePerfHud-<version>.exe`.
- The repo already has tags `v0.1.0` to `v0.3.0`, inherited from the fork. They have no releases, so they don't
  matter here, but the new tag has to be newer than them.

## 8. Check it

- `curl -s https://api.github.com/repos/finaea/frame-perf-hud/releases/latest` names `v0.4.0` and lists all three
  files.
- On the headset: dashboard → Frame Perf HUD → Check now. A 0.4.0 install says "Up to date"; an older one offers
  Install.
- On the PC: Settings → Check now. A 0.4.0 exe says "Up to date (0.4.0)"; an older one shows
  "Update available (0.4.0)" in the status bar and the tray.

## How the PC app updates itself

At start and at most once a day, or on Check now, it asks GitHub for the latest release. When that release is newer,
the status bar and the tray offer Install.

Install works like this:

1. It downloads `SHA256SUMS` and the exe to `%LOCALAPPDATA%\frame-perf-hud\update\`, and checks the exe's SHA-256.
2. It renames the running exe to `FramePerfHud.exe.old` and puts the new one in its place.
3. It starts the new exe and exits the normal way (saying BYE to the headset).
4. The new exe waits for the old process to end, then takes over. On its next start it deletes the `.old`.

If the exe's folder isn't writable (e.g. Program Files), it opens the release page instead.

To test it without GitHub, point it at a local server with `FPH_UPDATE_API_URL=http://127.0.0.1:8765` and
`FPH_UPDATE_INSECURE=1`. The server needs to answer `/repos/finaea/frame-perf-hud/releases/latest` like GitHub does.
Use a copy of the exe with `--offline --data-dir <folder>`, never a real install.
