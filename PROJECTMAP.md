# Map of AFMF_Linux

> Generated 2026-09-15 against `177eaf6`. If anything here does not match the repo, **the
> repo wins**: fix the line and move on. Maintained per the `project-map` skill.

An open equivalent of AMD Fluid Motion Frames for Linux: a **Vulkan implicit layer**
(`VK_LAYER_AFMF_frame_generation`) that will interpolate frames from colour only. No kernel code, no
Mesa patch. Phase 1 (this tree): loader plumbing + pass-through with per-swapchain bookkeeping.

## I want to change… → go to…
| To… | Go to | Note |
|---|---|---|
| What happens on present / swapchain create | `src/swapchain.c` | `afmf_swapchain_present` is the hot path; frame generation lands here |
| Loader plumbing, dispatch tables, which functions are hooked | `src/layer.c` | `instance_hooks` / `device_hooks` / `swapchain_hooks` tables near the end |
| The shared per-device state | `src/layer.h` | `struct afmf_device` |
| Environment configuration (`AFMF_LOG`, future `AFMF_*`) | `src/config.c` | Parsed once via `pthread_once`; never log from `init()` |
| Logging | `src/log.h` | `AFMF_ERR/WARN/INFO/DEBUG`; rate-limit anything per frame |
| Layer manifest (name, enable/disable env vars) | `layer/VkLayer_AFMF.json.in` | `library_path` filled by CMake with the built `.so` |
| Build flags, sanitizers, tests | `CMakeLists.txt` | `AFMF_WARNINGS`, `AFMF_SANITIZE`, `add_test(headless)` |
| Headless integration test (validation + sanitizer-friendly) | `tests/headless.c` | Enables the layer **by name**; `VK_EXT_headless_surface` |
| Real-window smoke test | `tests/smoke.sh` | vkcube, implicit enable via `AFMF_ENABLE=1`, negative control |

## Structure
- `src/` — the layer. `layer.c` routes, `swapchain.c` behaves; nothing else knows about the loader.
- `layer/` — manifest template.
- `tests/` — `headless.c` (ctest) and `smoke.sh` (manual, opens windows).
- `.idea/runConfigurations/` — local, not versioned: `Build: all`, `Test: headless`, `Test: smoke`, plus
  the IDE-generated `All CTest`.

## Entry points
- Loader → `vkNegotiateLoaderLayerInterfaceVersion` (`src/layer.c`), the only exported symbol.
- Application → `afmf_GetInstanceProcAddr` / `afmf_GetDeviceProcAddr` hand out the hooks.

## Commands
| What | Command | Verified |
|---|---|---|
| Configure + build | `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build` | 2026-09-15 |
| Headless test (validation on) | `ctest --test-dir build --output-on-failure` | 2026-09-15 |
| Smoke with vkcube | `BUILD_DIR=build tests/smoke.sh` | 2026-09-15 |
| Use from the build tree, any Vulkan app | `VK_ADD_IMPLICIT_LAYER_PATH=$PWD/build/layer AFMF_ENABLE=1 AFMF_LOG=2 <app>` | 2026-09-15 |
| Sanitizer build | `cmake -S . -B build-asan -G Ninja -DAFMF_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan` | Declared gap: needs `libasan`/`libubsan` installed (missing on this host) |
| Shell lint | `shellcheck -x -s bash tests/smoke.sh` | 2026-09-15 |

## Conventions and invariants
- C11 (`gnu11`), warning set in `AFMF_WARNINGS`, `-Werror` on by default (`AFMF_WERROR`).
- Every exported symbol carries the `afmf_` prefix except the loader entry point, whose name is fixed.
- Config env vars mirror the public ADLX enums: `AFMF_ALGORITHM`, `AFMF_SEARCH_MODE`,
  `AFMF_PERFORMANCE_MODE`, `AFMF_FAST_MOTION_RESPONSE` (only `AFMF_LOG` exists so far).
- Commits: Conventional Commits, signed, as `Lain <lain@digitalexperiments.dev>`.

## Minefields
- **Implicit vs explicit search paths**: `VK_ADD_LAYER_PATH` is for explicit layers only. From a build
  tree, implicit enabling (`AFMF_ENABLE=1`) needs `VK_ADD_IMPLICIT_LAYER_PATH`; enabling by name
  (`tests/headless.c`, ctest) needs `VK_ADD_LAYER_PATH`.
- **Two GPUs** in the dev box (RX 9070 XT = `renderD129`, RX 7800 XT = `renderD128`); the headless
  test picks the first device that can present, which today is the 9070 XT.
- **64-bit only** (`_Static_assert` in `src/layer.c`); 32-bit DXVK titles need a separate build.
- `vkcube --validate` is not validation-clean by itself (`VUID-vkAcquireNextImageKHR-surface-07783`):
  do not add validation checks to `tests/smoke.sh`, the headless test owns that gate.
- An installed `lsfg-vk` implicit layer in `~/.local/share/vulkan/implicit_layer.d` makes the loader
  emit a GENERAL-type error; `tests/headless.c` counts VALIDATION-type messages only for that reason.
- CLion caches run configurations in memory: editing an XML under `.idea/runConfigurations/` while the
  IDE holds the old one needs a new configuration name to take effect.

## Out of the map
`build*/`, `cmake-build-*/`, `.idea/`.
