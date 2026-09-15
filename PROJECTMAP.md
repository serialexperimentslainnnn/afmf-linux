# Map of afmf-linux

> Generated 2026-09-15 against `9535e06`+phase 3. If anything here does not match the repo, **the
> repo wins**: fix the line and move on. Maintained per the `project-map` skill.

An open equivalent of AMD Fluid Motion Frames for Linux: a **Vulkan implicit layer**
(`VK_LAYER_AFMF`, package `afmf-linux`) that interpolates frames from colour only. No kernel code,
no Mesa patch. Every application present gets a companion frame presented in front of it:
FidelityFX Optical Flow (vendored, MIT) estimates 8x8 block motion between the previous and the new
frame, and the layer's own compute shader synthesises the frame in between. Swapchains whose format
has no interpolation variant fall back to repeating the previous frame.

## I want to change… → go to…
| To… | Go to | Note |
|---|---|---|
| The interpolation maths (warp, fallback, block filtering) | `shaders/afmf_interpolate.comp` | 4 output-format variants compiled by CMake |
| Per-stage GPU timing | `src/framegen.c` `profiler_*` | Query pool per slot, read on slot reuse, never blocks |
| Half-resolution flow input | `shaders/afmf_downsample.comp`, `src/framegen.c` `PASS_DOWNSAMPLE` | Compute, not vkCmdBlitImage: blits need a graphics queue |
| Optical flow sequencing, resources, descriptor sets | `src/framegen.c` | `afmf_framegen_record` = 1 copy + 7 FFX passes (x levels) + interpolate |
| Which swapchain formats interpolate and how | `src/framegen.c` `describe_format` | R8G8B8A8, B8G8R8A8 (+sRGB), A2B10G10R10, R16G16B16A16F |
| FidelityFX sources (never edited) | `shaders/fidelityfx/` | `NOTICE.md` has tag, commit and mapping |
| What is recorded per frame around framegen | `src/swapchain.c` `record_frame` | Image i arrives PRESENT_SRC and leaves PRESENT_SRC |
| The present flow: take the spare, submit, two presents, refill the spare | `src/swapchain.c` `present_generated`, `spare_take`, `spare_refill` | On `dev->async_queue` when the swapchain is `sc->async`; never waits for the presentation engine (`AFMF_ACQUIRE_TIMEOUT_US=0`) |
| Host time the game's thread spends in the layer | `src/swapchain.c` `update_cadence` | Every 300 presents with `AFMF_PROFILE=1`: real fps, hook/fence/acquire/present us |
| The layer's own compute queue (family choice, extra queue request) | `src/layer.c` `choose_async_family`, `queues_with_extra` | Stamped with `pfnSetDeviceLoaderData` like command buffers |
| Why a swapchain falls back to pass-through | `src/swapchain.c` `generation_blocker` | Logged at INFO with the reason |
| Swapchain creation patch (extra images, transfer usage) | `src/swapchain.c` `afmf_swapchain_create` | |
| Loader plumbing, dispatch tables, hooked functions | `src/layer.c` | `instance_hooks` / `device_hooks` / `swapchain_hooks` |
| Next-layer function pointers, per-device state | `src/layer.h` | `struct afmf_device`, `struct afmf_device_fns` |
| Environment configuration | `src/config.c` | Parsed once via `pthread_once`; never log from `init()` |
| Logging | `src/log.h` | `AFMF_ERR/WARN/INFO/DEBUG`; rate-limit anything per frame |
| Layer manifest (name, enable/disable env vars) | `layer/afmf-linux.json.in` | Generated twice: build tree path and install path |
| Build flags, shader compilation, sanitizers, tests, install | `CMakeLists.txt` | `afmf_glsl()`, `AFMF_WARNINGS`, `AFMF_SANITIZE`, `add_test(headless)`, `install()` |
| SPIR-V embedding | `cmake/embed_spirv.cmake` | `.spv` -> `uint32_t` arrays in `build/shaders/afmf_spirv.h` |
| Headless integration test (validation, generation count, golden check) | `tests/headless.c` | Synthetic sliding square; reads the layer's PPM dumps |
| Real-window smoke test | `tests/smoke.sh` | vkcube, implicit enable via `AFMF_ENABLE=1`, negative control |

