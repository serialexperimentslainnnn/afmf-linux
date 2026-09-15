---
title: FAQ
description: Answers about afmf-linux, the open AMD Fluid Motion Frames for Linux - latency, Proton, GPUs, HDR, Gamescope and why the frame rate is not exactly double.
faq:
  - q: Is afmf-linux really AMD Fluid Motion Frames?
    a: It is the same idea built the same way, driver-level frame generation from the colour buffer at present time using AMD's FidelityFX Optical Flow for the motion and an interpolation pass, applied to any game. It is not AMD's code for the interpolation and it is not affiliated with AMD.
  - q: Does it add latency?
    a: Not noticeably, as with AFMF on Windows. The pacing holds the real frame back by half a frame time so the generated one lands in between, the same thing AMD's implementation does. AFMF_PACING=0 removes the hold at the cost of an uneven cadence.
  - q: Does it work with Proton and DirectX games?
    a: Yes. DXVK and vkd3d-proton present through Vulkan, which is where the layer sits.
  - q: Does it need an AMD GPU?
    a: No. It needs a Vulkan 1.1 driver with compute queues and 32-bit image atomics. Tuned on RDNA4 and verified on RDNA3, both with RADV; a user reports it working on an NVIDIA GTX 1050 Ti. RDNA2 and Intel are untested.
  - q: Does it work with the game's own frame generation (FSR 3/4 FG)?
    a: Yes, and keep it on. The layer doubles whatever the game presents; in Cyberpunk 2077 under vkd3d-proton it only produced companions with the game's frame generation enabled. Leave the in-game setting as you would on Windows with AFMF.
  - q: Can I use it together with lsfg-vk or OptiScaler?
    a: No. Two frame generation layers fight over the same presents. With the lsfg-vk implicit layer installed, Vulkan presentation on our RDNA3 test system hung even with afmf-linux disabled (uninstall it or set DISABLE_LSFGVK=1), and OptiScaler, which replaces the game's upscaler and frame generation inside the game process, does not work together with afmf-linux either.
  - q: Does it work on RDNA3?
    a: Yes. Verified on an RX 7800 XT with the same tests and in Cyberpunk 2077, at about three times the GPU cost per frame of an RX 9070 XT (1.2 ms at 3440x1440), so the gain is smaller when the game already saturates the GPU.
  - q: Does it work with HDR?
    a: HDR10 and scRGB swapchains are interpolated. Whether the game gets HDR at all is decided between Wine, the compositor and Mesa, not by the layer.
  - q: Why is my frame rate not exactly double?
    a: The ceiling is twice the base the game reaches on Linux without the layer, minus GPU contention when the game already saturates the GPU. In FIFO at the display's refresh rate there is no free image for companions.
  - q: Does it work with Gamescope?
    a: Yes, with AFMF_EXTRA_IMAGES=4 or 5. Gamescope keeps more swapchain images in flight than a desktop compositor, so with the default 2 extra images most presents find no free image for the companion (31 percent generated in a test with gamescope 3.16 at 165 Hz); 4 extra gave 86 percent and 5 gave every present one. The Steam Deck itself (RDNA2, Gamescope as the session) is untested; reports welcome.
  - q: Is it safe? It loads into every Vulkan process.
    a: It is mapped into every Vulkan process but does nothing unless AFMF_ENABLE=1 is set. It reads only AFMF_* variables, writes files only when AFMF_DUMP_DIR is set, opens no sockets and spawns nothing. See SECURITY.md.
---

# FAQ

{% for item in page.faq %}
## {{ item.q }}

{{ item.a }}
{% endfor %}

## Which games and GPUs has it been tried with?

The [tested hardware and games]({{ '/tested/' | relative_url }}) page lists every case, ours and
users', with the result. Reports from other games, GPUs and compositors are welcome as issues; the
template asks for the log.

## What does the log's "no free image" mean?

The presentation engine had no image to give for the companion, so that real frame went out
alone. It is normal for a game in FIFO that already runs at the display's refresh rate (every slot
is taken) and otherwise rare; a steady count usually means `AFMF_EXTRA_IMAGES` is too low for that
compositor (Gamescope needs 4 or 5).

## Why 64-bit only?

The layer casts handles to pointers for logging and is built for x86_64. A 32-bit build for
32-bit DXVK titles is possible but not provided.
