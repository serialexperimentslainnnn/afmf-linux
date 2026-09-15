#include "swapchain.h"

#include "config.h"
#include "framegen.h"
#include "log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

/* Presents between two cadence reports at debug level. */
#define AFMF_STATS_INTERVAL 300u
/* Wait semaphores an application may attach to one present before the layer gives up on it. */
#define AFMF_MAX_APP_WAITS 8u

/* One in-flight layer submission: its command buffer, the semaphore its acquire signals and the
 * fence that says the slot can be reused. */
struct afmf_slot {
    VkCommandBuffer cmd;
    VkSemaphore acquired;
    VkFence fence;
    bool pending;
};

struct afmf_swapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode;
    uint32_t min_image_count;

    /* Frame generation state; everything below `gen_enabled` is unused when it is false. */
    bool gen_enabled;
    uint32_t image_count;
    VkImage *images;
    VkSemaphore *sem_generated; /* per image: signals "the generated frame in image k is ready" */
    VkSemaphore *sem_real;      /* per image: signals "the layer is done reading image k" */
    struct afmf_slot *slots;    /* one per image, used round-robin */
    uint32_t slot_index;
    VkCommandPool pool;
    uint32_t pool_family;
    struct afmf_framegen *fg;   /* optical flow + interpolation; NULL means repeat frames */
    VkImage history;            /* the previous real frame, only used when fg is NULL */
    VkDeviceMemory history_memory;
    VkImageLayout history_layout;
    bool have_history;

    uint64_t present_count;
    uint64_t generated;
    uint64_t skipped_no_image;
    uint64_t skipped_no_history;
    struct timespec last_present;
    double frame_time_ms_accum;
    uint32_t frame_time_samples;

    struct afmf_swapchain *next;
};

static double elapsed_ms(const struct timespec *from, const struct timespec *to)
{
    return (double)(to->tv_sec - from->tv_sec) * 1e3 + (double)(to->tv_nsec - from->tv_nsec) / 1e6;
}

/* Caller holds dev->lock. */
static struct afmf_swapchain *find_locked(const struct afmf_device *dev, VkSwapchainKHR handle)
{
    for (struct afmf_swapchain *sc = dev->swapchains; sc != NULL; sc = sc->next)
        if (sc->handle == handle)
            return sc;
    return NULL;
}

/* Caller holds dev->lock. Returns the unlinked entry, or NULL when the handle was never tracked. */
static struct afmf_swapchain *unlink_locked(struct afmf_device *dev, VkSwapchainKHR handle)
{
    for (struct afmf_swapchain **link = &dev->swapchains; *link != NULL; link = &(*link)->next) {
        if ((*link)->handle == handle) {
            struct afmf_swapchain *sc = *link;
            *link = sc->next;
            return sc;
        }
    }
    return NULL;
}

/* ---- generation resources ------------------------------------------------------------------ */

static bool find_memory_type(const struct afmf_device *dev, uint32_t type_bits,
                             VkMemoryPropertyFlags wanted, uint32_t *out)
{
    const VkPhysicalDeviceMemoryProperties *props = &dev->memory_properties;
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & wanted) == wanted) {
            *out = i;
            return true;
        }
    }
    return false;
}

