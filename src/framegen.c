/* Frame generation: FidelityFX Optical Flow 1.1.2 (seven compute passes, host sequencing ported
 * from the SDK's ffx_opticalflow.cpp, see shaders/fidelityfx/NOTICE.md) plus the layer's own
 * interpolation shader (shaders/afmf_interpolate.comp).
 *
 * Every internal image lives in VK_IMAGE_LAYOUT_GENERAL for its whole life and the passes are
 * separated by one global memory barrier each: simple, correct, and fast enough for a first
 * implementation. Descriptor sets are built once per swapchain, for both frame parities, so the
 * flow and the interpolation are a fixed sequence of bind + dispatch per (slot, parity,
 * companion): recorded once into a secondary command buffer and executed from the primary, which
 * keeps only what changes per frame (the copies from and to the swapchain images, the dump). */

#include "framegen.h"

#include "afmf_spirv.h"
#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AFMF_FLOW_BLOCK 8u
#define AFMF_DUMP_FRAMES 4u
#define AFMF_PROFILE_QUERIES 32u  /* timestamps per slot: one at start, one after each stage */
#define AFMF_PROFILE_INTERVAL 300u

/* Profiling stages (AFMF_PROFILE=1): GPU time between consecutive timestamps is attributed to
 * the stage that ended at the second one; the search/filter/scale of all levels add up. */
enum stage {
    STAGE_INGEST,
    STAGE_PREPARE,
    STAGE_PYRAMID,
    STAGE_SCD,
    STAGE_SEARCH,
    STAGE_FILTER,
    STAGE_SCALE,
    STAGE_INTERPOLATE,
    STAGE_OUTPUT,
    STAGE_COUNT
};

static const char *const stage_names[STAGE_COUNT] = {
    "ingest copy", "prepare luma", "luma pyramid", "scene change detector", "search (all levels)",
    "filter (all levels)", "scale (all levels)", "interpolate", "output copy",
};
#define AFMF_LEVELS 7u
#define AFMF_HISTOGRAM_BINS 256u
#define AFMF_HISTOGRAMS_PER_DIM 3u
#define AFMF_HISTOGRAM_SHIFTS 3u
#define AFMF_SCD_SLOTS 3u
#define AFMF_CB_SIZE 32u
#define AFMF_CB_PER_SLOT (AFMF_LEVELS + 1u) /* one per pyramid level, one for the downsampler */
#define AFMF_MIN_EXTENT 128u
#define AFMF_MAX_TRUSTED_MOTION 64.0f /* pixels between frames; beyond it the flow is guesswork */

/* ---- pass descriptions --------------------------------------------------------------------- */

enum pass {
    PASS_PREPARE_LUMA,
    PASS_PYRAMID,
    PASS_SCD_HISTOGRAM,
    PASS_SCD_DIVERGENCE,
    PASS_SEARCH,
    PASS_FILTER,
    PASS_SCALE,
    PASS_INTERPOLATE,
    PASS_DOWNSAMPLE,
    PASS_COUNT
};

enum variant { VARIANT_RGBA8, VARIANT_RGBA8_BGRA, VARIANT_RGB10A2, VARIANT_RGBA16F, VARIANT_COUNT };

#define SAMPLED VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
#define STORAGE VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
#define COMBINED VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define UBO VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC

