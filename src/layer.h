#pragma once

#include <pthread.h>
#include <vulkan/vulkan.h>

struct afmf_swapchain;

/* One entry per VkDevice that passed through the layer. `key` is the loader's dispatch pointer,
 * shared by the device and every VkQueue obtained from it. The swapchain-related entry points are
 * NULL when the application did not enable VK_KHR_swapchain on this device. */
struct afmf_device {
    void *key;
    VkDevice handle;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkQueuePresentKHR queue_present;

    pthread_mutex_t lock; /* guards `swapchains` and the per-swapchain counters */
    struct afmf_swapchain *swapchains;

    struct afmf_device *next;
};
