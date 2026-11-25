# Native shader sources

Every shader here is compiled by the pinned DirectX Shader Compiler
(`tools/bootstrap-dxc.ps1`, DXC 1.9.2609) and embedded into `build/NativeGPU.exe`
as a byte blob. There is no runtime dependency on `dxcompiler.dll`, and there is no
path where a shader silently falls back to a different compiler for the same effect.

## Two compilation paths, on purpose

| Path | Compiler | Profile | Why |
| --- | --- | --- | --- |
| Raster + composite | `D3DCompile` from an embedded string in `renderer/d3d12_renderer.cpp` | `vs_5_1` / `ps_5_1` | The base renderer must stay runnable when DXC is missing, so the fallback path cannot depend on DXC. |
| Ray tracing library | DXC, this directory | `lib_6_5` | D3DCompile cannot produce ray tracing libraries. |
| Mesh shaders (work package 4) | DXC, this directory | `as_6_5` / `ms_6_5` | D3DCompile cannot produce mesh shaders. |

If DXC is unavailable the generated header still exists, but it reports
`kDxcAvailable = false` and every blob has size 0. The renderer must then report
`dxcUnavailable`, keep the raster path, and never report the effect as implemented.

## Files

| File | Target | Exports |
| --- | --- | --- |
| `rt_shadows.hlsl` | `lib_6_5` | `ShadowRayGen`, `ShadowMiss`, `ShadowClosestHit` (hit group `ShadowHitGroup`) |

`manifest.json` is the single source of truth for what gets compiled and what the
generated symbol names are. `tools/compile-shaders.ps1` reads it and writes:

- `build/shaders/*.dxil` — compiled blobs for inspection and capture tools,
- `build/shaders/manifest.json` — per-blob target, size, SHA-256 and the DXC identity,
- `build/generated/nativegpu_shaders.h` — the embedded arrays consumed by the renderer.

`tools/compile-shaders.ps1 -SelfTest` compiles a deliberately broken shader and
requires DXC to reject it with diagnostics preserved; `.incrementalTesting/core/verify.ps1` runs that
check on every verification.

## Shadow pass contract

`rt_shadows.hlsl` reads the raster depth buffer, reconstructs world position with the
inverse view-projection, traces one ray per pixel toward the fixed key light and writes
a visibility mask. The raster pass writes ambient + point lights to `SV_TARGET0` and the
key-light contribution to `SV_TARGET1`; the composite pass computes
`base + key * visibility`. Forcing the mask to 1.0 therefore reproduces the pure raster
image, which is exactly what the `--rt-shadows off` path does.