/* Binding numbers come from the FidelityFX pass files under shaders/fidelityfx/passes/. */
static const VkDescriptorType bindings_prepare[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_pyramid[] = {STORAGE, STORAGE, STORAGE, STORAGE, STORAGE,
                                                    STORAGE, STORAGE, UBO, UBO};
static const VkDescriptorType bindings_scd_histogram[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_scd_divergence[] = {STORAGE, STORAGE, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_search[] = {SAMPLED, SAMPLED, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_filter[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_scale[] = {SAMPLED, SAMPLED, SAMPLED, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_interpolate[] = {COMBINED, COMBINED, SAMPLED, STORAGE, STORAGE};
static const VkDescriptorType bindings_downsample[] = {COMBINED, STORAGE};

struct pass_desc {
    const uint32_t *spirv;
    size_t spirv_size;
    const VkDescriptorType *bindings;
    uint32_t binding_count;
    uint32_t push_size;
};

struct interpolate_push {
    int32_t size[2];
    int32_t block;
    int32_t fallback;
    float max_motion;
    float flow_scale;
};

struct downsample_push {
    int32_t size[2];
};

#define PASS(name, spv, push)                                                                     \
    {spv, spv##_size, bindings_##name, (uint32_t)(sizeof bindings_##name / sizeof(VkDescriptorType)), push}

static const struct pass_desc passes[PASS_COUNT] = {
    [PASS_PREPARE_LUMA] = PASS(prepare, ffx_opticalflow_prepare_luma_pass_spv, 0),
    [PASS_PYRAMID] = PASS(pyramid, ffx_opticalflow_compute_luminance_pyramid_pass_spv, 0),
    [PASS_SCD_HISTOGRAM] = PASS(scd_histogram, ffx_opticalflow_generate_scd_histogram_pass_spv, 0),
    [PASS_SCD_DIVERGENCE] = PASS(scd_divergence, ffx_opticalflow_compute_scd_divergence_pass_spv, 0),
    [PASS_SEARCH] = PASS(search, ffx_opticalflow_compute_optical_flow_advanced_pass_v5_spv, 0),
    [PASS_FILTER] = PASS(filter, ffx_opticalflow_filter_optical_flow_pass_v5_spv, 0),
    [PASS_SCALE] = PASS(scale, ffx_opticalflow_scale_optical_flow_advanced_pass_v5_spv, 0),
    [PASS_INTERPOLATE] = PASS(interpolate, afmf_interpolate_rgba8_spv,
                              (uint32_t)sizeof(struct interpolate_push)),
    [PASS_DOWNSAMPLE] = PASS(downsample, afmf_downsample_rgba8_spv,
                             (uint32_t)sizeof(struct downsample_push)),
};

/* Half-resolution colour for the flow: 8-bit unless the source is scRGB half floats. */
enum half_variant { HALF_RGBA8, HALF_RGBA16F, HALF_COUNT };

static const struct {
    const uint32_t *spirv;
    size_t spirv_size;
    VkFormat format;
} downsample_variants[HALF_COUNT] = {
    [HALF_RGBA8] = {afmf_downsample_rgba8_spv, afmf_downsample_rgba8_spv_size, VK_FORMAT_R8G8B8A8_UNORM},
    [HALF_RGBA16F] = {afmf_downsample_rgba16f_spv, afmf_downsample_rgba16f_spv_size,
                      VK_FORMAT_R16G16B16A16_SFLOAT},
};

static const struct {
    const uint32_t *spirv;
    size_t spirv_size;
} interpolate_variants[VARIANT_COUNT] = {
    [VARIANT_RGBA8] = {afmf_interpolate_rgba8_spv, afmf_interpolate_rgba8_spv_size},
    [VARIANT_RGBA8_BGRA] = {afmf_interpolate_rgba8_bgra_spv, afmf_interpolate_rgba8_bgra_spv_size},
    [VARIANT_RGB10A2] = {afmf_interpolate_rgb10a2_spv, afmf_interpolate_rgb10a2_spv_size},
    [VARIANT_RGBA16F] = {afmf_interpolate_rgba16f_spv, afmf_interpolate_rgba16f_spv_size},
};

/* ---- per-device pipelines ------------------------------------------------------------------ */

struct afmf_framegen_pipelines {
    VkDescriptorSetLayout set_layouts[PASS_COUNT];
    VkPipelineLayout layouts[PASS_COUNT];
    VkPipeline pipelines[PASS_COUNT]; /* PASS_INTERPOLATE / PASS_DOWNSAMPLE unused: see below */
    VkPipeline interpolate[VARIANT_COUNT];
    VkPipeline downsample[HALF_COUNT];
};

static VkResult create_compute_pipeline(struct afmf_device *dev, const uint32_t *spirv,
                                        size_t spirv_size, VkPipelineLayout layout,
                                        const VkSpecializationInfo *specialization, VkPipeline *out)
{
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_size,
        .pCode = spirv,
    };
    VkShaderModule module;
    VkResult res = dev->fns.create_shader_module(dev->handle, &module_info, NULL, &module);
    if (res != VK_SUCCESS)
        return res;

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = specialization,
        },
        .layout = layout,
    };
    res = dev->fns.create_compute_pipelines(dev->handle, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out);
    dev->fns.destroy_shader_module(dev->handle, module, NULL);
    return res;
}

void afmf_framegen_pipelines_destroy(struct afmf_device *dev)
{
    struct afmf_framegen_pipelines *p = dev->framegen_pipelines;
    if (p == NULL)
        return;
    for (uint32_t v = 0; v < VARIANT_COUNT; v++)
        if (p->interpolate[v] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->interpolate[v], NULL);
    for (uint32_t v = 0; v < HALF_COUNT; v++)
        if (p->downsample[v] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->downsample[v], NULL);
    for (uint32_t i = 0; i < PASS_COUNT; i++) {
        if (p->pipelines[i] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->pipelines[i], NULL);
        if (p->layouts[i] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline_layout(dev->handle, p->layouts[i], NULL);
        if (p->set_layouts[i] != VK_NULL_HANDLE)
            dev->fns.destroy_descriptor_set_layout(dev->handle, p->set_layouts[i], NULL);
    }
    free(p);
    dev->framegen_pipelines = NULL;
}

static VkResult pipelines_create(struct afmf_device *dev)
{
    struct afmf_framegen_pipelines *p = calloc(1, sizeof *p);
    if (p == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->framegen_pipelines = p;

    for (uint32_t i = 0; i < PASS_COUNT; i++) {
        const struct pass_desc *pass = &passes[i];
        VkDescriptorSetLayoutBinding bindings[16];
        for (uint32_t b = 0; b < pass->binding_count; b++) {
            bindings[b] = (VkDescriptorSetLayoutBinding){
                .binding = b,
                .descriptorType = pass->bindings[b],
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };
        }
        VkDescriptorSetLayoutCreateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = pass->binding_count,
            .pBindings = bindings,
        };
        VkResult res = dev->fns.create_descriptor_set_layout(dev->handle, &set_info, NULL,
                                                             &p->set_layouts[i]);
        if (res != VK_SUCCESS)
            return res;

        VkPushConstantRange push = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .size = pass->push_size,
        };
        VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &p->set_layouts[i],
            .pushConstantRangeCount = pass->push_size != 0 ? 1u : 0u,
            .pPushConstantRanges = &push,
        };
        res = dev->fns.create_pipeline_layout(dev->handle, &layout_info, NULL, &p->layouts[i]);
        if (res != VK_SUCCESS)
            return res;

        if (i == PASS_INTERPOLATE || i == PASS_DOWNSAMPLE)
            continue;
        /* The search's static-block threshold (afmfStaticBlockSad in the patched v5 header). */
        uint32_t static_sad = afmf_config_get()->static_block_sad;
        VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof static_sad};
        VkSpecializationInfo search_constants = {
            .mapEntryCount = 1,
            .pMapEntries = &entry,
            .dataSize = sizeof static_sad,
            .pData = &static_sad,
        };
        res = create_compute_pipeline(dev, pass->spirv, pass->spirv_size, p->layouts[i],
                                      i == PASS_SEARCH ? &search_constants : NULL, &p->pipelines[i]);
        if (res != VK_SUCCESS)
            return res;
    }
    for (uint32_t v = 0; v < VARIANT_COUNT; v++) {
        VkResult res = create_compute_pipeline(dev, interpolate_variants[v].spirv,
                                               interpolate_variants[v].spirv_size,
                                               p->layouts[PASS_INTERPOLATE], NULL, &p->interpolate[v]);
        if (res != VK_SUCCESS)
            return res;
    }
    for (uint32_t v = 0; v < HALF_COUNT; v++) {
        VkResult res = create_compute_pipeline(dev, downsample_variants[v].spirv,
                                               downsample_variants[v].spirv_size,
                                               p->layouts[PASS_DOWNSAMPLE], NULL, &p->downsample[v]);
        if (res != VK_SUCCESS)
            return res;
    }
    return VK_SUCCESS;
}

static struct afmf_framegen_pipelines *pipelines_get(struct afmf_device *dev)
{
    if (dev->framegen_pipelines == NULL) {
        VkResult res = pipelines_create(dev);
        if (res != VK_SUCCESS) {
            AFMF_WARN("frame generation pipelines failed (VkResult %d)", (int)res);
            afmf_framegen_pipelines_destroy(dev);
            return NULL;
        }
    }
    return dev->framegen_pipelines;
}

/* ---- per-swapchain state ------------------------------------------------------------------- */

struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

enum set_index {
    SET_PREPARE,
    SET_PYRAMID,
    SET_SCD_HISTOGRAM,
    SET_SCD_DIVERGENCE,
    SET_SEARCH,                  /* + level, 7 */
    SET_FILTER = SET_SEARCH + 7, /* + level, 7 */
    SET_SCALE = SET_FILTER + 7,  /* + level - 1, 6 */
    SET_INTERPOLATE = SET_SCALE + 6,
    SET_DOWNSAMPLE,
    SET_COUNT
};

struct afmf_framegen {
    VkExtent2D extent;
    VkExtent2D of_extent;   /* what the optical flow sees: extent, or half of it in performance mode */
    uint32_t flow_scale;    /* extent / of_extent: 1 or 2 */
    uint32_t levels;        /* pyramid levels the search walks now: 5..max_levels */
    uint32_t max_levels;    /* what the configuration asked for: 5 or 7 (AFMF_LEVELS) */
    struct image color_half; /* of_extent-sized downscale of the new frame; unused when scale is 1 */
    enum half_variant half;
    VkFormat color_format; /* UNORM sibling of the swapchain format: same bytes, no sRGB decode */
    VkFormat out_format;
    enum variant variant;
    uint32_t transfer_function; /* FidelityFX backbuffer transfer function id */
    float min_luminance, max_luminance;

    uint32_t slots;
    uint32_t cb_stride;

    struct image luma[2][AFMF_LEVELS];
    VkExtent2D luma_size[AFMF_LEVELS];
    struct image flow[2][AFMF_LEVELS];
    VkExtent2D flow_size[AFMF_LEVELS];
    struct image flow_out;
    struct image scd_histogram, scd_previous_histogram, scd_temp, scd_output;
    struct image color[2];
    struct image output;
    VkSampler sampler;

    VkBuffer cb;
    VkDeviceMemory cb_memory;
    uint8_t *cb_mapped;

    VkDescriptorPool pool;
    VkDescriptorSet sets[2][SET_COUNT]; /* per frame parity */

    /* Pre-recorded fixed part of a frame, [slot][parity][companion]; NULL records inline. */
    VkCommandPool secondary_pool;
    struct secondary *secondaries;

    bool initialized; /* layouts transitioned and detector cleared in a command buffer */
    uint32_t frame_index;

    /* GPU timestamps per stage (AFMF_PROFILE=1). One query range per slot; a slot's results are
     * read back when the slot is reused, i.e. after its fence, so it never blocks. */
    VkQueryPool queries;
    uint32_t *query_count;  /* per slot: timestamps written last time */
    uint8_t *query_stage;   /* per slot x AFMF_PROFILE_QUERIES: stage each timestamp closes */
    double stage_ns[STAGE_COUNT];
    uint32_t profiled_frames;

    /* Debug readback of generated frames, only when AFMF_DUMP_DIR is set (8-bit variants). */
    VkBuffer dump_buffer;
    VkDeviceMemory dump_memory;
    uint8_t *dump_mapped;
    uint32_t dumps_written;
    bool dump_recorded; /* the command buffer in flight copies into dump_buffer */
};

struct secondary {
    VkCommandBuffer cmd;
    bool recorded;
    uint32_t levels;                      /* search levels it was recorded with; re-recorded on change */
    uint8_t stages[AFMF_PROFILE_QUERIES]; /* profiler stages its timestamps close, replayed per use */
    uint32_t stage_count;
};

/* std140 layout of cbOF_t / cbOF_SPD_t in ffx_opticalflow_callbacks_glsl.h. */
struct cb_of {
    int32_t input_luma_resolution[2];
    uint32_t pyramid_level;
    uint32_t pyramid_level_count;
    uint32_t frame_index;
    uint32_t backbuffer_transfer_function;
    float min_max_luminance[2];
};

struct cb_spd {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t num_work_groups_pyramid;
    uint32_t pad[3];
};

_Static_assert(sizeof(struct cb_of) == AFMF_CB_SIZE, "cbOF layout");
_Static_assert(sizeof(struct cb_spd) == AFMF_CB_SIZE, "cbOF_SPD layout");

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

static VkResult image_create(struct afmf_device *dev, struct image *img, VkFormat format,
                             VkExtent2D extent, VkImageUsageFlags usage)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = dev->fns.create_image(dev->handle, &info, NULL, &img->image);
    if (res != VK_SUCCESS)
        return res;

    VkMemoryRequirements reqs;
    dev->fns.get_image_memory_requirements(dev->handle, img->image, &reqs);
    uint32_t type;
    if (!find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !find_memory_type(dev, reqs.memoryTypeBits, 0, &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    res = dev->fns.allocate_memory(dev->handle, &alloc, NULL, &img->memory);
    if (res != VK_SUCCESS)
        return res;
    res = dev->fns.bind_image_memory(dev->handle, img->image, img->memory, 0);
    if (res != VK_SUCCESS)
        return res;

    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    return dev->fns.create_image_view(dev->handle, &view, NULL, &img->view);
}

static void image_destroy(struct afmf_device *dev, struct image *img)
{
    if (img->view != VK_NULL_HANDLE)
        dev->fns.destroy_image_view(dev->handle, img->view, NULL);
    if (img->image != VK_NULL_HANDLE)
        dev->fns.destroy_image(dev->handle, img->image, NULL);
    if (img->memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, img->memory, NULL);
    memset(img, 0, sizeof *img);
}

/* Swapchain format -> how the colour ring is viewed, what the interpolator writes, and how the
 * luma pass should read the values. False for formats without a variant. */
static bool describe_format(VkFormat swapchain_format, struct afmf_framegen *fg)
{
    switch (swapchain_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        fg->color_format = VK_FORMAT_R8G8B8A8_UNORM;
        fg->out_format = VK_FORMAT_R8G8B8A8_UNORM;
        fg->variant = VARIANT_RGBA8;
        break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        fg->color_format = VK_FORMAT_B8G8R8A8_UNORM;
        fg->out_format = VK_FORMAT_R8G8B8A8_UNORM; /* the shader swaps channels for the raw copy */
        fg->variant = VARIANT_RGBA8_BGRA;
        break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        fg->color_format = swapchain_format;
        fg->out_format = swapchain_format;
        fg->variant = VARIANT_RGB10A2;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        fg->color_format = swapchain_format;
        fg->out_format = swapchain_format;
        fg->variant = VARIANT_RGBA16F;
        break;
    default:
        return false;
    }
    /* Encoded 8/10-bit values are fed as-is (transfer function 0: plain Rec.709 luma), which is
     * perceptually uniform enough for block matching. scRGB half floats go through the SDK's
     * scRGB path with the 80 nit reference white. */
    bool scrgb = fg->variant == VARIANT_RGBA16F;
    fg->transfer_function = scrgb ? 2u : 0u;
    fg->min_luminance = 0.0f;
    fg->max_luminance = scrgb ? 80.0f : 1.0f;
    return true;
}

static bool format_supports(const struct afmf_device *dev, VkFormat format,
                            VkFormatFeatureFlags features)
{
    if (dev->ifns.get_format_properties == NULL)
        return false;
    VkFormatProperties props;
    dev->ifns.get_format_properties(dev->physical_device, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

static VkResult resources_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    const VkImageUsageFlags internal = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkResult res;

    /* Luma pyramid: level k is the flow resolution >> k; flow: 8x8 blocks, then halved. */
    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        fg->luma_size[k].width = fg->of_extent.width >> k ? fg->of_extent.width >> k : 1u;
        fg->luma_size[k].height = fg->of_extent.height >> k ? fg->of_extent.height >> k : 1u;
        if (k == 0) {
            fg->flow_size[0].width = (fg->of_extent.width + AFMF_FLOW_BLOCK - 1) / AFMF_FLOW_BLOCK;
            fg->flow_size[0].height = (fg->of_extent.height + AFMF_FLOW_BLOCK - 1) / AFMF_FLOW_BLOCK;
        } else {
            fg->flow_size[k].width = (fg->flow_size[k - 1].width + 1) / 2;
            fg->flow_size[k].height = (fg->flow_size[k - 1].height + 1) / 2;
        }
        for (uint32_t parity = 0; parity < 2; parity++) {
            res = image_create(dev, &fg->luma[parity][k], VK_FORMAT_R8_UINT, fg->luma_size[k],
                               internal);
            if (res != VK_SUCCESS)
                return res;
            res = image_create(dev, &fg->flow[parity][k], VK_FORMAT_R16G16_SINT, fg->flow_size[k],
                               internal);
            if (res != VK_SUCCESS)
                return res;
        }
    }
    res = image_create(dev, &fg->flow_out, VK_FORMAT_R16G16_SINT, fg->flow_size[0], internal);
    if (res != VK_SUCCESS)
        return res;

    VkExtent2D histogram = {AFMF_HISTOGRAM_BINS * AFMF_HISTOGRAMS_PER_DIM * AFMF_HISTOGRAMS_PER_DIM, 1};
    VkExtent2D scd = {AFMF_SCD_SLOTS, 1};
    res = image_create(dev, &fg->scd_histogram, VK_FORMAT_R32_UINT, histogram, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_previous_histogram, VK_FORMAT_R32_SFLOAT, histogram, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_temp, VK_FORMAT_R32_UINT, scd, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_output, VK_FORMAT_R32_UINT, scd, internal);
    if (res != VK_SUCCESS)
        return res;

    const VkImageUsageFlags colour = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    for (uint32_t parity = 0; parity < 2; parity++) {
        res = image_create(dev, &fg->color[parity], fg->color_format, fg->extent, colour);
        if (res != VK_SUCCESS)
            return res;
    }
    if (fg->flow_scale > 1) {
        res = image_create(dev, &fg->color_half, downsample_variants[fg->half].format, fg->of_extent,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (res != VK_SUCCESS)
            return res;
    }
    res = image_create(dev, &fg->output, fg->out_format, fg->extent,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (res != VK_SUCCESS)
        return res;

    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    res = dev->fns.create_sampler(dev->handle, &sampler, NULL, &fg->sampler);
    if (res != VK_SUCCESS)
        return res;

    /* Constants: one host-coherent buffer, [slot][level 0..6, spd] regions at the UBO alignment. */
    uint32_t alignment = (uint32_t)dev->limits.minUniformBufferOffsetAlignment;
    fg->cb_stride = align_up(AFMF_CB_SIZE, alignment != 0 ? alignment : 1u);
    VkDeviceSize cb_size = (VkDeviceSize)fg->cb_stride * AFMF_CB_PER_SLOT * fg->slots;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cb_size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    res = dev->fns.create_buffer(dev->handle, &buffer, NULL, &fg->cb);
    if (res != VK_SUCCESS)
        return res;
    VkMemoryRequirements reqs;
    dev->fns.get_buffer_memory_requirements(dev->handle, fg->cb, &reqs);
    uint32_t type;
    if (!find_memory_type(dev, reqs.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    res = dev->fns.allocate_memory(dev->handle, &alloc, NULL, &fg->cb_memory);
    if (res == VK_SUCCESS)
        res = dev->fns.bind_buffer_memory(dev->handle, fg->cb, fg->cb_memory, 0);
    if (res == VK_SUCCESS) {
        void *mapped = NULL;
        res = dev->fns.map_memory(dev->handle, fg->cb_memory, 0, cb_size, 0, &mapped);
        fg->cb_mapped = mapped;
    }
    return res;
}

/* Host-visible readback buffer for AFMF_DUMP_DIR; a failure here only disables the dumps. */
static void dump_buffer_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    bool eight_bit = fg->variant == VARIANT_RGBA8 || fg->variant == VARIANT_RGBA8_BGRA;
    if (afmf_config_get()->dump_dir == NULL || !eight_bit)
        return;

    VkDeviceSize size = (VkDeviceSize)fg->extent.width * fg->extent.height * 4u;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (dev->fns.create_buffer(dev->handle, &buffer, NULL, &fg->dump_buffer) != VK_SUCCESS)
        return;
    VkMemoryRequirements reqs;
    dev->fns.get_buffer_memory_requirements(dev->handle, fg->dump_buffer, &reqs);
    uint32_t type;
    if (!find_memory_type(dev, reqs.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &type))
        return;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    void *mapped = NULL;
    if (dev->fns.allocate_memory(dev->handle, &alloc, NULL, &fg->dump_memory) != VK_SUCCESS ||
        dev->fns.bind_buffer_memory(dev->handle, fg->dump_buffer, fg->dump_memory, 0) != VK_SUCCESS ||
        dev->fns.map_memory(dev->handle, fg->dump_memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return;
    fg->dump_mapped = mapped;
}

/* ---- profiling ----------------------------------------------------------------------------- */

static void profiler_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (!afmf_config_get()->profile)
        return;
    if (!dev->limits.timestampComputeAndGraphics || dev->limits.timestampPeriod <= 0.0f) {
        AFMF_WARN("profiling requested but the device has no timestamps on compute queues");
        return;
    }
    VkQueryPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = AFMF_PROFILE_QUERIES * fg->slots,
    };
    fg->query_count = calloc(fg->slots, sizeof *fg->query_count);
    fg->query_stage = calloc((size_t)fg->slots * AFMF_PROFILE_QUERIES, sizeof *fg->query_stage);
    if (fg->query_count == NULL || fg->query_stage == NULL ||
        dev->fns.create_query_pool(dev->handle, &pool, NULL, &fg->queries) != VK_SUCCESS) {
        AFMF_WARN("profiling unavailable: query pool creation failed");
        fg->queries = VK_NULL_HANDLE;
    }
}

/* Reads the timestamps a slot wrote the last time it ran and folds them into the stage totals.
 * Called once the slot's fence has been waited on, so the results are complete. */
static void profiler_collect(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t slot)
{
    uint32_t count = fg->query_count[slot];
    fg->query_count[slot] = 0; /* consumed: never folded in twice */
    if (count < 2)
        return;
    uint64_t ticks[AFMF_PROFILE_QUERIES];
    VkResult res = dev->fns.get_query_pool_results(dev->handle, fg->queries,
                                                   slot * AFMF_PROFILE_QUERIES, count, sizeof ticks,
                                                   ticks, sizeof ticks[0], VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS)
        return;
    const uint8_t *stages = fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES;
    for (uint32_t i = 1; i < count; i++)
        fg->stage_ns[stages[i]] += (double)(ticks[i] - ticks[i - 1]) * (double)dev->limits.timestampPeriod;
    fg->profiled_frames++;
}

static void profiler_report(struct afmf_framegen *fg)
{
    if (fg->profiled_frames == 0)
        return;
    double total = 0.0;
    for (uint32_t s = 0; s < STAGE_COUNT; s++)
        total += fg->stage_ns[s];
    AFMF_INFO("GPU time per frame over %u frames at %ux%u: %.0f us total", fg->profiled_frames,
              fg->extent.width, fg->extent.height, total / fg->profiled_frames / 1e3);
    for (uint32_t s = 0; s < STAGE_COUNT; s++) {
        AFMF_INFO("  %-24s %7.0f us  %5.1f%%", stage_names[s],
                  fg->stage_ns[s] / fg->profiled_frames / 1e3,
                  total > 0.0 ? 100.0 * fg->stage_ns[s] / total : 0.0);
        fg->stage_ns[s] = 0.0;
    }
    fg->profiled_frames = 0;
}

/* Writes the timestamp that closes `stage`; a no-op without profiling. */
static void profiler_mark(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, enum stage stage)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    uint32_t *count = &fg->query_count[slot];
    if (*count >= AFMF_PROFILE_QUERIES)
        return;
    fg->query_stage[(size_t)slot * AFMF_PROFILE_QUERIES + *count] = (uint8_t)stage;
    dev->fns.cmd_write_timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg->queries,
                                 slot * AFMF_PROFILE_QUERIES + *count);
    (*count)++;
}

static void profiler_begin(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                           uint32_t slot)
{
    if (fg->queries == VK_NULL_HANDLE)
        return;
    profiler_collect(dev, fg, slot);
    if (fg->profiled_frames >= AFMF_PROFILE_INTERVAL)
        profiler_report(fg);
    dev->fns.cmd_reset_query_pool(cmd, fg->queries, slot * AFMF_PROFILE_QUERIES,
                                  AFMF_PROFILE_QUERIES);
    fg->query_count[slot] = 0;
    profiler_mark(dev, fg, cmd, slot, STAGE_INGEST); /* the opening timestamp; stage unused */
}

bool afmf_framegen_dump_pending(const struct afmf_framegen *fg)
{
    return fg->dump_mapped != NULL && fg->dumps_written < AFMF_DUMP_FRAMES;
}

void afmf_framegen_dump_write(struct afmf_device *dev, struct afmf_framegen *fg)
{
    (void)dev;
    if (!fg->dump_recorded)
        return;
    fg->dump_recorded = false;
    fg->dumps_written++;

    char path[512];
    int n = snprintf(path, sizeof path, "%s/afmf_generated_%u.ppm", afmf_config_get()->dump_dir,
                     fg->dumps_written);
    if (n < 0 || (size_t)n >= sizeof path)
        return;
    FILE *out = fopen(path, "wb");
    if (out == NULL) {
        AFMF_WARN("cannot write %s", path);
        return;
    }
    uint32_t w = fg->extent.width, h = fg->extent.height;
    (void)fprintf(out, "P6\n%u %u\n255\n", w, h);
    /* The buffer holds what the swapchain sees: RGBA, or BGRA when the shader swapped for a
     * B8G8R8A8 target, so undo the swap here. */
    bool bgra = fg->variant == VARIANT_RGBA8_BGRA;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t *px = fg->dump_mapped + i * 4;
        uint8_t rgb[3] = {px[bgra ? 2 : 0], px[1], px[bgra ? 0 : 2]};
        (void)fwrite(rgb, 1, sizeof rgb, out);
    }
    if (fclose(out) != 0)
        AFMF_WARN("error writing %s", path);
    else
        AFMF_INFO("generated frame written to %s", path);
}

/* ---- descriptor sets ----------------------------------------------------------------------- */

struct set_writer {
    struct afmf_device *dev;
    VkDescriptorSet set;
    VkWriteDescriptorSet writes[16];
    VkDescriptorImageInfo images[16];
    VkDescriptorBufferInfo buffers[16];
    uint32_t count;
};

static void writer_begin(struct set_writer *w, struct afmf_device *dev, VkDescriptorSet set)
{
    memset(w, 0, sizeof *w);
    w->dev = dev;
    w->set = set;
}

static void writer_image(struct set_writer *w, uint32_t binding, VkDescriptorType type,
                         VkImageView view, VkSampler sampler)
{
    uint32_t i = w->count++;
    w->images[i] = (VkDescriptorImageInfo){
        .sampler = sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    w->writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = w->set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = &w->images[i],
    };
}

static void writer_ubo(struct set_writer *w, uint32_t binding, VkBuffer buffer)
{
    uint32_t i = w->count++;
    w->buffers[i] = (VkDescriptorBufferInfo){.buffer = buffer, .offset = 0, .range = AFMF_CB_SIZE};
    w->writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = w->set,
        .dstBinding = binding,
        .descriptorCount = 1,
        .descriptorType = UBO,
        .pBufferInfo = &w->buffers[i],
    };
}

static void writer_end(struct set_writer *w)
{
    w->dev->fns.update_descriptor_sets(w->dev->handle, w->count, w->writes, 0, NULL);
}

/* Which of the two flow textures the search at `level` writes, for a frame of parity `p`: the
 * SDK alternates per frame and per level so that each level's output is the next level's input. */
static uint32_t flow_parity_a(uint32_t p, uint32_t level)
{
    return (p != (level & 1u)) ? 1u : 0u;
}

static VkResult descriptor_sets_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;

    VkDescriptorPoolSize sizes[] = {
        {SAMPLED, 2 * 42}, {STORAGE, 2 * 49}, {COMBINED, 2 * 3}, {UBO, 2 * 25},
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2 * SET_COUNT,
        .poolSizeCount = (uint32_t)(sizeof sizes / sizeof sizes[0]),
        .pPoolSizes = sizes,
    };
    VkResult res = dev->fns.create_descriptor_pool(dev->handle, &pool, NULL, &fg->pool);
    if (res != VK_SUCCESS)
        return res;

    VkDescriptorSetLayout layouts[SET_COUNT];
    layouts[SET_PREPARE] = pl->set_layouts[PASS_PREPARE_LUMA];
    layouts[SET_PYRAMID] = pl->set_layouts[PASS_PYRAMID];
    layouts[SET_SCD_HISTOGRAM] = pl->set_layouts[PASS_SCD_HISTOGRAM];
    layouts[SET_SCD_DIVERGENCE] = pl->set_layouts[PASS_SCD_DIVERGENCE];
    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        layouts[SET_SEARCH + k] = pl->set_layouts[PASS_SEARCH];
        layouts[SET_FILTER + k] = pl->set_layouts[PASS_FILTER];
    }
    for (uint32_t k = 1; k < AFMF_LEVELS; k++)
        layouts[SET_SCALE + k - 1] = pl->set_layouts[PASS_SCALE];
    layouts[SET_INTERPOLATE] = pl->set_layouts[PASS_INTERPOLATE];
    layouts[SET_DOWNSAMPLE] = pl->set_layouts[PASS_DOWNSAMPLE];

    for (uint32_t p = 0; p < 2; p++) {
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = fg->pool,
            .descriptorSetCount = SET_COUNT,
            .pSetLayouts = layouts,
        };
        res = dev->fns.allocate_descriptor_sets(dev->handle, &alloc, fg->sets[p]);
        if (res != VK_SUCCESS)
            return res;
    }

    for (uint32_t p = 0; p < 2; p++) {
        uint32_t q = 1u - p; /* the other parity: previous frame's luma, previous colour */
        struct set_writer w;

        writer_begin(&w, dev, fg->sets[p][SET_PREPARE]);
        writer_image(&w, 0, SAMPLED, fg->flow_scale > 1 ? fg->color_half.view : fg->color[p].view,
                     VK_NULL_HANDLE);
        writer_image(&w, 1, STORAGE, fg->luma[p][0].view, VK_NULL_HANDLE);
        writer_ubo(&w, 2, fg->cb);
        writer_end(&w);

        writer_begin(&w, dev, fg->sets[p][SET_PYRAMID]);
        for (uint32_t k = 0; k < AFMF_LEVELS; k++)
            writer_image(&w, k, STORAGE, fg->luma[p][k].view, VK_NULL_HANDLE);
        writer_ubo(&w, 7, fg->cb);
        writer_ubo(&w, 8, fg->cb);
        writer_end(&w);

        writer_begin(&w, dev, fg->sets[p][SET_SCD_HISTOGRAM]);
        writer_image(&w, 0, SAMPLED, fg->luma[p][0].view, VK_NULL_HANDLE);
        writer_image(&w, 1, STORAGE, fg->scd_histogram.view, VK_NULL_HANDLE);
        writer_ubo(&w, 2, fg->cb);
        writer_end(&w);

        writer_begin(&w, dev, fg->sets[p][SET_SCD_DIVERGENCE]);
        writer_image(&w, 0, STORAGE, fg->scd_histogram.view, VK_NULL_HANDLE);
        writer_image(&w, 1, STORAGE, fg->scd_previous_histogram.view, VK_NULL_HANDLE);
        writer_image(&w, 2, STORAGE, fg->scd_temp.view, VK_NULL_HANDLE);
        writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
        writer_ubo(&w, 4, fg->cb);
        writer_end(&w);

        for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
            uint32_t fa = flow_parity_a(p, k);
            uint32_t fb = 1u - fa;

            writer_begin(&w, dev, fg->sets[p][SET_SEARCH + k]);
            writer_image(&w, 0, SAMPLED, fg->luma[p][k].view, VK_NULL_HANDLE);
            writer_image(&w, 1, SAMPLED, fg->luma[q][k].view, VK_NULL_HANDLE);
            writer_image(&w, 2, STORAGE, fg->flow[fa][k].view, VK_NULL_HANDLE);
            writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
            writer_ubo(&w, 4, fg->cb);
            writer_end(&w);

            writer_begin(&w, dev, fg->sets[p][SET_FILTER + k]);
            writer_image(&w, 0, SAMPLED, fg->flow[fa][k].view, VK_NULL_HANDLE);
            writer_image(&w, 1, STORAGE, k == 0 ? fg->flow_out.view : fg->flow[fb][k].view,
                         VK_NULL_HANDLE);
            writer_ubo(&w, 2, fg->cb);
            writer_end(&w);

            if (k == 0)
                continue;
            writer_begin(&w, dev, fg->sets[p][SET_SCALE + k - 1]);
            writer_image(&w, 0, SAMPLED, fg->luma[p][k].view, VK_NULL_HANDLE);
            writer_image(&w, 1, SAMPLED, fg->luma[q][k].view, VK_NULL_HANDLE);
            writer_image(&w, 2, SAMPLED, fg->flow[fb][k].view, VK_NULL_HANDLE);
            writer_image(&w, 3, STORAGE, fg->flow[fb][k - 1].view, VK_NULL_HANDLE);
            writer_image(&w, 4, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
            writer_ubo(&w, 5, fg->cb);
            writer_end(&w);
        }

        writer_begin(&w, dev, fg->sets[p][SET_INTERPOLATE]);
        writer_image(&w, 0, COMBINED, fg->color[q].view, fg->sampler);
        writer_image(&w, 1, COMBINED, fg->color[p].view, fg->sampler);
        writer_image(&w, 2, SAMPLED, fg->flow_out.view, VK_NULL_HANDLE);
        writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
        writer_image(&w, 4, STORAGE, fg->output.view, VK_NULL_HANDLE);
        writer_end(&w);

        if (fg->flow_scale > 1) {
            writer_begin(&w, dev, fg->sets[p][SET_DOWNSAMPLE]);
            writer_image(&w, 0, COMBINED, fg->color[p].view, fg->sampler);
            writer_image(&w, 1, STORAGE, fg->color_half.view, VK_NULL_HANDLE);
            writer_end(&w);
        }
    }
    return VK_SUCCESS;
}

