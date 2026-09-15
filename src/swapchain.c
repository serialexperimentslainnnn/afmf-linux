#include "swapchain.h"

#include "config.h"
#include "framegen.h"
#include "log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Presents between two cadence reports at debug level. */
#define AFMF_STATS_INTERVAL 300u
/* Queue families a CONCURRENT swapchain can be shared between (the application's plus ours). */
#define AFMF_MAX_FAMILIES 8u
/* Wait semaphores an application may attach to one present before the layer gives up on it. */
#define AFMF_MAX_APP_WAITS 8u
/* Presents queued for the presentation thread; bounded by the images in flight in practice. */
#define AFMF_MAX_JOBS 16u
/* The application's acquire is run in slices of this length under the swapchain lock, so the
 * presentation thread is never held off for longer than one slice. */
#define AFMF_ACQUIRE_SLICE_NS 1000000ull
/* Pacing delay bounds for the real frame: half the frame time, clamped. */
#define AFMF_PACING_MIN_NS 500000ull
#define AFMF_PACING_MAX_NS 20000000ull
/* Governor: the generated frame ready later than this fraction of the frame time, this many
 * frames in a row, steps generation down; ready before the lower fraction for this many frames
 * steps it back up. Steps: 0 everything, 1 five search levels, 2 one companion in two, 3 in three.
 * Late means a frame and a half: the fence includes the game's own queue depth, a GPU at 100 %
 * is the normal state of a game, and a companion ready within the frame still lands before the
 * next real one. Half a second of lateness steps down; a quarter of a second within three
 * quarters of a frame steps back up, so a step down costs fps for as short a time as it can. */
#define AFMF_GOVERNOR_LATE 1.5
#define AFMF_GOVERNOR_EARLY 0.75
#define AFMF_GOVERNOR_LATE_FRAMES 30u
#define AFMF_GOVERNOR_EARLY_FRAMES 15u
#define AFMF_GOVERNOR_STEPS 3u

/* One frame handed to the presentation thread: the generated image first, the real one after
 * the pacing delay, each with what the application's pNext chain carried that survives being
 * copied (present id, per-present mode). */
struct afmf_present_job {
    uint32_t real_image;
    uint32_t companion_image;
    bool generate;
    struct timespec arrival;
    uint64_t hold_ns;         /* half a frame: the real frame goes out this long after the generated one is ready */
    VkFence done;             /* the layer's submission for this frame: signalled once the generated frame is ready */
    bool have_present_id;
    uint64_t present_id;
    bool present_id_v2;   /* VK_KHR_present_id2 carried the id (same shape, its own sType) */
    bool have_present_mode;
    VkPresentModeKHR present_mode;
    VkFence present_fence; /* VK_EXT_swapchain_maintenance1: signalled with the real frame */
#ifdef VK_EXT_present_timing
    /* Newer headers only (distribution packages build against older ones): without the
     * extension in the header, a timing request makes that present inline, like any unknown
     * structure. */
    bool have_timing;      /* VK_EXT_present_timing: the application's timing request, real frame */
    VkPresentTimingInfoEXT timing;
#endif
};

/* One in-flight layer submission: its command buffer and the fence that says the slot can be
 * reused. */
struct afmf_slot {
    VkCommandBuffer cmd;
    VkFence fence;
    bool pending;
};

struct afmf_swapchain {
    VkSwapchainKHR handle;
    VkFormat format;
    VkExtent2D extent;
    VkPresentModeKHR present_mode; /* the swapchain's, after the layer's rewrite */
    bool fifo_to_mailbox;          /* the application asked for FIFO; per-present modes are rewritten too */
    /* The per-present modes the swapchain allows (VK_EXT_swapchain_maintenance1) after the
     * rewrite: the application's list cut down to what is compatible with MAILBOX. Empty when
     * the application gave no list. */
    VkPresentModeKHR allowed_modes[8];
    uint32_t allowed_mode_count;
    bool storage_usage; /* the images carry STORAGE usage for direct output (AFMF_DIRECT_OUTPUT) */
    bool sampled_usage; /* the images carry SAMPLED usage for the direct ingest (AFMF_DIRECT_INGEST) */
    uint32_t min_image_count;

    /* Frame generation state; everything below `gen_enabled` is unused when it is false. */
    bool gen_enabled;
    bool async;                 /* work and presents go to dev->async_queue */
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
    /* The image the next companion goes into. Acquired without waiting, ideally at the end of the
     * previous present, so the application's thread never waits for the presentation engine: on a
     * FIFO desktop a free image comes back one refresh period after a present, and waiting for it
     * in the hook cost the application that whole period (measured: 5.1-5.9 ms at 165 Hz). */
    bool spare_valid;
    uint32_t spare_image;
    VkFence spare_fence; /* signalled once the presentation engine has released spare_image */

    uint64_t present_count;
    uint64_t generated;
    uint64_t skipped_no_image;
    uint64_t skipped_no_history;
    struct timespec last_present;
    double frame_time_ms_accum;
    uint32_t frame_time_samples;
    /* Host time the application's present thread spends inside the layer, per interval: the
     * whole hook and the three places it can block (slot fence, companion acquire, presents). */
    double hook_ms, fence_ms, acquire_ms, record_ms, submit_ms;
    double present_ms, present_real_ms, refill_ms, hold_ms;
    double gpu_delay_ms; /* generated frame ready this long after the present call (GPU contention) */
    double last_delay_ms; /* the latest such delay, what the governor reads */
    /* Governor (hook side, under dev->lock with the cadence): current step and the streaks that
     * move it. */
    uint32_t governor_step;
    uint32_t late_streak, early_streak;
    uint64_t reduced_frames; /* presents that went out without a companion because of the step */
    double frame_ms_ema; /* smoothed time between the application's presents, for pacing */

    /* Presentation thread. Under Proton each vkQueuePresentKHR measured 170-400 us of host time
     * on the application's thread; both presents (and the spare refill) run here instead, and
     * the real frame is held back half a frame so the generated one lands in between. Only used
     * when the layer presents from its own queue (`async`): from the application's queue its
     * thread would race the application's submits. */
    struct afmf_device *dev;
    pthread_t presenter;
    bool presenter_running;
    bool presenter_stop;
    pthread_mutex_t job_lock;
    pthread_cond_t job_cond;   /* new job, or stop */
    pthread_cond_t drain_cond; /* the queue emptied */
    struct afmf_present_job jobs[AFMF_MAX_JOBS];
    uint32_t job_head, job_count;
    VkResult deferred_result; /* worst result of presents done so far, returned by the next present call */
    /* vkAcquireNextImageKHR and vkQueuePresentKHR both need external synchronisation on the
     * swapchain: the application's acquires, the layer's spare acquires and the presentation
     * thread's presents all take this. */
    pthread_mutex_t wsi_lock;
    /* Taken before wsi_lock by everyone (wsi_take/wsi_give): a glibc mutex is not fair, and the
     * application's acquire loop re-took wsi_lock the instant it released it, so the
     * presentation thread never got a turn, presented nothing, and no image ever came free. */
    pthread_mutex_t wsi_turn;

