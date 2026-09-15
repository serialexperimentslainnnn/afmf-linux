#pragma once

#include "layer.h"

/* Phase 1: pass-through. Each hook forwards to the driver and keeps per-swapchain bookkeeping
 * (format, extent, present cadence) that the frame-generation phases build on. */

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out);

void afmf_swapchain_destroy(struct afmf_device *dev, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *alloc);

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info);

/* Frees whatever bookkeeping the application left behind when the device goes away. */
void afmf_swapchain_forget_all(struct afmf_device *dev);