static VkResult create_history(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    VkImageCreateInfo image = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = sc->format,
        .extent = {sc->extent.width, sc->extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = dev->fns.create_image(dev->handle, &image, NULL, &sc->history);
    if (res != VK_SUCCESS)
        return res;

    VkMemoryRequirements reqs;
    dev->fns.get_image_memory_requirements(dev->handle, sc->history, &reqs);
    uint32_t type;
    if (!find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(dev, reqs.memoryTypeBits, 0, &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    res = dev->fns.allocate_memory(dev->handle, &alloc, NULL, &sc->history_memory);
    if (res != VK_SUCCESS)
        return res;
    sc->history_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return dev->fns.bind_image_memory(dev->handle, sc->history, sc->history_memory, 0);
}

/* Everything that does not depend on the presenting queue's family. */
static VkResult gen_init(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    VkResult res = dev->fns.get_swapchain_images(dev->handle, sc->handle, &sc->image_count, NULL);
    if (res != VK_SUCCESS)
        return res;
    sc->images = calloc(sc->image_count, sizeof *sc->images);
    sc->sem_generated = calloc(sc->image_count, sizeof *sc->sem_generated);
    sc->sem_real = calloc(sc->image_count, sizeof *sc->sem_real);
    sc->slots = calloc(sc->image_count, sizeof *sc->slots);
    if (sc->images == NULL || sc->sem_generated == NULL || sc->sem_real == NULL || sc->slots == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    res = dev->fns.get_swapchain_images(dev->handle, sc->handle, &sc->image_count, sc->images);
    if (res != VK_SUCCESS)
        return res;

    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < sc->image_count; i++) {
        res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->sem_generated[i]);
        if (res == VK_SUCCESS)
            res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->sem_real[i]);
        if (res == VK_SUCCESS)
            res = dev->fns.create_semaphore(dev->handle, &semaphore, NULL, &sc->slots[i].acquired);
        if (res == VK_SUCCESS)
            res = dev->fns.create_fence(dev->handle, &fence, NULL, &sc->slots[i].fence);
        if (res != VK_SUCCESS)
            return res;
    }
    if (afmf_config_get()->interpolate)
        sc->fg = afmf_framegen_create(dev, sc->format, sc->extent, sc->image_count);
    return sc->fg != NULL ? VK_SUCCESS : create_history(dev, sc);
}

/* The command pool needs the presenting queue's family, only known at the first present. */
static bool ensure_pool(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t family)
{
    if (sc->pool != VK_NULL_HANDLE)
        return sc->pool_family == family;

    const VkQueueFlags copy_capable =
        VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    if (family >= dev->queue_family_count ||
        (dev->queue_families[family].queueFlags & copy_capable) == 0) {
        AFMF_WARN("swapchain %p: presenting queue family %u cannot copy images", (void *)sc->handle,
                  family);
        return false;
    }

    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->fns.create_command_pool(dev->handle, &pool, NULL, &sc->pool) != VK_SUCCESS)
        return false;
    sc->pool_family = family;

    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = sc->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (dev->set_loader_data == NULL) {
        AFMF_WARN("swapchain %p: loader offered no pfnSetDeviceLoaderData", (void *)sc->handle);
        return false;
    }
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (dev->fns.allocate_command_buffers(dev->handle, &alloc, &sc->slots[i].cmd) != VK_SUCCESS)
            return false;
        /* Allocated below the loader's trampoline: stamp the dispatch pointer ourselves. */
        if (dev->set_loader_data(dev->handle, sc->slots[i].cmd) != VK_SUCCESS)
            return false;
    }
    return true;
}

static void gen_teardown(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    const struct afmf_device_fns *f = &dev->fns;
    if (sc->slots != NULL) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            struct afmf_slot *slot = &sc->slots[i];
            if (slot->pending)
                (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
            if (slot->fence != VK_NULL_HANDLE)
                f->destroy_fence(dev->handle, slot->fence, NULL);
            if (slot->acquired != VK_NULL_HANDLE)
                f->destroy_semaphore(dev->handle, slot->acquired, NULL);
        }
    }
    if (sc->pool != VK_NULL_HANDLE)
        f->destroy_command_pool(dev->handle, sc->pool, NULL); /* frees the command buffers */
    afmf_framegen_destroy(dev, sc->fg);
    sc->fg = NULL;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (sc->sem_generated != NULL && sc->sem_generated[i] != VK_NULL_HANDLE)
            f->destroy_semaphore(dev->handle, sc->sem_generated[i], NULL);
        if (sc->sem_real != NULL && sc->sem_real[i] != VK_NULL_HANDLE)
            f->destroy_semaphore(dev->handle, sc->sem_real[i], NULL);
    }
    if (sc->history != VK_NULL_HANDLE)
        f->destroy_image(dev->handle, sc->history, NULL);
    if (sc->history_memory != VK_NULL_HANDLE)
        f->free_memory(dev->handle, sc->history_memory, NULL);
    free(sc->slots);
    free(sc->sem_real);
    free(sc->sem_generated);
    free(sc->images);
    sc->slots = NULL;
    sc->sem_real = NULL;
    sc->sem_generated = NULL;
    sc->images = NULL;
    sc->pool = VK_NULL_HANDLE;
    sc->history = VK_NULL_HANDLE;
    sc->history_memory = VK_NULL_HANDLE;
    sc->gen_enabled = false;
}

