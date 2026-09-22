# Changelog

All notable changes to afmf-linux are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- A device-level command resolved through `vkGetInstanceProcAddr` was hooked even when the
  driver below lacked it, so on a driver without core `vkQueueSubmit2` a game resolving it that
  way had every submission dropped. The instance route now answers as the device route does:
  a hook only for a command the chain below has, and nothing for a null instance.
- The hooks answered an unknown device with `VK_ERROR_INITIALIZATION_FAILED`, which
  `vkQueueSubmit`, `vkQueuePresentKHR` and the swapchain commands cannot return; they answer
  `VK_ERROR_DEVICE_LOST`.
- The registry lock taken on every submission and present of the game is a read-write lock:
  a game submitting from several threads no longer serialises them on the layer.
- The layer's queue joined a protected queue request of the game's when both were in the same
  family, which made it protected too and its handle invalid; it joins an unprotected request
  or asks for one of its own.
- When `vkCreateDevice` failed with the layer's additions (its queue, `shaderInt16`), the
  failure reached the game. The request goes down once more exactly as the game wrote it.
- The governor's one-in-two and one-in-three steps paced on the present counter, which the
  game's thread had moved past the frame being decided, so the cadence came out as pairs and
  gaps; each frame now carries its own number.
- A per-present mode list longer than the layer can carry was cut short in silence; the
  game's mode is kept instead. The FIFO-to-MAILBOX rewrite also applies on the inline path
  when the mode request heads the present chain.
- With pacing on, a hold under half a millisecond became no hold at all, and in MAILBOX two
  presents in the same instant dropped the generated frame; the minimum hold applies. The
  hold's ceiling is 40 ms, so the generated frame stays halfway down to 12.5 fps.
- The release fence of the companion's image got no time when the acquire had spent the whole
  budget; it gets a millisecond, and a companion that was there is no longer skipped.
- The HDR luma is clamped before it is stored as a byte: a pixel brighter than the peak used to
  wrap to a dark value, and an out-of-gamut scRGB value reached a cube root as a negative.
- `AFMF_DUMP_DIR` is copied out of the environment block, which Wine moves.
- A swapchain image index outside what the direct paths keep a view for is an error and
  records nothing, instead of a fallback that left the luma unwritten or copied from an image
  direct output never creates.
- The half-resolution downscale on the copy path sampled the centre of each 2x2 quad against
  the destination size, which drifts by a texel on an odd frame.

## [1.3.0] - 2026-09-22

### Changed
- The defaults ask for less of the GPU, which is where the frame rate goes when a game already
  saturates it: `AFMF_PERFORMANCE_MODE` is `auto` (the optical flow at half resolution from
  2560x1440 up, display resolution below it) and `AFMF_SEARCH_MODE` is `auto` (five pyramid
  levels when the flow runs at half resolution, which still covers 256 pixels of motion on
  screen between two frames). At 3440x1440 that is 716 us per generated frame before and 578
  after. `AFMF_PERFORMANCE_MODE=quality AFMF_SEARCH_MODE=high` restores the previous behaviour.
- `AFMF_PERFORMANCE_MODE=performance` reads the game's frame once, as full resolution already
  did: the ingest pass writes the colour ring at frame size and the luma at the flow's size in
  the same pass. It used to copy the whole frame, downscale the copy and take the luma from it,
  three passes over the frame instead of one.

### Fixed
- The block search wrote a trailing row or column of motion vectors outside the flow image on
  any pyramid level whose size is not a multiple of sixteen, which is most of them once the
  optical flow runs below full resolution. The driver discarded the write, so nothing was
  corrupted; the store clips now.

## [1.2.0] - 2026-09-22

### Fixed
- A block of the picture with no detail to match (fog, snow, an open sky) used to keep whatever
  vector the coarser pyramid level handed it, because there every candidate matches about as
  well as any other and the match looked good enough to skip the search. The layer then warped
  the background with the motion of whatever was next to it, which is what smeared a moving
  silhouette over its surroundings. A block now keeps a vector only with evidence for it: the
  prediction has to match at least twice as well as staying put, and so does the winner of the
  search, at every pyramid level; otherwise the block stores the zero vector. The search's cost
  is unchanged, both sums travelling in one word.

