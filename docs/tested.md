---
title: Tested hardware and games
description: Where afmf-linux, the open AMD Fluid Motion Frames for Linux, has been run - GPUs, drivers, games and runtimes - by the maintainer and by users, with the result of each.
---

# Tested hardware and games

What has actually been run, by whom, and what happened. Numbers are frames per second on screen
(MangoHud) unless stated. Add yours: an [issue](https://github.com/{{ site.repository }}/issues/new/choose)
with GPU, driver, game, runtime and the result is enough, a log with `AFMF_LOG=2 AFMF_PROFILE=1`
is better.

## By the maintainer

Fedora 44, KDE Plasma 6.7 Wayland, Mesa 26.1.8 RADV, 3440&times;1440 at 165 Hz.

| GPU | Game | API, runtime | Result |
|---|---|---|---|
| RX 9070 XT (RDNA4) | Monster Hunter Wilds, max + RT, Native AA | DX12, vkd3d-proton, Proton-CachyOS Wineland | 120 &rarr; **265** |
| RX 9070 XT | Cyberpunk 2077, RT Ultra, FSR 4 Quality | DX12, vkd3d-proton, Proton-GE | 116 &rarr; **243** |
| RX 9070 XT | DOOM: The Dark Ages, Ultra Nightmare | Vulkan (id Tech 8), Proton-GE | **268** (FSR Quality), **255** (Native AA + VRS); keep `AFMF_EXTRA_IMAGES` &le; 5 and no `rtcps` |
| RX 9070 XT | Overwatch 2, Epic | DX11, DXVK, Proton-GE | 223 &rarr; **449** |
| RX 9070 XT | Borderlands 4, Badass | DX12, vkd3d-proton (UE5), Proton-GE | **125** |
| RX 7800 XT (RDNA3) | Cyberpunk 2077, same settings | DX12, vkd3d-proton | 60-110 &rarr; 120-220; 1.2 ms of GPU per frame, three times the 9070 XT |
| RX 9070 XT | Cyberpunk 2077 under Gamescope 3.16 (nested, `AFMF_GAMESCOPE=1`) | DX12, vkd3d-proton, Xwayland bypass | 35,283 of 35,285 presents got a companion at 1920&times;1080; 200-215 real fps in game |

## Reported by users

| GPU | Game | API, runtime | Result | Source |
|---|---|---|---|---|
| NVIDIA GeForce GTX 1050 Ti (Pascal) | The Witcher 3 | DX11, DXVK | Works, "a smoother feeling gameplay"; smearing and artifacts on text and UI (the colour-only limit, see below) | r/linux_gaming, 2026-09-15 |

## Not tested yet

RDNA2 (including the Steam Deck), Intel Arc, NVK, GNOME and Sway compositors. Nothing in the layer
is vendor-specific except the tuning; a report from any of these is welcome.

## What the artifacts are

The layer works from the colour buffer alone: no depth, no motion vectors, no HUD mask from the
game. Every pixel of a generated frame is the average of the two real frames warped halfway
along the block's motion. Where the two warped samples disagree (a wrong vector, the silhouette
of an object the camera follows against a fast background, a background uncovered from behind a
passing object, which only one frame holds) the pixel leans on the current frame's own unmoved
value instead of drawing both, so a double edge is the exception rather than the rule. A pixel
that is identical in both frames while its block moves is a static overlay (HUD, crosshair,
subtitles) and is kept as it is (`AFMF_HUD_DETECT`). Motion beyond 128 pixels between frames, and
the frames after a scene cut, are not interpolated: the pixel falls back to a blend of the two
frames (`AFMF_FAST_MOTION_RESPONSE=blend`, the default) or to the previous frame (`repeat`). What
is behind an object as it passes is in neither frame and cannot be invented. AMD's AFMF on
Windows has the same limits. The gain is larger the further the game is from the display's
refresh rate.