## Structure
- `README.md` — user documentation: build, install, usage, the configuration table, limitations.
  Keep its table and this map's in step.
- `src/` — the layer. `layer.c` routes, `swapchain.c` behaves; nothing else knows about the loader.
- `layer/` — manifest template.
- `shaders/` — `afmf_interpolate.comp` (ours) and `fidelityfx/` (vendored Optical Flow: core headers,
  `opticalflow/`, `spd/`, `passes/*.glsl`).
- `cmake/` — build helpers.
- `tests/` — `headless.c` (ctest) and `smoke.sh` (opens windows; run it from the IDE configuration).
- `.idea/runConfigurations/` — local, not versioned: `Build: all`, `Test: headless`, `Test: smoke`, plus
  the IDE-generated `All CTest`.

## Entry points
- Loader → `vkNegotiateLoaderLayerInterfaceVersion` (`src/layer.c`), the only exported symbol.
- Application → `afmf_GetInstanceProcAddr` / `afmf_GetDeviceProcAddr` hand out the hooks.

## Configuration (environment)
| Variable | Default | Meaning |
|---|---|---|
| `AFMF_ENABLE=1` | unset | Loads the layer implicitly (manifest `enable_environment`) |
| `DISABLE_AFMF=1` | unset | Keeps it out even if enabled |
| `AFMF_LOG` | `1` | 0 error, 1 warn, 2 info, 3 debug (stderr) |
| `AFMF_EXTRA_IMAGES` | `2` | Swapchain images added beyond what the app asked (1..8) |
| `AFMF_ACQUIRE_TIMEOUT_US` | `0` | Longest wait for the spare image's release before presenting without a companion; the spare is acquired a frame ahead (`spare_*`) |
| `AFMF_INTERPOLATE` | `1` | `0` repeats the previous frame instead of interpolating (debug) |
| `AFMF_SEARCH_MODE` | `auto` | `standard` = 5 pyramid levels, `high` = 7; `auto` = 7 at full flow resolution, 5 at half (`fg->levels`) |
| `AFMF_FAST_MOTION_RESPONSE` | `repeat` | `repeat` or `blend` for pixels the flow cannot trust (scene change, > 64 px) |
| `AFMF_DUMP_DIR` | unset | Writes the first 4 generated frames as `afmf_generated_<n>.ppm` (8-bit formats) |
| `AFMF_PROFILE` | `0` | `1` (or `AFMF_LOG=3`) logs GPU time per stage every 300 frames and at teardown |
| `AFMF_PERFORMANCE_MODE` | `auto` | `quality` = flow at display resolution, `performance` = at half (16 px blocks); `auto` = performance from 2560x1440 up |
| `AFMF_ASYNC` | `1` | `0` keeps the work on the application's queue (diagnosis, fallback) |

## Performance register
Method: `AFMF_TEST_EXTENT=3440x1440 AFMF_PROFILE=1 ./build/afmf_headless`, GPU timestamps per
stage (`profiler_*` in `src/framegen.c`), RX 9070 XT, RADV Mesa 26.1.8. Re-measure when the
shaders, the driver or the resolution change; review this table with every optimisation commit.

| Date | Build | Total GPU/frame | Where it goes | Note |
|---|---|---|---|---|
| 2026-09-15 | `631b0c0` + profiler | 1222 us | block search 85 % (1038 us); everything else < 40 us each | Baseline; all on the application's queue |
| 2026-09-15 | async queue | 1222 us (unchanged) | same | Work and presents moved to the layer's compute queue (RADV family 1); headless host critical path 5.58 -> 4.82 ms median of 3 (host is upload-bound, not a game proxy) |
| 2026-09-15 | performance mode | 493 us on the app queue (quality 1224) | search 331 us (67 %) | Flow at half resolution; golden test identical. On the compute queue the same work reads 1395 us (quality 4329) of wall time: the ACE shares the GPU with graphics and the idle host lowers clocks; the host's own frame is still shorter with async (4.24 vs 4.70 ms) |
| 2026-09-15 | spare image, no acquire wait | host: 5,900 -> 60 us per present in the hook (vkcube, FIFO 165 Hz); GPU unchanged | acquire wait was 5,100-5,900 us of it | The companion's image is acquired a frame ahead with timeout 0 and its release fence waited on the host at use time. In mailbox/immediate vkcube generates 995 of 996 with a 40 us hook; in FIFO at the refresh rate it generates nothing, which is right. Explains MH Wilds: base 120 real fps, 92 with the layer = 2.5 ms per frame lost, 0.43 of them GPU |
| 2026-09-15 | 5 levels at half + scoped barriers | 434 us (433-445, N=3) | search 298 us (69 %) | Two coarsest levels dropped in `auto` at half resolution (they searched +-256/+-512 screen px for 46 us), compute-only barriers between passes, none between pyramid and SCD histogram. Measured and rejected: wave32 for compute (`RADV_PERFTEST=cswave32`: search unchanged, total +17 us); native SAD (`v_sad_u8`/`v_msad_u8`) is unreachable from GLSL, ACO emits neither for any SAD shape |