    struct afmf_swapchain *next;
};

static void wsi_take(struct afmf_swapchain *sc)
{
    pthread_mutex_lock(&sc->wsi_turn);
    pthread_mutex_lock(&sc->wsi_lock);
    pthread_mutex_unlock(&sc->wsi_turn);
}

static void wsi_give(struct afmf_swapchain *sc)
{
    pthread_mutex_unlock(&sc->wsi_lock);
}

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
            res = dev->fns.create_fence(dev->handle, &fence, NULL, &sc->slots[i].fence);
        if (res != VK_SUCCESS)
            return res;
    }
    res = dev->fns.create_fence(dev->handle, &fence, NULL, &sc->spare_fence);
    if (res != VK_SUCCESS)
        return res;
    if (afmf_config_get()->interpolate)
        sc->fg = afmf_framegen_create(dev, sc->format, sc->extent, sc->image_count, sc->images,
                                      sc->image_count, sc->storage_usage, sc->sampled_usage);
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
    if (sc->fg != NULL)
        afmf_framegen_set_family(dev, sc->fg, family);
    return true;
}

static void presenter_stop(struct afmf_swapchain *sc);
static void presenter_drain(struct afmf_swapchain *sc);

static void gen_teardown(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    const struct afmf_device_fns *f = &dev->fns;
    presenter_stop(sc); /* drains what is queued: the swapchain is still alive here */
    if (sc->async) {
        /* The layer's queue may still be presenting from this swapchain. */
        pthread_mutex_lock(&dev->async_lock);
        (void)f->queue_wait_idle(dev->async_queue);
        pthread_mutex_unlock(&dev->async_lock);
    }
    if (sc->slots != NULL) {
        for (uint32_t i = 0; i < sc->image_count; i++) {
            struct afmf_slot *slot = &sc->slots[i];
            if (slot->pending)
                (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
            if (slot->fence != VK_NULL_HANDLE)
                f->destroy_fence(dev->handle, slot->fence, NULL);
        }
    }
    if (sc->spare_fence != VK_NULL_HANDLE) {
        /* A spare never presented still has its release pending; bounded, a compositor that never
         * answers must not hang the application's teardown. */
        if (sc->spare_valid &&
            f->wait_for_fences(dev->handle, 1, &sc->spare_fence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
            AFMF_WARN("swapchain %p: spare image never released", (void *)sc->handle);
        f->destroy_fence(dev->handle, sc->spare_fence, NULL);
        sc->spare_fence = VK_NULL_HANDLE;
        sc->spare_valid = false;
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
    presenter_stop(sc);
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
    if (afmf_config_get()->passive)
        return "this process is Gamescope itself (AFMF_GAMESCOPE=1); the game inside gets the layer";
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

static bool surface_offers(const struct afmf_device *dev, VkSurfaceKHR surface, VkPresentModeKHR mode)
{
    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    if (dev->ifns.get_surface_present_modes == NULL)
        return false;
    VkResult res = dev->ifns.get_surface_present_modes(dev->physical_device, surface, &n, modes);
    if (res != VK_SUCCESS && res != VK_INCOMPLETE)
        return false;
    for (uint32_t i = 0; i < n; i++)
        if (modes[i] == mode)
            return true;
    return false;
}

/* Size of a structure that may hang off VkSwapchainCreateInfoKHR, 0 for one the layer does not
 * know (the chain is then left alone). */
static size_t chain_node_size(VkStructureType type)
{
    switch ((int)type) {
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT:
        return sizeof(VkSwapchainPresentModesCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT:
        return sizeof(VkSwapchainPresentScalingCreateInfoEXT);
    case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO:
        return sizeof(VkImageFormatListCreateInfo);
    case VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT:
        return sizeof(VkImageCompressionControlEXT);
    case VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR:
        return sizeof(VkDeviceGroupSwapchainCreateInfoKHR);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT:
        return sizeof(VkSwapchainCounterCreateInfoEXT);
    case VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD:
        return sizeof(VkSwapchainDisplayNativeHdrCreateInfoAMD);
#ifdef VK_NV_low_latency2
    case VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV:
        return sizeof(VkSwapchainLatencyCreateInfoNV);
#endif
#ifdef VK_NV_present_barrier
    case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_BARRIER_CREATE_INFO_NV:
        return sizeof(VkSwapchainPresentBarrierCreateInfoNV);
#endif
    default:
        return 0;
    }
}

/* Bytes and present modes a copied create chain may need. */
#define AFMF_CHAIN_BYTES 1024u
#define AFMF_MAX_PRESENT_MODES 8u

/* Whether the swapchain may be created in MAILBOX with this chain. The list of allowed per-present
 * modes (VK_EXT_swapchain_maintenance1) must only hold modes compatible with the swapchain's, so
 * when the application gave one the whole chain is copied into `storage` (the chain is const,
 * and a node cannot be replaced without copying what precedes it) with the list cut down to what
 * the surface declares compatible with MAILBOX, MAILBOX included; `modes` receives that list. `*out`
 * is the chain to use. False means a structure the layer cannot copy, or no way to ask the surface:
 * the application's mode then stays. */
static bool chain_allows_mailbox(const struct afmf_device *dev, VkSurfaceKHR surface,
                                 const void *chain, uint64_t *storage, VkPresentModeKHR *modes,
                                 uint32_t *mode_count, const void **out)
{
    const VkSwapchainPresentModesCreateInfoEXT *list = NULL;
    for (const VkBaseInStructure *s = chain; s != NULL; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT)
            list = (const VkSwapchainPresentModesCreateInfoEXT *)s;
    *out = chain;
    *mode_count = 0;
    if (list == NULL)
        return true;

    /* What may share a swapchain with MAILBOX on this surface. */
    VkPresentModeKHR compatible[16];
    VkSurfacePresentModeCompatibilityEXT compatibility = {
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_EXT,
        .presentModeCount = 16,
        .pPresentModes = compatible,
    };
    VkSurfaceCapabilities2KHR caps2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
                                       .pNext = &compatibility};
    VkSurfacePresentModeEXT mailbox = {.sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_EXT,
                                       .presentMode = VK_PRESENT_MODE_MAILBOX_KHR};
    VkPhysicalDeviceSurfaceInfo2KHR surface_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
        .pNext = &mailbox,
        .surface = surface,
    };
    if (dev->ifns.get_surface_capabilities2 == NULL ||
        dev->ifns.get_surface_capabilities2(dev->physical_device, &surface_info, &caps2) != VK_SUCCESS)
        return false;
    uint32_t n = 0;
    for (uint32_t i = 0; i < list->presentModeCount && n < AFMF_MAX_PRESENT_MODES - 1; i++) {
        bool ok = false;
        for (uint32_t k = 0; k < compatibility.presentModeCount && !ok; k++)
            ok = compatible[k] == list->pPresentModes[i];
        if (ok && list->pPresentModes[i] != VK_PRESENT_MODE_MAILBOX_KHR)
            modes[n++] = list->pPresentModes[i];
    }
    modes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    *mode_count = n;

    size_t used = 0;
    VkBaseOutStructure *prev = NULL;
    for (const VkBaseInStructure *s = chain; s != NULL; s = s->pNext) {
        size_t size = chain_node_size(s->sType);
        used = (used + 15u) & ~(size_t)15u;
        if (size == 0 || used + size > AFMF_CHAIN_BYTES)
            return false;
        size_t index = used / sizeof *storage;
        VkBaseOutStructure *copy = (VkBaseOutStructure *)(storage + index);
        memcpy(copy, s, size);
        copy->pNext = NULL;
        if (copy->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT) {
            VkSwapchainPresentModesCreateInfoEXT *l = (VkSwapchainPresentModesCreateInfoEXT *)copy;
            l->presentModeCount = n;
            l->pPresentModes = modes;
        }
        if (prev != NULL)
            prev->pNext = copy;
        else
            *out = copy;
        prev = copy;
        used += size;
    }
    return true;
}

VkResult afmf_swapchain_create(struct afmf_device *dev, const VkSwapchainCreateInfoKHR *info,
                               const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    VkSurfaceCapabilitiesKHR caps;
    const char *blocker = generation_blocker(dev, info, &caps);

    VkSwapchainCreateInfoKHR patched = *info;
    uint32_t families[AFMF_MAX_FAMILIES];
    _Alignas(16) uint64_t chain_storage[AFMF_CHAIN_BYTES / sizeof(uint64_t)];
    VkPresentModeKHR chain_modes[AFMF_MAX_PRESENT_MODES];
    uint32_t chain_mode_count = 0;
    bool async = false;
    bool fifo_to_mailbox = false;
    bool storage_usage = false;
    bool sampled_usage = false;
    if (blocker == NULL) {
        /* Direct ingest: the layer samples the game's frame straight into its colour ring and
         * luma (one read instead of a copy and a luma pass); needs SAMPLED usage on the images. */
        if (afmf_config_get()->direct_ingest) {
            sampled_usage = (caps.supportedUsageFlags & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 &&
                            afmf_framegen_can_ingest_direct(dev, info->imageFormat);
            if (sampled_usage)
                patched.imageUsage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            else
                AFMF_INFO("direct ingest unavailable for format %d on this surface; copying",
                          (int)info->imageFormat);
        }
        /* Direct output: the interpolator writes the swapchain image, so it needs STORAGE usage;
         * only where the surface and the format take it (sRGB formats do not). */
        if (afmf_config_get()->direct_output) {
            storage_usage = (caps.supportedUsageFlags & VK_IMAGE_USAGE_STORAGE_BIT) != 0 &&
                            afmf_framegen_can_write_direct(dev, info->imageFormat);
            if (storage_usage)
                patched.imageUsage |= VK_IMAGE_USAGE_STORAGE_BIT;
            else
                AFMF_INFO("direct output unavailable for format %d on this surface; copying",
                          (int)info->imageFormat);
        }
        /* Every present takes a refresh slot in FIFO and the layer doubles the presents: a game
         * above half the refresh rate loses real frames (120 at 165 Hz -> 82). MAILBOX shows the
         * latest frame and drops the excess instead, which is what the doubling needs. */
        if (afmf_config_get()->present_mode == AFMF_PRESENT_AUTO &&
            (info->presentMode == VK_PRESENT_MODE_FIFO_KHR ||
             info->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) &&
            surface_offers(dev, info->surface, VK_PRESENT_MODE_MAILBOX_KHR)) {
            if (chain_allows_mailbox(dev, info->surface, info->pNext, chain_storage, chain_modes,
                                     &chain_mode_count, &patched.pNext)) {
                patched.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                fifo_to_mailbox = true;
            } else {
                AFMF_INFO("swapchain create chain carries a structure the layer cannot copy: "
                          "present mode %d kept",
                          (int)info->presentMode);
            }
        }
        patched.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        /* Generated frames are presented from images of the same swapchain; the presentation
         * engine keeps a few queued, so one extra is not enough to find one free at present time. */
        patched.minImageCount += afmf_config_get()->extra_images;

        /* With a queue of its own the layer works and presents from there, so the images must
         * be usable from the application's families and ours without ownership transfers. */
        VkBool32 can_present = VK_FALSE;
        if (dev->async_queue != VK_NULL_HANDLE && dev->ifns.get_surface_support != NULL &&
            dev->ifns.get_surface_support(dev->physical_device, dev->async_family, info->surface,
                                          &can_present) == VK_SUCCESS &&
            can_present) {
            uint32_t n = 0;
            const uint32_t *base = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT
                                       ? info->pQueueFamilyIndices
                                       : dev->app_families;
            uint32_t base_count = info->imageSharingMode == VK_SHARING_MODE_CONCURRENT
                                      ? info->queueFamilyIndexCount
                                      : dev->app_family_count;
            for (uint32_t i = 0; i < base_count && n < AFMF_MAX_FAMILIES; i++)
                if (base[i] != dev->async_family)
                    families[n++] = base[i];
            if (n < AFMF_MAX_FAMILIES)
                families[n++] = dev->async_family;
            if (n >= 2) {
                patched.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                patched.queueFamilyIndexCount = n;
                patched.pQueueFamilyIndices = families;
            }
            async = true; /* n == 1 means the application already lives on our family */
        }
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
    sc->dev = dev;
    sc->format = info->imageFormat;
    sc->extent = info->imageExtent;
    sc->present_mode = patched.presentMode;
    sc->fifo_to_mailbox = fifo_to_mailbox;
    sc->storage_usage = storage_usage;
    sc->sampled_usage = sampled_usage;
    memcpy(sc->allowed_modes, chain_modes, chain_mode_count * sizeof *chain_modes);
    sc->allowed_mode_count = chain_mode_count;
    sc->min_image_count = info->minImageCount;
    sc->async = async;
    sc->deferred_result = VK_SUCCESS;
    pthread_mutex_init(&sc->wsi_lock, NULL);
    pthread_mutex_init(&sc->wsi_turn, NULL);
    pthread_mutex_init(&sc->job_lock, NULL);
    pthread_condattr_t monotonic;
    pthread_condattr_init(&monotonic);
    pthread_condattr_setclock(&monotonic, CLOCK_MONOTONIC);
    pthread_cond_init(&sc->job_cond, &monotonic);
    pthread_cond_init(&sc->drain_cond, NULL);
    pthread_condattr_destroy(&monotonic);

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

    AFMF_INFO("swapchain %p created: %ux%u, format %d, present mode %d (app asked %d), %u images "
              "(app asked %u), generation %s%s",
              (void *)sc->handle, sc->extent.width, sc->extent.height, (int)sc->format,
              (int)sc->present_mode, (int)info->presentMode, sc->image_count, sc->min_image_count,
              sc->gen_enabled ? "on" : "off", sc->async ? " (layer queue)" : "");
    return VK_SUCCESS;
}

static void report_and_free(struct afmf_device *dev, struct afmf_swapchain *sc)
{
    AFMF_INFO("swapchain %p destroyed after %" PRIu64 " presents: %" PRIu64 " generated, %" PRIu64
              " skipped (%" PRIu64 " no free image, %" PRIu64 " no history, %" PRIu64
              " held back by the governor)",
              (void *)sc->handle, sc->present_count, sc->generated,
              sc->skipped_no_image + sc->skipped_no_history + sc->reduced_frames,
              sc->skipped_no_image, sc->skipped_no_history, sc->reduced_frames);
    gen_teardown(dev, sc);
    pthread_cond_destroy(&sc->drain_cond);
    pthread_cond_destroy(&sc->job_cond);
    pthread_mutex_destroy(&sc->job_lock);
    pthread_mutex_destroy(&sc->wsi_lock);
    pthread_mutex_destroy(&sc->wsi_turn);
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

void afmf_swapchain_drain_all(struct afmf_device *dev)
{
    pthread_mutex_lock(&dev->lock);
    for (struct afmf_swapchain *sc = dev->swapchains; sc != NULL; sc = sc->next)
        presenter_drain(sc); /* the thread never needs dev->lock to finish a job */
    pthread_mutex_unlock(&dev->lock);
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
        /* The target is written by a copy (TRANSFER_DST) or, with direct output, by the
         * interpolator's stores (GENERAL). */
        bool direct = afmf_framegen_direct(sc->fg);
        bool ingest = afmf_framegen_ingest_direct(sc->fg);
        const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkImageLayout written = direct ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags write = direct ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags writer = direct ? compute : transfer;
        /* The current image is sampled by the ingest pass (GENERAL) or read by a copy. */
        VkImageLayout read = ingest ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAccessFlags read_access = ingest ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
        VkPipelineStageFlags reader = ingest ? compute : transfer;
        image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read, 0, read_access,
                      top, reader);
        if (generate)
            image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_UNDEFINED, written, 0, write, top,
                          writer);
        afmf_framegen_record(dev, sc->fg, cmd, slot, sc->images[i], i,
                             generate ? sc->images[j] : VK_NULL_HANDLE, j);
        if (generate)
            image_barrier(dev, cmd, sc->images[j], written, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, write, 0,
                          writer, bottom);
        image_barrier(dev, cmd, sc->images[i], read, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read_access, 0,
                      reader, bottom);
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
            double dt = elapsed_ms(&sc->last_present, &now);
            sc->frame_time_ms_accum += dt;
            sc->frame_time_samples++;
            /* Smoothed for pacing and the governor: quick enough to follow a scene change,
             * steady enough not to jitter the hold with every frame. Ignores loading pauses,
             * and a single hitch moves it by at most a factor of two: a 100 ms frame in a
             * 6 ms cadence must not turn the next holds into 20 ms ones and trip the fps floor. */
            if (dt < 250.0) {
                if (sc->frame_ms_ema > 0.0 && dt > 2.0 * sc->frame_ms_ema)
                    dt = 2.0 * sc->frame_ms_ema;
                sc->frame_ms_ema = sc->frame_ms_ema == 0.0 ? dt : 0.9 * sc->frame_ms_ema + 0.1 * dt;
            }
        }
        sc->last_present = now;
    }
    sc->present_count++;
    if (sc->frame_time_samples == AFMF_STATS_INTERVAL) {
        double n = (double)sc->frame_time_samples;
        double frame_ms = sc->frame_time_ms_accum / n;
        /* Where the application's thread waits: with AFMF_PROFILE this is the number that says
         * whether the layer costs the game host time (the GPU work is off its queue). */
#define STATS_LINE                                                                                \
    "swapchain %p: %" PRIu64 " presents, %" PRIu64 " generated, %" PRIu64 " no free image; "    \
    "%.2f ms between presents (%.0f real fps); in the layer %.0f us per present: slot fence "   \
    "%.0f, acquire %.0f, record %.0f, submit %.0f; presentation thread: present generated "   \
    "%.0f, present real %.0f, refill %.0f, gpu done +%.2f ms, pacing hold %.2f ms; governor "    \
    "step %u"
#define STATS_ARGS                                                                                \
    (void *)sc->handle, sc->present_count, sc->generated, sc->skipped_no_image, frame_ms,       \
        1e3 / frame_ms, 1e3 * sc->hook_ms / n, 1e3 * sc->fence_ms / n, 1e3 * sc->acquire_ms / n, \
        1e3 * sc->record_ms / n, 1e3 * sc->submit_ms / n, 1e3 * sc->present_ms / n,             \
        1e3 * sc->present_real_ms / n, 1e3 * sc->refill_ms / n, sc->gpu_delay_ms / n,           \
        sc->hold_ms / n, sc->governor_step
        pthread_mutex_lock(&sc->job_lock); /* the presentation thread's counters */
        if (afmf_config_get()->profile)
            AFMF_INFO(STATS_LINE, STATS_ARGS);
        else
            AFMF_DEBUG(STATS_LINE, STATS_ARGS);
#undef STATS_LINE
#undef STATS_ARGS
        sc->present_ms = sc->present_real_ms = sc->refill_ms = sc->gpu_delay_ms = sc->hold_ms = 0.0;
        pthread_mutex_unlock(&sc->job_lock);
        sc->frame_time_ms_accum = 0.0;
        sc->frame_time_samples = 0;
        sc->hook_ms = sc->fence_ms = sc->acquire_ms = sc->record_ms = sc->submit_ms = 0.0;
    }
    pthread_mutex_unlock(&dev->lock);
}

/* Whether this present gets a companion, after the governor has looked at how late the last
 * generated frame was ready and at the real frame rate. Called with dev->lock held. */
static bool governor_allows(struct afmf_swapchain *sc, double last_delay_ms)
{
    const struct afmf_config *cfg = afmf_config_get();
    double frame_ms = sc->frame_ms_ema;
    if (cfg->min_fps > 0 && frame_ms > 0.0 && frame_ms > 1e3 / cfg->min_fps) {
        sc->reduced_frames++;
        return false;
    }
    if (!cfg->governor || frame_ms <= 0.0)
        return true;

    double ratio = last_delay_ms / frame_ms;
    uint32_t before = sc->governor_step;
    if (ratio > AFMF_GOVERNOR_LATE) {
        sc->early_streak = 0;
        if (++sc->late_streak >= AFMF_GOVERNOR_LATE_FRAMES && sc->governor_step < AFMF_GOVERNOR_STEPS) {
            sc->governor_step++;
            sc->late_streak = 0;
        }
    } else if (ratio < AFMF_GOVERNOR_EARLY) {
        sc->late_streak = 0;
        if (++sc->early_streak >= AFMF_GOVERNOR_EARLY_FRAMES && sc->governor_step > 0) {
            sc->governor_step--;
            sc->early_streak = 0;
        }
    }
    if (sc->governor_step != before) {
        if (sc->fg != NULL)
            afmf_framegen_set_levels(sc->fg, sc->governor_step >= 1 ? 5u
                                                                    : afmf_framegen_max_levels(sc->fg));
        AFMF_INFO("swapchain %p: governor step %u (generated frame ready %.0f%% into the frame): %s",
                  (void *)sc->handle, sc->governor_step, 100.0 * ratio,
                  sc->governor_step == 0   ? "every frame, full search"
                  : sc->governor_step == 1 ? "every frame, five search levels"
                  : sc->governor_step == 2 ? "one companion in two"
                                           : "one companion in three");
    }
    /* Step 2 and 3 thin the companions out; the reduced frames still enter the history. */
    uint64_t every = sc->governor_step >= 2 ? sc->governor_step : 1;
    if (sc->present_count % every != 0) {
        sc->reduced_frames++;
        return false;
    }
    return true;
}

/* Acquires the spare when there is none, waiting at most `timeout` (0 from the presentation
 * thread: it must not hold the swapchain lock). Caller holds the lock. */
static void spare_refill(struct afmf_device *dev, struct afmf_swapchain *sc, uint64_t timeout)
{
    if (sc->spare_valid)
        return;
    VkResult res = dev->fns.acquire_next_image(dev->handle, sc->handle, timeout, VK_NULL_HANDLE,
                                               sc->spare_fence, &sc->spare_image);
    sc->spare_valid = res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR;
}

/* Hands out the spare for this frame's companion once the presentation engine has released it;
 * false leaves it for the next frame (or means there was none to be had). Only the acquire and
 * the spare's state need the swapchain lock: the fence wait runs without it, so the presentation
 * thread keeps presenting meanwhile. The thread only refills once `spare_valid` drops, so the
 * fence is reset before that, and never raced. */
static bool spare_take(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t *image)
{
    /* AFMF_ACQUIRE_TIMEOUT_US bounds the whole wait for a released image: the acquire, in
     * slices so the presentation thread keeps its turn (its presents are what free images),
     * then the release fence. */
    uint64_t left = afmf_config_get()->acquire_timeout_ns;
    bool valid;
    uint32_t spare;
    for (;;) {
        uint64_t slice = left > AFMF_ACQUIRE_SLICE_NS ? AFMF_ACQUIRE_SLICE_NS : left;
        wsi_take(sc);
        spare_refill(dev, sc, slice);
        valid = sc->spare_valid;
        spare = sc->spare_image;
        wsi_give(sc);
        left -= slice;
        if (valid || left == 0)
            break;
    }
    if (!valid)
        return false;
    if (dev->fns.wait_for_fences(dev->handle, 1, &sc->spare_fence, VK_TRUE, left) != VK_SUCCESS)
        return false;
    (void)dev->fns.reset_fences(dev->handle, 1, &sc->spare_fence);
    wsi_take(sc);
    sc->spare_valid = false;
    wsi_give(sc);
    *image = spare;
    return true;
}

/* ---- presentation thread ------------------------------------------------------------------ */

static void timespec_add_ns(struct timespec *t, uint64_t ns)
{
    t->tv_nsec += (long)(ns % 1000000000ull);
    t->tv_sec += (time_t)(ns / 1000000000ull) + t->tv_nsec / 1000000000L;
    t->tv_nsec %= 1000000000L;
}

/* One present from the layer's queue, with the parts of the application's chain that were kept.
 * The present id only goes on the real frame: ids must increase, and the companion has none. */
static VkResult present_one(struct afmf_device *dev, struct afmf_swapchain *sc, uint32_t image,
                            VkSemaphore wait, const struct afmf_present_job *job, bool real)
{
    VkPresentIdKHR id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#ifdef VK_KHR_present_id2
    VkPresentId2KHR id2 = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
        .swapchainCount = 1,
        .pPresentIds = &job->present_id,
    };
#endif
    VkSwapchainPresentModeInfoEXT mode = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .swapchainCount = 1,
        .pPresentModes = &job->present_mode,
    };
    VkSwapchainPresentFenceInfoEXT fence = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
        .swapchainCount = 1,
        .pFences = &job->present_fence,
    };
#ifdef VK_EXT_present_timing
    VkPresentTimingsInfoEXT timings = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
        .swapchainCount = 1,
        .pTimingInfos = &job->timing,
    };
