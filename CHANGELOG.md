# Changelog

All notable changes to afmf-linux are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- `AFMF_GOVERNOR` (on by default): under GPU contention, when the generated frame is ready later
  than half the frame time three frames in a row, generation steps down (five search levels, then
  one companion in two, then one in three) and steps back up once the GPU catches up; each step is
  logged. `AFMF_MIN_FPS` (30): no companions below that real frame rate.
- Present ids (`VK_KHR_present_id` and `present_id2`, what DXVK attaches) are carried on the real
  frame by the presentation thread instead of forcing the present inline; tests `headless_present_id1`
  and `headless_present_id2`.

### Changed
- Pacing measures the half-frame hold from the moment the generated frame is ready on the GPU, not
  from the present call, capped at one frame after it; the profile line shows the delay as `gpu done`.

### Fixed
- The application's `vkGetSwapchainImagesKHR` is serialised with the presentation thread's presents
  (a thread-safety validation error when both ran at once).

## [0.4.0] - 2026-09-15

### Added
- `AFMF_GAMESCOPE=1` for games under Gamescope: five extra swapchain images instead of two
  (Gamescope keeps more in flight; measured 31 % companions with two, all of them with five) and
  pass-through in the gamescope process itself, so `AFMF_ENABLE=1 AFMF_GAMESCOPE=1 gamescope --
  <game>` does the right thing on both sides. Verified with Cyberpunk 2077 under gamescope 3.16
  (35,283 of 35,285 presents got a companion); tests `headless_gamescope` and
  `headless_gamescope_passive`.
- Verified on RDNA3 (RX 7800 XT): same tests, same behaviour, about three times the GPU cost per
  frame; numbers on the performance page.
- Tested hardware and games page, with user reports (first one: NVIDIA GTX 1050 Ti, The Witcher 3).

### Changed
- Documentation: do not stack with another frame generation layer or injector (lsfg-vk hung
  presentation on the RDNA3 test system even with afmf-linux disabled; OptiScaler does not work
  together with the layer). The game's own frame generation stays on.

## [0.3.0] - 2026-09-15

First public release.

### Added
- Presentation thread per swapchain: the application's thread returns right after the layer's
  submit; both presents, the spare refill and the pacing hold run on the thread.
- Half-frame pacing (`AFMF_PACING`, on by default): the real frame is held back half the
  smoothed frame time so the generated frame lands evenly in between, the same half-frame of added
  latency AMD's implementation has.
- Shared compute queue: when the application took every compute queue (vkd3d-proton asks for all
  of RADV's), the layer works on the application's last one and serialises the application's own
  submits on it.
- Spare image acquired a frame ahead with a fence, never waited for in the present hook
  (`AFMF_ACQUIRE_TIMEOUT_US` defaults to 0).
- Host-side profile (`AFMF_PROFILE=1`): real fps, microseconds spent in the hook and on the
  thread, pacing hold; surfaces log their window system (Wayland, X11, headless).
- `vkAcquireNextImage(2)KHR` and `vkDeviceWaitIdle` hooks, serialised with the thread.
- Present chains carried to the thread: `VkPresentIdKHR`, `VkSwapchainPresentModeInfoEXT`,
  `VkSwapchainPresentFenceInfoEXT`, `VkPresentTimingsInfoEXT`; anything else presents inline.
- `AFMF_SEARCH_MODE=auto` picks 5 pyramid levels at half flow resolution, 7 at full.
- Headless test variants: shared queue (`AFMF_TEST_ALL_QUEUES`), paced presents
  (`AFMF_TEST_FRAME_MS`, `AFMF_TEST_FRAMES`).

### Changed
- Barriers between compute passes no longer name the transfer stage; the pyramid and the scene
  change histogram run without a barrier between them (493 -> 434 us per frame at 3440x1440).

## [0.2.0] - 2026-09-15

### Added
- Optical flow (FidelityFX Optical Flow 1.1.2, vendored, MIT) between the previous and the new
  frame, and an interpolation shader that synthesises the frame in between; fallback to repeat or
  blend where the flow cannot be trusted (`AFMF_FAST_MOTION_RESPONSE`).
- Frame generation on a compute queue of the layer's own, with `VK_KHR_global_priority` HIGH
  requested when the kernel allows it.
- Performance mode (`AFMF_PERFORMANCE_MODE`): optical flow at half resolution, automatic from
  2560x1440 up.
- GPU timestamps per stage (`AFMF_PROFILE=1`).
- Interpolation variants for RGBA8/BGRA8 (UNORM and sRGB), A2B10G10R10 and RGBA16F swapchains.

## [0.1.0] - 2026-09-15

### Added
- Vulkan implicit layer `VK_LAYER_AFMF` (`AFMF_ENABLE=1` / `DISABLE_AFMF=1`) that presents a
  companion frame in front of every application frame, with extra swapchain images.
- Headless integration test under the Khronos validation layer, vkcube smoke test, sanitizer
  build, GCC `-fanalyzer` and ShellCheck gates.

[Unreleased]: https://github.com/serialexperimentslainnnn/afmf-linux/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v0.4.0
[0.3.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v0.3.0
[0.2.0]: https://github.com/serialexperimentslainnnn/afmf-linux/compare/9535e06...2f442f5
[0.1.0]: https://github.com/serialexperimentslainnnn/afmf-linux/commit/9535e06
