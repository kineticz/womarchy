# 07: Start-up cost of GL apps, and the LLVM clash with the Windows GPU driver

2026-11-11, reference machine: Intel Iris Xe (driver 32.0.101.7088), Mesa 26.2.3 d3d12, GTK 4, Hyprland
0.56.2 with linux-dmabuf on vgem ([research 06](06-zero-copy-gpu-buffers.md)).

## Why GTK apps felt slower than Edge

The steady state is not the problem. A GTK 4 GL window animating 40 rectangles runs at 56–57 FPS, as close to
60 as Edge. The difference is the **start of every app** and the **first use of new visuals**:

| GTK 4 test window | First real frame | 30 frames done |
|---|---|---|
| `GSK_RENDERER=gl`, stock Mesa | ~1.35 s after the first tick | ~2.5–2.8 s |
| `GSK_RENDERER=cairo` (CPU) | — | ~1.05 s |

It happens on every start, and again (100–400 ms) when a widget state needs a shader the process hasn't used.
A wall-clock profile of the main thread (LD_PRELOAD sampler walking frame pointers, `~/src/zc-spike/prof`)
shows where the time goes:

- 0.4–0.95 s: Mesa compiles and links GLSL (`st_link_shader` / `gl_nir_link_glsl`, ~0.65 s in total).
- 0.95–1.75 s: the main thread waits on a futex while the D3D12 driver's worker threads run Intel's shader
  compiler (IGC, with its own LLVM 14) on every pipeline state.
- 2.1–2.6 s: more pipelines, compiled by IGC on the main thread.

Mesa's d3d12 driver had **no shader cache at all**: no `get_disk_shader_cache` (so st/mesa's cache of linked
programs was off) and every `CreatePipelineState` without a cached blob. Edge hides the same costs: its GPU
process lives as long as the browser and Chromium keeps its own program cache.

Fix, [Mesa 0005](../../patches/mesa/0005-d3d12-on-disk-shader-cache-for-GLSL-programs-and-pip.patch):
- A disk cache per screen (identity: Mesa build id, adapter ids, driver version, `D3D12_DEBUG`), returned from
  `get_disk_shader_cache`. That turns on st/mesa's NIR cache of linked programs, and
  `GL_ARB_get_program_binary` for ANGLE/Chromium.
- Driver-compiled pipeline states (`GetCachedBlob`) in the same cache, keyed by the whole description
  (shader bytecode, all state, the root signature key). The runtime validates a cached blob; a rejected blob
  is compiled again and replaced.

Result, warm cache: first real frame ~0.2 s instead of ~1.4 s, 30 frames at ~1.4 s (the remainder is Python
and GTK start-up). Screenshots of the window with a cold and a warm cache are pixel-identical. Edge's
counter test (research 06) stays at 0 stale frames, and its pages start a little sooner.

## LLVM clash: quickshell crashes in the Windows GPU driver's compiler

All five quickshell crash reports on the machine (2026-11-03 to 11-11, Mesa 2.1–2.3, LLVM 22 and 23) have the
same stack: Qt's `swapBuffers` → d3d12 → `libd3d12core` → Intel `libigd12um64xel` → `libigc` →
`libLLVM-14.so` (`FoldingSetBase::FindNodeOrInsertPos`, `AttributeSetNode::get`), and the faulting frame in
**Arch's `libLLVM.so.23.1`**.

Cause: Arch builds LLVM with GCC, which marks objects such as
`FoldingSet<AttributeListImpl>::getFoldingSetInfo()::Info` as `STB_GNU_UNIQUE`. glibc keeps **one
process-wide table** of unique symbols, keyed by name only: symbol versions (`LLVM_14` against
`LLVM_23.1`) and dlopen namespaces don't separate them. Mesa loads LLVM 23 first (for llvmpipe), so when IGC
is loaded to compile a shader, `LD_DEBUG=bindings` shows **37 of LLVM 14's references bound to LLVM 23**,
among them the `FoldingSetInfo` tables of attributes, SCEV and SDNodes. IGC then calls LLVM 23's functions on
LLVM 14 objects. Usually that goes silently wrong; sometimes it crashes. Any process using Mesa's d3d12
driver can hit it: quickshell, Hyprland, GTK apps, Edge's GPU process.

