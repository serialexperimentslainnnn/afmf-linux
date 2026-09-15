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
| The present flow: take the spare, record, submit, hand off | `src/swapchain.c` `present_generated`, `spare_take` | Returns after the submit; the results of earlier presents come back deferred |
| Presentation thread: two presents, pacing hold, spare refill | `src/swapchain.c` `presenter_main`, `present_one`, `job_from_chain` | One per swapchain, only when `sc->async`; the real frame waits `hold_ns` = half the EMA frame time (`AFMF_PACING`) |
| The application's acquire, serialised with the thread | `src/swapchain.c` `afmf_swapchain_acquire` | 1 ms slices under `wsi_lock` |
| vkDeviceWaitIdle with threads in flight | `src/layer.c` `afmf_DeviceWaitIdle`, `afmf_swapchain_drain_all` | Drains every presenter, then idles under `async_lock` |
| Host time the game's thread spends in the layer | `src/swapchain.c` `update_cadence` | Every 300 presents with `AFMF_PROFILE=1`: real fps, hook/fence/acquire/each present/refill us |
| Which window system a surface is (Wayland, X11, headless) | `src/layer.c` `surface_hooks` | Logged at INFO; generic signature, no platform headers |
| The layer's own compute queue (family choice, extra queue request) | `src/layer.c` `choose_async_family`, `queues_with_extra` | Stamped with `pfnSetDeviceLoaderData` like command buffers |
| Sharing the application's last compute queue when none is spare | `src/layer.c` `choose_shared_family`, `afmf_Queue*` hooks, `afmf_device_queue_present` | vkd3d-proton takes all 4 of RADV's; the application's submits on that queue go through `async_lock` |
| Why a swapchain falls back to pass-through | `src/swapchain.c` `generation_blocker` | Logged at INFO with the reason |
| Swapchain creation patch (extra images, transfer usage) | `src/swapchain.c` `afmf_swapchain_create` | |
| Loader plumbing, dispatch tables, hooked functions | `src/layer.c` | `instance_hooks` / `device_hooks` / `swapchain_hooks` |
| Next-layer function pointers, per-device state | `src/layer.h` | `struct afmf_device`, `struct afmf_device_fns` |
| Environment configuration | `src/config.c` | Parsed once via `pthread_once`; never log from `init()` |
| Logging | `src/log.h` | `AFMF_ERR/WARN/INFO/DEBUG`; rate-limit anything per frame |
| Layer manifest (name, enable/disable env vars) | `layer/afmf-linux.json.in` | Generated twice: build tree path and install path |
| Build flags, shader compilation, sanitizers, tests, install | `CMakeLists.txt` | `afmf_glsl()`, `AFMF_WARNINGS`, `AFMF_SANITIZE`, `add_test(headless)`, `install()` |
| SPIR-V embedding | `cmake/embed_spirv.cmake` | `.spv` -> `uint32_t` arrays in `build/shaders/afmf_spirv.h` |
| Headless integration test (validation, generation count, golden check) | `tests/headless.c` | Synthetic sliding square; reads the layer's PPM dumps; `AFMF_TEST_ALL_QUEUES=1` takes every compute queue like vkd3d-proton (ctest `headless_shared_queue`); `AFMF_TEST_FRAMES`, `AFMF_TEST_FRAME_MS` pace it like a game to see the pacing hold |
| Real-window smoke test | `tests/smoke.sh` | vkcube, implicit enable via `AFMF_ENABLE=1`, negative control |

## Structure
- `README.md` — public front page (search-friendly headline, status, FAQ, configuration table).
  Keep its table, `docs/configuration.md` and this map's in step.
