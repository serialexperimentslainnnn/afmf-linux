#include "swapchain.h"

#include "log.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

/* Presents between two cadence reports at debug level. */
#define AFMF_STATS_INTERVAL 300u

struct afmf_swapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode;
    uint32_t min_image_count;

    uint64_t present_count;
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

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    VkResult res = dev->create_swapchain(dev->handle, info, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_swapchain *sc = calloc(1, sizeof *sc);
    if (sc == NULL) {
        dev->destroy_swapchain(dev->handle, *out, alloc);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    sc->handle = *out;
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->present_mode = info->presentMode;
    sc->min_image_count = info->minImageCount;

    pthread_mutex_lock(&dev->lock);
    sc->next = dev->swapchains;
    dev->swapchains = sc;
    pthread_mutex_unlock(&dev->lock);

    AFMF_INFO("swapchain %p created: %ux%u, format %d, present mode %d, min images %u",
              (void *)sc->handle, sc->extent.width, sc->extent.height, (int)sc->format,
              (int)sc->present_mode, sc->min_image_count);
    return VK_SUCCESS;
}

static void report_and_free(struct afmf_swapchain *sc)
{
    AFMF_INFO("swapchain %p destroyed after %" PRIu64 " presents", (void *)sc->handle,
              sc->present_count);
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
            report_and_free(sc);
        else
            AFMF_WARN("destroying swapchain %p the layer never saw", (void *)swapchain);
    }
    dev->destroy_swapchain(dev->handle, swapchain, alloc);
}

VkResult afmf_swapchain_present(struct afmf_device *dev, VkQueue queue, const VkPresentInfoKHR *info)
{
    struct timespec now;
    bool have_now = clock_gettime(CLOCK_MONOTONIC, &now) == 0;

    pthread_mutex_lock(&dev->lock);
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        struct afmf_swapchain *sc = find_locked(dev, info->pSwapchains[i]);
        if (sc == NULL)
            continue;

        /* Cadence is measured between consecutive present calls on the CPU side: enough to size
         * the pacing work of phase 2, not a measurement of when the frame reached the display. */
        if (have_now) {
            if (sc->present_count > 0) {
                sc->frame_time_ms_accum += elapsed_ms(&sc->last_present, &now);
                sc->frame_time_samples++;
            }
            sc->last_present = now;
        }
        sc->present_count++;

        if (sc->frame_time_samples == AFMF_STATS_INTERVAL) {
            AFMF_DEBUG("swapchain %p: %" PRIu64 " presents, avg %.2f ms between presents",
                       (void *)sc->handle, sc->present_count,
                       sc->frame_time_ms_accum / (double)sc->frame_time_samples);
            sc->frame_time_ms_accum = 0.0;
            sc->frame_time_samples = 0;
        }
    }
    pthread_mutex_unlock(&dev->lock);

    return dev->queue_present(queue, info);
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
        report_and_free(sc);
        sc = next;
    }
}