Fix (womarchy-compat 0.4.3), with no change to Mesa or LLVM packages:
- `/usr/lib/womarchy/llvm-deunique` writes a copy of each `/usr/lib/libLLVM.so.<ver>` to
  `/usr/lib/womarchy/llvm/` with every `GNU_UNIQUE` `.dynsym` entry rewritten as `GLOBAL` (183 symbols for
  LLVM 23.1). That is how a clang-built LLVM exports them anyway.
- `/usr/lib/ld.so.conf.d/womarchy-llvm.conf` puts that directory ahead of `/usr/lib`, so every process loads
  the copy.
- Pacman hook `10-womarchy-llvm-unique` regenerates it whenever `libLLVM.so.*` changes, before glibc's
  ldconfig hook; stale copies are removed.

Verified: with the copy, LLVM 14 binds all its own `getFoldingSetInfo()::Info` tables (0 bindings to LLVM
23), and llvmpipe still compiles and renders with it.

## Vulkan apps crashed in the device-select layer

Since linux-dmabuf runs on vgem (research 06), every Vulkan instance with `VK_KHR_wayland_surface` or
`VK_KHR_xcb_surface` crashed in `vkEnumeratePhysicalDevices`, inside Mesa's implicit
`VkLayer_MESA_device_select` (GTK with `GSK_RENDERER=vulkan`, GTK texture downloads, any Wayland Vulkan app).
The layer asks the display which device it uses (linux-dmabuf default feedback `main_device`, or DRI3 on
X11), gets vgem (a platform device), and reads `businfo.pci` / `deviceinfo.pci` without checking
`bustype`. For a platform device those unions hold other pointers. Mesa main has the same code.

Fix, [Mesa 0006](../../patches/mesa/0006-vulkan-device-select-only-match-PCI-devices-against-.patch): skip the
display-device match unless the device is on the PCI bus. Verified with a minimal instance (Wayland and
X11 surface extensions) and a GTK window on the Vulkan renderer: both crash with the stock layer and work
with the fix. Before the package update, `NODEVICE_SELECT=1` is a workaround.

## GTK exported GL textures as dmabufs without an fd

GTK 4.24 exports GL textures as dmabufs when `EGL_MESA_image_dma_buf_export` is present (GtkGLArea,
`gsk_renderer_render_texture`). d3d12 can only export resources created shareable, so the fd query failed,
but `eglExportDMABUFImageMESA` ignored that and returned `EGL_TRUE` with `fds[0]` unset (reproducer:
`~/src/zc-spike/export/q.c`). GTK then built a dmabuf texture from an uninitialized fd: a
`planes[0].fd > 0` assertion on every GtkGLArea frame, an empty `render_texture` ("Failed to download …
dmabuf texture"), and `g_close()` of a garbage fd number, which can close an unrelated file.

Fix, [Mesa 0007](../../patches/mesa/0007-egl-dri2-don-t-report-dmabuf-export-of-images-the-dr.patch): the
export fails with `EGL_BAD_ALLOC` when there is no fd, and the query only reports images the driver can give
an fd for, so GTK falls back quietly to a GL texture. GBM-allocated buffers (Hyprland, Edge) still export.

`GSK_RENDERER=ngl` is now `GSK_RENDERER=gl` (the generator and `/etc/profile.d`): GTK 4.24 renamed it and
warned on every start.

## Other findings, not fixed yet

- Edge stalls for 4–9 s in about half of its starts, with or without these patches (12 runs: 6 of 12),
  and with `--disable-gpu-sandbox` too. No error in its log and no GPU-process restart.
