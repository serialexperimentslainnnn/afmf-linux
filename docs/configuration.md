---
title: Configuration
description: Every environment variable of afmf-linux, the open AMD Fluid Motion Frames layer for Linux, with defaults and what each one changes.
---

# Configuration

Everything is an environment variable, set per game (Steam launch options) or in the shell. The
names mirror the settings AMD exposes for AFMF on Windows where such a setting exists.

| Variable | Default | Meaning |
|---|---|---|
| `AFMF_ENABLE` | unset | `1` loads the layer |
| `DISABLE_AFMF` | unset | `1` keeps it out even when enabled |
| `AFMF_LOG` | `1` | `0` errors, `1` warnings, `2` info, `3` debug, all on stderr |
| `AFMF_PERFORMANCE_MODE` | `auto` | `quality`: optical flow at display resolution (8 px blocks). `performance`: at half resolution (16 px blocks), about 2.5x cheaper. `auto`: `performance` from 2560x1440 up |
| `AFMF_SEARCH_MODE` | `auto` | `standard`: 5 pyramid levels. `high`: 7 levels, motion up to +-512 flow pixels. `auto`: 7 at full resolution, 5 at half |
| `AFMF_FAST_MOTION_RESPONSE` | `repeat` | What to show where the flow is unreliable: `repeat` the previous frame, or `blend` both |
| `AFMF_EXTRA_IMAGES` | `2` | Swapchain images added beyond what the application asked for (1..8). Fewer means more presents without a companion; more means more memory |
| `AFMF_ACQUIRE_TIMEOUT_US` | `0` | Longest the layer waits for the companion's image to be released before presenting the real frame alone. The image is requested a frame ahead, so the default never stalls the game |
| `AFMF_ASYNC` | `1` | `0` runs the work on the application's queue and presents inline (diagnosis) |
| `AFMF_PACING` | `1` | `0` presents the real frame right behind the generated one instead of half a frame later: uneven cadence, the compositor may drop generated frames |
| `AFMF_INTERPOLATE` | `1` | `0` repeats the previous frame instead of interpolating (debug) |
| `AFMF_PROFILE` | `0` | `1` logs GPU time per stage and host time per present every 300 frames and at teardown |
| `AFMF_DUMP_DIR` | unset | Writes the first generated frames as PPM files into that directory (8-bit formats only) |

## Presets

**Default** (what AMD calls quality search, repeat): `AFMF_ENABLE=1`.

**AMD "high search, blend"**: `AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=blend`.

**Without pacing** (uneven cadence, the compositor may drop generated frames above the refresh
rate): add `AFMF_PACING=0`.

**Diagnosis**: add `AFMF_LOG=2 AFMF_PROFILE=1` and `2>afmf.log` after `%command%`; attach the file
to a bug report.

## Reading the log

```
[AFMF info] device 0x... created, VK_KHR_swapchain enabled, no spare compute queue: sharing the application's queue 3 of family 1
[AFMF info] surface 0x... created: Wayland
[AFMF info] interpolation ready: 3440x1440, flow at 1720x720 (215x90 blocks of 16 px), 5 pyramid levels
[AFMF info] swapchain 0x... created: 3440x1440, format 37, present mode 2, 5 images (app asked 3), generation on (layer queue)
[AFMF info] swapchain 0x...: 3001 presents, 2999 generated, 0 no free image; 8.05 ms between presents (124 real fps); in the layer 85 us per present: slot fence 2, acquire 3, record 41, submit 35; presentation thread: present generated 279, present real 252, refill 29, pacing hold 4.53 ms
[AFMF info] swapchain 0x... destroyed after 26381 presents: 26380 generated, 1 skipped (0 no free image, 1 no history)
```

- **real fps** is the game's own rate; MangoHud shows roughly twice that.
- **no free image** counts presents that got no companion because the presentation engine had no
  image to give; a game in FIFO at the display's refresh rate has none to give.
- **in the layer** is host time on the game's thread per present; **presentation thread** is the
  layer's own thread, off the game's critical path.
- **pacing hold** is how long the real frame was held back: half the frame time.
