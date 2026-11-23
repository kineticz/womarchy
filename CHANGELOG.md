# Changelog

## 0.3.1 (2026-11-23)

**New**
- **Touchpad swipes.** On a precision touchpad, three- (or four-) finger swipes reach Hyprland as touchpad gestures, so `hl.gesture({ fingers = 3, direction = "horizontal", action = "workspace" })` switches workspaces. Set Windows' three-finger gestures to "Nothing" (Settings > Bluetooth & devices > Touchpad), or Windows acts on them as well. Needs the new viewer and aquamarine 0.15.1-1.12.

**Fixed**
- **Touchpad scrolling no longer jumps back.** Windows sends touchpad scrolling as tiny wheel steps; Hyprland's discrete-scroll emulation turned each step, including the backward jitter at the end of a scroll, into a whole wheel click. The viewer now sends it as touchpad (finger) scrolling while fingers are on the pad (speed: `input:touchpad:scroll_factor`), then an axis stop so apps scroll on kinetically as on Linux; Windows' own inertia is dropped. Without a precision touchpad, wheel steps smaller than a click are sent as smooth scrolling. Needs the new viewer; aquamarine 0.15.1-1.12 for the touchpad source.
- **Evolution and other WebKitGTK apps open again.** The session lets audio clients discover the default PipeWire socket instead of setting `PULSE_SERVER` to an explicit Unix address that crashes WebKitGTK 2.54.1 during sandbox setup. On an existing installation, update to womarchy-compat 0.4.9-2, then replace the old `PULSE_SERVER` export block with `unset PULSE_SERVER` in `~/.config/uwsm/env.d/10-womarchy` and restart the Womarchy session.
- **Ghostty opens again.** Ghostty needs OpenGL 4.3, but the WSL GPU driver (Mesa d3d12) offers 4.1, so Ghostty could not create its GL context and opened no window. It now renders on Mesa's software driver (llvmpipe); other apps stay on the GPU. `/usr/local/bin/ghostty` points to womarchy's wrapper while Ghostty is installed, so the terminal key, the app launcher and Ghostty's background service all use it (womarchy-compat 0.4.9). An admin's own `/usr/local/bin/ghostty` is left alone.

## 0.3.0 (2026-11-17)

First release of the [kineticz](https://github.com/kineticz/womarchy) fork of [sytelus/womarchy](https://github.com/sytelus/womarchy): the installer, image and packages come from this fork's releases and its signed `[womarchy]` repository.

**New**
- **Audio through the viewer.** Speakers and microphone no longer go through WSLg (which could hang and stay silent). `omarchy.exe` plays Linux audio on Windows' default output device and records Windows' default microphone, via the new `womarchy-mediad` in the session (PipeWire devices "Windows audio" and "Windows microphone"). The microphone is only opened while a Linux app records. It follows changes of Windows' default devices. Needs the new viewer and `womarchy-session` 0.1.0-17; with an older viewer the session keeps using WSLg.
- **Windows cameras in Linux.** Each Windows camera becomes a PipeWire camera ("Integrated Webcam (Windows)"), which browsers and other apps find through the camera portal, up to 1080p. The camera is only on while an app uses it. Chromium and Edge need the `WebRtcPipeWireCamera` feature. `womarchy-provision-user` enables it in their flags page setting (`Local State`, so it also applies to web apps such as Teams, which start the browser directly) while the browser is closed, and in their flags files (womarchy-compat 0.4.8). On an existing install: enable `edge://flags/#enable-webrtc-pipewire-camera` (or `chrome://flags/...`) and restart the browser, or close it and run `womarchy-provision-user --overlay-only --force`.

**Fixed**
- Keyboard workspace/focus changes across monitors now forward explicit cursor warps to Windows, instead of leaving the host cursor on the previous monitor. Requires the rebuilt Hyprland/Aquamarine packages and Windows viewer; ordinary mouse motion is unchanged.

## 0.2.0 (2026-10-01)

**New**
- **Undoing updates.** `omarchy rollback` puts back the package versions from before the last update; the distro records them automatically before every update. `omarchy backup` and `omarchy restore` save and restore the whole distro as one file. `omarchy update` runs Omarchy's updater and asks once whether to make a full backup first.
- **Clipboard images.** Images are now shared between Windows and Omarchy in both directions (PNG; Windows bitmaps are converted).
- **Signed package repository.** Updates of womarchy's packages (Hyprland, aquamarine, Mesa, the WSL overlay) come from a signed repository, and installs refuse unsigned databases. Installs from 0.1.0 move over automatically with their next update.
- **WSL overlay updates.** Fixes to the WSL adjustments (`womarchy-compat`) now reach installed systems through normal updates instead of needing a new image.
- **Upgrading `omarchy.exe`:** run the installer again. It keeps your distro and says so, and only updates `omarchy.exe` and its shortcuts.
- **`omarchy --version`**, and `omarchy status` now shows the version (useful in bug reports).
- **Safer updates.** An update that would break the GPU driver (an LLVM bump Mesa wasn't built for) now stops with a clear message instead of leaving a broken desktop. A daily check notices when Omarchy's package snapshot moves ahead of womarchy's builds.

**Project**
- Continuous integration, issue forms, contributing and security guides. Contributions come through issues; pull requests aren't accepted directly.
- A new README with a demo GIF, and [the journey](docs/JOURNEY.md): how this was built, the bugs, and how each was found and fixed.
- New troubleshooting entries for browser 3D (WebGL), the microphone, and updates.

## 0.1.0 (2026-10-01)

First public preview:
- the one-step installer;
- a full-screen, GPU-composited Omarchy desktop on every monitor, with per-monitor DPI;
- text clipboard, audio and live display changes;
- `omarchy install`, `uninstall` and `status`.
