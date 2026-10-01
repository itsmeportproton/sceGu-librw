sceGu-librw
===========

A fork of [aap/librw](https://github.com/aap/librw), a re-implementation of
RenderWare Graphics, aimed at running on the **Sony PSP** via `sceGu`.

> **Status: work in progress.** The backend is written but not yet
> considered usable; tests and sample programs are coming soon.
> Expect missing features and API churn.

The goal is a general-purpose PSP renderer for RenderWare content, not tied to
any particular game: load DFF/TXD, render clumps, skinned/animated
characters and immediate-mode geometry on the GE.

# What the PSP backend does

New platform `PLATFORM_PSP` (`RW_PSP`), sources in `src/psp/`:

| File | Contents |
|------|----------|
| `pspdevice.cpp` | GU init, display list / frame handling, camera matrices, viewport, render states, fog, lights (4 GE lights), texture state |
| `pspraster.cpp` | Textures (8888, 5551, 565, 4444, mipmaps), image conversion, native TXD streaming |
| `psppipe.cpp` | Geometry instancing into GE vertex format, default pipeline |
| `pspskin.cpp` | Skin pipeline using hardware bone matrices |
| `pspimmed.cpp` | Im2D / Im3D |

Design notes:

* **Input:** regular DFF/TXD (including D3D8/D3D9 native textures, converted
  on load). Geometry is instanced into GE vertex buffers when first rendered.
* **Skinning:** done on the GE. The GE has 8 bone matrices, so every mesh is
  split at instance time into batches that use at most 8 bones; weights are
  remapped to matrix slots per batch.
* **Depth:** reversed depth range (`GU_GEQUAL`), standard on the GE.
* **Textures** live in system RAM, must be power of two, at most 512x512,
  at most 8 mip levels.

# Known gaps

* MatFX (env map, bump map, dual pass) is not implemented; such atomics are
  drawn like plain ones.
* Palettized and DXT textures are expanded to true color on load.
* Only the first UV set is used.
* Prelit + lit geometry uses vertex colors as material (material color is
  ignored in that case); point light falloff is approximate.
* No render-to-texture, no stencil.
* Culling winding and depth setup are not yet verified.
* BSP/world sectors are not supported (as in upstream).

# Building

Requires the [pspdev](https://github.com/pspdev/pspdev) toolchain.

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake \
  -DLIBRW_PLATFORM=PSP
cmake --build build
```

This produces the `librw` static library; link it together with
`pspgu pspge pspdisplay` (the CMake target does this for you) into your
EBOOT. Minimal usage:

```cpp
rw::Engine::init();
rw::Engine::open(nullptr);    // PSP needs no open parameters
rw::Engine::start();
// create camera + framebuffer/zbuffer rasters (480x272), then per frame:
//   camera->clear(...); camera->beginUpdate(); ...render...; camera->endUpdate();
//   camera->frameBuffer->show(rw::Raster::FLIPWAITVSYNCH);
```

# Upstream

All other platforms (D3D9, OpenGL, PS2) and the file format code come from
upstream librw; see `ARCHITECTURE.MD` for an overview of the engine. The PSP
backend only adds `src/psp/` and a few platform hooks in the shared code
(`rwbase.h`, `base.cpp`, `engine.cpp`, `texture.cpp`, `geoplg.cpp`,
`skin.cpp`, `matfx.cpp`, CMake).

# License

MIT, see `LICENSE`.