/* ---- create / destroy ---------------------------------------------------------------------- */

void afmf_framegen_set_family(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t family)
{
    if (fg->secondary_pool != VK_NULL_HANDLE || dev->set_loader_data == NULL)
        return;
    uint32_t count = fg->slots * 4u;
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->fns.create_command_pool(dev->handle, &pool, NULL, &fg->secondary_pool) != VK_SUCCESS)
        return;
    VkCommandBuffer *cmds = calloc(count, sizeof *cmds);
    fg->secondaries = calloc(count, sizeof *fg->secondaries);
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = fg->secondary_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
        .commandBufferCount = count,
    };
    bool ok = cmds != NULL && fg->secondaries != NULL &&
              dev->fns.allocate_command_buffers(dev->handle, &alloc, cmds) == VK_SUCCESS;
    /* Allocated below the loader's trampoline: stamp the dispatch pointer ourselves. */
    for (uint32_t i = 0; ok && i < count; i++)
        ok = dev->set_loader_data(dev->handle, cmds[i]) == VK_SUCCESS;
    if (ok) {
        for (uint32_t i = 0; i < count; i++)
            fg->secondaries[i].cmd = cmds[i];
    } else {
        AFMF_WARN("pre-recorded command buffers unavailable; recording every frame");
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL);
        fg->secondary_pool = VK_NULL_HANDLE;
        free(fg->secondaries);
        fg->secondaries = NULL;
    }
    free(cmds);
}

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (fg == NULL)
        return;
    if (fg->secondary_pool != VK_NULL_HANDLE)
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL); /* frees the buffers */
    free(fg->secondaries);
    if (fg->pool != VK_NULL_HANDLE)
        dev->fns.destroy_descriptor_pool(dev->handle, fg->pool, NULL); /* frees the sets */
    if (fg->queries != VK_NULL_HANDLE) {
        /* The caller has waited for every slot; fold the last results in and report. */
        for (uint32_t s = 0; s < fg->slots; s++)
            profiler_collect(dev, fg, s);
        profiler_report(fg);
        dev->fns.destroy_query_pool(dev->handle, fg->queries, NULL);
    }
    free(fg->query_count);
    free(fg->query_stage);
    if (fg->dump_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->dump_memory);
    if (fg->dump_buffer != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->dump_buffer, NULL);
    if (fg->dump_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->dump_memory, NULL);
    if (fg->cb_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->cb_memory);
    if (fg->cb != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->cb, NULL);
    if (fg->cb_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->cb_memory, NULL);
    if (fg->sampler != VK_NULL_HANDLE)
        dev->fns.destroy_sampler(dev->handle, fg->sampler, NULL);
    image_destroy(dev, &fg->output);
    image_destroy(dev, &fg->color_half);
    for (uint32_t p = 0; p < 2; p++) {
        image_destroy(dev, &fg->color[p]);
        for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
            image_destroy(dev, &fg->luma[p][k]);
            image_destroy(dev, &fg->flow[p][k]);
        }
    }
    image_destroy(dev, &fg->flow_out);
    image_destroy(dev, &fg->scd_histogram);
    image_destroy(dev, &fg->scd_previous_histogram);
    image_destroy(dev, &fg->scd_temp);
    image_destroy(dev, &fg->scd_output);
    free(fg);
}

struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots)
{
    struct afmf_framegen *fg = calloc(1, sizeof *fg);
    if (fg == NULL)
        return NULL;
    fg->extent = extent;
    fg->slots = slots;

    /* Performance mode: the block search, 85 % of the cost, runs on a half-size frame; blocks
     * become 16 pixels on screen. Auto picks it from 1440p up, where the search dominates. */
    const struct afmf_config *cfg = afmf_config_get();
    bool half = cfg->performance == AFMF_PERFORMANCE_FAST ||
                (cfg->performance == AFMF_PERFORMANCE_AUTO &&
                 (uint64_t)extent.width * extent.height >= 2560u * 1440u);
    if (extent.width / 2 < AFMF_MIN_EXTENT || extent.height / 2 < AFMF_MIN_EXTENT)
        half = false; /* too small for the pyramid at half size: full resolution instead */
    fg->flow_scale = half ? 2u : 1u;
    fg->of_extent.width = (extent.width + fg->flow_scale - 1) / fg->flow_scale;
    fg->of_extent.height = (extent.height + fg->flow_scale - 1) / fg->flow_scale;
    fg->half = swapchain_format == VK_FORMAT_R16G16B16A16_SFLOAT ? HALF_RGBA16F : HALF_RGBA8;
    /* At reduced flow resolution five levels already cover +-128 flow pixels (+-256 on screen): the
     * two coarsest levels are drains, not accuracy. `high` still forces all seven. */
    fg->levels = cfg->flow_levels;
    if (fg->flow_scale > 1 && cfg->search_mode != AFMF_SEARCH_HIGH && fg->levels > 5)
        fg->levels = 5;
    if (fg->levels > AFMF_LEVELS)
        fg->levels = AFMF_LEVELS;
    fg->max_levels = fg->levels;

    const char *blocker = NULL;
    if (dev->api_version < VK_API_VERSION_1_1)
        blocker = "application uses Vulkan 1.0; the flow shaders need 1.1 subgroups";
    else if (!describe_format(swapchain_format, fg))
        blocker = "swapchain format has no interpolation variant";
    else if (extent.width < AFMF_MIN_EXTENT || extent.height < AFMF_MIN_EXTENT)
        blocker = "swapchain too small for the optical flow pyramid";
    else if (!format_supports(dev, fg->out_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
             !format_supports(dev, fg->color_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        blocker = "device lacks storage or filtered sampling for the swapchain format";
    else if (half && !format_supports(dev, downsample_variants[fg->half].format,
                                      VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        blocker = "device lacks storage for the half-resolution colour";
    else if (!format_supports(dev, VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
        blocker = "device lacks r32ui image atomics";
    else if (pipelines_get(dev) == NULL)
        blocker = "pipelines unavailable";

    VkResult res = VK_SUCCESS;
    if (blocker == NULL) {
        res = resources_create(dev, fg);
        if (res == VK_SUCCESS)
            res = descriptor_sets_create(dev, fg);
        if (res != VK_SUCCESS)
            blocker = "resource creation failed";
        else {
            dump_buffer_create(dev, fg);
            profiler_create(dev, fg);
        }
    }
    if (blocker != NULL) {
        AFMF_WARN("interpolation unavailable for this swapchain: %s (VkResult %d); repeating frames",
                  blocker, (int)res);
        afmf_framegen_destroy(dev, fg);
        return NULL;
    }
    AFMF_INFO("interpolation ready: %ux%u, flow at %ux%u (%ux%u blocks of %u px), %u pyramid levels",
              extent.width, extent.height, fg->of_extent.width, fg->of_extent.height,
              fg->flow_size[0].width, fg->flow_size[0].height, AFMF_FLOW_BLOCK * fg->flow_scale,
              fg->levels);
    return fg;
}

/* ---- recording ----------------------------------------------------------------------------- */

/* Everything before this point is visible to everything after it, for compute and transfer. */
static void sync(struct afmf_device *dev, VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dev->fns.cmd_pipeline_barrier(cmd, stages, stages, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Between two compute passes only: no transfer scopes, so the driver need not flush the caches a
 * copy engine would read. */
static void sync_compute(struct afmf_device *dev, VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    dev->fns.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0,
                                  NULL);
}

static void to_general(struct afmf_device *dev, VkCommandBuffer cmd, VkImage image)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    dev->fns.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, NULL, 0, NULL, 1, &barrier);
}

static void clear_zero(struct afmf_device *dev, VkCommandBuffer cmd, VkImage image)
{
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dev->fns.cmd_clear_color_image(cmd, image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
}

/* First use: every internal image to GENERAL, and the detector state cleared as the SDK does on
 * reset (a fresh luma history reads as black, which the first search treats as no motion). */
static void initialize(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd)
{
    if (fg->color_half.image != VK_NULL_HANDLE)
        to_general(dev, cmd, fg->color_half.image);
    for (uint32_t p = 0; p < 2; p++) {
        to_general(dev, cmd, fg->color[p].image);
        for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
            to_general(dev, cmd, fg->luma[p][k].image);
            to_general(dev, cmd, fg->flow[p][k].image);
            clear_zero(dev, cmd, fg->luma[p][k].image);
        }
    }
    to_general(dev, cmd, fg->flow_out.image);
    to_general(dev, cmd, fg->output.image);
    to_general(dev, cmd, fg->scd_histogram.image);
    to_general(dev, cmd, fg->scd_previous_histogram.image);
    to_general(dev, cmd, fg->scd_temp.image);
    to_general(dev, cmd, fg->scd_output.image);
    clear_zero(dev, cmd, fg->scd_histogram.image);
    clear_zero(dev, cmd, fg->scd_previous_histogram.image);
    clear_zero(dev, cmd, fg->scd_temp.image);
    clear_zero(dev, cmd, fg->scd_output.image);
    sync(dev, cmd);
    fg->initialized = true;
    fg->frame_index = 0;
}

static uint32_t cb_offset(const struct afmf_framegen *fg, uint32_t slot, uint32_t index)
{
    return (slot * AFMF_CB_PER_SLOT + index) * fg->cb_stride;
}

static void write_constants(struct afmf_framegen *fg, uint32_t slot, uint32_t level_count)
{
    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        struct cb_of of = {
            .input_luma_resolution = {(int32_t)fg->of_extent.width, (int32_t)fg->of_extent.height},
            .pyramid_level = k,
            .pyramid_level_count = level_count,
            .frame_index = fg->frame_index,
            .backbuffer_transfer_function = fg->transfer_function,
            .min_max_luminance = {fg->min_luminance, fg->max_luminance},
        };
        memcpy(fg->cb_mapped + cb_offset(fg, slot, k), &of, sizeof of);
    }

    /* ffxSpdSetup for the whole flow surface, 64x64 tiles, 6 mips generated by the pass. */
    uint32_t tiles_x = (fg->of_extent.width - 1) / 64 + 1;
    uint32_t tiles_y = (fg->of_extent.height - 1) / 64 + 1;
    struct cb_spd spd = {
        .mips = 6,
        .num_work_groups = tiles_x * tiles_y,
        .work_group_offset = {0, 0},
        .num_work_groups_pyramid = tiles_x * tiles_y,
    };
    memcpy(fg->cb_mapped + cb_offset(fg, slot, AFMF_LEVELS), &spd, sizeof spd);
}

/* Records one compute pass. `barrier` false lets the next pass start without waiting: only for
 * passes that neither read nor overwrite each other's outputs. */
static void dispatch_pass(struct afmf_device *dev, VkCommandBuffer cmd, VkPipeline pipeline,
                          VkPipelineLayout layout, VkDescriptorSet set, const uint32_t *offsets,
                          uint32_t offset_count, uint32_t x, uint32_t y, uint32_t z, bool barrier)
{
    dev->fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    dev->fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set,
                                      offset_count, offsets);
    dev->fns.cmd_dispatch(cmd, x, y, z);
    if (barrier)
        sync_compute(dev, cmd);
}

static void dispatch(struct afmf_device *dev, VkCommandBuffer cmd, VkPipeline pipeline,
                     VkPipelineLayout layout, VkDescriptorSet set, const uint32_t *offsets,
                     uint32_t offset_count, uint32_t x, uint32_t y, uint32_t z)
{
    dispatch_pass(dev, cmd, pipeline, layout, set, offsets, offset_count, x, y, z, true);
}

static void copy_whole(struct afmf_device *dev, VkCommandBuffer cmd, VkImage src,
                       VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
                       VkExtent2D extent)
{
    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    dev->fns.cmd_copy_image(cmd, src, src_layout, dst, dst_layout, 1, &region);
}

void afmf_framegen_set_levels(struct afmf_framegen *fg, uint32_t levels)
{
    fg->levels = levels < 5u ? 5u : levels > fg->max_levels ? fg->max_levels : levels;
}

uint32_t afmf_framegen_max_levels(const struct afmf_framegen *fg)
{
    return fg->max_levels;
}

/* The fixed part of a frame: luma, pyramid, scene change detector, then, with a companion, the
 * coarse-to-fine search and the interpolated frame into fg->output. Depends only on the slot
 * (constants region), the parity (descriptor sets), `companion` and `levels`. */
static void record_flow(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                        uint32_t slot, uint32_t p, uint32_t levels, bool companion)
{
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;
    uint32_t w = fg->of_extent.width, h = fg->of_extent.height; /* optical flow dimensions */
    uint32_t of0 = cb_offset(fg, slot, 0);
    dispatch(dev, cmd, pl->pipelines[PASS_PREPARE_LUMA], pl->layouts[PASS_PREPARE_LUMA],
             fg->sets[p][SET_PREPARE], &of0, 1, ((w + 1) / 2 + 15) / 16, ((h + 1) / 2 + 15) / 16, 1);
    profiler_mark(dev, fg, cmd, slot, STAGE_PREPARE);

    /* The pyramid (reads luma 0, writes levels 1-6) and the detector's histogram (reads luma 0)
     * are independent: no barrier between them, one after both. */
    uint32_t pyramid_offsets[2] = {of0, cb_offset(fg, slot, AFMF_LEVELS)};
    dispatch_pass(dev, cmd, pl->pipelines[PASS_PYRAMID], pl->layouts[PASS_PYRAMID],
                  fg->sets[p][SET_PYRAMID], pyramid_offsets, 2, (w - 1) / 64 + 1, (h - 1) / 64 + 1,
                  1, false);
    profiler_mark(dev, fg, cmd, slot, STAGE_PYRAMID);

    uint32_t strata_width = (w / 4) / AFMF_HISTOGRAMS_PER_DIM;
    dispatch(dev, cmd, pl->pipelines[PASS_SCD_HISTOGRAM], pl->layouts[PASS_SCD_HISTOGRAM],
             fg->sets[p][SET_SCD_HISTOGRAM], &of0, 1, (strata_width + 31) / 32, 16,
             AFMF_HISTOGRAMS_PER_DIM * AFMF_HISTOGRAMS_PER_DIM);
    dispatch(dev, cmd, pl->pipelines[PASS_SCD_DIVERGENCE], pl->layouts[PASS_SCD_DIVERGENCE],
             fg->sets[p][SET_SCD_DIVERGENCE], &of0, 1,
             AFMF_HISTOGRAMS_PER_DIM * AFMF_HISTOGRAMS_PER_DIM, AFMF_HISTOGRAM_SHIFTS, 1);
    profiler_mark(dev, fg, cmd, slot, STAGE_SCD);

    for (uint32_t k = companion ? levels : 0; k-- > 0;) {
        uint32_t ofk = cb_offset(fg, slot, k);
        uint32_t luma_w = fg->luma_size[k].width, luma_h = fg->luma_size[k].height;
        dispatch(dev, cmd, pl->pipelines[PASS_SEARCH], pl->layouts[PASS_SEARCH],
                 fg->sets[p][SET_SEARCH + k], &ofk, 1, ((luma_w + 3) / 4 * 16 + 63) / 64,
                 (luma_h + 15) / 16, 1);
        profiler_mark(dev, fg, cmd, slot, STAGE_SEARCH);
        dispatch(dev, cmd, pl->pipelines[PASS_FILTER], pl->layouts[PASS_FILTER],
                 fg->sets[p][SET_FILTER + k], &ofk, 1, (fg->flow_size[k].width + 15) / 16,
                 (fg->flow_size[k].height + 3) / 4, 1);
        profiler_mark(dev, fg, cmd, slot, STAGE_FILTER);
        if (k > 0) {
            dispatch(dev, cmd, pl->pipelines[PASS_SCALE], pl->layouts[PASS_SCALE],
                     fg->sets[p][SET_SCALE + k - 1], &ofk, 1, (fg->flow_size[k - 1].width + 3) / 4,
                     (fg->flow_size[k - 1].height + 3) / 4, 1);
            profiler_mark(dev, fg, cmd, slot, STAGE_SCALE);
        }
    }

    /* The frame in between, into fg->output. */
    if (companion) {
        uint32_t out_w = fg->extent.width, out_h = fg->extent.height;
        struct interpolate_push push = {
            .size = {(int32_t)out_w, (int32_t)out_h},
            .block = (int32_t)AFMF_FLOW_BLOCK,
            .fallback = afmf_config_get()->fast_motion == AFMF_RESPONSE_BLENDED_FRAMES ? 1 : 0,
            .max_motion = AFMF_MAX_TRUSTED_MOTION,
            .flow_scale = (float)fg->flow_scale,
        };
        dev->fns.cmd_push_constants(cmd, pl->layouts[PASS_INTERPOLATE], VK_SHADER_STAGE_COMPUTE_BIT,
                                    0, (uint32_t)sizeof push, &push);
        dispatch(dev, cmd, pl->interpolate[fg->variant], pl->layouts[PASS_INTERPOLATE],
                 fg->sets[p][SET_INTERPOLATE], NULL, 0, (out_w + 7) / 8, (out_h + 7) / 8, 1);
        profiler_mark(dev, fg, cmd, slot, STAGE_INTERPOLATE);
    }
}

/* Executes the pre-recorded fixed part for (slot, p, companion), recording it first when it has
 * never run or the search levels changed since; the slot's fence has been waited on by the
 * caller, so nothing of this slot is in flight. Records inline when there are no secondaries. */
static void execute_flow(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                         uint32_t slot, uint32_t p, uint32_t levels, bool companion)
{
    struct secondary *sec =
        fg->secondaries != NULL ? &fg->secondaries[(slot * 2u + p) * 2u + (companion ? 1u : 0u)] : NULL;
    if (sec == NULL) {
        record_flow(dev, fg, cmd, slot, p, levels, companion);
        return;
    }
    bool profiling = fg->queries != VK_NULL_HANDLE;
    uint32_t before = profiling ? fg->query_count[slot] : 0;
    if (!sec->recorded || sec->levels != levels) {
        /* The marks it writes land at fixed query indices: the primary always writes the same
         * two before it (opening, ingest), so a replay only needs the stage list. */
        VkCommandBufferInheritanceInfo inherit = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pInheritanceInfo = &inherit};
        sec->recorded = false;
        if (dev->fns.begin_command_buffer(sec->cmd, &begin) == VK_SUCCESS) {
            record_flow(dev, fg, sec->cmd, slot, p, levels, companion);
            sec->recorded = dev->fns.end_command_buffer(sec->cmd) == VK_SUCCESS;
        }
        if (!sec->recorded) {
            if (profiling)
                fg->query_count[slot] = before;
            record_flow(dev, fg, cmd, slot, p, levels, companion);
            return;
        }
        sec->levels = levels;
        sec->stage_count = profiling ? fg->query_count[slot] - before : 0;
        if (sec->stage_count > 0)
            memcpy(sec->stages, fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES + before,
                   sec->stage_count);
    } else if (profiling && before + sec->stage_count <= AFMF_PROFILE_QUERIES) {
        memcpy(fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES + before, sec->stages,
               sec->stage_count);
        fg->query_count[slot] = before + sec->stage_count;
    }
    dev->fns.cmd_execute_commands(cmd, 1, &sec->cmd);
}

void afmf_framegen_record(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, VkImage target)
{
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;
    uint32_t levels = fg->levels;
    uint32_t p = fg->frame_index & 1u;
    uint32_t w = fg->of_extent.width, h = fg->of_extent.height; /* optical flow dimensions */
    bool companion = target != VK_NULL_HANDLE;

    if (!fg->initialized)
        initialize(dev, fg, cmd);
    write_constants(fg, slot, levels);

    /* The previous frame's work on this queue may still be reading the ring image about to be
     * overwritten: order it before anything below. */
    sync(dev, cmd);
    profiler_begin(dev, fg, cmd, slot);

    /* 1. The new frame into the colour ring, and downscaled for the flow when in performance
     *    mode. */
    copy_whole(dev, cmd, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fg->color[p].image,
               VK_IMAGE_LAYOUT_GENERAL, fg->extent);
    sync(dev, cmd);
    if (fg->flow_scale > 1) {
        struct downsample_push push = {.size = {(int32_t)w, (int32_t)h}};
        dev->fns.cmd_push_constants(cmd, pl->layouts[PASS_DOWNSAMPLE], VK_SHADER_STAGE_COMPUTE_BIT,
                                    0, (uint32_t)sizeof push, &push);
        dispatch(dev, cmd, pl->downsample[fg->half], pl->layouts[PASS_DOWNSAMPLE],
                 fg->sets[p][SET_DOWNSAMPLE], NULL, 0, (w + 7) / 8, (h + 7) / 8, 1);
    }
    profiler_mark(dev, fg, cmd, slot, STAGE_INGEST);

    /* 2. Optical flow and interpolation: the pre-recorded part. */
    execute_flow(dev, fg, cmd, slot, p, levels, companion);

    /* 3. The frame in between, into the target. */
    if (companion) {
        uint32_t out_w = fg->extent.width, out_h = fg->extent.height;
        copy_whole(dev, cmd, fg->output.image, VK_IMAGE_LAYOUT_GENERAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fg->extent);
        profiler_mark(dev, fg, cmd, slot, STAGE_OUTPUT);

        if (afmf_framegen_dump_pending(fg)) {
            VkBufferImageCopy region = {
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {out_w, out_h, 1},
            };
            dev->fns.cmd_copy_image_to_buffer(cmd, fg->output.image, VK_IMAGE_LAYOUT_GENERAL,
                                              fg->dump_buffer, 1, &region);
            fg->dump_recorded = true;
        }
    }

    fg->frame_index++;
}
