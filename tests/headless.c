/* Headless integration test for VK_LAYER_AFMF.
 *
 * Enables the layer by name (so a missing or broken layer fails vkCreateInstance instead of
 * silently running without it), puts VK_LAYER_KHRONOS_validation below it when available so the
 * calls the layer forwards are validated, creates a swapchain on a VK_EXT_headless_surface and
 * presents FRAMES frames. No window, no environment tricks: built with -DAFMF_SANITIZE=ON it is an
 * ordinary instrumented executable. Exit codes: 0 pass, 1 fail, 77 skipped (no headless surface). */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define LAYER_NAME "VK_LAYER_AFMF"
#define VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"
#define FRAMES 120u
#define EXIT_SKIP 77

/* Synthetic content: a white square sliding right on black, SQUARE_STEP pixels per frame, so an
 * interpolated frame must show it halfway between two real ones. */
#define SQUARE_SIZE 64u
#define SQUARE_X0 64u
#define SQUARE_Y 200u
#define SQUARE_STEP 8u
#define SQUARE_WRAP 448u

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        VkResult check_result_ = (expr);                                                    \
        if (check_result_ != VK_SUCCESS) {                                                  \
            (void)fprintf(stderr, "%s:%d: %s -> VkResult %d\n", __FILE__, __LINE__, #expr,   \
                          (int)check_result_);                                              \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

struct ctx {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore acquired;
    VkSemaphore ready;
    VkExtent2D extent;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    uint8_t *staging_mapped;
    uint32_t validation_errors;
    int present_id; /* AFMF_TEST_PRESENT_ID: 0 none, 1 VK_KHR_present_id, 2 VK_KHR_present_id2 */
};

static int wanted_present_id(void)
{
    const char *v = getenv("AFMF_TEST_PRESENT_ID");
    return v == NULL ? 0 : v[0] == '2' ? 2 : v[0] == '1' ? 1 : 0;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL on_debug_message(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
    /* Only the validation layer's verdicts count. GENERAL messages are the loader talking, e.g.
     * about some other installed implicit layer it had to skip, which is not this layer's bug. */
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) &&
        (type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)) {
        struct ctx *ctx = user;
        ctx->validation_errors++;
        (void)fprintf(stderr, "validation: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static bool instance_layer_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkLayerProperties *props = calloc(count, sizeof *props);
    if (props == NULL)
        return false;
    bool found = false;
    if (vkEnumerateInstanceLayerProperties(&count, props) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(props[i].layerName, name) == 0;
    free(props);
    return found;
}

static bool instance_extension_available(const char *name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return false;
    VkExtensionProperties *props = calloc(count, sizeof *props);
    if (props == NULL)
        return false;
    bool found = false;
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, props) == VK_SUCCESS)
        for (uint32_t i = 0; i < count && !found; i++)
            found = strcmp(props[i].extensionName, name) == 0;
    free(props);
    return found;
}

static bool create_instance(struct ctx *ctx, bool with_validation)
{
    /* Index 0 is closest to the application: the layer under test first, validation below it. */
    const char *layers[2] = {LAYER_NAME, VALIDATION_LAYER_NAME};
    const char *extensions[4] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    uint32_t extension_count = 2;
    if (with_validation)
        extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    if (wanted_present_id() == 2) /* VK_KHR_present_id2 depends on it */
        extensions[extension_count++] = VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME;
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "afmf_headless",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledLayerCount = with_validation ? 2u : 1u,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    CHECK(vkCreateInstance(&info, NULL, &ctx->instance));

    if (with_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(ctx->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create == NULL)
            return false;
        VkDebugUtilsMessengerCreateInfoEXT messenger = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = on_debug_message,
            .pUserData = ctx,
        };
        CHECK(create(ctx->instance, &messenger, NULL, &ctx->messenger));
    }

    PFN_vkCreateHeadlessSurfaceEXT create_surface = (PFN_vkCreateHeadlessSurfaceEXT)
        vkGetInstanceProcAddr(ctx->instance, "vkCreateHeadlessSurfaceEXT");
    if (create_surface == NULL)
        return false;
    VkHeadlessSurfaceCreateInfoEXT surface = {
        .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,
    };
    CHECK(create_surface(ctx->instance, &surface, NULL, &ctx->surface));
    return true;
}