#endif
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &wait,
        .swapchainCount = 1,
        .pSwapchains = &sc->handle,
        .pImageIndices = &image,
    };
    const void **tail = &present.pNext;
    if (job->have_present_mode) {
        *tail = &mode;
        tail = &mode.pNext;
    }
    if (real && job->have_present_id) {
#ifdef VK_KHR_present_id2
        if (job->present_id_v2) {
            *tail = &id2;
            tail = &id2.pNext;
        } else
#endif
        {
            *tail = &id;
            tail = &id.pNext;
        }
    }
    if (real && job->present_fence != VK_NULL_HANDLE) {
        *tail = &fence;
        tail = &fence.pNext;
    }
#ifdef VK_EXT_present_timing
    if (real && job->have_timing)
        *tail = &timings;
#endif

    struct timespec t0, t1;
    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    wsi_take(sc);
    pthread_mutex_lock(&dev->async_lock);
    VkResult res = dev->fns.queue_present(dev->async_queue, &present);
    pthread_mutex_unlock(&dev->async_lock);
    wsi_give(sc);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Thread-side counters live under job_lock (never dev->lock: vkDeviceWaitIdle drains the
     * thread while holding dev->lock). */
    pthread_mutex_lock(&sc->job_lock);
    if (real)
        sc->present_real_ms += elapsed_ms(&t0, &t1);
    else
        sc->present_ms += elapsed_ms(&t0, &t1);
    pthread_mutex_unlock(&sc->job_lock);
    return res;
}

