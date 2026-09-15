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
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_family_properties;
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
    free(dev->queues);
    free(dev->queue_families);
    free(dev);
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
    if (inst == NULL || next_destroy == NULL || get_memory == NULL || get_families == NULL) {
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        free(inst);
        *out = VK_NULL_HANDLE;
        return inst == NULL ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    }

    inst->key = dispatch_key(*out);
    inst->handle = *out;
    inst->gipa = next_gipa;
    inst->destroy_instance = next_destroy;
    inst->get_memory_properties = get_memory;
    inst->get_queue_family_properties = get_families;

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
           f->cmd_copy_image && f->queue_submit;
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

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    VkResult res = next_create(physical_device, info, alloc, out);
    if (res != VK_SUCCESS)
        return res;

    struct afmf_device *dev = calloc(1, sizeof *dev);
    if (dev == NULL || !load_device_fns(dev, next_gdpa, out)) {
        PFN_vkDestroyDevice next_destroy = (PFN_vkDestroyDevice)next_gdpa(*out, "vkDestroyDevice");
        if (next_destroy != NULL)
            next_destroy(*out, alloc);
        free(dev);
        *out = VK_NULL_HANDLE;
        return dev == NULL ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_INITIALIZATION_FAILED;
    }

    dev->key = dispatch_key(*out);
    dev->handle = *out;
    dev->physical_device = physical_device;
    dev->gdpa = next_gdpa;
    dev->set_loader_data = loader_data != NULL ? loader_data->u.pfnSetDeviceLoaderData : NULL;
    dev->lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    dev->ifns.get_surface_capabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)next_gipa(
        inst->handle, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    inst->get_memory_properties(physical_device, &dev->memory_properties);
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count, NULL);
    dev->queue_families = calloc(dev->queue_family_count, sizeof *dev->queue_families);
    for (uint32_t i = 0; i < info->queueCreateInfoCount; i++)
        dev->queue_capacity += info->pQueueCreateInfos[i].queueCount;
    dev->queues = calloc(dev->queue_capacity, sizeof *dev->queues);
    if (dev->queue_families == NULL || dev->queues == NULL) {
        dev->fns.destroy_device(*out, alloc);
        device_free(dev);
        *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    inst->get_queue_family_properties(physical_device, &dev->queue_family_count,
                                      dev->queue_families);

    pthread_mutex_lock(&g_lock);
    dev->next = g_devices;
    g_devices = dev;
    pthread_mutex_unlock(&g_lock);

    AFMF_INFO("device %p created, VK_KHR_swapchain %s", (void *)*out,
              dev->fns.queue_present != NULL ? "enabled" : "not enabled");
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