## [1.1.1] - 2026-09-22

### Added
- `touch <AFMF_DUMP_DIR>/dump-now` while a game runs writes the next four generated frames and
  their flow fields. The four a swapchain writes on its own are a loading screen in most games,
  which made the dump useless for looking at a scene.

### Fixed
- `AFMF_DUMP_DIR` creates the directory it names. It used to write nothing at all when the
  directory did not exist yet, and say so only at warning level.
- `AFMF_DUMP_DIR` also dumps 10-bit swapchains (`A2B10G10R10`), which is what games present in
  HDR; only the two 8-bit formats were written before, and a format that cannot be dumped said
  nothing. Every frame or flow file that cannot be written is now logged as an error with the
  reason.

## [1.1.0] - 2026-09-21

### Fixed
- GPU hang on Intel (ANV): the block search's cross-subgroup sums and minimums assumed two
  subgroups of 32 lanes, so on the 8- or 16-lane subgroups ANV picks the early-outs differed
  between subgroups and the group split at its next barrier. They now combine however many
  subgroups the driver chose, through shared memory indexed by subgroup id; the same on RADV.
- The scene change detector's divergence pass runs before the coarsest search again instead
  of alongside it: that search returns early on a cut, and reading the verdict while it was
  being written could split a group the same way.
- The scene change detector's shared-memory reductions ran their last six steps without
  barriers, which only holds when the first 32 lanes are one subgroup; on Intel's 8- and
  16-lane subgroups a lane could read a partial sum before it was written, so the detector's
  value was wrong. Every step now has a barrier.
- Generation checks what the flow shaders need before starting instead of failing inside them:
  subgroup basic, arithmetic and quad operations in compute, and storage support for the
  `r8ui` luma and `rg16i` flow images; a device without them gets a log line and repeated
  frames. The device line at debug level shows the subgroup size and operations.

## [1.0.0] - 2026-09-16

### Added
- `AFMF_PRESENT_MODE` (`auto`): a swapchain created in FIFO is created in MAILBOX when the surface
  offers it, the allowed per-present modes (`VK_EXT_swapchain_maintenance1`) get MAILBOX added,
  and a per-present switch back to FIFO is rewritten. In FIFO the doubled presents each took a
  refresh slot: vkcube at 165 Hz got 2 companions in 1196 presents, now 595 in 596. `keep`
  leaves the game's mode alone.
- `AFMF_STATIC_BLOCK_SAD` (`128`): the block search skips blocks that did not change between the
  two frames (their SAD at rest, which the SDK already computed for its level-0 fallback, is taken
  first); still parts of the picture cost nothing to search. One marked edit in the vendored
  search shader, listed in `shaders/fidelityfx/NOTICE.md`.
- The scene change detector's histogram runs alongside the luma pyramid and its divergence
  alongside the coarsest search, with no barrier in between (both only read the level-0 luma;
  the coarsest search reads the detector's output without waiting, which every finer level and
  the interpolator still do), and the block search checks a group's four predictions at once
  before any of them searches.
- `AFMF_SAD_INT16` (on): the block search's sum of absolute differences runs on packed 16-bit
  byte pairs (`v_pk_*` on RDNA) instead of one byte at a time, from a second build of the SDK's
  search pass; the layer enables `shaderInt16` at device creation when the game did not and the
  device offers it. Test `headless_pan_scalar_sad` checks the SDK's sum gives the same field.
- `AFMF_DIRECT_INGEST` (on): the game's frame is read once, from its swapchain image, into the
  colour ring and the flow's luma by one pass, instead of copied and then read again by the
  SDK's luma pass. Not on sRGB swapchains.
- `AFMF_DIRECT_OUTPUT` (off): the interpolator writes the swapchain image itself instead of an
  internal image copied there; opt-in because it needs storage usage on the game's swapchain
  images, whose cost to the game's rendering only a game measurement can tell. Test
  `headless_direct_output`.