static void *presenter_main(void *arg)
{
    struct afmf_swapchain *sc = arg;
    struct afmf_device *dev = sc->dev;

    pthread_mutex_lock(&sc->job_lock);
    for (;;) {
        while (sc->job_count == 0 && !sc->presenter_stop)
            pthread_cond_wait(&sc->job_cond, &sc->job_lock);
        if (sc->job_count == 0)
            break; /* stopping, and drained */
        struct afmf_present_job job = sc->jobs[sc->job_head];
        bool draining = sc->presenter_stop;

        VkResult first = VK_SUCCESS;
        if (job.generate) {
            pthread_mutex_unlock(&sc->job_lock);
            first = present_one(dev, sc, job.companion_image, sc->sem_generated[job.companion_image],
                                &job, false);
            /* Hold the real frame back so the generated one gets its half of the interval. The
             * generated frame cannot show before its GPU work is done, and under contention that
             * is milliseconds after the present call: the half frame counts from there, capped at
             * one frame after arrival so a starved GPU does not pile latency on. A stop request
             * (teardown) cuts the wait short. */
            struct timespec until = job.arrival;
            if (job.hold_ns > 0 && !draining) {
                (void)dev->fns.wait_for_fences(dev->handle, 1, &job.done, VK_TRUE, 2 * job.hold_ns);
                struct timespec ready;
                (void)clock_gettime(CLOCK_MONOTONIC, &ready);
                double delay_ms = elapsed_ms(&job.arrival, &ready);
                if (delay_ms < 0.0)
                    delay_ms = 0.0;
                uint64_t delay_ns = (uint64_t)(delay_ms * 1e6);
                uint64_t hold = job.hold_ns + (delay_ns < job.hold_ns ? delay_ns : job.hold_ns);
                timespec_add_ns(&until, hold);
                pthread_mutex_lock(&sc->job_lock);
                sc->gpu_delay_ms += delay_ms;
                sc->last_delay_ms = delay_ms;
                sc->hold_ms += (double)hold / 1e6;
                /* The next real frame arriving ends the hold: past that point it only piles
                 * latency and images up (after a hitch the frame time EMA overstates the
                 * frame for a while). The hook broadcasts job_cond when it queues. */
                while (!sc->presenter_stop && sc->job_count < 2 &&
                       pthread_cond_timedwait(&sc->job_cond, &sc->job_lock, &until) != ETIMEDOUT)
                    ;
            } else {
                pthread_mutex_lock(&sc->job_lock);
            }
        }
        pthread_mutex_unlock(&sc->job_lock);
        VkResult second = present_one(dev, sc, job.real_image, sc->sem_real[job.real_image], &job,
                                      true);

        /* Line up the next companion's image while the application renders. */
        struct timespec r0, r1;
        (void)clock_gettime(CLOCK_MONOTONIC, &r0);
        wsi_take(sc);
        spare_refill(dev, sc, 0);
        wsi_give(sc);
        (void)clock_gettime(CLOCK_MONOTONIC, &r1);

        pthread_mutex_lock(&sc->job_lock);
        sc->refill_ms += elapsed_ms(&r0, &r1);
        /* The worst outcome reaches the application with its next present call. Errors beat
         * SUBOPTIMAL, SUBOPTIMAL beats SUCCESS. */
        VkResult worst = first < 0 ? first : second < 0 ? second : first != VK_SUCCESS ? first : second;
        if (sc->deferred_result >= 0 && (worst < 0 || sc->deferred_result == VK_SUCCESS))
            sc->deferred_result = worst;
        sc->job_head = (sc->job_head + 1) % AFMF_MAX_JOBS;
        sc->job_count--;
        /* Both waiters re-check their own condition: the drain wants zero, a full queue wants
         * one free slot. */
        pthread_cond_broadcast(&sc->drain_cond);
    }
    pthread_mutex_unlock(&sc->job_lock);
    return NULL;
}

