---
title: Performance
description: Measured cost of afmf-linux, the open AMD Fluid Motion Frames layer for Linux, per GPU stage and per frame, and frame rates reached in games.
---

# Performance

Every number below comes from the layer's own instrumentation (`AFMF_PROFILE=1`: GPU timestamps
per stage, host timers per present) on an RX 9070 XT (RDNA4), Mesa 26.1.8 RADV, Fedora 44, KDE
Plasma 6.7 Wayland, 3440&times;1440. Re-measure on your hardware; the method is the same.

## What a frame costs

GPU, per generated frame, optical flow at half resolution (the default from 2560&times;1440 up):

| Stage | Time |
|---|---|
| Ingest copy | 47 &micro;s |
| Luma, pyramid, scene-change detector | 42 &micro;s |
| Block search (FidelityFX Optical Flow, 5 levels) | 250 &micro;s |
| Filter and scale | 32 &micro;s |
| Interpolation | 50 &micro;s |
| Output copy | 33 &micro;s |
| **Total** | **~450 &micro;s** |

At full flow resolution (`AFMF_PERFORMANCE_MODE=quality`) the total is about 1.2 ms. All of it
runs on a compute queue of the layer's own (or the game's last compute queue when the game took
them all, as vkd3d-proton does), so it competes for the GPU but never sits in the game's queue.

Host, on the game's thread, per present: **76-91 &micro;s** (fence 2, spare image 3, command
recording ~40, submit ~35). The two presents (170-400 &micro;s each under Proton's Mesa WSI) and
the pacing hold run on the layer's presentation thread.

## How the numbers were reached

| Change | Effect |
|---|---|
| Baseline, everything on the game's queue at full resolution | 1222 &micro;s GPU, in series with rendering |
| Optical flow at half resolution | 493 &micro;s |
| 5 pyramid levels at half resolution, compute-only barriers | 434 &micro;s |
| Companion image acquired a frame ahead instead of waited for | host time in the present hook 5.9 ms &rarr; 60 &micro;s (FIFO desktop) |
| Shared compute queue when the game holds every compute queue | GPU work off the graphics queue in vkd3d-proton titles |
| Presentation thread with half-frame pacing | hook 546 &rarr; 80 &micro;s in game; generated frames evenly spaced |

Measured and rejected: wave32 compute (no gain), native SAD instructions (unreachable from GLSL),
writing the interpolator straight into the swapchain image (needs `STORAGE` usage on the game's
images, which can cost the game more than the 33 &micro;s copy it saves).

## In games

| Game | Base (Linux, `DISABLE_AFMF=1`) | With afmf-linux | Notes |
|---|---|---|---|
| Monster Hunter Wilds, Native AA, max, vkd3d-proton | 120 fps | 250 moving / 271 still | 26,380 of 26,381 presents got a companion; hook 76-91 &micro;s; hold 3.7-4.3 ms |
| Cyberpunk 2077, RT Ultra, FSR 4 Quality, game FG on, vkd3d-proton | ~100-125 fps | 180-250 | `RADV_PERFTEST=rtcps` raised the base; the gap to Windows is RADV's ray tracing, not the layer |
| Cyberpunk 2077, same settings, **RX 7800 XT (RDNA3)**, FSR 4 in FP16 | ~60-110 fps | ~120-220 | 15,330 of 15,332 generated; hook 60-90 &micro;s; hold 4.6-7.6 ms. GPU cost 1,245 &micro;s per frame (search 700, ingest copy 148, interpolate 120, output copy 99): about 11 % of the GPU at 90 fps, against 5 % on the 9070 XT |

## RDNA3 (RX 7800 XT)

Same code, same tests (validation, golden check in both modes, sanitizers), same behaviour in
vkcube and in a game. What differs is the cost: at 3440&times;1440 with the flow at half
resolution, **1,070 &micro;s** per frame in the headless test and **1,245 &micro;s** in game (7
pyramid levels with `AFMF_SEARCH_MODE=high`), against 434 on the RX 9070 XT. The copies in and
out of the swapchain weigh much more than on RDNA4 (148 + 99 &micro;s against 47 + 33). Use
`AFMF_SEARCH_MODE=auto` (5 levels, about 90 &micro;s less) on this generation, and expect the 2&times;
to fall short sooner when the game saturates the GPU.

## Not compatible with other frame generation layers

With the **lsfg-vk** implicit layer installed, Vulkan presentation on the RDNA3 test system hung
(no window, 8 fps in vkcube) even with afmf-linux disabled; removing it fixed everything. Two
layers generating frames on the same swapchain cannot both be right about which present is
which: run one or the other.

The rule that decides what you will see: the ceiling is **2&times; the base**, minus GPU
contention when the game already saturates the GPU. A fixed cost of ~0.5 ms per frame weighs more
the higher the base is.

## Measure it yourself

```
DISABLE_AFMF=1 %command%                                   # base
AFMF_ENABLE=1 AFMF_LOG=2 AFMF_PROFILE=1 %command% 2>afmf.log
```

Compare MangoHud's average and 1 % low in the same scene, and read `real fps`, `no free image` and
`pacing hold` from the log. The headless test does the same on a synthetic scene:
`AFMF_TEST_EXTENT=3440x1440 AFMF_PROFILE=1 ./build/afmf_headless`.