- `AFMF_HUD_DETECT` (on): a pixel unchanged between the two frames on a moving block is kept as
  it is instead of warped, so a HUD, crosshair or subtitle over motion stays whole in the
  generated frame. Tests `headless_hud` and its negative control `headless_hud_negative`.
- Defaults now aim at quality: flow at display resolution with all seven pyramid levels
  (`AFMF_PERFORMANCE_MODE=quality`, `AFMF_SEARCH_MODE=high`), `AFMF_FAST_MOTION_RESPONSE=blend`,
  `AFMF_LOG=0`. The block search skips every block whose vector from the coarser level already
  matches (`AFMF_STATIC_BLOCK_SAD`), which is what keeps the cost of the full search near the
  previous release's half-resolution one.
- `AFMF_GOVERNOR` (on by default): under GPU contention, when the generated frame is ready later
  than a frame and a half thirty frames in a row, generation steps down (five search levels, then
  one companion in two, then one in three) and steps back up after 15 frames ready within three
  quarters of a frame; each step is logged. `AFMF_MIN_FPS` (30): no companions below that real frame rate.
- Present ids (`VK_KHR_present_id` and `present_id2`, what DXVK attaches) are carried on the real
  frame by the presentation thread instead of forcing the present inline; tests `headless_present_id1`
  and `headless_present_id2`.

### Changed
- The present hook no longer records or submits: it consumes the game's wait semaphores with
  an empty submission (they must be waited before the game signals them again) and queues the
  frame to a work thread, which waits the slot, takes the companion's image, records, submits and
  hands the presents to the presentation thread. The profile line splits the three: `in the
  hook`, `work thread`, `presentation thread`. Test `headless_async_drain` destroys the swapchain
  with frames still in both threads.
- The layer's pipelines compile on a thread of their own from vkCreateDevice; the first
  swapchain only waits for what is left instead of compiling everything on the first present.
- The optical flow and the interpolation are recorded once per (slot, parity, companion) into
  secondary command buffers and executed from then on; the per-frame primary keeps only the
  copies from and to the swapchain images. Host time recording a frame in the present hook:
  170-190 us to 80-117 us in the headless test under the validation layer.
- Pacing measures the half-frame hold from the moment the generated frame is ready on the GPU, not
  from the present call, capped at one frame after it; the profile line shows the delay as `gpu done`.

### Fixed
- The headless golden test looked at the companions of real frames 1 and 2, which the optical
  flow's scene change detector zeroes during its six-frame warm-up: what it measured was a
  motionless blend of the two frames, whose centre is the same. Dumps now start at frame 8 and
  the test also checks the flow vectors on the square (`afmf_flow_<frame>.txt`); a failed check
  now fails the test (`FAIL_REGULAR_EXPRESSION` matched a `^` that CMake does not treat as a
  line start).
- The application's `vkGetSwapchainImagesKHR` is serialised with the presentation thread's presents
  (a thread-safety validation error when both ran at once).
- The presentation thread could starve on the swapchain lock behind an application spinning in
  `vkAcquireNextImageKHR` (a glibc mutex is not fair): presents stalled for seconds and no image
  ever came free. The lock is now taken in turn, and the spare's release fence is waited on outside
  it.
- The pacing hold ends when the next real frame arrives, and a single hitch (a 100 ms frame) no
  longer inflates the frame-time estimate: after one, the real frames were held up to 20 ms each,
  images ran out and `AFMF_MIN_FPS` withheld companions for a while.
- `AFMF_ACQUIRE_TIMEOUT_US` also bounds the acquire of the companion's image, not only its release
  fence; frames withheld by `AFMF_MIN_FPS` are counted in the swapchain's report.

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

[Unreleased]: https://github.com/serialexperimentslainnnn/afmf-linux/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v1.2.0
[1.1.1]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v1.1.1
[1.1.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v1.1.0
[1.0.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v1.0.0
[0.4.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v0.4.0
[0.3.0]: https://github.com/serialexperimentslainnnn/afmf-linux/releases/tag/v0.3.0
[0.2.0]: https://github.com/serialexperimentslainnnn/afmf-linux/compare/9535e06...2f442f5
[0.1.0]: https://github.com/serialexperimentslainnnn/afmf-linux/commit/9535e06