/* Starts the thread on first use; false leaves the presents on the application's thread. */
static bool presenter_start(struct afmf_swapchain *sc)
{
    if (sc->presenter_running)
        return true;
    if (!sc->async || sc->presenter_stop)
        return false;
    if (pthread_create(&sc->presenter, NULL, presenter_main, sc) != 0) {
        AFMF_WARN("swapchain %p: no presentation thread; presenting inline", (void *)sc->handle);
        sc->presenter_stop = true; /* do not retry every frame */
        return false;
    }
    sc->presenter_running = true;
    return true;
}

/* Waits until every queued present went out: needed before presenting inline behind them. */
static void presenter_drain(struct afmf_swapchain *sc)
{
    if (!sc->presenter_running)
        return;
    pthread_mutex_lock(&sc->job_lock);
    while (sc->job_count > 0)
        pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
    pthread_mutex_unlock(&sc->job_lock);
}

/* Drains and joins; safe to call more than once and without a running thread. */
static void presenter_stop(struct afmf_swapchain *sc)
{
    if (!sc->presenter_running)
        return;
    pthread_mutex_lock(&sc->job_lock);
    sc->presenter_stop = true;
    pthread_cond_broadcast(&sc->job_cond);
    pthread_mutex_unlock(&sc->job_lock);
    (void)pthread_join(sc->presenter, NULL);
    sc->presenter_running = false;
}