/* Stops generating on a live swapchain. Its semaphores may still be awaited by the presentation
 * engine, so the resources stay until the swapchain is destroyed. */
static void gen_disable(struct afmf_swapchain *sc, const char *why)
{
    AFMF_WARN("swapchain %p: %s; pass-through from now on", (void *)sc->handle, why);
    sc->gen_enabled = false;
}

/* ---- create / destroy ---------------------------------------------------------------------- */

/* Why this swapchain cannot host generated frames, or NULL when it can. */
static const char *generation_blocker(const struct afmf_device *dev,
                                      const VkSwapchainCreateInfoKHR *info,
                                      VkSurfaceCapabilitiesKHR *caps)
{
    const VkImageUsageFlags copy_usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (dev->ifns.get_surface_capabilities == NULL ||
        dev->ifns.get_surface_capabilities(dev->physical_device, info->surface, caps) != VK_SUCCESS)
        return "surface capabilities unavailable";
    if (info->imageArrayLayers != 1)
        return "layered swapchain";
    if (info->presentMode == VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR ||
        info->presentMode == VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR)
        return "shared present mode";
    if ((caps->supportedUsageFlags & copy_usage) != copy_usage)
        return "surface images cannot be copied";
    if (caps->maxImageCount != 0 &&
        info->minImageCount + afmf_config_get()->extra_images > caps->maxImageCount)
        return "no room for the extra images";
    return NULL;
}

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    VkSurfaceCapabilitiesKHR caps;
    const char *blocker = generation_blocker(dev, info, &caps);

    VkSwapchainCreateInfoKHR patched = *info;
    if (blocker == NULL) {
        patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        /* Generated frames are presented from images of the same swapchain; the presentation
         * engine keeps a few queued, so one extra is not enough to find one free at present time. */
        patched.minImageCount += afmf_config_get()->extra_images;
    }

    VkResult res = dev->fns.create_swapchain(dev->handle, &patched, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_swapchain *sc = calloc(1, sizeof *sc);
    if (sc == NULL) {
        dev->fns.destroy_swapchain(dev->handle, *out, alloc);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    sc->handle = *out;
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->present_mode = info->presentMode;
    sc->min_image_count = info->minImageCount;

    if (blocker == NULL) {
        sc->gen_enabled = true;
        VkResult gen = gen_init(dev, sc);
        if (gen != VK_SUCCESS) {
            AFMF_WARN("swapchain %p: generation resources failed (VkResult %d); pass-through",
                      (void *)sc->handle, (int)gen);
            gen_teardown(dev, sc);
        }
    } else {
        AFMF_INFO("swapchain %p: pass-through (%s)", (void *)sc->handle, blocker);
    }

    pthread_mutex_lock(&dev->lock);
    sc->next = dev->swapchains;
    dev->swapchains = sc;
    pthread_mutex_unlock(&dev->lock);

    AFMF_INFO("swapchain %p created: %ux%u, format %d, present mode %d, %u images (app asked %u), "
              "generation %s",
              (void *)sc->handle, sc->extent.width, sc->extent.height, (int)sc->format,
              (int)sc->present_mode, sc->image_count, sc->min_image_count,
              sc->gen_enabled ? "on" : "off");
    return VK_SUCCESS;
}

static void report_and_free(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    AFMF_INFO("swapchain %p destroyed after %" PRIu64 " presents: %" PRIu64 " generated, %" PRIu64
              " skipped (%" PRIu64 " no free image, %" PRIu64 " no history)",
              (void *)sc->handle, sc->present_count, sc->generated,
              sc->skipped_no_image + sc->skipped_no_history, sc->skipped_no_image,
              sc->skipped_no_history);
    gen_teardown(dev, sc);
    free(sc);
}

void afmf_swapchain_destroy(struct afmf_device *dev, VkSwapchainKHR swapchain,
                            const VkAllocationCallbacks *alloc)
{
    if (swapchain != VK_NULL_HANDLE) {
        pthread_mutex_lock(&dev->lock);
        struct afmf_swapchain *sc = unlink_locked(dev, swapchain);
        pthread_mutex_unlock(&dev->lock);

        if (sc != NULL)
            report_and_free(dev, sc);
        else
            AFMF_WARN("destroying swapchain %p the layer never saw", (void *)swapchain);
    }
    dev->fns.destroy_swapchain(dev->handle, swapchain, alloc);
}

void afmf_swapchain_forget_all(struct afmf_device *dev)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = dev->swapchains;
    dev->swapchains = NULL;
    pthread_mutex_unlock(&dev->lock);

    while (sc != NULL) {
        struct afmf_swapchain *next = sc->next;
        AFMF_WARN("swapchain %p still alive at device destruction", (void *)sc->handle);
        report_and_free(dev, sc);
        sc = next;
    }
}

