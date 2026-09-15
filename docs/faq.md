---
title: FAQ
description: Answers about afmf-linux, the open AMD Fluid Motion Frames for Linux - latency, Proton, GPUs, HDR, Gamescope and why the frame rate is not exactly double.
faq:
  - q: Is afmf-linux really AMD Fluid Motion Frames?
    a: It is the same idea built the same way, driver-level frame generation from the colour buffer at present time using AMD's FidelityFX Optical Flow for the motion and an interpolation pass, applied to any game. It is not AMD's code for the interpolation and it is not affiliated with AMD.
  - q: How much latency does it add?
    a: Half a frame time, the same as AFMF. At 120 real fps that is about 4 ms. AFMF_PACING=0 removes the hold at the cost of an uneven cadence.
  - q: Does it work with Proton and DirectX games?
    a: Yes. DXVK and vkd3d-proton present through Vulkan, which is where the layer sits.
  - q: Does it need an AMD GPU?
    a: No. It needs a Vulkan 1.1 driver with compute queues and 32-bit image atomics. Tuned on RDNA4 with RADV; other GPUs are untested.
  - q: Does it stack with the game's own frame generation?
    a: Yes, it doubles whatever the game presents. Turn the game's frame generation off for a fair comparison.
  - q: Does it work with HDR?
    a: HDR10 and scRGB swapchains are interpolated. Whether the game gets HDR at all is decided between Wine, the compositor and Mesa, not by the layer.
  - q: Why is my frame rate not exactly double?
    a: The ceiling is twice the base the game reaches on Linux without the layer, minus GPU contention when the game already saturates the GPU. In FIFO at the display's refresh rate there is no free image for companions.
  - q: Does it work with Gamescope?
    a: Untested. Gamescope has its own WSI layer between the game and the compositor.
  - q: Is it safe? It loads into every Vulkan process.
    a: It is mapped into every Vulkan process but does nothing unless AFMF_ENABLE=1 is set. It reads only AFMF_* variables, writes files only when AFMF_DUMP_DIR is set, opens no sockets and spawns nothing. See SECURITY.md.
---

# FAQ

{% for item in page.faq %}
## {{ item.q }}

{{ item.a }}
{% endfor %}

## Which games has it been tried with?

Monster Hunter Wilds and Cyberpunk 2077, both DirectX 12 under vkd3d-proton, on an RX 9070 XT.
Numbers on the [performance]({{ '/performance/' | relative_url }}) page. Reports from other games,
GPUs and compositors are welcome as issues; the template asks for the log.

## What does the log's "no free image" mean?

The presentation engine had no image to give for the companion, so that real frame went out
alone. It is normal for a game in FIFO that already runs at the display's refresh rate (every slot
is taken) and otherwise rare; a steady count usually means `AFMF_EXTRA_IMAGES` is too low for that
compositor.

## Why 64-bit only?

The layer casts handles to pointers for logging and is built for x86_64. A 32-bit build for
32-bit DXVK titles is possible but not provided.
