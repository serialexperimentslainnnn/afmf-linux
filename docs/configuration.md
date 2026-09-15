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
| `AFMF_GAMESCOPE` | unset | `1` when the game runs under Gamescope (Steam Deck, or `gamescope -- <game>`): five extra images instead of two, and the layer passes through in the gamescope process itself |
| `AFMF_EXTRA_IMAGES` | `2` (`5` with `AFMF_GAMESCOPE=1`) | Swapchain images added beyond what the application asked for (1..8). Fewer means more presents without a companion; more means more memory; some engines abort above 8 images in total (id Tech 8) |
| `AFMF_ACQUIRE_TIMEOUT_US` | `0` | Longest the layer waits for the companion's image to be released before presenting the real frame alone. The image is requested a frame ahead, so the default never stalls the game |
| `AFMF_ASYNC` | `1` | `0` runs the work on the application's queue and presents inline (diagnosis) |
| `AFMF_PACING` | `1` | `0` presents the real frame right behind the generated one instead of half a frame later: uneven cadence, the compositor may drop generated frames |
| `AFMF_GOVERNOR` | `1` | Steps generation down while the GPU is contended (the generated frame ready later than half the frame time, three frames in a row): five search levels first, then one companion in two, then one in three; back up a step after 60 frames ready early. `0` generates every frame regardless |
| `AFMF_MIN_FPS` | `30` | Below this real frame rate no companion is made: doubling 25 fps is not worth its latency. `0` removes the floor |
| `AFMF_INTERPOLATE` | `1` | `0` repeats the previous frame instead of interpolating (debug) |
| `AFMF_PROFILE` | `0` | `1` logs GPU time per stage and host time per present every 300 frames and at teardown |
| `AFMF_DUMP_DIR` | unset | Writes the first generated frames as PPM files into that directory (8-bit formats only) |

## Presets

**Default** (what AMD calls quality search, repeat): `AFMF_ENABLE=1`.

**AMD "high search, blend"**: `AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=blend`.

**Without pacing** (uneven cadence, the compositor may drop generated frames above the refresh
rate): add `AFMF_PACING=0`.

**Under Gamescope** (Steam Deck, or nested on the desktop): `AFMF_ENABLE=1 AFMF_GAMESCOPE=1`, on
the gamescope command line or in the Steam launch options; see the [FAQ]({{ '/faq/' | relative_url }}).

**Diagnosis**: add `AFMF_LOG=2 AFMF_PROFILE=1` and `2>afmf.log` after `%command%`; attach the file
to a bug report.

## Reading the log

```
[AFMF info] device 0x... created, VK_KHR_swapchain enabled, no spare compute queue: sharing the application's queue 3 of family 1
[AFMF info] surface 0x... created: Wayland
[AFMF info] interpolation ready: 3440x1440, flow at 1720x720 (215x90 blocks of 16 px), 5 pyramid levels
[AFMF info] swapchain 0x... created: 3440x1440, format 37, present mode 2, 5 images (app asked 3), generation on (layer queue)
[AFMF info] swapchain 0x...: 3001 presents, 2999 generated, 0 no free image; 8.05 ms between presents (124 real fps); in the layer 85 us per present: slot fence 2, acquire 3, record 41, submit 35; presentation thread: present generated 279, present real 252, refill 29, gpu done +0.41 ms, pacing hold 4.53 ms; governor step 0
[AFMF info] swapchain 0x... destroyed after 26381 presents: 26380 generated, 1 skipped (0 no free image, 1 no history, 0 held back by the governor)
```

- **real fps** is the game's own rate; MangoHud shows roughly twice that.
- **no free image** counts presents that got no companion because the presentation engine had no
  image to give; a game in FIFO at the display's refresh rate has none to give, and Gamescope
  keeps more in flight than a desktop compositor (`AFMF_GAMESCOPE=1`).
- **in the layer** is host time on the game's thread per present; **presentation thread** is the
  layer's own thread, off the game's critical path.
- **gpu done** is how long after the present call the generated frame was ready; **pacing hold**
  is how long the real frame was held back: half the frame time from that point, capped at a frame.
- **governor step** is where `AFMF_GOVERNOR` sits: `0` every frame with the full search, `1` every
  frame with five search levels, `2` one companion in two, `3` one in three; each change is logged
  with the reason. **held back by the governor** counts the presents that step 2 or 3 left without a
  companion.