/* ---- present ------------------------------------------------------------------------------- */

static void image_barrier(const struct afmf_device *dev, VkCommandBuffer cmd, VkImage image,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    dev->fns.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void copy_whole(const struct afmf_device *dev, VkCommandBuffer cmd, VkImage src, VkImage dst,
                       VkExtent2D extent)
{
    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    dev->fns.cmd_copy_image(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

/* Records the companion for image i (frame N+1) into image j, and remembers frame N+1 for next
 * time. With interpolation: the optical flow and the interpolated frame (framegen.c). Without it:
 * a copy of the history image into j. Image i arrives in PRESENT_SRC (the application's
 * obligation) and is handed back in PRESENT_SRC. */
static VkResult record_frame(struct afmf_device *dev, struct afmf_swapchain *sc, VkCommandBuffer cmd,
                             uint32_t slot, uint32_t i, bool generate, uint32_t j)
{
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = dev->fns.begin_command_buffer(cmd, &begin);
    if (res != VK_SUCCESS)
        return res;

    const VkPipelineStageFlags top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags bottom = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (sc->fg != NULL) {
        image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, top,
                      transfer);
        if (generate)
            image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, top,
                          transfer);
        afmf_framegen_record(dev, sc->fg, cmd, slot, sc->images[i],
                             generate ? sc->images[j] : VK_NULL_HANDLE);
        if (generate)
            image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, transfer,
                          bottom);
        image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0, transfer,
                      bottom);
        return dev->fns.end_command_buffer(cmd);
    }

    if (generate) {
        image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, top,
                      transfer);
        image_barrier(dev, cmd, sc->history, sc->history_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, transfer, transfer);
        sc->history_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy_whole(dev, cmd, sc->history, sc->images[j], sc->extent);
        image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, transfer,
                      bottom);
    }

    image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, top,
                  transfer);
    VkAccessFlags history_access =
        sc->history_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier(dev, cmd, sc->history, sc->history_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  history_access, VK_ACCESS_TRANSFER_WRITE_BIT, transfer, transfer);
    sc->history_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy_whole(dev, cmd, sc->images[i], sc->history, sc->extent);
    image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0, transfer,
                  bottom);

    return dev->fns.end_command_buffer(cmd);
}

static void update_cadence(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    struct timespec now;
    bool have_now = clock_gettime(CLOCK_MONOTONIC, &now) == 0;

    pthread_mutex_lock(&dev->lock);
    /* Cadence is measured between consecutive present calls on the CPU side: enough to size the
     * pacing work, not a measurement of when the frame reached the display. */
    if (have_now) {
        if (sc->present_count > 0) {
            sc->frame_time_ms_accum += elapsed_ms(&sc->last_present, &now);
            sc->frame_time_samples++;
        }
        sc->last_present = now;
    }
    sc->present_count++;
    if (sc->frame_time_samples == AFMF_STATS_INTERVAL) {
        AFMF_DEBUG("swapchain %p: %" PRIu64 " presents, %" PRIu64 " generated, avg %.2f ms between "
                   "presents",
                   (void *)sc->handle, sc->present_count, sc->generated,
                   sc->frame_time_ms_accum / (double)sc->frame_time_samples);
        sc->frame_time_ms_accum = 0.0;
        sc->frame_time_samples = 0;
    }
    pthread_mutex_unlock(&dev->lock);
}