/* Picks the first physical device with a graphics queue family that can present to the surface. */
static bool pick_physical_device(struct ctx *ctx)
{
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(ctx->instance, &count, NULL));
    if (count == 0)
        return false;
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (devices == NULL)
        return false;
    bool found = false;
    if (vkEnumeratePhysicalDevices(ctx->instance, &count, devices) == VK_SUCCESS) {
        for (uint32_t d = 0; d < count && !found; d++) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, NULL);
            VkQueueFamilyProperties *props = calloc(families, sizeof *props);
            if (props == NULL)
                break;
            vkGetPhysicalDeviceQueueFamilyProperties(devices[d], &families, props);
            for (uint32_t f = 0; f < families && !found; f++) {
                VkBool32 present = VK_FALSE;
                if ((props[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    vkGetPhysicalDeviceSurfaceSupportKHR(devices[d], f, ctx->surface, &present) ==
                        VK_SUCCESS &&
                    present) {
                    ctx->physical_device = devices[d];
                    ctx->queue_family = f;
                    found = true;
                }
            }
            free(props);
        }
    }
    free(devices);
    if (found) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(ctx->physical_device, &props);
        (void)fprintf(stderr, "using %s\n", props.deviceName);
    }
    return found;
}

#define MAX_FAMILIES 16u
#define MAX_QUEUES 16u

static bool create_device_and_swapchain(struct ctx *ctx)
{
    static const float priorities[MAX_QUEUES] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                                 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    const char *extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, NULL};
    uint32_t extension_count = 1;
    /* AFMF_TEST_PRESENT_ID=1|2: present with VK_KHR_present_id / VK_KHR_present_id2 ids, which
     * the layer must carry on the real frame instead of falling back to inline presents. Skipped
     * when the driver or the headless surface does not offer the extension. */
    ctx->present_id = wanted_present_id();
    VkPhysicalDevicePresentIdFeaturesKHR id_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, .presentId = VK_TRUE};
#ifdef VK_KHR_present_id2
    VkPhysicalDevicePresentId2FeaturesKHR id2_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, .presentId2 = VK_TRUE};
