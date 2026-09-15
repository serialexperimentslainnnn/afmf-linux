#pragma once

#include "layer.h"

/* Frame generation proper: FidelityFX Optical Flow (colour in, 8x8 block motion + scene change
 * detector out) followed by the layer's own interpolation shader. One instance per swapchain; the
 * pipelines are shared per device and built on first use. */

struct afmf_framegen;

/* NULL when the swapchain format has no interpolation variant or the device lacks a required
 * format feature; the caller then falls back to repeating frames. */
struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots);

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg);

/* Records, into a command buffer on a compute-capable queue:
 *   1. a copy of `current` (a swapchain image in TRANSFER_SRC_OPTIMAL) into the colour ring,
 *   2. the luma pyramid and scene change detector on the new frame, and, when `target` is not
 *      VK_NULL_HANDLE, the block search,
 *   3. when `target` is not VK_NULL_HANDLE (a swapchain image in TRANSFER_DST_OPTIMAL): the
 *      interpolated frame between the previous and the new one, copied into it.
 * Without a target the frame only enters the history (about a fifth of the cost).
 * `slot` selects the constants region so `slots` command buffers can be in flight. */
void afmf_framegen_record(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, VkImage target);

/* Pyramid levels the search uses from now on, between 5 and what the swapchain was set up with;
 * fewer levels cost less and reach shorter motion. The governor lowers it under GPU contention. */
void afmf_framegen_set_levels(struct afmf_framegen *fg, uint32_t levels);
uint32_t afmf_framegen_max_levels(const struct afmf_framegen *fg);

/* Debug dumps (AFMF_DUMP_DIR): afmf_framegen_record also copies the generated frame into a
 * readback buffer while `afmf_framegen_dump_pending` is true; once the submission has completed,
 * afmf_framegen_dump_write stores it as <dir>/afmf_generated_<n>.ppm and returns false when no
 * more dumps are wanted. 8-bit variants only. */
bool afmf_framegen_dump_pending(const struct afmf_framegen *fg);
void afmf_framegen_dump_write(struct afmf_device *dev, struct afmf_framegen *fg);

/* Frees the per-device pipelines; called from vkDestroyDevice. */
void afmf_framegen_pipelines_destroy(struct afmf_device *dev);
