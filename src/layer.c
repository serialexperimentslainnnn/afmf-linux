/* VK_LAYER_AFMF: loader plumbing.
 *
 * From a build tree the layer is found through VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer when it is
 * enabled implicitly (AFMF_ENABLE=1), or through VK_ADD_LAYER_PATH when an application enables it
 * explicitly by name; the loader searches the two kinds of manifest separately.
 *
 * Implements the loader/layer interface version 2 (vk_layer.h, LoaderLayerInterface.md): version
 * negotiation, the instance and device chains, and the dispatch tables keyed by the loader's
 * dispatch pointer. The behaviour lives in swapchain.c; this file only routes calls there. */

#include "layer.h"

#include "config.h"
#include "framegen.h"
#include "log.h"
#include "swapchain.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vk_layer.h>

#define AFMF_EXPORT __attribute__((visibility("default")))

/* Handle logging casts VkSwapchainKHR to void*, which only holds on 64-bit builds. A 32-bit layer
 * (for 32-bit DXVK titles) is a separate build target, not this one. */
_Static_assert(sizeof(void *) == 8, "64-bit builds only");

struct afmf_instance {
    void *key;
    VkInstance handle;
    uint32_t api_version; /* VkApplicationInfo::apiVersion, 1.0 when absent */
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties;
    PFN_vkGetPhysicalDeviceProperties get_properties;
    struct afmf_instance *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct afmf_instance *g_instances;
static struct afmf_device *g_devices;

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetInstanceProcAddr(VkInstance instance,
                                                                          const char *name);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetDeviceProcAddr(VkDevice device,
                                                                        const char *name);

/* Every dispatchable handle begins with the loader's dispatch pointer. It is shared by an instance
 * and its physical devices, and by a device and its queues, which is what makes it a usable key. */
static void *dispatch_key(const void *handle)
{
    void *key;
    memcpy(&key, handle, sizeof key);
    return key;
}

/* ---- registries -------------------------------------------------------------------------- */

static struct afmf_instance *instance_find(void *key)
{
    pthread_mutex_lock(&g_lock);
    struct afmf_instance *inst = g_instances;
    while (inst != NULL && inst->key != key)
        inst = inst->next;
    pthread_mutex_unlock(&g_lock);
    return inst;
}

static struct afmf_instance *instance_take(void *key)
{
    pthread_mutex_lock(&g_lock);
    struct afmf_instance **link = &g_instances;
    while (*link != NULL && (*link)->key != key)
        link = &(*link)->next;
    struct afmf_instance *inst = *link;
    if (inst != NULL)
        *link = inst->next;
    pthread_mutex_unlock(&g_lock);
    return inst;
}

static struct afmf_device *device_find(void *key)
{
    pthread_mutex_lock(&g_lock);
    struct afmf_device *dev = g_devices;
    while (dev != NULL && dev->key != key)
        dev = dev->next;
    pthread_mutex_unlock(&g_lock);
    return dev;
}

static struct afmf_device *device_take(void *key)
{
    pthread_mutex_lock(&g_lock);
    struct afmf_device **link = &g_devices;
    while (*link != NULL && (*link)->key != key)
        link = &(*link)->next;
    struct afmf_device *dev = *link;
    if (dev != NULL)
        *link = dev->next;
    pthread_mutex_unlock(&g_lock);
    return dev;
}

static void device_free(struct afmf_device *dev)
{
    pthread_mutex_destroy(&dev->lock);
    pthread_mutex_destroy(&dev->async_lock);
    free(dev->queues);
    free(dev->queue_families);
    free(dev->app_families);
    free(dev);
}

/* The queue family the layer's own queue should come from: compute without graphics first (the
 * async compute engines on RDNA), then any compute family, provided it has a queue the
 * application did not ask for. Returns UINT32_MAX when there is none; *index is the queue index
 * to request within the family. */
static uint32_t choose_async_family(const struct afmf_device *dev, const VkDeviceCreateInfo *info,
                                    uint32_t *index)
{
    for (int compute_only = 1; compute_only >= 0; compute_only--) {
        for (uint32_t f = 0; f < dev->queue_family_count; f++) {
            VkQueueFlags flags = dev->queue_families[f].queueFlags;
            if (!(flags & VK_QUEUE_COMPUTE_BIT) || (compute_only && (flags & VK_QUEUE_GRAPHICS_BIT)))
                continue;
            uint32_t requested = 0;
            for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
                if (info->pQueueCreateInfos[i].queueFamilyIndex == f)
                    requested += info->pQueueCreateInfos[i].queueCount;
            if (requested < dev->queue_families[f].queueCount) {
                *index = requested;
                return f;
            }
        }
    }
    return UINT32_MAX;
}

/* Copies the application's queue requests plus one queue in `family`. The arrays are the caller's
 * to free after vkCreateDevice returned; NULL on allocation failure. */
static VkDeviceQueueCreateInfo *queues_with_extra(const VkDeviceCreateInfo *info, uint32_t family,
                                                  uint32_t *count, float **priorities)
{
    static const float one = 1.0f;
    VkDeviceQueueCreateInfo *queues = calloc(info->queueCreateInfoCount + 1, sizeof *queues);
    if (queues == NULL)
        return NULL;
    memcpy(queues, info->pQueueCreateInfos, info->queueCreateInfoCount * sizeof *queues);
    *count = info->queueCreateInfoCount;
    *priorities = NULL;

    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        if (queues[i].queueFamilyIndex != family)
            continue;
        /* The family is already requested: one more queue, its priorities array extended. */
        uint32_t n = queues[i].queueCount + 1;
        *priorities = calloc(n, sizeof **priorities);
        if (*priorities == NULL) {
            free(queues);
            return NULL;
        }
        memcpy(*priorities, queues[i].pQueuePriorities, queues[i].queueCount * sizeof **priorities);
        (*priorities)[n - 1] = one;
        queues[i].queueCount = n;
        queues[i].pQueuePriorities = *priorities;
        return queues;
    }
    queues[*count] = (VkDeviceQueueCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &one,
    };
    (*count)++;
    return queues;
}