#endif
    const void *device_next = NULL;
    if (ctx->present_id != 0) {
        const char *ext = ctx->present_id == 2 ? "VK_KHR_present_id2" : VK_KHR_PRESENT_ID_EXTENSION_NAME;
        uint32_t n = 0;
        bool found = false;
        if (vkEnumerateDeviceExtensionProperties(ctx->physical_device, NULL, &n, NULL) == VK_SUCCESS && n > 0) {
            VkExtensionProperties *props = calloc(n, sizeof *props);
            if (props != NULL && vkEnumerateDeviceExtensionProperties(ctx->physical_device, NULL, &n, props) == VK_SUCCESS)
                for (uint32_t k = 0; k < n && !found; k++)
                    found = strcmp(props[k].extensionName, ext) == 0;
            free(props);
        }
#ifndef VK_KHR_present_id2
        if (ctx->present_id == 2)
            found = false; /* headers too old to build the structures */
#endif
#ifdef VK_KHR_present_id2
        if (found && ctx->present_id == 2) { /* also a per-surface capability */
            VkSurfaceCapabilitiesPresentId2KHR id2_caps = {
                .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR};
            VkSurfaceCapabilities2KHR caps2 = {.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
                                               .pNext = &id2_caps};
            VkPhysicalDeviceSurfaceInfo2KHR surface_info = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = ctx->surface};
            PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_caps2 =
                (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)vkGetInstanceProcAddr(
                    ctx->instance, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
            found = get_caps2 != NULL &&
                    get_caps2(ctx->physical_device, &surface_info, &caps2) == VK_SUCCESS &&
                    id2_caps.presentId2Supported == VK_TRUE;
        }
#endif
        if (!found) {
            (void)fprintf(stderr, "skipped: %s unavailable on this surface\n", ext);
            exit(EXIT_SKIP);
        }
        extensions[extension_count++] = ext;
#ifdef VK_KHR_present_id2
        device_next = ctx->present_id == 2 ? (const void *)&id2_features : (const void *)&id_features;
#else
        device_next = &id_features;
#endif
    }
    VkDeviceQueueCreateInfo queues[MAX_FAMILIES] = {{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
        .queueCount = 1,
        .pQueuePriorities = priorities,
    }};
    uint32_t queue_info_count = 1;
    /* AFMF_TEST_ALL_QUEUES=1: take every queue of every compute-capable family, as vkd3d-proton
     * does, so the layer has to share one of ours instead of getting its own. */
    const char *all = getenv("AFMF_TEST_ALL_QUEUES");
    if (all != NULL && strcmp(all, "1") == 0) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &families, NULL);
        VkQueueFamilyProperties props[MAX_FAMILIES];
        if (families > MAX_FAMILIES)
            families = MAX_FAMILIES;
        vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device, &families, props);
        for (uint32_t f = 0; f < families; f++) {
            if (!(props[f].queueFlags & VK_QUEUE_COMPUTE_BIT))
                continue;
            uint32_t n = props[f].queueCount < MAX_QUEUES ? props[f].queueCount : MAX_QUEUES;
            if (f == ctx->queue_family) {
                queues[0].queueCount = n;
                continue;
            }
            queues[queue_info_count++] = (VkDeviceQueueCreateInfo){
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = f,
                .queueCount = n,
                .pQueuePriorities = priorities,
            };
        }
    }
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = device_next,
        .queueCreateInfoCount = queue_info_count,
        .pQueueCreateInfos = queues,
        .enabledExtensionCount = extension_count,
        .ppEnabledExtensionNames = extensions,
    };
    CHECK(vkCreateDevice(ctx->physical_device, &device, NULL, &ctx->device));
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physical_device, ctx->surface, &caps));
    uint32_t format_count = 1;
    VkSurfaceFormatKHR format;
    VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physical_device, ctx->surface,
                                                        &format_count, &format);
    if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || format_count == 0)
        return false;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { /* the surface lets the swapchain choose */
        extent.width = 640;
        extent.height = 480;
        /* AFMF_TEST_EXTENT=WxH to time the layer at a game-like resolution. */
        const char *wanted = getenv("AFMF_TEST_EXTENT");
        unsigned w = 0, h = 0;
        if (wanted != NULL && sscanf(wanted, "%ux%u", &w, &h) == 2 && w >= 128 && h >= 128 &&
            w <= 8192 && h <= 8192) {
            extent.width = w;
            extent.height = h;
        }
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR swapchain = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = ctx->surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(vkCreateSwapchainKHR(ctx->device, &swapchain, NULL, &ctx->swapchain));

    /* AFMF_TEST_EXTRA_IMAGES=<n>: the layer must have added at least n images to what was asked
     * (the driver may round up on its own, so only the lower bound is checked). */
    uint32_t got = 0;
    CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &got, NULL));
    const char *extra = getenv("AFMF_TEST_EXTRA_IMAGES");
    long expected = extra != NULL ? strtol(extra, NULL, 10) : 0;
    (void)fprintf(stderr, "swapchain has %u images (asked %u)\n", got, image_count);
    if (expected > 0 && got < image_count + (uint32_t)expected) {
        (void)fprintf(stderr, "expected at least %u images\n", image_count + (uint32_t)expected);
        return false;
    }

    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = ctx->queue_family,
    };
    CHECK(vkCreateCommandPool(ctx->device, &pool, NULL, &ctx->command_pool));
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = ctx->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    CHECK(vkAllocateCommandBuffers(ctx->device, &alloc, &ctx->command_buffer));

    VkSemaphoreCreateInfo semaphore = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    CHECK(vkCreateSemaphore(ctx->device, &semaphore, NULL, &ctx->acquired));
    CHECK(vkCreateSemaphore(ctx->device, &semaphore, NULL, &ctx->ready));

    /* Host-visible staging for the synthetic frames (RGBA8 or BGRA8: 4 bytes per pixel). */
    ctx->extent = extent;
    VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4u;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(ctx->device, &buffer, NULL, &ctx->staging));
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(ctx->device, ctx->staging, &reqs);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory);
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((reqs.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            type = i;
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo alloc_memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    CHECK(vkAllocateMemory(ctx->device, &alloc_memory, NULL, &ctx->staging_memory));
    CHECK(vkBindBufferMemory(ctx->device, ctx->staging, ctx->staging_memory, 0));
    void *mapped = NULL;
    CHECK(vkMapMemory(ctx->device, ctx->staging_memory, 0, size, 0, &mapped));
    ctx->staging_mapped = mapped;
    return true;
}

static uint32_t square_x(uint32_t frame)
{
    return SQUARE_X0 + (frame * SQUARE_STEP) % SQUARE_WRAP;
}

static void paint_frame(struct ctx *ctx, uint32_t frame)
{
    uint32_t w = ctx->extent.width, h = ctx->extent.height;
    memset(ctx->staging_mapped, 0, (size_t)w * h * 4u);
    uint32_t x0 = square_x(frame);
    for (uint32_t y = SQUARE_Y; y < SQUARE_Y + SQUARE_SIZE && y < h; y++)
        for (uint32_t x = x0; x < x0 + SQUARE_SIZE && x < w; x++)
            memset(ctx->staging_mapped + ((size_t)y * w + x) * 4u, 0xff, 4);
}

/* Acquire, upload the synthetic frame, transition the image to PRESENT_SRC, present, then drain
 * the queue so the same two semaphores can be reused next frame without any tracking.
 * Correctness, not throughput. */
static bool present_frame(struct ctx *ctx, uint32_t frame)
{
    paint_frame(ctx, frame);
    uint32_t index = 0;
    CHECK(vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX, ctx->acquired,
                                VK_NULL_HANDLE, &index));

    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count, NULL));
    VkImage *images = calloc(image_count, sizeof *images);
    if (images == NULL)
        return false;
    VkResult res = vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count, images);
    VkImage image = res == VK_SUCCESS && index < image_count ? images[index] : VK_NULL_HANDLE;
    free(images);
    if (image == VK_NULL_HANDLE)
        return false;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(ctx->command_buffer, &begin));
    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {ctx->extent.width, ctx->extent.height, 1},
    };
    vkCmdCopyBufferToImage(ctx->command_buffer, ctx->staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_present = to_transfer;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    CHECK(vkEndCommandBuffer(ctx->command_buffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &ctx->ready,
    };
    CHECK(vkQueueSubmit(ctx->queue, 1, &submit, VK_NULL_HANDLE));

    uint64_t present_id = frame + 1;
    VkPresentIdKHR id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &present_id};
#ifdef VK_KHR_present_id2
    VkPresentId2KHR id2 = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .swapchainCount = 1, .pPresentIds = &present_id};
