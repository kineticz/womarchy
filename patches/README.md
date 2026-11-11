# Patches

Our changes to upstream projects. Each series is plain `git format-patch` output against the release
pinned in [versions.sh](versions.sh), one commit per logical change, ready to send upstream (see
[docs/UPSTREAMING.md](../docs/UPSTREAMING.md)). The packages in `linux/packages` apply them on top of the
Arch packages.

| Patch | What and why | Depends on |
|---|---|---|
| **aquamarine** (v0.15.1) | | |
| [0001](aquamarine/0001-allocator-shm-allocator-for-backends-without-DRM.patch) allocator: shm allocator | CPU buffers (memfd, or files in a directory another process maps), for backends without DRM. | — |
| [0002](aquamarine/0002-backend-wsl-a-remote-viewer-backend.patch) backend: wsl | A remote-viewer backend: listens on vsock, authenticates the viewer, one output per viewer monitor, frames as damage rectangles over [WDP](../protocol/wdp.h), input and cursor over the same socket. | 0001 |
| [0003](aquamarine/0003-backend-wsl-forward-explicit-cursor-warps-to-the-hos.patch) backend: explicit cursor warps | Sends compositor-requested host pointer warps without echoing ordinary input. | 0002 |
| [0004](aquamarine/0004-backend-wsl-offers-the-vgem-render-node-to-clients-W.patch) backend: wsl offers the vgem render node | With `WOMARCHY_DMABUF=1`, `drmRenderNodeFD()` returns the vgem node built into newer WSL kernels, so Hyprland enables linux-dmabuf; output buffers stay shm. The `60-womarchy-gpu` generator sets it when a vgem node exists; see [research 06](../docs/research/06-zero-copy-gpu-buffers.md). | 0002 |
| **Hyprland** (v0.56.2) | | |
| [0001](hyprland/0001-render-DRM-free-mode-surfaceless-EGL-shm-output-buff.patch) render: DRM-free mode | Surfaceless EGL when there is no DRM fd; read damaged regions back into shm output buffers; two shm swapchain buffers. | aquamarine 0001 |
| [0002](hyprland/0002-compositor-select-aquamarine-s-wsl-backend-with-HYPR.patch) compositor: `HYPRLAND_BACKEND=wsl` | Selects the wsl backend. | aquamarine 0002 |
| [0003](hyprland/0003-pointer-CPU-cursor-buffers-without-dmabuf.patch) pointer: CPU cursor buffers | Hardware-cursor path with shm buffers. | aquamarine 0001 |
| [0004](hyprland/0004-protocols-screen-capture-without-linux-dmabuf.patch) protocols: screen capture without linux-dmabuf | Fixes a null dereference: any screenshot crashed a compositor without linux-dmabuf. | — |
| [0005](hyprland/0005-input-absolute-pointer-motion-relative-to-the-backen.patch) input: absolute motion per output | Absolute pointer positions map onto the output they refer to, not the whole layout (nested/remote backends with several outputs). | — |
| [0006](hyprland/0006-pointer-synchronize-explicit-warps-with-the-WSL-view.patch) pointer: synchronize explicit WSL warps | Keyboard focus/workspace changes also move the real Windows cursor; respects `cursor:no_warps`. | aquamarine 0003 |
| [0007](hyprland/0007-render-linux-dmabuf-work-on-a-vgem-render-node-WSL.patch) render, linux-dmabuf: vgem render node | On vgem: EGL through GBM (the device platform can't initialise there) and no PRIME check on client buffers (dxgkrnl fds that EGL imports but vgem's PRIME rejects). | aquamarine 0004 |
| **Mesa** (26.2.3) | | |
| [0001](mesa/0001-d3d12-reclaim-outside-pb-manager-locks.patch) d3d12: reclaim outside pb manager locks | Fixes a self-deadlock on the first text texture upload. | — |
| [0002](mesa/0002-d3d12-write-back-upload-heaps-on-wsl.patch) d3d12: write-back upload heaps on WSL | CPU writes to write-combine heaps run at ~9 MB/s on WSL 3.0.1; texture uploads were 500x slower. `D3D12_UPLOAD_WRITE_BACK=0` turns it off. | — |
| [0003](mesa/0003-d3d12-finish-shared-resource-writes-at-flush-on-WSL.patch) d3d12: finish shared-resource writes at flush | dxgkrnl dmabufs carry no fence, so a compositor could sample a client buffer before its GPU work ran. A flush that submitted writes to a shared resource waits for them. See [research 06](../docs/research/06-zero-copy-gpu-buffers.md). | — |
| [0004](mesa/0004-d3d12-emulate-native-fence-fds-for-Chromium-GPU-proc.patch) d3d12: native fence fds for Chromium | Chromium (kernel ≥ 6.0) skips its `glFlush` before presenting and expects a native acquire fence, which d3d12 can't make on WSL: its frames stayed queued and Edge/PWAs flickered with stale frames. In Chromium/Electron GPU processes only, exported fences wait and return an already-signaled fd. | 0003 |
| [pending 0003](mesa-pending/0003-glapi-expose-EXT_external_objects-entry-points-on-Op.patch) glapi: external objects on GLES 3.0 | `GL_EXT_memory_object(_fd)`/`semaphore(_fd)` were advertised on ES 3.0 but dispatched only from ES 3.2, so d3d12 (ES 3.0) couldn't import Vulkan/dzn buffers. Kept out of the Mesa package (`mesa-pending/`, not needed for dmabuf); see [research 06](../docs/research/06-zero-copy-gpu-buffers.md). | — |
| [pending 0004](mesa-pending/0004-d3d12-don-t-use-an-uninitialized-pointer-when-a-memo.patch) d3d12: failed memobj import | Uninitialized pointer crash when `OpenSharedHandle` rejects a handle. Kept out of the Mesa package (`mesa-pending/`). | — |

The Mesa patches are labelled `Generated-by: LLM`, as Mesa's contribution rules require for
AI-generated code.

The [workspace cursor upgrade audit](../docs/research/05-workspace-cursor-upgrade.md) records why
Hyprland 0.57.1/Aquamarine 0.16.0 are not yet adopted and which patches can be removed or need rebasing.

## Working on them

The forks live in `src/` (not committed). Commits go on a `womarchy` branch at the pinned tag:

```
tools/setup-src.sh                    # clone aquamarine and Hyprland at the pinned tags and apply these patches
# ...edit, then commit in src/<project> (fixups: git commit --fixup=<sha>, then
#    GIT_SEQUENCE_EDITOR=: git rebase -i --autosquash <tag>)
linux/packages/refresh-patches.sh     # export the series back here and into linux/packages/*
linux/packages/regen-pkgbuilds.sh     # refresh the PKGBUILDs (bump *_REL in it first)
linux/packages/build-all.sh aquamarine hyprland   # in the Arch build distro
```

Mesa has no fork here: its two patches are edited directly and applied by
`linux/packages/mesa-womarchy`.

To move to a new upstream release:
1. Change [versions.sh](versions.sh).
2. Run `FORCE=1 tools/setup-src.sh` and resolve any conflicts in `src/`.
3. Run `refresh-patches.sh` and `regen-pkgbuilds.sh`.

[lab/vgem-patches](../lab/vgem-patches) holds an abandoned experiment (a vgem render node), kept for reference only.
