---
title: Performance
description: Measured cost of afmf-linux, the open AMD Fluid Motion Frames layer for Linux, per GPU stage and per frame, and frame rates reached in games.
---

# Performance

Every number below comes from the layer's own instrumentation (`AFMF_PROFILE=1`: GPU timestamps
per stage, host timers per present). **Tested hardware: RX 9070 XT (RDNA4) and RX 7800 XT
(RDNA3)**, Mesa 26.1.8 RADV, Fedora 44, KDE Plasma 6.7 Wayland, 3440&times;1440; the 9070 XT unless
stated. Re-measure on your hardware; the method is the same.

## What a frame costs

GPU, per generated frame, with the defaults (at this resolution: optical flow at half size, five
pyramid levels), on the worst case the headless test has: a textured picture panning across the
whole frame at 3440&times;1440, every block moving (the host paints it, so the GPU sits at low
clocks; a game runs it faster):

| Stage | Time |
|---|---|
| Ingest (one read of the game's frame into the colour ring and the luma) | 112-138 &micro;s |
| Luma preparation | 1 &micro;s (the ingest wrote it) |
| Pyramid and scene-change detector histogram, side by side | 20-21 &micro;s |
| Block search (FidelityFX Optical Flow, 5 levels; packed 16-bit SAD; blocks whose coarser-level vector already matches skip it) | 214-219 &micro;s |
| Filter and scale | 94-111 &micro;s |
| Interpolation | 67-77 &micro;s |
| Output copy | 47-62 &micro;s (0 with `AFMF_DIRECT_OUTPUT=1`) |
| **Total** | **560-610 &micro;s** |

The same scene with the optical flow at display resolution and all seven pyramid levels
(`AFMF_PERFORMANCE_MODE=quality AFMF_SEARCH_MODE=high`) costs **674-716 &micro;s**: about a
quarter more for a flow whose extra detail a moving picture does not show.

What keeps the search at that cost: the sum of absolute differences runs on packed 16-bit pairs
(`AFMF_SAD_INT16`, about a third of the ALU work per candidate; the search takes 410 &micro;s
without it at display resolution) and a block whose vector from the coarser level already matches
keeps it instead of searching 256 candidates again (`AFMF_STATIC_BLOCK_SAD`; without it the same
search takes 1.5 ms, and a game's share of still blocks decides its gain). The frame is read
once whatever the flow resolution: the ingest pass writes the colour ring at frame size and the
luma at the flow's size in the same pass, so half resolution costs no copy and no downscale
(`AFMF_DIRECT_INGEST=0` puts those back, and about 20 &micro;s with them). All of it runs on a
compute queue of the layer's own (or the game's last compute queue when the game took them all,
as vkd3d-proton does), so it competes for the GPU but never sits in the game's queue.

Host, on the game's thread, per present: **11 &micro;s under the validation layer** in the
headless test: the hook only consumes the game's semaphores and queues the frame. The slot's
fence, the spare image, recording and the submission (150-200 &micro;s together) run on the
layer's work thread, the two presents and the pacing hold on its presentation thread.

## Measured and rejected

| Idea | Why not |
|---|---|
| The detector's divergence overlapping the coarsest search | that search returns early on a cut, so a group reading the verdict while it was being written split at its next barrier and hung Intel GPUs; the pass runs in front of the search |
| Refining a nearly matching prediction over +-4 (64 candidates, one per lane) instead of searching +-8 | the coarse levels' vectors came out a pixel off, the fine levels stopped skipping their search, and the search went from 274 to 706 &micro;s |
| A second compute queue | see below: the layer's frame is over before the next one arrives |
| wave32 compute | no gain |
| Native SAD instructions | unreachable from GLSL |
| Direct output as the default | it needs storage usage on the game's swapchain images, which can cost the game more than the 44-59 &micro;s copy it saves; it stays opt-in (`AFMF_DIRECT_OUTPUT=1`) |

## Threads and queues

The game's thread only consumes its semaphores with an empty submission and queues the frame.
A work thread waits the slot's fence, takes the companion's image, records and submits; a
presentation thread presents the generated frame, holds the real one back half a frame and
presents it, then acquires the next spare. The pipelines compile on a fourth thread from device
creation. On the GPU, everything runs on one compute queue of the layer's own: within a frame
the passes form a dependency chain (ingest, pyramid, search level by level, filter, scale,
interpolation), and only the scene change detector's histogram is independent, so it runs alongside the
pyramid with no barrier of its own. A second queue would overlap one frame's interpolation with the next frame's ingest, but
the next frame arrives milliseconds later and the layer's whole frame takes under one: the two
would never coincide. On a GPU the game keeps at 100 %, what the layer costs is the sum of its
dispatches, and that is what the table above measures.

Two things learned from the screenshots' games that are worth more than a number: id Tech 8
(DOOM) aborts if a swapchain has more than 8 images, so `AFMF_EXTRA_IMAGES` above 5 kills it
(the default, 2, is fine); and `RADV_PERFTEST=rtcps`, which gives Cyberpunk +40 % in ray tracing,
makes RADV crash while compiling DOOM's ray tracing pipelines. Both are per-game choices, not
defaults.

## In games

| Game | Base (Linux, `DISABLE_AFMF=1`) | With afmf-linux | Notes |
|---|---|---|---|
| Monster Hunter Wilds, Native AA, max, vkd3d-proton | 120 fps | 250 moving / 271 still | 26,380 of 26,381 presents got a companion; hold 3.7-4.3 ms |
| Cyberpunk 2077, RT Ultra, FSR 4 Quality, game FG on (needed), vkd3d-proton | ~100-125 fps | 180-250 | `RADV_PERFTEST=rtcps` raised the base; the gap to Windows is RADV's ray tracing, not the layer |
| DOOM: The Dark Ages, Ultra Nightmare, FSR Quality, **native Vulkan** (id Tech 8) | | 268 | The first native Vulkan title through the layer; 12,900 presents in the menu all got a companion. Note: `RADV_PERFTEST=rtcps` crashes this game inside RADV's ray tracing pipeline compiler; leave it out here |
| DOOM: The Dark Ages, Ultra Nightmare, Native AA + VRS | | 255 | |
| Overwatch 2, Epic, FidelityFX Quality, Reduced Buffering, DXVK | 223 fps (game's counter) | 449 | The highest base so far; the layer keeps up at 223 presents per second from the game |
| Borderlands 4, Badass, FSR Quality, vkd3d-proton | ~60 fps | 125 | Unreal Engine 5 is heavy under vkd3d-proton; the layer doubles what it gets |
| Cyberpunk 2077, same settings, **RX 7800 XT (RDNA3)**, FSR 4 in FP16 | ~60-110 fps | ~120-220 | 15,330 of 15,332 generated; hold 4.6-7.6 ms. GPU cost 1,245 &micro;s per frame with the flow at half resolution: about 11 % of the GPU at 90 fps |

## RDNA3 (RX 7800 XT)

Same code, same tests (validation, golden check in both modes, sanitizers), same behaviour in
vkcube and in a game. What differs is the cost: at 3440&times;1440 with the flow at half
resolution (`AFMF_PERFORMANCE_MODE=performance`), **1,070 &micro;s** per frame in the headless
test and **1,245 &micro;s** in game with seven pyramid levels, about three times the RX 9070 XT.
The reads and writes of the swapchain images weigh much more than on RDNA4, which is what the
defaults now spare it: half resolution and five levels from 1440p up. Expect the 2&times; to fall
short sooner when the game saturates the GPU.

## Not compatible with other frame generation layers or injectors

With the **lsfg-vk** implicit layer installed, Vulkan presentation on the RDNA3 test system hung
(no window, 8 fps in vkcube) even with afmf-linux disabled; removing it fixed everything.
**OptiScaler** (`PROTON_USE_OPTISCALER`, or its DLLs dropped into the game folder), which replaces
the game's upscaler and frame generation inside the process, does not work together with
afmf-linux either. Two things generating frames on the same swapchain cannot both be right about
which present is which: run one or the other.

The rule that decides what you will see: the ceiling is **2&times; the base**, minus GPU
contention when the game already saturates the GPU. A fixed cost of ~0.7 ms per frame weighs more
the higher the base is.

## Measure it yourself

```
DISABLE_AFMF=1 %command%                                   # base
AFMF_ENABLE=1 AFMF_LOG=2 AFMF_PROFILE=1 %command% 2>afmf.log
```

Compare MangoHud's average and 1 % low in the same scene, and read `real fps`, `no free image` and
`pacing hold` from the log. The headless test does the same on a synthetic scene:
`AFMF_TEST_EXTENT=3440x1440 AFMF_PROFILE=1 ./build/afmf_headless`.
