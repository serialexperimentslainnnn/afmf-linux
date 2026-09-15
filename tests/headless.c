/* Headless integration test for VK_LAYER_AFMF.
 *
 * Enables the layer by name (so a missing or broken layer fails vkCreateInstance instead of
 * silently running without it), puts VK_LAYER_KHRONOS_validation below it when available so the
 * calls the layer forwards are validated, creates a swapchain on a VK_EXT_headless_surface and
 * presents FRAMES frames. No window, no environment tricks: built with -DAFMF_SANITIZE=ON it is an
 * ordinary instrumented executable. Exit codes: 0 pass, 1 fail, 77 skipped (no headless surface). */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define LAYER_NAME "VK_LAYER_AFMF"
#define VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"
#define FRAMES 120u
#define EXIT_SKIP 77

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
    uint32_t validation_errors;
};

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
    const char *extensions[3] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                                 VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
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
        .enabledExtensionCount = with_validation ? 3u : 2u,
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

static bool create_device_and_swapchain(struct ctx *ctx)
{
    const float priority = 1.0f;
    const char *extensions[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = ctx->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue,
        .enabledExtensionCount = 1,
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
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(vkCreateSwapchainKHR(ctx->device, &swapchain, NULL, &ctx->swapchain));

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
    return true;
}

/* Acquire, transition the image to PRESENT_SRC, present, then drain the queue so the same two
 * semaphores can be reused next frame without any tracking. Correctness, not throughput. */
static bool present_frame(struct ctx *ctx)
{
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
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
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

    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->ready,
        .swapchainCount = 1,
        .pSwapchains = &ctx->swapchain,
        .pImageIndices = &index,
    };
    res = vkQueuePresentKHR(ctx->queue, &present);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        (void)fprintf(stderr, "vkQueuePresentKHR -> VkResult %d\n", (int)res);
        return false;
    }
    CHECK(vkQueueWaitIdle(ctx->queue));
    return true;
}

static void destroy(struct ctx *ctx)
{
    if (ctx->device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(ctx->device);
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
    for (uint32_t i = 0; i < FRAMES; i++)
        if (!present_frame(ctx))
            return false;
    return true;
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

    struct ctx ctx = {0};
    bool ok = run(&ctx);
    destroy(&ctx);

    if (ctx.validation_errors > 0) {
        (void)fprintf(stderr, "%" PRIu32 " validation error(s)\n", ctx.validation_errors);
        return EXIT_FAILURE;
    }
    (void)fprintf(stderr, ok ? "presented %u frames through " LAYER_NAME "\n" : "failed\n", FRAMES);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
