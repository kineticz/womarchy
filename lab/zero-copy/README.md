# Zero-copy GPU buffer spikes

Findings: [docs/research/06-zero-copy-gpu-buffers.md](../../docs/research/06-zero-copy-gpu-buffers.md).

| Program | Shows | Build |
|---|---|---|
| `drmdev.c` | libdrm sees the built-in vgem render node | `gcc drmdev.c -o drmdev $(pkg-config --cflags --libs libdrm) -include sys/sysmacros.h` |
| `gbmtest.c` | GBM + EGL on the render node run on d3d12 and offer dmabuf import/export | `gcc gbmtest.c -o gbmtest -lEGL -lGLESv2 -lgbm` |
| `dmabuf_loop.c` | client→compositor GBM buffers across processes, correct with `flush` or `finish` | `gcc -O1 dmabuf_loop.c -o dmabuf_loop -lEGL -lGLESv2 -lgbm`; `GALLIUM_DRIVER=d3d12 ./dmabuf_loop flush 300 3` |
| `loop_prod.c` + `loop_cons.c` | Vulkan (dzn) image → GLES via `GL_EXT_memory_object_fd` (needs Mesa patch 0003) | `-I<Vulkan-Headers>/include -lvulkan` / `-lEGL -lGLESv2`; `. use-mesa.sh; ./loop_prod ./loop_cons 300` |
| `producer2.c` + `consumer2.c`, `vkimport.c` | shared timeline fences across processes fail on WSL | as above |

`use-mesa.sh` points at a locally built Mesa (`~/opt/womarchy-mesa`) with the patches applied.