/* Copies what the application's chain carries that can outlive the call. False means something
 * the layer cannot carry (a present fence, display timing, regions of a kind it does not know):
 * the present then happens inline with the original chain. */
static bool job_from_chain(const struct afmf_swapchain *sc, const VkPresentInfoKHR *info,
                           struct afmf_present_job *job)
{
    for (const VkBaseInStructure *s = info->pNext; s != NULL; s = s->pNext) {
        switch ((int)s->sType) {
        case VK_STRUCTURE_TYPE_PRESENT_ID_KHR: {
            const VkPresentIdKHR *id = (const VkPresentIdKHR *)s;
            if (id->pPresentIds != NULL && id->pPresentIds[0] != 0) {
                job->have_present_id = true;
                job->present_id = id->pPresentIds[0];
            }
            break;
        }
#ifdef VK_KHR_present_id2
        case VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR: {
            /* DXVK 2.7+ under a Mesa that offers present_id2; without carrying it every present
             * went inline, without pacing. */
            const VkPresentId2KHR *id = (const VkPresentId2KHR *)s;
            if (id->pPresentIds != NULL && id->pPresentIds[0] != 0) {
                job->have_present_id = true;
                job->present_id_v2 = true;
                job->present_id = id->pPresentIds[0];
            }
            break;
        }
#endif
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT: {
            const VkSwapchainPresentModeInfoEXT *m = (const VkSwapchainPresentModeInfoEXT *)s;
            job->have_present_mode = true;
            job->present_mode = m->pPresentModes[0];
            /* The swapchain was moved to MAILBOX at creation: a switch back to FIFO would bring
             * the halved rate back, and a mode outside the cut-down list is invalid. */
            if (sc->fifo_to_mailbox) {
                bool allowed = false;
                for (uint32_t k = 0; k < sc->allowed_mode_count && !allowed; k++)
                    allowed = sc->allowed_modes[k] == job->present_mode;
                if (!allowed || job->present_mode == VK_PRESENT_MODE_FIFO_KHR ||
                    job->present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
                    job->present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
            }
            break;
        }
        case VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT: {
            /* The application waits on it to reuse the present's resources: it goes with the
             * real frame, so the wait covers the layer's use of the image too. */
            const VkSwapchainPresentFenceInfoEXT *f = (const VkSwapchainPresentFenceInfoEXT *)s;
            if (f->pFences != NULL)
                job->present_fence = f->pFences[0];
            break;
        }
#ifdef VK_EXT_present_timing
        case VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT: {
            /* The application's timing request (target time, stages to query) belongs to its
             * frame: it goes on the real present, whose past-presentation timing it will read
             * back. The generated frame carries none. */
            const VkPresentTimingsInfoEXT *t = (const VkPresentTimingsInfoEXT *)s;
            if (t->pTimingInfos != NULL && t->swapchainCount >= 1) {
                job->timing = t->pTimingInfos[0];
                job->timing.pNext = NULL;
                job->have_timing = true;
            }
            break;
        }
#endif
        case VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR:
            break; /* a hint about what changed; the generated frame changes everything anyway */
        default: {
            static bool reported;
            if (!reported) {
                reported = true;
                AFMF_WARN("present chain carries sType %d, which the presentation thread cannot "
                          "carry: presenting inline, without pacing",
                          (int)s->sType);
            }
            return false;
        }
        }
    }
    return true;
}

/* The application's vkAcquireNextImage(2)KHR: under the swapchain lock in slices, so the
 * presentation thread never waits more than one slice and a long acquire never holds it off.
 * Everything but the timeout is the application's. */
VkResult afmf_swapchain_acquire(struct afmf_device *dev, const VkAcquireNextImageInfoKHR *info,
                                bool v2, uint32_t *index)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = find_locked(dev, info->swapchain);
    pthread_mutex_unlock(&dev->lock);

