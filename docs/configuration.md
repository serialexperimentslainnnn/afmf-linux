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
| `AFMF_LOG` | `0` | `0` errors, `1` warnings, `2` info, `3` debug, all on stderr |
| `AFMF_PERFORMANCE_MODE` | `auto` | `auto`: `performance` from 2560x1440 up, `quality` below. `quality`: optical flow at display resolution (8 px blocks). `performance`: at half resolution (16 px blocks) |
| `AFMF_SEARCH_MODE` | `auto` | `auto`: 7 pyramid levels at full flow resolution, 5 at half (+-256 px of motion on screen). `standard`: 5 levels. `high`: 7 levels always, motion up to +-512 flow pixels |
| `AFMF_FAST_MOTION_RESPONSE` | `blend` | What to show where the flow is unreliable: `repeat` the previous frame, or `blend` both |
| `AFMF_GAMESCOPE` | unset | `1` when the game runs under Gamescope (Steam Deck, or `gamescope -- <game>`): five extra images instead of two, and the layer passes through in the gamescope process itself |
| `AFMF_EXTRA_IMAGES` | `2` (`5` with `AFMF_GAMESCOPE=1`) | Swapchain images added beyond what the application asked for (1..8). Fewer means more presents without a companion; more means more memory; some engines abort above 8 images in total (id Tech 8) |
| `AFMF_ACQUIRE_TIMEOUT_US` | `0` | Longest the layer waits for the companion's image to be released before presenting the real frame alone. The image is requested a frame ahead, so the default never stalls the game |
| `AFMF_PRESENT_MODE` | `auto` | `auto`: a swapchain the game creates in FIFO (vsync) is created in MAILBOX when the surface offers it, and a per-present switch back to FIFO is rewritten too. In FIFO every present takes a refresh slot and the layer doubles them, so a game above half the refresh rate loses real frames (120 at 165 Hz becomes 82); MAILBOX shows the latest frame and drops the excess. `keep` leaves the game's mode alone |
| `AFMF_ASYNC` | `1` | `0` runs the work on the application's queue and presents inline (diagnosis) |
| `AFMF_PACING` | `1` | `0` presents the real frame right behind the generated one instead of half a frame later: uneven cadence, the compositor may drop generated frames |
| `AFMF_GOVERNOR` | `1` | `1` steps generation down while the GPU is contended (the generated frame ready later than a frame and a half, thirty frames in a row): five search levels first, then one companion in two, then one in three; back up a step after 15 frames ready within three quarters of a frame. Off, every frame gets a companion regardless |
| `AFMF_MIN_FPS` | `30` | Below this real frame rate no companion is made: doubling 25 fps is not worth its latency. `0` removes the floor |
| `AFMF_STATIC_BLOCK_SAD` | `128` | A block whose 64 pixels differ from the previous frame's at rest by no more than this (sum of absolute 8-bit luma differences) is static: vector 0, search skipped. `0` searches every block |
| `AFMF_DIRECT_INGEST` | `1` | The game's frame is read once, straight from its swapchain image, into the layer's colour ring and the flow's luma. Puts sampled usage on the game's swapchain images; `0` puts the copy back. Not for sRGB swapchains (the copy path stays there) |
| `AFMF_SAD_INT16` | `1` | The block search sums its pixel differences on packed 16-bit pairs (two per instruction; needs `shaderInt16`, which the layer enables on the device when the game did not). `0` keeps the SDK's byte-at-a-time sum |
| `AFMF_DIRECT_OUTPUT` | `0` | `1` writes the interpolated frame straight into the swapchain image instead of copying it there (saves the copy, 44-59 us at 3440x1440), which puts storage usage on the game's swapchain images; that can cost the game's own rendering more than it saves, so measure it per game. Needs a format that takes storage writes (not sRGB) |
| `AFMF_HUD_DETECT` | `1` | A pixel that is the same in both frames (within one 8-bit level) while its block moves is a static overlay (HUD, crosshair, subtitles): it is kept instead of warped. `0` warps everything |
| `AFMF_INTERPOLATE` | `1` | `0` repeats the previous frame instead of interpolating (debug) |
| `AFMF_PROFILE` | `0` | `1` logs GPU time per stage and host time per present every 300 frames and at teardown |
| `AFMF_DUMP_DIR` | unset | Writes four generated frames into that directory, created if it does not exist: `afmf_generated_<frame>.ppm` with the block flow that made it as `afmf_flow_<frame>.txt` (`vx vy` per block, `prev = cur + v`), and a log line with the luma and flow statistics. They are the companions of real frames 8-11, before which the scene change detector is still warming up, which in most games is a loading screen: `touch <dir>/dump-now` while playing writes another four from what is on screen at that moment, as often as you like. 8- and 10-bit swapchains; a format that cannot be dumped is named in the log |

## Presets

**Default**: `AFMF_ENABLE=1`. The flow runs at half resolution from 1440p up, which is where a
game is most likely to be saturating the GPU already.

**Every pixel of the flow**, at about a quarter more GPU cost per generated frame:
`AFMF_ENABLE=1 AFMF_PERFORMANCE_MODE=quality AFMF_SEARCH_MODE=high`.

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
[AFMF info] swapchain 0x... created: 3440x1440, format 37, present mode 1 (app asked 2), 5 images (app asked 3), generation on (layer queue)
[AFMF info] swapchain 0x...: 3001 presents, 2999 generated, 0 no free image; 8.05 ms between presents (124 real fps); in the hook 31 us per present; work thread: slot fence 2, acquire 3, record 41, submit 35; presentation thread: present generated 279, present real 252, refill 29, gpu done +0.41 ms, pacing hold 4.53 ms; governor step 0
[AFMF info] swapchain 0x... destroyed after 26381 presents: 26380 generated, 1 skipped (0 no free image, 1 no history, 0 held back by the governor)
```

- **present mode** is the swapchain's after the layer (`1` mailbox, `2` FIFO) and what the game
  asked for; `AFMF_PRESENT_MODE=keep` stops the rewrite.
- **real fps** is the game's own rate; MangoHud shows roughly twice that.
- **no free image** counts presents that got no companion because the presentation engine had no
  image to give; a game in FIFO at the display's refresh rate has none to give, and Gamescope
  keeps more in flight than a desktop compositor (`AFMF_GAMESCOPE=1`).
- **in the hook** is host time on the game's thread per present: it consumes the game's
  semaphores with an empty submission and queues the frame. **work thread** (the slot's fence,
  the companion's image, recording, the submission) and **presentation thread** (both presents,
  the pacing hold, the next spare) are the layer's own threads, off the game's critical path.
- **gpu done** is how long after the present call the generated frame was ready; **pacing hold**
  is how long the real frame was held back: half the frame time from that point, capped at a frame.
- **governor step** is where `AFMF_GOVERNOR` sits: `0` every frame with the full search, `1` every
  frame with five search levels, `2` one companion in two, `3` one in three; each change is logged
  with the reason. **held back by the governor** counts the presents that step 2 or 3 left without a
  companion.
