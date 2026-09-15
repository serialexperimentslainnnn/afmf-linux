# Map of afmf-linux

> Generated 2026-09-15 against `64bb6f1`+phase 2. If anything here does not match the repo, **the
> repo wins**: fix the line and move on. Maintained per the `project-map` skill.

An open equivalent of AMD Fluid Motion Frames for Linux: a **Vulkan implicit layer**
(`VK_LAYER_AFMF`, package `afmf-linux`) that interpolates frames from colour only. No kernel code,
no Mesa patch. Phase 2 (this tree): every application present gets a companion frame presented in
front of it, made by copying the previous frame. Phase 3 replaces that copy with optical flow +
interpolation.

## I want to change… → go to…
| To… | Go to | Note |
|---|---|---|
| What is recorded per frame (copy today, OF+FI in phase 3) | `src/swapchain.c` `record_frame` | Image i arrives PRESENT_SRC and leaves PRESENT_SRC |
| The present flow: acquire companion, submit, two presents | `src/swapchain.c` `present_generated` | Never blocks longer than `AFMF_ACQUIRE_TIMEOUT_US` |
| Why a swapchain falls back to pass-through | `src/swapchain.c` `generation_blocker` | Logged at INFO with the reason |
| Swapchain creation patch (extra images, transfer usage) | `src/swapchain.c` `afmf_swapchain_create` | |
| Loader plumbing, dispatch tables, hooked functions | `src/layer.c` | `instance_hooks` / `device_hooks` / `swapchain_hooks` |
| Next-layer function pointers, per-device state | `src/layer.h` | `struct afmf_device`, `struct afmf_device_fns` |
| Environment configuration | `src/config.c` | Parsed once via `pthread_once`; never log from `init()` |
| Logging | `src/log.h` | `AFMF_ERR/WARN/INFO/DEBUG`; rate-limit anything per frame |
| Layer manifest (name, enable/disable env vars) | `layer/afmf-linux.json.in` | Generated twice: build tree path and install path |
| Build flags, sanitizers, tests, install | `CMakeLists.txt` | `AFMF_WARNINGS`, `AFMF_SANITIZE`, `add_test(headless)`, `install()` |
| Headless integration test (validation + generation count) | `tests/headless.c` | Enables the layer **by name**; `VK_EXT_headless_surface` |
| Real-window smoke test | `tests/smoke.sh` | vkcube, implicit enable via `AFMF_ENABLE=1`, negative control |

## Structure
- `src/` — the layer. `layer.c` routes, `swapchain.c` behaves; nothing else knows about the loader.
- `layer/` — manifest template.
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
| `AFMF_ACQUIRE_TIMEOUT_US` | `16000` | Longest wait for a free image before presenting without a companion |

## Commands
| What | Command | Verified |
|---|---|---|
| Configure + build | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build` | 2026-09-15 |
| Headless test (validation on, expects 119/120 generated) | `ctest --test-dir build --output-on-failure` | 2026-09-15 |
| Smoke with vkcube | `BUILD_DIR=build tests/smoke.sh` (or the `Test: smoke` IDE configuration) | 2026-09-15 |
| Use from the build tree, any Vulkan app | `VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer AFMF_ENABLE=1 AFMF_LOG=2 <app>` | 2026-09-15 |
| Install system-wide | `cmake --install build --prefix /usr/local` (then just `AFMF_ENABLE=1 <app>`) | Declared gap: not run |
| Sanitizer build | `cmake -S . -B build-asan -G Ninja -DAFMF_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan` | Declared gap: needs `libasan`/`libubsan` installed |
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
- **Command buffers allocated by the layer need `pfnSetDeviceLoaderData`** (`VK_LOADER_DATA_CALLBACK`
  from `VkLayerDeviceCreateInfo`): they are created below the loader's trampoline, so nothing else
  stamps the dispatch pointer, and the validation layer aborts on the first use. Same for any VkQueue
  the layer would obtain itself.
- **Implicit vs explicit search paths**: `VK_ADD_LAYER_PATH` is for explicit layers only. From a build
  tree, implicit enabling (`AFMF_ENABLE=1`) needs `VK_ADD_IMPLICIT_LAYER_PATH`; enabling by name
  (`tests/headless.c`, ctest) needs `VK_ADD_LAYER_PATH`.
- **Free images come back one refresh period late** on Wayland/FIFO (measured on a 165 Hz KDE
  desktop): with 1 extra image and no wait, 1 of 596 presents got a companion; 2 extra + 8 ms gave
  294 of 296. Hence the defaults. Headless surfaces release immediately, so ctest cannot catch this.
- **Pacing is the display's, not ours**: in FIFO the companion and the real frame take consecutive
  refresh slots, so the cadence is even only when the refresh rate is a multiple of the game's frame
  rate (cap the game at half the refresh). Half-frame-time pacing needs a presentation thread; not
  built yet.
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
