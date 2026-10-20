# Native shader sources

Every shader in this directory is compiled by the pinned DirectX Shader Compiler
(`tools/bootstrap-dxc.ps1`, DXC 1.9.2609) and embedded into `build/NativeGPU.exe`
as a byte blob. There is no runtime dependency on `dxcompiler.dll`, and there is no
path where a shader silently falls back to `D3DCompile` — ray tracing and mesh
shaders need DXC, so a missing DXC is reported as `dxcUnavailable` instead of being
faked.

## Files

| File | Target | Purpose |
| --- | --- | --- |
| `rt_shadows.hlsl` | `lib_6_5` | DXR static shadow ray generation, miss, closest hit |
| `mesh_shaders.hlsl` | `as_6_5` / `ms_6_5` | Amplification + mesh shader meshlet sample |
| `post.hlsl` | `vs_6_0` / `ps_6_0` | Fullscreen composite (base + key light * shadow mask) |
| `bindless.hlsl` | `ps_6_6` | Optional SM 6.6 `ResourceDescriptorHeap` sampling path |

`manifest.json` is the single source of truth for what gets compiled and what the
generated symbol names are. `tools/compile-shaders.ps1` reads it and writes:

- `build/shaders/*.dxil` — compiled blobs for inspection and capture tools,
- `build/shaders/manifest.json` — per-blob target, size, SHA-256 and the DXC identity,
- `build/generated/nativegpu_shaders.h` — the embedded arrays consumed by the renderer.

If DXC is unavailable the generated header still exists, but it reports
`kDxcAvailable = false` and every blob has size 0. The renderer must then report
`dxcUnavailable` and keep the raster path; it must never report the effect as
implemented.