#endif
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->ready,
        .swapchainCount = 1,
        .pSwapchains = &ctx->swapchain,
        .pImageIndices = &index,
    };
    if (ctx->present_id == 1)
        present.pNext = &id;
#ifdef VK_KHR_present_id2
    if (ctx->present_id == 2)
        present.pNext = &id2;
#endif
    res = vkQueuePresentKHR(ctx->queue, &present);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        (void)fprintf(stderr, "vkQueuePresentKHR -> VkResult %d\n", (int)res);
        return false;
    }
    CHECK(vkQueueWaitIdle(ctx->queue));

    /* AFMF_TEST_FRAME_MS=<n>: pace the presents like a game at that frame time, so the layer's
     * pacing hold (half of it) shows in its profile line. */
    static long frame_ms = -1;
    if (frame_ms < 0) {
        const char *wanted = getenv("AFMF_TEST_FRAME_MS");
        frame_ms = wanted != NULL ? strtol(wanted, NULL, 10) : 0;
        if (frame_ms < 0 || frame_ms > 1000)
            frame_ms = 0;
    }
    if (frame_ms > 0) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = frame_ms * 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    return true;
}

static void destroy(struct ctx *ctx)
{
    if (ctx->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(ctx->device);
        if (ctx->staging_mapped != NULL)
            vkUnmapMemory(ctx->device, ctx->staging_memory);
        if (ctx->staging != VK_NULL_HANDLE)
            vkDestroyBuffer(ctx->device, ctx->staging, NULL);
        if (ctx->staging_memory != VK_NULL_HANDLE)
            vkFreeMemory(ctx->device, ctx->staging_memory, NULL);
        if (ctx->ready != VK_NULL_HANDLE)
            vkDestroySemaphore(ctx->device, ctx->ready, NULL);
        if (ctx->acquired != VK_NULL_HANDLE)
            vkDestroySemaphore(ctx->device, ctx->acquired, NULL);
        if (ctx->command_pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
        if (ctx->swapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(ctx->device, ctx->swapchain, NULL);
        vkDestroyDevice(ctx->device, NULL);
    }
    if (ctx->instance != VK_NULL_HANDLE) {
        if (ctx->surface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(ctx->instance, ctx->surface, NULL);
        if (ctx->messenger != VK_NULL_HANDLE) {
            PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger =
                (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                    ctx->instance, "vkDestroyDebugUtilsMessengerEXT");
            if (destroy_messenger != NULL)
                destroy_messenger(ctx->instance, ctx->messenger, NULL);
        }
        vkDestroyInstance(ctx->instance, NULL);
    }
}

static bool run(struct ctx *ctx)
{
    bool with_validation = instance_layer_available(VALIDATION_LAYER_NAME);
    if (!with_validation)
        (void)fprintf(stderr, "note: " VALIDATION_LAYER_NAME " not installed, running without it\n");

    if (!create_instance(ctx, with_validation) || !pick_physical_device(ctx) ||
        !create_device_and_swapchain(ctx))
        return false;
    /* AFMF_TEST_FRAMES=<n> for longer runs (the layer's profile line comes every 300). */
    uint32_t frames = FRAMES;
    const char *wanted = getenv("AFMF_TEST_FRAMES");
    if (wanted != NULL) {
        long n = strtol(wanted, NULL, 10);
        if (n >= 2 && n <= 100000)
            frames = (uint32_t)n;
    }
    struct timespec start, end;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint32_t i = 0; i < frames; i++)
        if (!present_frame(ctx, i))
            return false;
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = ((double)(end.tv_sec - start.tv_sec) * 1e3 + (double)(end.tv_nsec - start.tv_nsec) / 1e6);
    (void)fprintf(stderr, "%ux%u: %.2f ms per present (upload + layer work, queue drained each frame)\n",
                  ctx->extent.width, ctx->extent.height, ms / frames);
    return true;
}

/* Reads the layer's dump of generated frame `n` (between real frames n-1 and n) and checks that
 * the square's centroid sits halfway between where those two frames drew it. */
static bool check_dump(const char *dir, uint32_t n)
{
    char path[512];
    if (snprintf(path, sizeof path, "%s/afmf_generated_%u.ppm", dir, n) >= (int)sizeof path)
        return false;
    FILE *in = fopen(path, "rb");
    if (in == NULL) {
        (void)fprintf(stderr, "no dump at %s: %s\n", path, strerror(errno));
        return false;
    }
    unsigned w = 0, h = 0, maxval = 0;
    bool ok = fscanf(in, "P6 %u %u %u", &w, &h, &maxval) == 3 && fgetc(in) == '\n' && w > 0 &&
              h > 0 && w <= 8192 && h <= 8192;
    double sum_x = 0, sum_y = 0;
    unsigned long bright = 0;
    for (unsigned y = 0; ok && y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            int r = fgetc(in), g = fgetc(in), b = fgetc(in);
            if (r == EOF || g == EOF || b == EOF) {
                ok = false;
                break;
            }
            if (r > 128 && g > 128 && b > 128) {
                sum_x += x;
                sum_y += y;
                bright++;
            }
        }
    }
    (void)fclose(in);
    if (!ok || bright == 0) {
        (void)fprintf(stderr, "%s: unreadable or no bright pixels\n", path);
        return false;
    }
    double cx = sum_x / (double)bright, cy = sum_y / (double)bright;
    double expected_x = ((double)square_x(n - 1) + (double)square_x(n)) / 2.0 + SQUARE_SIZE / 2.0;
    double expected_y = SQUARE_Y + SQUARE_SIZE / 2.0;
    /* Centroid of pixel indices sits half a pixel left/up of the geometric centre. */
    expected_x -= 0.5;
    expected_y -= 0.5;
    bool placed = cx > expected_x - 2.0 && cx < expected_x + 2.0 && cy > expected_y - 2.0 &&
                  cy < expected_y + 2.0;
    bool sized = bright > SQUARE_SIZE * SQUARE_SIZE / 2 && bright < SQUARE_SIZE * SQUARE_SIZE * 2;
    (void)fprintf(stderr, "%s: %lu bright pixels, centroid (%.1f, %.1f), expected (%.1f, %.1f)%s\n",
                  path, bright, cx, cy, expected_x, expected_y,
                  placed && sized ? "" : " MISMATCH");
    return placed && sized;
}

int main(void)
{
    if (!instance_layer_available(LAYER_NAME)) {
        (void)fprintf(stderr, LAYER_NAME " not found: set VK_ADD_LAYER_PATH to the build's layer/ dir\n");
        return EXIT_FAILURE;
    }
    if (!instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        (void)fprintf(stderr, "skipped: " VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME " unavailable\n");
        return EXIT_SKIP;
    }

    const char *dump_dir = getenv("AFMF_DUMP_DIR");
    if (dump_dir != NULL && mkdir(dump_dir, 0755) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "cannot create %s: %s\n", dump_dir, strerror(errno));
        return EXIT_FAILURE;
    }

    struct ctx ctx = {0};
    bool ok = run(&ctx);
    destroy(&ctx);

    /* Dumps 1 and 2 are the companions of real frames 1 and 2: the square must be halfway. */
    if (ok && dump_dir != NULL)
        ok = check_dump(dump_dir, 1) && check_dump(dump_dir, 2);

    if (ctx.validation_errors > 0) {
        (void)fprintf(stderr, "%" PRIu32 " validation error(s)\n", ctx.validation_errors);
        return EXIT_FAILURE;
    }
    (void)fprintf(stderr, ok ? "presented %u frames through " LAYER_NAME "\n" : "failed\n", FRAMES);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