bool afmf_device_queue_family(struct afmf_device *dev, VkQueue queue, uint32_t *family)
{
    bool found = false;
    pthread_mutex_lock(&dev->lock);
    for (uint32_t i = 0; i < dev->queue_count && !found; i++) {
        if (dev->queues[i].handle == queue) {
            *family = dev->queues[i].family;
            found = true;
        }
    }
    pthread_mutex_unlock(&dev->lock);
    return found;
}

static void device_record_queue(struct afmf_device *dev, VkQueue queue, uint32_t family)
{
    if (queue == VK_NULL_HANDLE)
        return;
    pthread_mutex_lock(&dev->lock);
    bool known = false;
    for (uint32_t i = 0; i < dev->queue_count && !known; i++)
        known = dev->queues[i].handle == queue;
    if (!known) {
        if (dev->queue_count < dev->queue_capacity) {
            dev->queues[dev->queue_count].handle = queue;
            dev->queues[dev->queue_count].family = family;
            dev->queue_count++;
        } else {
            AFMF_WARN("more queues handed out than VkDeviceCreateInfo declared; ignoring %p",
                      (void *)queue);
        }
    }
    pthread_mutex_unlock(&dev->lock);
}

/* ---- chain info ---------------------------------------------------------------------------- */

/* The loader hands each layer the chain link inside pNext precisely so the layer advances it
 * (LoaderLayerInterface.md, "Layer Dispatch Initialization"). The const on pNext is the API's
 * promise to the driver, not to the loader's own structure, hence the cast through uintptr_t. */
static VkLayerInstanceCreateInfo *instance_link_info(const VkInstanceCreateInfo *info)
{
    for (const VkBaseInStructure *p = info->pNext; p != NULL; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO)
            continue;
        const VkLayerInstanceCreateInfo *link = (const VkLayerInstanceCreateInfo *)p;
        if (link->function == VK_LAYER_LINK_INFO)
            return (VkLayerInstanceCreateInfo *)(uintptr_t)link;
    }
    return NULL;
}

static VkLayerDeviceCreateInfo *device_loader_info(const VkDeviceCreateInfo *info,
                                                   VkLayerFunction function)
{
    for (const VkBaseInStructure *p = info->pNext; p != NULL; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        const VkLayerDeviceCreateInfo *link = (const VkLayerDeviceCreateInfo *)p;
        if (link->function == function)
            return (VkLayerDeviceCreateInfo *)(uintptr_t)link;
    }
    return NULL;
}

