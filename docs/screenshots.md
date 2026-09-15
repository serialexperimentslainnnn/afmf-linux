---
title: Screenshots
description: afmf-linux in Monster Hunter Wilds, Cyberpunk 2077, DOOM The Dark Ages, Overwatch 2 and Borderlands 4 at 3440x1440, with the graphics settings and the exact Steam launch options of each.
wide: true
---

# Screenshots

RX 9070 XT, Mesa 26.1.8 RADV, KDE Plasma Wayland, all at **3440&times;1440 (21:9)**. MangoHud
(top left) counts what reaches the screen; where the game has its own counter, that one shows
the real frames. **Click a capture to open it at full 3440&times;1440.** Under each one, the
graphics settings and the full Steam launch options used.

{% assign common = "SteamDeck=0 PROTON_ENABLE_WAYLAND=1 MANGOHUD=1 DXVK_HDR=1 PROTON_FSR4_UPGRADE=1 PROTON_MLFG_UPGRADE=1 WINE_VK_USE_SYNC2=1 PROTON_PRIORITY_HIGH=1 PROTON_DISCORD_BRIDGE=1 PROTON_PREFER_SDL=1 PROTON_NO_STEAMINPUT=1 MESA_VK_WSI_PRESENT_MODE=mailbox AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=repeat game-performance %command%" %}

## Monster Hunter Wilds &mdash; 265 fps

{% include shot.html file="monster-hunter-wilds-max-rt-native-aa" alt="Monster Hunter Wilds at max settings with ray tracing and Native AA on Linux, 265 fps with afmf-linux" %}

**Settings:** max, ray tracing on, FSR Native AA. vkd3d-proton, Proton-CachyOS Wineland. 120 fps
without the layer.

```
RADV_PERFTEST=sam,nggc,rtcps {{ common }} /HID/UseISteamInput:False /WineDetectionEnabled:False
```

## Cyberpunk 2077 &mdash; 243 fps (116 real)

{% include shot.html file="cyberpunk-2077-rt-ultra-fsr-quality-5" alt="Cyberpunk 2077 with ray tracing on Linux: MangoHud shows 243 fps with afmf-linux while the game's own counter shows 116 real frames per second" %}

**Settings:** RT Ultra, FSR 4 Quality, in-game frame generation on. vkd3d-proton, Proton-GE. The
game's own counter (top centre) shows the real frames, 116; MangoHud what reaches the screen, 243.
`rtcps` is worth +40 % in ray tracing here.

```
RADV_PERFTEST=sam,nggc,rtcps {{ common }}
```

<div class="shots">
{% include shot.html file="cyberpunk-2077-rt-ultra-fsr-quality-1" alt="Cyberpunk 2077, RT Ultra, FSR 4 Quality with afmf-linux, scene 1" %}
{% include shot.html file="cyberpunk-2077-rt-ultra-fsr-quality-2" alt="Cyberpunk 2077, RT Ultra, FSR 4 Quality with afmf-linux, scene 2" %}
{% include shot.html file="cyberpunk-2077-rt-ultra-fsr-quality-3" alt="Cyberpunk 2077, RT Ultra, FSR 4 Quality with afmf-linux, scene 3" %}
{% include shot.html file="cyberpunk-2077-rt-ultra-fsr-quality-4" alt="Cyberpunk 2077, RT Ultra, FSR 4 Quality with afmf-linux, scene 4" %}
</div>

## DOOM: The Dark Ages &mdash; 268 fps

{% include shot.html file="doom-the-dark-ages-ultra-nightmare-fsr-quality" alt="DOOM: The Dark Ages at Ultra Nightmare with FSR Quality on Linux, 268 fps with afmf-linux" %}

**Settings:** Ultra Nightmare, FSR Quality. Native Vulkan (id Tech 8), Proton-GE. Two things
specific to this game: `rtcps` crashes RADV's ray tracing pipeline compiler here, so it is left
out; and the engine aborts above 8 swapchain images, so `AFMF_EXTRA_IMAGES` stays at its default.

```
RADV_PERFTEST=sam,nggc {{ common }}
```

## DOOM: The Dark Ages, Native AA + VRS &mdash; 255 fps

{% include shot.html file="doom-the-dark-ages-ultra-nightmare-native-aa-vrs" alt="DOOM: The Dark Ages at Ultra Nightmare with Native AA and VRS on Linux, 255 fps with afmf-linux" %}

**Settings:** Ultra Nightmare, FSR Native AA, variable rate shading on. Same launch options as above.

## Overwatch 2 &mdash; 449 fps (223 real)

{% include shot.html file="overwatch-2-epic-fidelityfx-quality" alt="Overwatch 2 at Epic with FidelityFX Quality on Linux: 449 fps on screen with afmf-linux, 223 real per the game's counter" %}

**Settings:** Epic, FidelityFX Quality, Reduced Buffering. DXVK, Proton-GE. The game's own
counter (top right) shows 223 real frames per second; MangoHud 449 on screen.

```
RADV_PERFTEST=sam,nggc,rtcps {{ common }}
```

## Borderlands 4 &mdash; 125 fps

{% include shot.html file="borderlands-4-badass-fsr-quality" alt="Borderlands 4 at Badass settings with FSR Quality on Linux, 125 fps with afmf-linux" %}

**Settings:** Badass, FSR Quality. vkd3d-proton (Unreal Engine 5), Proton-GE.

```
RADV_PERFTEST=sam,nggc,rtcps {{ common }}
```

## About the common part

`AFMF_ENABLE=1 AFMF_SEARCH_MODE=high AFMF_FAST_MOTION_RESPONSE=repeat` is the layer with AMD's
"high" search preset. (The captures were taken with `AFMF_PACING=0` as well, a personal choice: no
hold on the real frame, at the cost of an uneven cadence that some compositors show as stutter.
It is not in the lines above on purpose: leave pacing on.) `PROTON_ENABLE_WAYLAND=1 DXVK_HDR=1` give Wine's Wayland driver
and HDR; `PROTON_FSR4_UPGRADE=1 PROTON_MLFG_UPGRADE=1` FSR 4 in games that ship FSR 3.1;
`MESA_VK_WSI_PRESENT_MODE=mailbox` removes vsync throttling; `game-performance` is CachyOS's
performance wrapper (`gamemoderun` elsewhere, or nothing); the rest is Proton housekeeping. With
two AMD GPUs, `MESA_VK_DEVICE_SELECT='1002:7550!' DXVK_FILTER_DEVICE_NAME='9070'` pins the game
to the 9070 XT. All the variables of the layer: [configuration]({{ '/configuration/' | relative_url }}).