- `docs/` — GitHub Pages site (Jekyll, GitHub's plugin allowlist only, no theme, no JS):
  `index/install/configuration/performance/faq.md`, `_layouts/default.html`,
  `_includes/structured-data.html` (JSON-LD), `assets/`, `PUBLISHING.md` (owner's checklist).
- `packaging/` — `build-package.sh tarball|rpm|deb|arch`, `install.sh` (tarball, user install),
  `rpm/afmf-linux.spec`, `debian/`, `arch/PKGBUILD` + `.SRCINFO`, `check-versions.sh`,
  `changelog-section.sh`, `release-key.sh` (creates/certifies/uploads the release key) and
  `afmf-linux-release-key.asc` (public key only).
- `.github/` — `workflows/ci.yml`, `workflows/release.yml`, `dependabot.yml`, issue template.
- `CHANGELOG.md`, `CONTRIBUTING.md`, `SECURITY.md`.
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
| `AFMF_ASYNC` | `1` | `0` keeps the work on the application's queue and presents inline (diagnosis, fallback) |
| `AFMF_PACING` | `1` | `0` presents the real frame right behind the generated one (no hold) |

## Performance register
Method: `AFMF_TEST_EXTENT=3440x1440 AFMF_PROFILE=1 ./build/afmf_headless`, GPU timestamps per
stage (`profiler_*` in `src/framegen.c`), RX 9070 XT, RADV Mesa 26.1.8. Re-measure when the
shaders, the driver or the resolution change; review this table with every optimisation commit.

| Date | Build | Total GPU/frame | Where it goes | Note |
|---|---|---|---|---|
| 2026-09-15 | `631b0c0` + profiler | 1222 us | block search 85 % (1038 us); everything else < 40 us each | Baseline; all on the application's queue |
| 2026-09-15 | async queue | 1222 us (unchanged) | same | Work and presents moved to the layer's compute queue (RADV family 1); headless host critical path 5.58 -> 4.82 ms median of 3 (host is upload-bound, not a game proxy) |
| 2026-09-15 | performance mode | 493 us on the app queue (quality 1224) | search 331 us (67 %) | Flow at half resolution; golden test identical. On the compute queue the same work reads 1395 us (quality 4329) of wall time: the ACE shares the GPU with graphics and the idle host lowers clocks; the host's own frame is still shorter with async (4.24 vs 4.70 ms) |
| 2026-09-15 | presentation thread, in game | MH Wilds Native AA, HDR10 swapchain (format 64), X11: hook **76-91 us** (record ~40, submit ~35), pacing hold 3.7-4.3 ms, 26380/26381 generated, real fps median 132 / p10 108 / min 57; MangoHud 250 moving, 271 still; three visible stutters in a session | thread: ~270 us per present, 30 us refill | Lain: "better than Windows" (his table has no Native AA row; FSR Quality was 240-280 there). GPU 507 us with `high`. The stutters are unattributed: no `no free image` on the game swapchain |
| 2026-09-15 | presentation thread + pacing | MH Wilds (Wayland, mailbox forced): hook 546 us of which the two presents 444; vkcube after: hook 21 us (record 4, submit 9), presents on the thread | game thread: fence, spare, record, submit only | Both presents, the refill and the half-frame hold run on a thread per swapchain. Headless at 6 ms frames: hold 3.44 ms, 399/400 generated, validation (thread safety included) and ASan clean. `record 471` in the ASan+validation line is instrumentation, not the layer |
| 2026-09-15 | shared queue, in game | MH Wilds log with the shared queue: 18199/18200 generated, 0 no free image, hook 425-719 us of which the two presents 370-630 (vkcube on the same desktop: 16 us) | present path, FIFO (mode 2) | Timers now split companion / real present / refill, and surfaces log their platform, to tell XWayland from Wayland and blit swapchains from direct ones |
| 2026-09-15 | shared compute queue | MH Wilds (vkd3d-proton) log: the layer had **never** had its own queue there, the game takes all 4 compute queues; 517 us of GPU work ran on the graphics queue in series with rendering, plus 340-590 us of host time in the two FIFO presents | now: the game's compute queue 3, shared under `async_lock` | Headless at 3440x1440 with all queues taken: 119/120 generated, validation clean, 602 us wall on the shared queue |
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
| Packaging checks (versions, changelog section, tarball) | IDE configuration `Packaging: checks` (the IDE guard refuses the scripts from the shell tool) | 2026-09-15 |
| RPM + tarball install test | IDE configuration `Packaging: rpm + install test`, then `Packaging: uninstall + clean` | 2026-09-15 |
| Release | `git tag -s vX.Y.Z && git push origin vX.Y.Z` after the local gates; `docs/PUBLISHING.md` | Not yet run |
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
- **vkd3d-proton asks for every queue of every family** (4 compute on RADV), so `choose_async_family`
  finds no spare in DX12 titles; the layer then shares the application's last compute queue and
  hooks `vkQueueSubmit`/`Submit2`/`Submit2KHR`/`BindSparse`/`WaitIdle`/`PresentKHR` to serialise the
  application's use of that one queue with its own (`async_lock`). `AFMF_LOG=3` prints what the
  application asked for. Debug-label queue calls are not hooked: harmless races on a label.
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
- **Lock order with the presentation thread**: `wsi_lock` (swapchain) before `async_lock`
  (queue); `dev->lock` before `job_lock`; the thread never takes `dev->lock` (vkDeviceWaitIdle
  drains it while holding `dev->lock`). Thread-side counters live under `job_lock`.
- **What the thread can carry from the application's present chain**: `VkPresentIdKHR` (real frame
  only, ids must increase), `VkSwapchainPresentModeInfoEXT` (both), `VkPresentRegionsKHR` (dropped,
  a hint). Anything else (a present fence, display timing) makes that present inline, after a
  drain, so order is kept. Present results are deferred to the next present call; OUT_OF_DATE also
  reaches the application through its acquire.
- **The application's acquire runs in 1 ms slices under `wsi_lock`**: holding the lock across a
  blocking acquire would stall the thread whose presents free the images (deadlock in FIFO with
  few images).
- **`vkAcquireNextImageKHR` with a fence, not a semaphore, for the spare**: a binary semaphore
  cannot be re-acquired until the submit that waited on it has run, and the spare lives across
  slots; the fence is waited on the host (already signalled a frame later) and reset.
- **Pacing**: without the hold, the companion goes out microseconds before the real frame and in
  MAILBOX/IMMEDIATE the compositor drops it whenever the presented rate exceeds the refresh rate
  (the counter doubles, the eye sees the real frames). The hold (half the EMA frame time, clamped
  0.5-20 ms, `AFMF_PACING`) is the half-frame of latency AMD documents (4-5 ms at 120 fps). A
  FIFO game at the refresh rate still gets no companions: no free image.
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

- **`.gitignore` is an allowlist**: a new top-level file or directory must be added there or git
  never sees it.
- **GitHub Pages runs only its plugin allowlist** (seo-tag, sitemap, feed, remote-theme…); the site
  cannot be built locally here (Ruby 4, no github-pages gem, the IDE guard refuses containers), so
  the first Pages build after a push is the check. The social preview image is uploaded by hand.
- **CI cannot run the GPU tests**; ctest/ASan/smoke are a local gate before every tag.
- **`VK_EXT_present_timing` is `#ifdef`'d** so packages build against older headers (Ubuntu 24.04);
  the minimum is 1.3.250, checked by CMake.
- **The release private key is never in the tree**: `packaging/out/` is ignored and
  `release-key.sh` shreds the export after uploading it to the `release` environment secrets.

## Out of the map
`build*/`, `cmake-build-*/`, `.idea/`, `packaging/out/`, `docs/_site/`.