/* ---- instance chain ------------------------------------------------------------------------ */

static VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateInstance(const VkInstanceCreateInfo *info,
                                                          const VkAllocationCallbacks *alloc,
                                                          VkInstance *out)
{
    if (afmf_config_get()->invalid)
        AFMF_WARN("an AFMF_* variable is set to an invalid value; using its default");

    VkLayerInstanceCreateInfo *link = instance_link_info(info);
    if (link == NULL || link->u.pLayerInfo == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkCreateInstance next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (next_create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = next_create(info, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_instance *inst = calloc(1, sizeof *inst);
    PFN_vkDestroyInstance next_destroy = (PFN_vkDestroyInstance)next_gipa(*out, "vkDestroyInstance");
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory =
        (PFN_vkGetPhysicalDeviceMemoryProperties)next_gipa(*out,
                                                           "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)next_gipa(
            *out, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetPhysicalDeviceProperties get_properties =
        (PFN_vkGetPhysicalDeviceProperties)next_gipa(*out, "vkGetPhysicalDeviceProperties");
    if (inst == NULL || next_destroy == NULL || get_memory == NULL || get_families == NULL ||
        get_properties == NULL) {
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        free(inst);
        *out = VK_NULL_HANDLE;
        return inst == NULL ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    }

    inst->key = dispatch_key(*out);
    inst->handle = *out;
    inst->api_version = info->pApplicationInfo != NULL && info->pApplicationInfo->apiVersion != 0
                            ? info->pApplicationInfo->apiVersion
                            : VK_API_VERSION_1_0;
    inst->gipa = next_gipa;
    inst->destroy_instance = next_destroy;
    inst->get_memory_properties = get_memory;
    inst->get_queue_family_properties = get_families;
    inst->get_properties = get_properties;

    pthread_mutex_lock(&g_lock);
    inst->next = g_instances;
    g_instances = inst;
    pthread_mutex_unlock(&g_lock);

    AFMF_INFO("layer active on instance %p", (void *)*out);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL afmf_DestroyInstance(VkInstance instance,
                                                       const VkAllocationCallbacks *alloc)
{
    if (instance == VK_NULL_HANDLE)
        return;

    struct afmf_instance *inst = instance_take(dispatch_key(instance));
    if (inst == NULL) {
        AFMF_ERR("vkDestroyInstance on instance %p the layer never created", (void *)instance);
        return;
    }
    inst->destroy_instance(instance, alloc);
    free(inst);
}

/* ---- device chain -------------------------------------------------------------------------- */

#define LOAD_DEVICE_FN(field, name) dev->fns.field = (PFN_##name)next_gdpa(*out, #name)

/* Resolves the next layer's device entry points. Returns false when a core function is missing,
 * which no conformant chain does. */
static bool load_device_fns(struct afmf_device *dev, PFN_vkGetDeviceProcAddr next_gdpa,
                            VkDevice *out)
{
    LOAD_DEVICE_FN(destroy_device, vkDestroyDevice);
    LOAD_DEVICE_FN(get_device_queue, vkGetDeviceQueue);
    LOAD_DEVICE_FN(get_device_queue2, vkGetDeviceQueue2);
    LOAD_DEVICE_FN(create_image, vkCreateImage);
    LOAD_DEVICE_FN(destroy_image, vkDestroyImage);
    LOAD_DEVICE_FN(get_image_memory_requirements, vkGetImageMemoryRequirements);
    LOAD_DEVICE_FN(allocate_memory, vkAllocateMemory);
    LOAD_DEVICE_FN(free_memory, vkFreeMemory);
    LOAD_DEVICE_FN(bind_image_memory, vkBindImageMemory);
    LOAD_DEVICE_FN(create_semaphore, vkCreateSemaphore);
    LOAD_DEVICE_FN(destroy_semaphore, vkDestroySemaphore);
    LOAD_DEVICE_FN(create_fence, vkCreateFence);
    LOAD_DEVICE_FN(destroy_fence, vkDestroyFence);
    LOAD_DEVICE_FN(wait_for_fences, vkWaitForFences);
    LOAD_DEVICE_FN(reset_fences, vkResetFences);
    LOAD_DEVICE_FN(create_command_pool, vkCreateCommandPool);
    LOAD_DEVICE_FN(destroy_command_pool, vkDestroyCommandPool);
    LOAD_DEVICE_FN(allocate_command_buffers, vkAllocateCommandBuffers);
    LOAD_DEVICE_FN(begin_command_buffer, vkBeginCommandBuffer);
    LOAD_DEVICE_FN(end_command_buffer, vkEndCommandBuffer);
    LOAD_DEVICE_FN(cmd_pipeline_barrier, vkCmdPipelineBarrier);
    LOAD_DEVICE_FN(cmd_copy_image, vkCmdCopyImage);
    LOAD_DEVICE_FN(queue_submit, vkQueueSubmit);
    LOAD_DEVICE_FN(queue_wait_idle, vkQueueWaitIdle);
    LOAD_DEVICE_FN(create_shader_module, vkCreateShaderModule);
    LOAD_DEVICE_FN(destroy_shader_module, vkDestroyShaderModule);
    LOAD_DEVICE_FN(create_descriptor_set_layout, vkCreateDescriptorSetLayout);
    LOAD_DEVICE_FN(destroy_descriptor_set_layout, vkDestroyDescriptorSetLayout);
    LOAD_DEVICE_FN(create_pipeline_layout, vkCreatePipelineLayout);
    LOAD_DEVICE_FN(destroy_pipeline_layout, vkDestroyPipelineLayout);
    LOAD_DEVICE_FN(create_compute_pipelines, vkCreateComputePipelines);
    LOAD_DEVICE_FN(destroy_pipeline, vkDestroyPipeline);
    LOAD_DEVICE_FN(create_descriptor_pool, vkCreateDescriptorPool);
    LOAD_DEVICE_FN(destroy_descriptor_pool, vkDestroyDescriptorPool);
    LOAD_DEVICE_FN(allocate_descriptor_sets, vkAllocateDescriptorSets);
    LOAD_DEVICE_FN(update_descriptor_sets, vkUpdateDescriptorSets);
    LOAD_DEVICE_FN(create_image_view, vkCreateImageView);
    LOAD_DEVICE_FN(destroy_image_view, vkDestroyImageView);
    LOAD_DEVICE_FN(create_sampler, vkCreateSampler);
    LOAD_DEVICE_FN(destroy_sampler, vkDestroySampler);
    LOAD_DEVICE_FN(create_buffer, vkCreateBuffer);
    LOAD_DEVICE_FN(destroy_buffer, vkDestroyBuffer);
    LOAD_DEVICE_FN(get_buffer_memory_requirements, vkGetBufferMemoryRequirements);
    LOAD_DEVICE_FN(bind_buffer_memory, vkBindBufferMemory);
    LOAD_DEVICE_FN(map_memory, vkMapMemory);
    LOAD_DEVICE_FN(unmap_memory, vkUnmapMemory);
    LOAD_DEVICE_FN(cmd_bind_pipeline, vkCmdBindPipeline);
    LOAD_DEVICE_FN(cmd_bind_descriptor_sets, vkCmdBindDescriptorSets);
    LOAD_DEVICE_FN(cmd_dispatch, vkCmdDispatch);
    LOAD_DEVICE_FN(cmd_push_constants, vkCmdPushConstants);
    LOAD_DEVICE_FN(cmd_clear_color_image, vkCmdClearColorImage);
    LOAD_DEVICE_FN(cmd_copy_image_to_buffer, vkCmdCopyImageToBuffer);
    LOAD_DEVICE_FN(create_query_pool, vkCreateQueryPool);
    LOAD_DEVICE_FN(destroy_query_pool, vkDestroyQueryPool);
    LOAD_DEVICE_FN(cmd_reset_query_pool, vkCmdResetQueryPool);
    LOAD_DEVICE_FN(cmd_write_timestamp, vkCmdWriteTimestamp);
    LOAD_DEVICE_FN(get_query_pool_results, vkGetQueryPoolResults);
    /* NULL when VK_KHR_swapchain is not enabled: the hooks are then never handed out (see GDPA). */
    LOAD_DEVICE_FN(create_swapchain, vkCreateSwapchainKHR);
    LOAD_DEVICE_FN(destroy_swapchain, vkDestroySwapchainKHR);
    LOAD_DEVICE_FN(get_swapchain_images, vkGetSwapchainImagesKHR);
    LOAD_DEVICE_FN(acquire_next_image, vkAcquireNextImageKHR);
    LOAD_DEVICE_FN(queue_present, vkQueuePresentKHR);

    const struct afmf_device_fns *f = &dev->fns;
    return f->destroy_device && f->get_device_queue && f->create_image && f->destroy_image &&
           f->get_image_memory_requirements && f->allocate_memory && f->free_memory &&
           f->bind_image_memory && f->create_semaphore && f->destroy_semaphore &&
           f->create_fence && f->destroy_fence && f->wait_for_fences && f->reset_fences &&
           f->create_command_pool && f->destroy_command_pool && f->allocate_command_buffers &&
           f->begin_command_buffer && f->end_command_buffer && f->cmd_pipeline_barrier &&
           f->cmd_copy_image && f->queue_submit && f->queue_wait_idle && f->create_shader_module &&
           f->destroy_shader_module && f->create_descriptor_set_layout &&
           f->destroy_descriptor_set_layout && f->create_pipeline_layout &&
           f->destroy_pipeline_layout && f->create_compute_pipelines && f->destroy_pipeline &&
           f->create_descriptor_pool && f->destroy_descriptor_pool &&
           f->allocate_descriptor_sets && f->update_descriptor_sets && f->create_image_view &&
           f->destroy_image_view && f->create_sampler && f->destroy_sampler && f->create_buffer &&
           f->destroy_buffer && f->get_buffer_memory_requirements && f->bind_buffer_memory &&
           f->map_memory && f->unmap_memory && f->cmd_bind_pipeline &&
           f->cmd_bind_descriptor_sets && f->cmd_dispatch && f->cmd_push_constants &&
           f->cmd_clear_color_image && f->cmd_copy_image_to_buffer && f->create_query_pool &&
           f->destroy_query_pool && f->cmd_reset_query_pool && f->cmd_write_timestamp &&
           f->get_query_pool_results;
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateDevice(VkPhysicalDevice physical_device,
                                                        const VkDeviceCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc,
                                                        VkDevice *out)
{
    struct afmf_instance *inst = instance_find(dispatch_key(physical_device));
    VkLayerDeviceCreateInfo *link = device_loader_info(info, VK_LAYER_LINK_INFO);
    VkLayerDeviceCreateInfo *loader_data = device_loader_info(info, VK_LOADER_DATA_CALLBACK);
    if (inst == NULL || link == NULL || link->u.pLayerInfo == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkCreateDevice next_create = (PFN_vkCreateDevice)next_gipa(inst->handle, "vkCreateDevice");
    if (next_create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;

    /* Everything about the physical device is needed before creation: the queue families decide
     * whether the layer can ask for a compute queue of its own. */
    struct afmf_device *dev = calloc(1, sizeof *dev);
    if (dev == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->physical_device = physical_device;
    dev->gdpa = next_gdpa;
    dev->set_loader_data = loader_data != NULL ? loader_data->u.pfnSetDeviceLoaderData : NULL;
    dev->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dev->async_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dev->ifns.get_surface_capabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    dev->ifns.get_surface_support = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceSupportKHR");
    dev->ifns.get_format_properties = (PFN_vkGetPhysicalDeviceFormatProperties)next_gipa(
        inst->handle, "vkGetPhysicalDeviceFormatProperties");

    VkPhysicalDeviceProperties properties;
    inst->get_properties(physical_device, &properties);
    dev->limits = properties.limits;
    dev->api_version = properties.apiVersion < inst->api_version ? properties.apiVersion
                                                                 : inst->api_version;
    inst->get_memory_properties(physical_device, &dev->memory_properties);
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count, NULL);
    dev->queue_families = calloc(dev->queue_family_count, sizeof *dev->queue_families);
    dev->app_families = calloc(info->queueCreateInfoCount, sizeof *dev->app_families);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        dev->queue_capacity += info->pQueueCreateInfos[i].queueCount;
    dev->queues = calloc(dev->queue_capacity, sizeof *dev->queues);
    if (dev->queue_families == NULL || dev->queues == NULL || dev->app_families == NULL) {
        device_free(dev);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count,
                                      dev->queue_families);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++) {
        uint32_t family = info->pQueueCreateInfos[i].queueFamilyIndex;
        bool known = false;
        for (uint32_t k = 0; k < dev->app_family_count && !known; k++)
            known = dev->app_families[k] == family;
        if (!known)
            dev->app_families[dev->app_family_count++] = family;
    }

    /* Ask for the layer's queue alongside the application's. */
    VkDeviceCreateInfo patched = *info;
    VkDeviceQueueCreateInfo *queues = NULL;
    float *priorities = NULL;
    uint32_t async_index = 0;
    uint32_t async_family = dev->set_loader_data != NULL
                                ? choose_async_family(dev, info, &async_index)
                                : UINT32_MAX;
    if (async_family != UINT32_MAX) {
        queues = queues_with_extra(info, async_family, &patched.queueCreateInfoCount, &priorities);
        if (queues != NULL)
            patched.pQueueCreateInfos = queues;
        else
            async_family = UINT32_MAX;
    }

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = next_create(physical_device, &patched, alloc, out);
    free(queues);
    free(priorities);
    if (res != VK_SUCCESS) {
        device_free(dev);
        return res;
    }

    if (!load_device_fns(dev, next_gdpa, out)) {
        PFN_vkDestroyDevice next_destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        device_free(dev);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    dev->key = dispatch_key(*out);
    dev->handle = *out;

    if (async_family != UINT32_MAX) {
        /* Obtained below the loader's trampoline: stamp the dispatch pointer, as for command
         * buffers, or the next layer cannot route calls made with it. */
        dev->fns.get_device_queue(*out, async_family, async_index, &dev->async_queue);
        if (dev->async_queue != VK_NULL_HANDLE &&
            dev->set_loader_data(*out, dev->async_queue) != VK_SUCCESS)
            dev->async_queue = VK_NULL_HANDLE;
        dev->async_family = async_family;
    }

    pthread_mutex_lock(&g_lock);
    dev->next = g_devices;
    g_devices = dev;
    pthread_mutex_unlock(&g_lock);

    if (dev->async_queue != VK_NULL_HANDLE)
        AFMF_INFO("device %p created, VK_KHR_swapchain %s, layer queue on family %u", (void *)*out,
                  dev->fns.queue_present != NULL ? "enabled" : "not enabled", dev->async_family);
    else
        AFMF_INFO("device %p created, VK_KHR_swapchain %s, no spare compute queue: working on "
                  "the application's",
                  (void *)*out, dev->fns.queue_present != NULL ? "enabled" : "not enabled");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL afmf_DestroyDevice(VkDevice device,
                                                     const VkAllocationCallbacks *alloc)
{
    if (device == VK_NULL_HANDLE)
        return;

    struct afmf_device *dev = device_take(dispatch_key(device));
    if (dev == NULL) {
        AFMF_ERR("vkDestroyDevice on device %p the layer never created", (void *)device);
        return;
    }
    afmf_swapchain_forget_all(dev);
    afmf_framegen_pipelines_destroy(dev);
    dev->fns.destroy_device(device, alloc);
    device_free(dev);
}

static VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue(VkDevice device, uint32_t family,
                                                      uint32_t index, VkQueue *queue)
{
    struct afmf_device *dev = device_find(dispatch_key(device));
    if (dev == NULL) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    dev->fns.get_device_queue(device, family, index, queue);
    device_record_queue(dev, *queue, family);
}

static VKAPI_ATTR void VKAPI_CALL afmf_GetDeviceQueue2(VkDevice device,
                                                       const VkDeviceQueueInfo2 *info,
                                                       VkQueue *queue)
{
    struct afmf_device *dev = device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.get_device_queue2 == NULL) {
        *queue = VK_NULL_HANDLE;
        return;
    }
    dev->fns.get_device_queue2(device, info, queue);
    device_record_queue(dev, *queue, info->queueFamilyIndex);
}

/* ---- swapchain hooks ----------------------------------------------------------------------- */

static VKAPI_ATTR VkResult VKAPI_CALL afmf_CreateSwapchainKHR(VkDevice device,
                                                              const VkSwapchainCreateInfoKHR *info,
                                                              const VkAllocationCallbacks *alloc,
                                                              VkSwapchainKHR *out)
{
    struct afmf_device *dev = device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.create_swapchain == NULL)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    return afmf_swapchain_create(dev, info, alloc, out);
}

static VKAPI_ATTR void VKAPI_CALL afmf_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                           const VkAllocationCallbacks *alloc)
{
    struct afmf_device *dev = device_find(dispatch_key(device));
    if (dev == NULL || dev->fns.destroy_swapchain == NULL) {
        AFMF_ERR("vkDestroySwapchainKHR on device %p without VK_KHR_swapchain", (void *)device);
        return;
    }
    afmf_swapchain_destroy(dev, swapchain, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL afmf_QueuePresentKHR(VkQueue queue,
                                                           const VkPresentInfoKHR *info)
{
    struct afmf_device *dev = device_find(dispatch_key(queue));
    if (dev == NULL || dev->fns.queue_present == NULL) {
        AFMF_ERR("vkQueuePresentKHR on queue %p of an unknown device", (void *)queue);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return afmf_swapchain_present(dev, queue, info);
}

/* ---- proc address routing ------------------------------------------------------------------ */

struct hook {
    const char *name;
    PFN_vkVoidFunction fn;
};

#define HOOK(function) {"vk" #function, (PFN_vkVoidFunction)afmf_##function}

static const struct hook instance_hooks[] = {
    HOOK(GetInstanceProcAddr), HOOK(CreateInstance), HOOK(DestroyInstance), HOOK(CreateDevice),
};

static const struct hook device_hooks[] = {
    HOOK(GetDeviceProcAddr), HOOK(DestroyDevice), HOOK(GetDeviceQueue), HOOK(GetDeviceQueue2),
};

/* Only handed out when the next layer/driver has them, i.e. when VK_KHR_swapchain is enabled. */
static const struct hook swapchain_hooks[] = {
    HOOK(CreateSwapchainKHR),
    HOOK(DestroySwapchainKHR),
    HOOK(QueuePresentKHR),
};

#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

static PFN_vkVoidFunction lookup(const struct hook *hooks, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++)
        if (strcmp(hooks[i].name, name) == 0)
            return hooks[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetInstanceProcAddr(VkInstance instance,
                                                                          const char *name)
{
    PFN_vkVoidFunction fn = lookup(instance_hooks, ARRAY_LEN(instance_hooks), name);
    if (fn == NULL)
        fn = lookup(device_hooks, ARRAY_LEN(device_hooks), name);
    if (fn == NULL)
        fn = lookup(swapchain_hooks, ARRAY_LEN(swapchain_hooks), name);
    if (fn != NULL)
        return fn;

    if (instance == VK_NULL_HANDLE)
        return NULL;
    struct afmf_instance *inst = instance_find(dispatch_key(instance));
    return inst != NULL ? inst->gipa(instance, name) : NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL afmf_GetDeviceProcAddr(VkDevice device,
                                                                        const char *name)
{
    if (device == VK_NULL_HANDLE)
        return NULL;
    struct afmf_device *dev = device_find(dispatch_key(device));
    if (dev == NULL)
        return NULL;

    PFN_vkVoidFunction fn = lookup(device_hooks, ARRAY_LEN(device_hooks), name);
    if (fn != NULL)
        return dev->gdpa(device, name) != NULL ? fn : NULL;

    fn = lookup(swapchain_hooks, ARRAY_LEN(swapchain_hooks), name);
    if (fn != NULL)
        return dev->gdpa(device, name) != NULL ? fn : NULL;

    return dev->gdpa(device, name);
}

/* ---- loader entry point -------------------------------------------------------------------- */

AFMF_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *negotiate)
{
    if (negotiate == NULL || negotiate->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (negotiate->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;

    negotiate->loaderLayerInterfaceVersion = 2;
    negotiate->pfnGetInstanceProcAddr = afmf_GetInstanceProcAddr;
    negotiate->pfnGetDeviceProcAddr = afmf_GetDeviceProcAddr;
    negotiate->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
