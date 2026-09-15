#pragma once

#include "layer.h"

/* Frame generation at present time.
 *
 * The swapchain is created with one image more than the application asked for and with transfer
 * usage. On every present of image i (frame N+1) the layer acquires a free image j without
 * blocking, records on the presenting queue a copy of the previous frame (kept in a history image)
 * into j and of image i into the history, then presents j followed by i. Phase 3 replaces the
 * history->j copy with optical flow + interpolation of (history, i); nothing else changes.
 *
 * When no image is free, or there is no history yet, only the real frame is presented and the
 * event is counted. Swapchains whose surface cannot support this fall back to pass-through. */

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out);

void afmf_swapchain_destroy(struct afmf_device *dev, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *alloc);

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info);

/* Frees whatever bookkeeping the application left behind when the device goes away. */
void afmf_swapchain_forget_all(struct afmf_device *dev);
