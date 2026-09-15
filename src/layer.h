#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

struct afmf_swapchain;

/* Next-layer entry points the layer calls. The swapchain ones are NULL when the application did
 * not enable VK_KHR_swapchain on the device; the rest are core 1.0 and always present. */
struct afmf_device_fns {
    PFN_vkDestroyDevice destroy_device;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetDeviceQueue2 get_device_queue2;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkGetImageMemoryRequirements get_image_memory_requirements;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    PFN_vkBindImageMemory bind_image_memory;
    PFN_vkCreateSemaphore create_semaphore;
    PFN_vkDestroySemaphore destroy_semaphore;
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkWaitForFences wait_for_fences;
    PFN_vkResetFences reset_fences;
    PFN_vkCreateCommandPool create_command_pool;
    PFN_vkDestroyCommandPool destroy_command_pool;
    PFN_vkAllocateCommandBuffers allocate_command_buffers;
    PFN_vkBeginCommandBuffer begin_command_buffer;
    PFN_vkEndCommandBuffer end_command_buffer;
    PFN_vkCmdPipelineBarrier cmd_pipeline_barrier;
    PFN_vkCmdCopyImage cmd_copy_image;
    PFN_vkQueueSubmit queue_submit;

    PFN_vkCreateShaderModule create_shader_module;
    PFN_vkDestroyShaderModule destroy_shader_module;
    PFN_vkCreateDescriptorSetLayout create_descriptor_set_layout;
    PFN_vkDestroyDescriptorSetLayout destroy_descriptor_set_layout;
    PFN_vkCreatePipelineLayout create_pipeline_layout;
    PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
    PFN_vkCreateComputePipelines create_compute_pipelines;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateDescriptorPool create_descriptor_pool;
    PFN_vkDestroyDescriptorPool destroy_descriptor_pool;
    PFN_vkAllocateDescriptorSets allocate_descriptor_sets;
    PFN_vkUpdateDescriptorSets update_descriptor_sets;
    PFN_vkCreateImageView create_image_view;
    PFN_vkDestroyImageView destroy_image_view;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements;
    PFN_vkBindBufferMemory bind_buffer_memory;
    PFN_vkMapMemory map_memory;
    PFN_vkUnmapMemory unmap_memory;
    PFN_vkCmdBindPipeline cmd_bind_pipeline;
    PFN_vkCmdBindDescriptorSets cmd_bind_descriptor_sets;
    PFN_vkCmdDispatch cmd_dispatch;
    PFN_vkCmdPushConstants cmd_push_constants;
    PFN_vkCmdClearColorImage cmd_clear_color_image;
    PFN_vkCmdCopyImageToBuffer cmd_copy_image_to_buffer;
    PFN_vkCreateQueryPool create_query_pool;
    PFN_vkDestroyQueryPool destroy_query_pool;
    PFN_vkCmdResetQueryPool cmd_reset_query_pool;
    PFN_vkCmdWriteTimestamp cmd_write_timestamp;
    PFN_vkGetQueryPoolResults get_query_pool_results;

    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images;
    PFN_vkAcquireNextImageKHR acquire_next_image;
    PFN_vkQueuePresentKHR queue_present;
};

/* Instance-level entry points resolved once per device for its physical device. */
struct afmf_instance_fns {
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_surface_capabilities;
    PFN_vkGetPhysicalDeviceFormatProperties get_format_properties;
};

struct afmf_framegen_pipelines;

struct afmf_queue {
    VkQueue handle;
    uint32_t family;
};

/* One entry per VkDevice that passed through the layer. `key` is the loader's dispatch pointer,
 * shared by the device and every VkQueue obtained from it. */
struct afmf_device {
    void *key;
    VkDevice handle;
    VkPhysicalDevice physical_device;
    uint32_t api_version; /* what the application asked for, capped by the device */
    PFN_vkGetDeviceProcAddr gdpa;
    /* Dispatchable objects the layer creates below the loader's trampolines (command buffers) must
     * be handed to this so the loader stamps its dispatch pointer on them. */
    PFN_vkSetDeviceLoaderData set_loader_data;
    struct afmf_device_fns fns;
    struct afmf_instance_fns ifns;

    VkPhysicalDeviceMemoryProperties memory_properties;
    VkPhysicalDeviceLimits limits;
    VkQueueFamilyProperties *queue_families;
    uint32_t queue_family_count;
    struct afmf_framegen_pipelines *framegen_pipelines; /* created on first use, freed with the device */

    pthread_mutex_t lock; /* guards `queues`, `swapchains` and the per-swapchain counters */
    struct afmf_queue *queues; /* every queue handed out, sized from VkDeviceCreateInfo */
    uint32_t queue_count;
    uint32_t queue_capacity;
    struct afmf_swapchain *swapchains;

    struct afmf_device *next;
};

/* The queue family a queue was created from; false when the layer never saw the queue. */
bool afmf_device_queue_family(struct afmf_device *dev, VkQueue queue, uint32_t *family);
