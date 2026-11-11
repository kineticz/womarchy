# Zero-copy GPU buffers between apps and Hyprland on WSL

Checked on 2026-11-10 on the owner's laptop: Windows 11, Intel Iris Xe (driver 32.0.101.7088), WSL kernel
`6.18.54.1-microsoft-standard-WSL2`, Mesa 26.2.3 (womarchy build). Test programs are in
[lab/zero-copy](../../lab/zero-copy) and `~/src/zc-spike` on that machine.

## Why it matters

Today every GPU-rendered app frame takes a detour: the app renders on the GPU, reads the frame back into
a `wl_shm` buffer (Mesa's software-winsys path, because there is no dmabuf), and Hyprland uploads it to the
GPU again. At 1080p on Iris Xe that is ~1.5 ms readback + ~1.6 ms upload + a GPU sync per frame. It is
also why GTK's GL renderer was *slower* than its Cairo (CPU) renderer here, and why Chromium/Edge fall back
to CPU raster.

## Findings

1. **The WSL kernel has DRM and vgem built in.** `CONFIG_DRM=y`, `CONFIG_DRM_VGEM=y`,
   `CONFIG_DMA_SHARED_BUFFER=y`, `CONFIG_SYNC_FILE=y`. `/dev/dri/renderD128` (mode 0666) and `card0`
   (root:video) are a built-in vgem device on the `faux` bus, present in every distro without `modprobe`.
   This corrects JOURNEY story 1 / research 03 ("newer WSL kernels have no DRM support at all"), which
   was true for the kernel tested then, not for 6.18.54.1. `drmGetDevice2()` describes the device
   (driver `vgem`, render + primary node); `DRM_CAP_SYNCOBJ_TIMELINE` is 0.
2. **GBM + EGL on that render node run on the GPU through d3d12, with the stock womarchy Mesa.**
   `gbm_create_device(renderD128)` → `EGL_PLATFORM_GBM_KHR` gives renderer
   `D3D12 (Intel(R) Iris(R) Xe Graphics)` and advertises `EGL_EXT_image_dma_buf_import`,
   `..._modifiers` and `EGL_MESA_image_dma_buf_export`. `gbm_bo_create(1920x1080, ARGB8888, RENDERING)`
   returns an fd that is `anon_inode:dxgresource` (a dxgkrnl shared resource, not a real dmabuf),
   stride 7680, modifier `DRM_FORMAT_MOD_INVALID`. (`lab/zero-copy` → `gbmtest`.)
3. **Those fds work across processes with the standard dmabuf APIs, with correct ordering and no extra
   sync.** A "client" renders into a GBM bo and passes the fd over a Unix socket; a "compositor" process
   imports it with `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`, samples it and checks the pixel.
   1,200 frames at 1080p, 0 wrong, both when the client `glFinish`es and when it only `glFlush`es
   (as a Wayland client does). With a deliberately heavy client (67 ms of GPU work per frame) and only
   `glFlush`, the compositor's read waited the full 67 ms and was still correct: work on the shared
   allocation executes in submission order across processes (one GPU, WDDM scheduler). Realistic loads:
   compositor sample + wait 1–2.5 ms. (`dmabuf_loop.c`.)
4. **Vulkan (dzn) ↔ GL sharing works too** via `VK_KHR_external_memory_fd` (opaque fd, also a
   `dxgresource`) and `GL_EXT_memory_object_fd`: 300/300 correct 1080p frames, 0.3 ms producer,
   0.7 ms compositor side (`loop_prod.c` / `loop_cons.c`). On GLES 3.0 contexts this needs Mesa patch
   [0003](../../patches/mesa-pending/0003-glapi-expose-EXT_external_objects-entry-points-on-Op.patch): the
   extension is advertised but its entry points were dispatched only from ES 3.2 (0/300 frames without
   the patch). Patch [0004](../../patches/mesa-pending/0004-d3d12-don-t-use-an-uninitialized-pointer-when-a-memo.patch)
   fixes a crash on a failed memobj import (uninitialized pointer).
5. **Shared fences across processes do not work on WSL.** A timeline semaphore exported by dzn
   (`anon_inode:dxgsyncobj`) can't be opened in another process, neither by dzn
   (`VK_ERROR_INVALID_EXTERNAL_HANDLE`) nor by d3d12 GL (`OpenSharedHandle` → `E_INVALIDARG`).
   Finding 3 makes them unnecessary for the compositor path; explicit sync (`drm_syncobj`) stays off
   anyway because vgem has no timeline syncobjs.

## Consequence

No private protocol is needed. If Hyprland uses the vgem render node, the **standard** path turns on:
Hyprland initialises EGL through GBM on the render node, binds `zwp_linux_dmabuf_v1` (and `wl_drm`), and
Mesa clients (GTK, Qt, terminals, mpv, SDL, …) allocate GBM buffers and hand them over without a copy.
Chromium/Edge use the same GBM/dmabuf path and may get GPU compositing back (to be tested).

What has to change:
- **aquamarine, wsl backend:** `drmRenderNodeFD()` returns an fd for the vgem render node (opt-in at
  first, e.g. `WOMARCHY_DMABUF=1`). `drmFD()` stays -1, so `CBackend::start` keeps the backend's shm
  allocator for output buffers (DAX shared memory for the viewer). ~30 lines.
- **Hyprland:** `CHyprOpenGLImpl` takes the device/GBM branch when the render node fd is valid; the shm
  output readback (patch 0001) is keyed on the buffer type and keeps working. Expected: little or no
  change; to verify (EGL device lookup for vgem, dmabuf feedback main device, format/modifier list with
  only `MOD_INVALID`).
- **Mesa:** nothing for the dmabuf path. 0003/0004 only for Vulkan↔GL interop.

Risks to test before making it the default: ordering on other GPUs (NVIDIA with hardware scheduling,
AMD; finding 3 was measured on Iris Xe only), buffer lifetime/release, memory budget with many windows,
Xwayland (glamor on the render node), screen capture, and apps that probe `/dev/dri/card0`.

## Result in the owner's desktop session (2026-11-10, later the same day)

Built aquamarine + Hyprland with [aquamarine 0004](../../patches/aquamarine/0004-backend-wsl-offers-the-vgem-render-node-to-clients-W.patch)
and [Hyprland 0007](../../patches/hyprland/0007-render-linux-dmabuf-work-on-a-vgem-render-node-WSL.patch) into a home-directory prefix
and ran the everyday session on it (user `hyprland.desktop` override + `~/.config/uwsm/env.d/20-womarchy-dmabuf`).

What was needed beyond the plan above:
- **Hyprland:** the EGL *device* platform finds an EGLDevice for vgem but can't initialise it; use GBM on vgem.
  linux-dmabuf's `commence()` PRIME-imports every client fd on the main device, which vgem rejects for
  dxgkrnl fds; skip it on vgem (the EGL import still validates the buffer).
- **Clients:** Mesa's Wayland/X11 loaders look for a driver named after the device (`vgem`) and fall back to
  wl_shm. `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` makes them run `GALLIUM_DRIVER` (d3d12) on the node.
  GBM users (Chromium/Edge) don't need it.
- **X11 GL apps:** Xwayland now has glamor on the node and offers DRI3. GLX can't load kms_swrast through DRI3
  and falls back to zink (Vulkan over dzn), which segfaults (`glxinfo`). `LIBGL_KOPPER_DISABLE=1` stops the
  zink fallback; GLX then uses its software winsys with d3d12, as before. EGL on X11 is fine.

Measured in the session (performance power mode, 60 Hz):
- Animated GTK4 window, 1800x1000, GL renderer, A/B per client in the same session (shm = override unset):
  both at 60 FPS (p95 17.0 ms); **client CPU 12% → 6%, Hyprland 17% → 14%** with dmabuf. Content checked
  pixel-exact against the shm path with grim on a lab compositor.
- **Edge** (guest profile, `--ozone-platform=wayland`): GPU compositing `disabled_software` → **enabled**,
  WebGL/WebGPU `enabled_readback` → **enabled**, on ANGLE → D3D12 (Iris Xe). A WebGL canvas renders correctly
  (it stayed blank on the shm path, the TROUBLESHOOTING caveat). A CPU A/B for Edge was inconclusive: Edge
  reaches the node through GBM even without the loader override.
- **glxgears** (X11): ~20 → ~50 FPS.

Packaged afterwards: aquamarine `0.15.1-1.11`, Hyprland `0.56.2-4.8` and womarchy-compat `0.4.2`, whose
`60-womarchy-gpu` user environment generator sets `WOMARCHY_DMABUF=1`, `WOMARCHY_RENDER_NODE`,
`MESA_LOADER_DRIVER_OVERRIDE=kms_swrast` and `LIBGL_KOPPER_DISABLE=1` when `/dev/dxg` and a vgem render node
exist (opt out with `WOMARCHY_DMABUF=0` in `~/.config/environment.d/`). Not yet checked: other GPUs (ordering),
Xwayland apps more widely, screen capture/recording, and memory use with many GPU windows. Note for lab tests: a fresh Edge profile signs in with the Windows
account and starts syncing; use `--guest`.

## Edge/Chromium flickered with stale frames (2026-11-11)

In the desktop, Edge and its PWAs (Outlook, Teams) flashed old page contents, especially on reload. A lab
compositor with a page that paints a frame counter as colour reproduced it: the displayed counter cycled
through three old values (Chromium's three buffers) while the browser committed 60 frames a second.

Cause, found by tracing Mesa (only ~7 flushes in a whole run) and reading Chromium's Wayland backend:
- Chromium decides from the kernel version (≥ 6.0) that dmabufs carry implicit fences
  (`WaylandConnection::UseImplicitSyncInterop`). It then tells the GPU process that acquire fences are
  supported, and `GbmSurfacelessWayland::Present` **skips its `glFlush()`**, expecting to ship each frame
  with a native acquire fence.
- It only creates that fence with `EGL_ANDROID_native_fence_sync`, which d3d12 can't offer on WSL (no
  sync_file for D3D12 work). With neither fence nor flush, the frame's GPU work stays queued until some
  later flush, and Hyprland samples the buffer's previous contents. (`DMA_BUF_IOCTL_EXPORT/IMPORT_SYNC_FILE`
  on the dxgkrnl fds fail with ENOTTY too; harmless once the frames are complete.)