    VkAcquireNextImageInfoKHR sliced = *info;
    uint64_t left = info->timeout;
    for (;;) {
        bool ours = sc != NULL && sc->gen_enabled;
        sliced.timeout = ours && left > AFMF_ACQUIRE_SLICE_NS ? AFMF_ACQUIRE_SLICE_NS : left;
        if (ours)
            wsi_take(sc);
        VkResult res = v2 ? dev->fns.acquire_next_image2(dev->handle, &sliced, index)
                          : dev->fns.acquire_next_image(dev->handle, sliced.swapchain, sliced.timeout,
                                                        sliced.semaphore, sliced.fence, index);
        if (ours)
            wsi_give(sc);
        if (!ours || (res != VK_TIMEOUT && res != VK_NOT_READY))
            return res;
        if (left != UINT64_MAX)
            left -= sliced.timeout;
        if (left == 0)
            return res;
    }
}

VkResult afmf_swapchain_get_images(struct afmf_device *dev, VkSwapchainKHR swapchain,
                                   uint32_t *count, VkImage *images)
{
    pthread_mutex_lock(&dev->lock);
    struct afmf_swapchain *sc = find_locked(dev, swapchain);
    pthread_mutex_unlock(&dev->lock);

    bool ours = sc != NULL && sc->gen_enabled;
    if (ours)
        wsi_take(sc);
    VkResult res = dev->fns.get_swapchain_images(dev->handle, swapchain, count, images);
    if (ours)
        wsi_give(sc);
    return res;
}

