# FidelityFX Optical Flow — vendored sources

Copied unmodified from the AMD FidelityFX SDK, tag `v1.1.4`, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55` (2025-05-08), MIT licence (`LICENSE.txt`).

| Here | There |
|---|---|
| `ffx_core*.h`, `ffx_common_types.h` | `sdk/include/FidelityFX/gpu/` |
| `opticalflow/*.h` | `sdk/include/FidelityFX/gpu/opticalflow/` (HLSL callbacks and the CMake fragment left out) |
| `spd/ffx_spd.h` | `sdk/include/FidelityFX/gpu/spd/` |
| `passes/*.glsl` | `sdk/src/backends/vk/shaders/opticalflow/` |

Technique version: FidelityFX Optical Flow 1.1.2 (`FFX_OPTICALFLOW_VERSION_*` in the SDK's host header).
Only the colour buffer goes in; block motion vectors (8x8, `rg16i`, `prev = cur + v`) and a scene
change detector come out. The host-side sequencing of the seven passes is ported to C in
`src/framegen.c`.

One local edit, every line of it marked `afmf-linux:`, in
`opticalflow/ffx_opticalflow_compute_optical_flow_v5.h`: the block's SAD at rest (which the SDK
already computes for its level-0 fallback) is taken before the search, and a block at or under the
specialization constant `afmfStaticBlockSad` (`AFMF_STATIC_BLOCK_SAD`, 0 = the SDK's behaviour)
is stored as static and skips the search. Everything else is as vendored.