- Separately, even with a flush, nothing orders the compositor's read after the client's GPU work: a
  two-process GBM test sampled the wrong frame 167 of 200 times with only `glFlush`.

Fix, [Mesa 0003](../../patches/mesa/0003-d3d12-finish-shared-resource-writes-at-flush-on-WSL.patch) and
[0004](../../patches/mesa/0004-d3d12-emulate-native-fence-fds-for-Chromium-GPU-proc.patch):
- A flush that submitted writes to a shared resource waits for them (two-process test: 0/200 wrong; GTK GL
  window 56–57 FPS either way).
- In Chromium/Electron GPU processes only (`--type=gpu-process`; `D3D12_WSL_NATIVE_FENCE_FD=0/1`
  overrides), d3d12 advertises native fence fds: exporting one waits for the fence and returns an
  already-readable eventfd; importing one waits for the fd on the CPU. Other processes (Hyprland, GTK,
  Xwayland) see no change.

Result in the desktop session (guest Edge, counter page): stock Mesa showed 72 of 120 sampled frames older
than the previous one; patched, 3 of 120, all within the first frames after start. Pages also start
faster (stock stalled up to 7 s after start). Interim workaround until the package is installed:
`--disable-gpu-compositing` for Edge (flags file and PWA launchers).

Pitfall from the investigation: every guest Edge profile is ~200 MB in `/tmp` (tmpfs); delete test
profiles, or a full `/tmp` produces misleading stalls and empty screenshots.