/* The application's present, with the generated frame in front of it when one could be made. */
static VkResult present_generated(struct afmf_device *dev, struct afmf_swapchain *sc, VkQueue queue,
                                  uint32_t family, const VkPresentInfoKHR *info)
{
    const struct afmf_device_fns *f = &dev->fns;
    uint32_t i = info->pImageIndices[0];

    /* With a queue of its own the layer records for that family and presents from it; the
     * application's queue is never waited on. */
    VkQueue work_queue = sc->async ? dev->async_queue : queue;
    if (sc->async)
        family = dev->async_family;

    if (!ensure_pool(dev, sc, family) || i >= sc->image_count ||
        info->waitSemaphoreCount > AFMF_MAX_APP_WAITS) {
        gen_disable(sc, "cannot generate on this present path");
        return afmf_device_queue_present(dev, queue, info);
    }

    struct afmf_present_job job = {.real_image = i};
    bool threaded = presenter_start(sc) && job_from_chain(sc, info, &job);
    if (!threaded)
        presenter_drain(sc); /* a chain the thread cannot carry: inline, but in order */

    struct timespec t_start, t_fence, t_acquire, t_record, t_submit, t_present, t_companion, t_real,
        t_end;
    (void)clock_gettime(CLOCK_MONOTONIC, &t_start);
    job.arrival = t_start;

    struct afmf_slot *slot = &sc->slots[sc->slot_index];
    if (threaded) {
        /* The presentation thread waits on this slot's fence for its pacing; the reset below
         * must not race that wait, so the job that last used the slot has to be done. */
        pthread_mutex_lock(&sc->job_lock);
        while (sc->job_count >= sc->image_count && !sc->presenter_stop)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        pthread_mutex_unlock(&sc->job_lock);
    }
    if (slot->pending) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        (void)f->reset_fences(dev->handle, 1, &slot->fence);
        slot->pending = false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_fence);

    /* The image the generated frame goes into: the spare, if the presentation engine has one
     * for us; otherwise this frame simply gets no companion, the application never waits. */
    bool generate = false;
    uint32_t j = 0;
    pthread_mutex_lock(&sc->job_lock);
    double last_delay_ms = sc->last_delay_ms;
    pthread_mutex_unlock(&sc->job_lock);
    pthread_mutex_lock(&dev->lock);
    bool allowed = governor_allows(sc, last_delay_ms);
    pthread_mutex_unlock(&dev->lock);
    if (!sc->have_history) {
        sc->skipped_no_history++;
    } else if (allowed) {
        generate = spare_take(dev, sc, &j);
        if (!generate)
            sc->skipped_no_image++;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_acquire);

    VkResult res = record_frame(dev, sc, slot->cmd, sc->slot_index, i, generate, j);
    (void)clock_gettime(CLOCK_MONOTONIC, &t_record);
    if (res == VK_SUCCESS) {
        VkSemaphore waits[AFMF_MAX_APP_WAITS];
        VkPipelineStageFlags stages[AFMF_MAX_APP_WAITS];
        uint32_t wait_count = 0;
        /* Only the application's semaphores: the spare's release was waited on the host. */
        for (uint32_t k = 0; k < info->waitSemaphoreCount; k++) {
            waits[wait_count] = info->pWaitSemaphores[k];
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
        /* Inline presents follow under the same locks, in the thread's order: swapchain, then
         * queue. */
        if (!threaded)
            wsi_take(sc);
        if (sc->async)
            pthread_mutex_lock(&dev->async_lock);
        res = f->queue_submit(work_queue, 1, &submit, slot->fence);
        if (sc->async && (res != VK_SUCCESS || threaded))
            pthread_mutex_unlock(&dev->async_lock);
        if (!threaded && res != VK_SUCCESS)
            wsi_give(sc);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_submit);
    sc->record_ms += elapsed_ms(&t_acquire, &t_record);
    sc->submit_ms += elapsed_ms(&t_record, &t_submit);
    if (res != VK_SUCCESS) {
        /* The application's semaphores were not consumed, so its own present still works. The
         * image acquired for the generated frame, if any, stays with the presentation engine. */
        gen_disable(sc, "layer submission failed");
        return afmf_device_queue_present(dev, queue, info);
    }
    slot->pending = true;
    sc->slot_index = (sc->slot_index + 1) % sc->image_count;
    sc->have_history = true;
    if (generate)
        sc->generated++;

    /* Debug dumps block on the submission; only while AFMF_DUMP_DIR asks for frames. */
    if (generate && sc->fg != NULL && afmf_framegen_dump_recorded(sc->fg)) {
        (void)f->wait_for_fences(dev->handle, 1, &slot->fence, VK_TRUE, UINT64_MAX);
        afmf_framegen_dump_write(dev, sc->fg);
        (void)clock_gettime(CLOCK_MONOTONIC, &job.arrival); /* the pacing and the governor start after the stall */
    }

    if (threaded) {
        /* Hand both presents to the presentation thread and return. The results of earlier
         * presents come back here; the application also learns OUT_OF_DATE from its acquire. */
        job.generate = generate;
        job.companion_image = j;
        if (generate && afmf_config_get()->pacing) {
            pthread_mutex_lock(&dev->lock);
            double half_ms = sc->frame_ms_ema / 2.0;
            pthread_mutex_unlock(&dev->lock);
            uint64_t hold = (uint64_t)(half_ms * 1e6);
            job.hold_ns = hold < AFMF_PACING_MIN_NS   ? 0
                          : hold > AFMF_PACING_MAX_NS ? AFMF_PACING_MAX_NS
                                                      : hold;
            job.done = slot->fence;
        }
        pthread_mutex_lock(&sc->job_lock);
        while (sc->job_count == AFMF_MAX_JOBS)
            pthread_cond_wait(&sc->drain_cond, &sc->job_lock);
        sc->jobs[(sc->job_head + sc->job_count) % AFMF_MAX_JOBS] = job;
        sc->job_count++;
        pthread_cond_broadcast(&sc->job_cond);
        res = sc->deferred_result;
        sc->deferred_result = VK_SUCCESS;
        pthread_mutex_unlock(&sc->job_lock);
        if (info->pResults != NULL)
            info->pResults[0] = res;

        (void)clock_gettime(CLOCK_MONOTONIC, &t_end);
        sc->hook_ms += elapsed_ms(&t_start, &t_end);
        sc->fence_ms += elapsed_ms(&t_start, &t_fence);
        sc->acquire_ms += elapsed_ms(&t_fence, &t_acquire);
        return res;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &t_present);
    if (generate) {
        VkPresentInfoKHR companion = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &sc->sem_generated[j],
            .swapchainCount = 1,
            .pSwapchains = &sc->handle,
            .pImageIndices = &j,
        };
        VkResult presented = f->queue_present(work_queue, &companion);
        if (presented < 0)
            AFMF_DEBUG("swapchain %p: generated present returned %d", (void *)sc->handle,
                       (int)presented);
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &t_companion);

    /* The real frame keeps the application's pNext chain and pResults; only the wait moves to the
     * semaphore the layer signals once it has finished reading the image. */
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &sc->sem_real[i];
    res = f->queue_present(work_queue, &real);
    if (sc->async)
        pthread_mutex_unlock(&dev->async_lock);
    (void)clock_gettime(CLOCK_MONOTONIC, &t_real);

    /* Try to line up the next companion's image now: by the next present, a frame later, the
     * presentation engine has had time to release one. */
    spare_refill(dev, sc, 0);
    wsi_give(sc);

    (void)clock_gettime(CLOCK_MONOTONIC, &t_end);
    sc->hook_ms += elapsed_ms(&t_start, &t_end);
    sc->fence_ms += elapsed_ms(&t_start, &t_fence);
    sc->acquire_ms += elapsed_ms(&t_fence, &t_acquire);
    sc->present_ms += elapsed_ms(&t_present, &t_companion);
    sc->present_real_ms += elapsed_ms(&t_companion, &t_real);
    sc->refill_ms += elapsed_ms(&t_real, &t_end);
    return res;
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
        return afmf_device_queue_present(dev, queue, info);
    }

    update_cadence(dev, sc);
    uint32_t family;
    if (!sc->gen_enabled || !afmf_device_queue_family(dev, queue, &family))
        return afmf_device_queue_present(dev, queue, info);
    return present_generated(dev, sc, queue, family, info);
}