/* The application's present, with the generated frame in front of it when one could be made. */
static VkResult present_generated(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                  uint32_t family, const VkPresentInfoKHR *info)
{
    const struct afmf_device_fns *f = &dev->fns;
    uint32_t i = info->pImageIndices[0];

    if (!ensure_pool(dev, sc, family) || i >= sc->image_count ||
        info->waitSemaphoreCount > AFMF_MAX_APP_WAITS) {
        gen_disable(sc, "cannot generate on this present path");
        return f->queue_present(queue, info);
    }

    struct afmf_slot *slot = &sc->slots[sc->slot_index];
    if (slot->pending) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        (void)f->reset_fences(dev->handle, 1, &slot->fence);
        slot->pending = false;
    }

    /* Acquire the image the generated frame goes into, waiting at most a short, bounded time: the
     * application may hold every other image, and then this frame simply gets no companion. */
    bool generate = sc->have_history;
    uint32_t j = 0;
    if (generate) {
        VkResult acquired = f->acquire_next_image(dev->handle, sc->handle,
                                                  afmf_config_get()->acquire_timeout_ns,
                                                  slot->acquired, VK_NULL_HANDLE, &j);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            generate = false;
            sc->skipped_no_image++;
        }
    } else {
        sc->skipped_no_history++;
    }

    VkResult res = record_frame(dev, sc, slot->cmd, sc->slot_index, i, generate, j);
    if (res == VK_SUCCESS) {
        VkSemaphore waits[AFMF_MAX_APP_WAITS + 1];
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS + 1];
        uint32_t wait_count = 0;
        for (uint32_t k = 0; k < info->waitSemaphoreCount; k++) {
            waits[wait_count] = info->pWaitSemaphores[k];
            stages[wait_count++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        if (generate) {
            waits[wait_count] = slot->acquired;
            stages[wait_count++] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        VkSemaphore signals[2] = {sc->sem_real[i], generate ? sc->sem_generated[j] : VK_NULL_HANDLE};
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = wait_count,
            .pWaitSemaphores = waits,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &slot->cmd,
            .signalSemaphoreCount = generate ? 2u : 1u,
            .pSignalSemaphores = signals,
        };
        res = f->queue_submit(queue, 1, &submit, slot->fence);
    }
    if (res != VK_SUCCESS) {
        /* The application's semaphores were not consumed, so its own present still works. The
         * image acquired for the generated frame, if any, stays with the presentation engine. */
        gen_disable(sc, "layer submission failed");
        return f->queue_present(queue, info);
    }
    slot->pending = true;
    sc->slot_index = (sc->slot_index + 1) % sc->image_count;
    sc->have_history = true;

    /* Debug dumps block on the submission; only while AFMF_DUMP_DIR asks for frames. */
    if (sc->fg != NULL && afmf_framegen_dump_pending(sc->fg)) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        afmf_framegen_dump_write(dev, sc->fg);
    }

    if (generate) {
        VkPresentInfoKHR companion = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sc->sem_generated[j],
            .swapchainCount = 1,
            .pSwapchains = &sc->handle,
            .pImageIndices = &j,
        };
        VkResult presented = f->queue_present(queue, &companion);
        if (presented < 0)
            AFMF_DEBUG("swapchain %p: generated present returned %d", (void *)sc->handle,
                       (int)presented);
        sc->generated++;
    }

    /* The real frame keeps the application's pNext chain and pResults; only the wait moves to the
     * semaphore the layer signals once it has finished reading the image. */
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &sc->sem_real[i];
    return f->queue_present(queue, &real);
}

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = info->swapchainCount == 1 ? find_locked(dev, info->pSwapchains[0])
                                                          : NULL;
    pthread_mutex_unlock(&dev->lock);
    if (sc == NULL) {
        for (uint32_t k = 0; k < info->swapchainCount; k++) {
            pthread_mutex_lock(&dev->lock);
            struct afmf_swapchain *each = find_locked(dev, info->pSwapchains[k]);
            pthread_mutex_unlock(&dev->lock);
            if (each != NULL)
                update_cadence(dev, each);
        }
        return dev->fns.queue_present(queue, info);
    }

    update_cadence(dev, sc);
    uint32_t family;
    if (!sc->gen_enabled || !afmf_device_queue_family(dev, queue, &family))
        return dev->fns.queue_present(queue, info);
    return present_generated(dev, sc, queue, family, info);
}
