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

Local edits, every line of them marked `afmf-linux:`:

- `opticalflow/ffx_opticalflow_compute_optical_flow_v5.h`: each block's SAD at the vector
  predicted by the coarser level (zero at the coarsest) is taken before the search, and a block at
  or under the specialization constant `afmfStaticBlockSad` (`AFMF_STATIC_BLOCK_SAD`, 0 = the
  SDK's behaviour) keeps that vector and skips the search; the SDK's level-0 zero-vector
  fallback only computes its sum at level 0.
- `opticalflow/ffx_opticalflow_common.h`, `..._v5.h` and
  `passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl`: with `AFMF_SAD_INT16=1`
  (a second build of the search pass, used on devices with `shaderInt16`) the SAD runs on packed
  16-bit byte pairs instead of one byte at a time. Without the define the SDK's code compiles
  unchanged.

Everything else is as vendored.