## Commands
| What | Command | Verified |
|---|---|---|
| Configure + build (needs `glslang`; `spirv-tools` optional) | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build` | 2026-09-15 |
| Headless test (validation on, 119/120 generated, square halfway in dumps 1-2) | `ctest --test-dir build --output-on-failure` | 2026-09-15 |
| Smoke with vkcube | `BUILD_DIR=build tests/smoke.sh` (or the `Test: smoke` IDE configuration) | 2026-09-15 |
| Use from the build tree, any Vulkan app | `VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer AFMF_ENABLE=1 AFMF_LOG=2 <app>` | 2026-09-15 |
| Install system-wide | `cmake --install build --prefix /usr/local` (then just `AFMF_ENABLE=1 <app>`) | Declared gap: not run |
| Sanitizer build (ASan+UBSan+LSan) | `cmake -S . -B build-asan -G Ninja -DAFMF_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan` | 2026-09-15 |
| Shell lint | `shellcheck -x -s bash tests/smoke.sh` | 2026-09-15 |
| Static analysis | `cc -fanalyzer -std=gnu11 -D_POSIX_C_SOURCE=200809L -Isrc -Wall -Wextra -Werror -c src/*.c` (one file at a time) | 2026-09-15 |

## Conventions and invariants
- C11 (`gnu11`), warning set in `AFMF_WARNINGS`, `-Werror` on by default (`AFMF_WERROR`).
- Every symbol with external linkage carries the `afmf_` prefix except the loader entry point.
- Frame generation never blocks the application beyond `AFMF_ACQUIRE_TIMEOUT_US` and never fails the
  application's present: any layer-side failure degrades to pass-through for that swapchain.
- Config env vars for the later phases mirror the public ADLX enums: `AFMF_ALGORITHM`,
  `AFMF_SEARCH_MODE`, `AFMF_PERFORMANCE_MODE`, `AFMF_FAST_MOTION_RESPONSE`.
- Commits: Conventional Commits, signed, as `Lain <lain@digitalexperiments.dev>`, no tool trailers.

## Minefields
- **Nothing graphics-only on the layer's command buffers**: they run on a compute family;
  `vkCmdBlitImage` there is a validation error and a failed submit. Copies, clears and dispatches
  are fine.
- **Never call down `vkCreateDevice` twice without restoring the chain link**: every layer below
  advances `VkLayerDeviceCreateInfo::u.pLayerInfo`; a retry (used when high queue priority is
  refused) must reset it to the link this layer handed down, or the next layer dereferences NULL.
- **High global priority for the layer's queue needs `CAP_SYS_NICE`** (amdgpu's rule for anything
  above NORMAL); a regular game process gets the fallback and the log says `(normal priority)`.
- **Async queue semantics**: swapchains are re-created `CONCURRENT` across the application's
  families and ours so images i/j need no ownership transfers; both presents happen on our queue
  (RADV reports present support on its compute family; checked per surface, fallback is the
  application's queue). `async_lock` serialises our queue because applications present from
  several threads. `gen_teardown` idles our queue before freeing anything.
- **Shaders target Vulkan 1.1 / SPIR-V 1.3 on purpose**: a layer's modules are validated against the
  *application's* API version, and 1.2 modules failed spirv-val inside a 1.1 app. Applications on
  Vulkan 1.0 get no interpolation (the search pass needs subgroups); they get repeated frames.
- **`PASS_REGULAR_EXPRESSION` makes CTest ignore the exit code**: `FAIL_REGULAR_EXPRESSION` on
  "validation error" is what keeps the headless test honest. A green ctest with errors in its
  output happened once.
- **Flow convention**: `prev = cur + v` in luma pixels, one vector per 8x8 block; the interpolator
  samples `cur(y - v/2)` and `prev(y + v/2)`. The golden test pins the sign.
- **Optical flow reads 8/10-bit values as-is** (transfer function 0), so block matching happens on
  encoded values; fine for SAD, wrong if anything downstream assumes linear light.
- **Command buffers allocated by the layer need `pfnSetDeviceLoaderData`** (`VK_LOADER_DATA_CALLBACK`
  from `VkLayerDeviceCreateInfo`): they are created below the loader's trampoline, so nothing else
  stamps the dispatch pointer, and the validation layer aborts on the first use. Same for any VkQueue
  the layer would obtain itself.
- **Implicit vs explicit search paths**: `VK_ADD_LAYER_PATH` is for explicit layers only. From a build
  tree, implicit enabling (`AFMF_ENABLE=1`) needs `VK_ADD_IMPLICIT_LAYER_PATH`; enabling by name
  (`tests/headless.c`, ctest) needs `VK_ADD_LAYER_PATH`.
- **Free images come back one refresh period late** on Wayland/FIFO (measured on a 165 Hz KDE
  desktop), and **waiting for one in the present hook costs the game that period**: 5.1-5.9 ms per
  present at 165 Hz. The companion's image is therefore acquired a frame ahead, never waited for.
  A FIFO game already at the refresh rate gets no companions (no free image, by design); mailbox
  and immediate get nearly all. Headless surfaces signal the release asynchronously, which is why
  ctest sets `AFMF_ACQUIRE_TIMEOUT_US=16000`: it checks generation, not the never-stall policy.
- **`vkAcquireNextImageKHR` with a fence, not a semaphore, for the spare**: a binary semaphore
  cannot be re-acquired until the submit that waited on it has run, and the spare lives across
  slots; the fence is waited on the host (already signalled a frame later) and reset.
- **Pacing is the display's, not ours**: in FIFO the companion and the real frame take consecutive
  refresh slots, so the cadence is even only when the refresh rate is a multiple of the game's frame
  rate (cap the game at half the refresh). In MAILBOX/IMMEDIATE the companion is presented
  microseconds before the real frame, so whenever the presented rate exceeds the refresh rate the
  compositor mostly drops the companion: the counter doubles, the eye sees the real frames. Half-
  frame-time pacing (delay the real frame by half the measured frame time from a presentation
  thread, the latency AMD's AFMF also pays) is the fix; not built yet.
- **Not done on purpose, with the numbers**: writing the interpolator straight into the swapchain
  image (saves the 19 us output copy) needs `STORAGE` usage on the swapchain images, which can cost
  the *game's* rendering (compression) more than 19 us on a queue that is already off its critical
  path; only a game measurement can decide it. Reading the swapchain image directly for the flow
  saves nothing: the history copy is the same bytes.
- **Two GPUs** in the dev box (RX 9070 XT = `renderD129`, RX 7800 XT = `renderD128`); the headless
  test picks the first device that can present, which today is the 9070 XT.
- **64-bit only** (`_Static_assert` in `src/layer.c`); 32-bit DXVK titles need a separate build.
- `vkcube --validate` is not validation-clean by itself (`VUID-vkAcquireNextImageKHR-surface-07783`):
  do not add validation checks to `tests/smoke.sh`, the headless test owns that gate.
- An installed `lsfg-vk` implicit layer in `~/.local/share/vulkan/implicit_layer.d` loads into every
  Vulkan process and logs errors; `tests/headless.c` counts VALIDATION-type messages only for that
  reason. Remember it when reading logs.
- The IDE's security guard reads scripts: no `env`, `timeout`, `LD_PRELOAD` or `#!/usr/bin/env`
  shebang in anything under `tests/`, and no `->`, `//` or `/*`-leading fragments in shell one-liners.
- CLion caches run configurations in memory: editing an XML under `.idea/runConfigurations/` while the
  IDE holds the old one needs a new configuration name to take effect. A configuration that runs a
  target must also build the layer (`EXPLICIT_BUILD_TARGET_NAME="all"`), or the manifest points at a
  `.so` that does not exist.

## Out of the map
`build*/`, `cmake-build-*/`, `.idea/`.
