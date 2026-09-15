---
title: Launch options per game
description: The Steam launch options used for afmf-linux in Monster Hunter Wilds, Cyberpunk 2077, DOOM The Dark Ages, Borderlands 4 and Overwatch 2, with the in-game settings of each screenshot.
---

# Launch options per game

The exact lines behind the screenshots on the [home page]({{ '/' | relative_url }}), on an RX 9070
XT with Mesa 26.1.8, KDE Plasma Wayland, Proton-GE (Proton-CachyOS Wineland for Monster Hunter
Wilds), all at 3440&times;1440 (21:9). Everything before `%command%` goes into the game's Steam
launch options.

## The common part

```
SteamDeck=0 PROTON_ENABLE_WAYLAND=1 MANGOHUD=1 DXVK_HDR=1 PROTON_FSR4_UPGRADE=1 PROTON_MLFG_UPGRADE=1 WINE_VK_USE_SYNC2=1 PROTON_PRIORITY_HIGH=1 PROTON_DISCORD_BRIDGE=1 PROTON_PREFER_SDL=1 PROTON_NO_STEAMINPUT=1 MESA_VK_WSI_PRESENT_MODE=mailbox AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=repeat AFMF_PACING=0 game-performance %command%
```

What each part is for:

- `AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=repeat`: the layer, with AMD's
  "high" search preset. `AFMF_PACING=0` is a personal choice here (no hold on the real frame);
  leave it out to get the evenly spaced cadence. See [configuration]({{ '/configuration/' | relative_url }}).
- `PROTON_ENABLE_WAYLAND=1 DXVK_HDR=1`: Wine's Wayland driver and HDR. `PROTON_FSR4_UPGRADE=1
  PROTON_MLFG_UPGRADE=1`: FSR 4 upscaling and frame generation in games that ship FSR 3.1.
- `MESA_VK_WSI_PRESENT_MODE=mailbox`: no vsync throttling in the presentation engine.
- `MANGOHUD=1`: the counter in the screenshots. `game-performance`: CachyOS's performance profile
  wrapper (use `gamemoderun` elsewhere, or nothing).
- The rest (`SteamDeck=0`, `WINE_VK_USE_SYNC2`, `PROTON_PRIORITY_HIGH`, `PROTON_DISCORD_BRIDGE`,
  `PROTON_PREFER_SDL`, `PROTON_NO_STEAMINPUT`) is Proton housekeeping unrelated to the layer.

With two AMD GPUs in the machine, `MESA_VK_DEVICE_SELECT='1002:7550!' DXVK_FILTER_DEVICE_NAME='9070'`
pins the game to the 9070 XT (`1002:747e` / `7800` for the 7800 XT); with one GPU, leave it out.

## Per game

| Game | In-game settings (as in the screenshots) | Add to the common part | On screen |
|---|---|---|---|
| Monster Hunter Wilds | Max settings + ray tracing, FSR Native AA | `RADV_PERFTEST=sam,nggc,rtcps` and, after `%command%`, `/HID/UseISteamInput:False /WineDetectionEnabled:False` | 265 fps |
| Cyberpunk 2077 | RT Ultra, FSR 4 Quality, in-game frame generation on | `RADV_PERFTEST=sam,nggc,rtcps` (**+40 % in ray tracing on RADV**) | 243 fps (116 real) |
| DOOM: The Dark Ages | Ultra Nightmare, FSR Quality (268) or Native AA + VRS (255) | `RADV_PERFTEST=sam,nggc` **without `rtcps`** (it crashes RADV's ray tracing pipeline compiler in this game), and never `AFMF_EXTRA_IMAGES` above 5 (id Tech 8 aborts above 8 swapchain images) | 268 / 255 fps |
| Borderlands 4 | Badass, FSR Quality | `RADV_PERFTEST=sam,nggc,rtcps` | 125 fps |
| Overwatch 2 | Epic, FidelityFX Quality, Reduced Buffering | `RADV_PERFTEST=sam,nggc,rtcps` | 449 fps (223 real) |

Not in these lines on purpose, because they did nothing or did harm: `ENABLE_LAYER_MESA_ANTI_LAG=1`
(Mesa's old anti-lag layer, measured as a no-op by the low_latency_layer project and one more
layer in the present chain), `AMD_VULKAN_ICD` (RADV is the only driver), `AFMF_EXTRA_IMAGES=8`
(no benefit over the default 2, 160 MB of images, and DOOM refuses to start), and
`AFMF_ACQUIRE_TIMEOUT_US=0` (already the default).
