# afmf-linux

An open equivalent of AMD Fluid Motion Frames for Linux: a Vulkan implicit layer (`VK_LAYER_AFMF`)
that generates one interpolated frame between every two frames an application presents, from the
colour buffer alone. No game integration, no kernel module, no driver patch: it works with any
64-bit Vulkan application, including DXVK and vkd3d-proton titles under Proton.

AMD's AFMF is not a hardware feature: on Windows it is compute-shader work the driver inserts at
present time. This layer does the same thing on Linux, with AMD's own FidelityFX Optical Flow
shaders for the motion estimation and a small interpolation shader of its own.

## How it works

1. The layer hooks swapchain creation and asks for a few extra images with transfer usage.
2. On every `vkQueuePresentKHR` it copies the new frame into a two-frame history, runs
   FidelityFX Optical Flow (luma pyramid, scene-change detector, coarse-to-fine 8x8 block search)
   between the previous frame and the new one, and synthesises the frame in between by warping both
   along half the estimated motion.
3. The generated frame is presented first, the real frame right after it. All of this runs on a
   compute queue of the layer's own, so the application's graphics queue never waits for it.

Where the flow cannot be trusted (scene cut, motion beyond 64 px) the pixel falls back to the
previous frame or to a blend, selectable with `AFMF_FAST_MOTION_RESPONSE`.

The frame counter you will see is `real + generated`: the ceiling is twice the frame rate the game
reaches on Linux without the layer, minus the GPU time the layer itself needs (about 0.45 ms per
frame at 3440x1440 on an RX 9070 XT).

## Requirements

- Linux, 64-bit, a Vulkan 1.1+ driver. Developed and tested on RADV (Mesa) with RDNA GPUs; nothing
  in it is AMD-specific except the tuning.
- To build: CMake >= 3.28, Ninja or Make, a C11 compiler (GCC or Clang), the Vulkan headers and
  loader, `glslang` (compiles the shaders). `spirv-tools` is optional and validates every module.

Fedora: `dnf install cmake ninja-build gcc vulkan-headers vulkan-loader-devel glslang spirv-tools`

## Build and install

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build --prefix /usr/local
```

The install puts `libafmf-linux.so` under `lib/` and the manifest `afmf-linux.json` under
`share/vulkan/implicit_layer.d/`. From then on the layer loads into every Vulkan process but stays
dormant until `AFMF_ENABLE=1` is set.

To use it straight from the build tree without installing:

```sh
VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer AFMF_ENABLE=1 vkcube
```

## Usage

Any Vulkan application:

```sh
AFMF_ENABLE=1 <game>
```

Steam launch options (Proton or native):

```
AFMF_ENABLE=1 %command%
```

Set `AFMF_LOG=2` to see what the layer decided for each swapchain on stderr, for example:

```
[AFMF info] interpolation ready: 3440x1440, flow at 1720x720 (215x90 blocks of 16 px), 5 pyramid levels
[AFMF info] swapchain 0x... destroyed after 5000 presents: 4998 generated, 2 skipped (1 no free image, 1 no history)
```

`DISABLE_AFMF=1` keeps the layer out of a process even when `AFMF_ENABLE=1` is set, which is the
quickest way to measure a game's base frame rate for comparison.

## Configuration

Everything is an environment variable. The names mirror the settings AMD exposes for AFMF on
Windows (search mode, performance mode, fast motion response) where such a setting exists.

| Variable | Default | Meaning |
|---|---|---|
| `AFMF_ENABLE` | unset | `1` loads the layer |
| `DISABLE_AFMF` | unset | `1` keeps it out even when enabled |
| `AFMF_LOG` | `1` | `0` errors, `1` warnings, `2` info, `3` debug, all on stderr |
| `AFMF_PERFORMANCE_MODE` | `auto` | `quality`: optical flow at display resolution (8 px blocks). `performance`: at half resolution (16 px blocks), about 2.5x cheaper. `auto`: `performance` from 2560x1440 up |
| `AFMF_SEARCH_MODE` | `auto` | `standard`: 5 pyramid levels. `high`: 7 levels, motion up to +-512 flow pixels. `auto`: 7 at full resolution, 5 at half |
| `AFMF_FAST_MOTION_RESPONSE` | `repeat` | What to show where the flow is unreliable: `repeat` the previous frame, or `blend` both |
| `AFMF_EXTRA_IMAGES` | `2` | Swapchain images added beyond what the application asked for (1..8). Fewer means more presents without a companion; more means more memory |
| `AFMF_ACQUIRE_TIMEOUT_US` | `16000` | Longest the layer waits for a free swapchain image before presenting the real frame alone. `0` never waits |
| `AFMF_ASYNC` | `1` | `0` runs the work on the application's queue (diagnosis, or drivers without a spare compute queue) |
| `AFMF_INTERPOLATE` | `1` | `0` repeats the previous frame instead of interpolating (debug) |
| `AFMF_PROFILE` | `0` | `1` logs GPU time per stage every 300 frames and at teardown |
| `AFMF_DUMP_DIR` | unset | Writes the first generated frames as PPM files into that directory (8-bit formats only) |

Example, the settings closest to AMD's "high search, blend":

```sh
AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=blend <game>
```

## Limitations

- Colour only: no depth, no motion vectors, no HUD detection. Overlays and fast-moving thin
  objects show the usual optical-flow artefacts.
- The application's presented frame rate at least doubles, its input latency does not improve: the
  generated frame is what the game already rendered, halfway.
- Applications on Vulkan 1.0 get no interpolation (the block search needs subgroup operations);
  they run unmodified except for the extra swapchain images.
- 32-bit applications need a 32-bit build of the layer; none is provided.
- Swapchain formats with an interpolation variant: 8-bit RGBA/BGRA (UNORM and sRGB),
  A2B10G10R10, and RGBA16F. Others fall back to pass-through, logged at info level.
- Frame pacing depends on the presentation mode: in FIFO the companion and the real frame take
  consecutive refresh slots; in mailbox or immediate mode the compositor may drop the companion when
  the presented rate exceeds the refresh rate.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

`headless` and `headless_performance` run the layer under the Khronos validation layer on a
headless surface, present 120 frames of a synthetic moving square, require 119 generated frames
and check that the generated frames show the square exactly halfway between the real ones.
`tests/smoke.sh` opens `vkcube` with the layer enabled implicitly and once more with it disabled.

A sanitizer build is one option away: `cmake -S . -B build-asan -DAFMF_SANITIZE=ON`.

## Licence and credits

MIT, see `LICENSE`. The optical flow under `shaders/fidelityfx/` is AMD's FidelityFX SDK, vendored
unmodified under its MIT licence; `shaders/fidelityfx/NOTICE.md` records the exact tag and commit.
"AMD", "Fluid Motion Frames" and "FidelityFX" are trademarks of Advanced Micro Devices, Inc.; this
project is not affiliated with or endorsed by AMD.
