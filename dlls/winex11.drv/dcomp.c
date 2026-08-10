/*
 * X11 DirectComposition retained scene consumer
 *
 * Copyright 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wine/dcomp_driver.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "x11drv.h"
#include "ntuser.h"

#define WINE_VULKAN_NO_X11_TYPES
#include "wine/vulkan.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

#define MAX_SCENE_ITEMS 4096
#define MAX_SURFACE_EXTENT 32768

struct dcomp_surface
{
    struct wine_dcomp_surface desc;
    uint32_t *pixels;
};

struct dcomp_state
{
    struct list entry;
    LONG refs;
    uint64_t device_id;
    uint64_t generation;
    uint64_t publication;
    uint32_t target_count;
    uint32_t visual_count;
    uint32_t surface_count;
    struct wine_dcomp_target *targets;
    LONG *target_live;
    struct wine_dcomp_visual *visuals;
    struct dcomp_surface *surfaces;
};

struct vk_context
{
    VkPhysicalDevice physical_device;
    uint8_t device_uuid[VK_UUID_SIZE];
    uint64_t adapter_luid;
    BOOL adapter_luid_valid;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkPhysicalDeviceMemoryProperties memory_properties;
    PFN_vkCreateImage p_vkCreateImage;
    PFN_vkDestroyImage p_vkDestroyImage;
    PFN_vkGetImageMemoryRequirements p_vkGetImageMemoryRequirements;
    PFN_vkAllocateMemory p_vkAllocateMemory;
    PFN_vkFreeMemory p_vkFreeMemory;
    PFN_vkBindImageMemory p_vkBindImageMemory;
    PFN_vkCreateBuffer p_vkCreateBuffer;
    PFN_vkDestroyBuffer p_vkDestroyBuffer;
    PFN_vkGetBufferMemoryRequirements p_vkGetBufferMemoryRequirements;
    PFN_vkBindBufferMemory p_vkBindBufferMemory;
    PFN_vkCreateCommandPool p_vkCreateCommandPool;
    PFN_vkDestroyCommandPool p_vkDestroyCommandPool;
    PFN_vkAllocateCommandBuffers p_vkAllocateCommandBuffers;
    PFN_vkBeginCommandBuffer p_vkBeginCommandBuffer;
    PFN_vkEndCommandBuffer p_vkEndCommandBuffer;
    PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer p_vkCmdCopyImageToBuffer;
    PFN_vkQueueSubmit p_vkQueueSubmit;
    PFN_vkQueueWaitIdle p_vkQueueWaitIdle;
    PFN_vkMapMemory p_vkMapMemory;
    PFN_vkUnmapMemory p_vkUnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges p_vkInvalidateMappedMemoryRanges;
    PFN_vkCreateSemaphore p_vkCreateSemaphore;
    PFN_vkDestroySemaphore p_vkDestroySemaphore;
    PFN_vkImportSemaphoreFdKHR p_vkImportSemaphoreFdKHR;
    PFN_vkGetSemaphoreCounterValue p_vkGetSemaphoreCounterValue;
};

struct vk_reader
{
    void *module;
    VkInstance instance;
    PFN_vkDestroyInstance p_vkDestroyInstance;
    PFN_vkGetDeviceProcAddr p_vkGetDeviceProcAddr;
    PFN_vkDestroyDevice p_vkDestroyDevice;
    struct vk_context *contexts;
    uint32_t context_count;
};

static struct vk_reader vk_reader;
static pthread_once_t vk_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t vk_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scene_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct list states = LIST_INIT(states);
static uint64_t publication;

static BOOL checked_array(const struct wine_dcomp_scene *scene, UINT size, uint32_t offset,
        uint32_t count, size_t element_size, const void **out)
{
    size_t bytes;

    *out = NULL;
    if (count > MAX_SCENE_ITEMS || count > SIZE_MAX / element_size) return FALSE;
    bytes = count * element_size;
    if (!count) return !offset || offset <= size;
    if (offset < sizeof(*scene) || offset > size || bytes > size - offset) return FALSE;
    *out = (const char *)scene + offset;
    return TRUE;
}

static BOOL finite_matrix(const float *m)
{
    unsigned int i;
    for (i = 0; i < 6; ++i) if (!isfinite(m[i])) return FALSE;
    return TRUE;
}

static BOOL validate_graph(const struct wine_dcomp_scene *scene,
        const struct wine_dcomp_target *targets, const struct wine_dcomp_visual *visuals)
{
    uint8_t *seen = NULL, *roots = NULL;
    unsigned int i, j;
    BOOL ret = FALSE;

    if (scene->visual_count && (!(seen = calloc(scene->visual_count, 1))
            || !(roots = calloc(scene->visual_count, 1)))) goto done;
    for (i = 0; i < scene->target_count; ++i)
    {
        uint32_t root = targets[i].root_visual;
        if (root == WINE_DCOMP_INVALID_INDEX) continue;
        if (visuals[root].parent != WINE_DCOMP_INVALID_INDEX || roots[root]) goto done;
        roots[root] = 1;
    }
    for (i = 0; i < scene->visual_count; ++i)
    {
        uint32_t child = visuals[i].first_child, count = 0;
        while (child != WINE_DCOMP_INVALID_INDEX)
        {
            if (child >= scene->visual_count || child == i || count++ >= scene->visual_count
                    || visuals[child].parent != i || seen[child]) goto done;
            seen[child] = 1;
            child = visuals[child].next_sibling;
        }
    }
    for (i = 0; i < scene->visual_count; ++i)
    {
        uint32_t current = i, count = 0;
        if ((visuals[i].parent == WINE_DCOMP_INVALID_INDEX) != !seen[i]) goto done;
        while (visuals[current].parent != WINE_DCOMP_INVALID_INDEX)
        {
            if (count++ >= scene->visual_count) goto done;
            current = visuals[current].parent;
        }
        if (!roots[current]) goto done; /* Every serialized visual belongs to exactly one target tree. */
    }
    for (j = 0; j < scene->visual_count; ++j)
        if (roots[j] && seen[j]) goto done;
    ret = TRUE;

done:
    free(roots);
    free(seen);
    return ret;
}

static BOOL validate_scene(const struct wine_dcomp_scene *scene, UINT size,
        const struct wine_dcomp_target **targets, const struct wine_dcomp_visual **visuals,
        const struct wine_dcomp_surface **surfaces)
{
    unsigned int i, j;

    if (!scene || size < sizeof(*scene) || scene->abi_version != WINE_DCOMP_DRIVER_ABI_VERSION
            || scene->byte_size != size || scene->reserved || !scene->device_id
            || !scene->generation) return FALSE;
    if (scene->update_kind < WINE_DCOMP_UPDATE_COMMIT
            || scene->update_kind > WINE_DCOMP_UPDATE_DESTROY) return FALSE;
    if (!checked_array(scene, size, scene->target_offset, scene->target_count,
            sizeof(**targets), (const void **)targets)
            || !checked_array(scene, size, scene->visual_offset, scene->visual_count,
            sizeof(**visuals), (const void **)visuals)
            || !checked_array(scene, size, scene->surface_offset, scene->surface_count,
            sizeof(**surfaces), (const void **)surfaces)) return FALSE;

    if (scene->update_kind == WINE_DCOMP_UPDATE_DESTROY)
        return !scene->target_count && !scene->visual_count && !scene->surface_count;
    if (scene->update_kind == WINE_DCOMP_UPDATE_SURFACES
            && (scene->target_count || scene->visual_count)) return FALSE;

    for (i = 0; i < scene->target_count; ++i)
    {
        if (!(*targets)[i].hwnd || !(*targets)[i].target_id
                || (*targets)[i].flags & ~WINE_DCOMP_TARGET_TOPMOST) return FALSE;
        if ((*targets)[i].root_visual != WINE_DCOMP_INVALID_INDEX
                && (*targets)[i].root_visual >= scene->visual_count) return FALSE;
        for (j = 0; j < i; ++j)
            if ((*targets)[j].target_id == (*targets)[i].target_id) return FALSE;
    }
    for (i = 0; i < scene->visual_count; ++i)
    {
        const struct wine_dcomp_visual *visual = *visuals + i;

        if (visual->flags & ~(WINE_DCOMP_VISUAL_HAS_TRANSFORM | WINE_DCOMP_VISUAL_HAS_CLIP))
            return FALSE;
        if (visual->parent != WINE_DCOMP_INVALID_INDEX && visual->parent >= scene->visual_count)
            return FALSE;
        if (visual->first_child != WINE_DCOMP_INVALID_INDEX && visual->first_child >= scene->visual_count)
            return FALSE;
        if (visual->next_sibling != WINE_DCOMP_INVALID_INDEX && visual->next_sibling >= scene->visual_count)
            return FALSE;
        if (visual->content_kind > WINE_DCOMP_CONTENT_SWAPCHAIN) return FALSE;
        /* These are the exact semantics implemented by the deterministic CPU
         * consumer.  Never silently substitute for a requested effect mode. */
        if ((visual->interpolation_mode != 0 && visual->interpolation_mode != 1
                && visual->interpolation_mode != UINT32_MAX)
                || (visual->border_mode != 0 && visual->border_mode != UINT32_MAX)
                || (visual->composite_mode != 0 && visual->composite_mode != UINT32_MAX)
                || (visual->opacity_mode != 0 && visual->opacity_mode != UINT32_MAX)
                || (visual->backface_visibility != 0 && visual->backface_visibility != UINT32_MAX)
                || !isfinite(visual->opacity) || visual->opacity < 0.0f || visual->opacity > 1.0f)
            return FALSE;
        if (visual->content_kind == WINE_DCOMP_CONTENT_NONE)
        {
            if (visual->content != WINE_DCOMP_INVALID_INDEX) return FALSE;
        }
        else if (visual->content >= scene->surface_count) return FALSE;
        if (!isfinite(visual->offset_x) || !isfinite(visual->offset_y)
                || !finite_matrix(visual->transform)) return FALSE;
        if ((visual->flags & WINE_DCOMP_VISUAL_HAS_CLIP)
                && (!isfinite(visual->clip[0]) || !isfinite(visual->clip[1])
                || !isfinite(visual->clip[2]) || !isfinite(visual->clip[3])
                || visual->clip[2] < visual->clip[0] || visual->clip[3] < visual->clip[1]))
            return FALSE;
    }
    for (i = 0; i < scene->surface_count; ++i)
    {
        const struct wine_dcomp_surface *surface = *surfaces + i;
        static const uint8_t zero_uuid[VK_UUID_SIZE];
        BOOL has_front = !!(surface->flags & WINE_DCOMP_SURFACE_HAS_FRONT);

        if (surface->flags & ~WINE_DCOMP_SURFACE_HAS_FRONT) return FALSE;
        if (!has_front && !surface->width)
        {
            if (surface->resource || surface->sync_resource || surface->sync_value
                    || surface->adapter_luid
                    || memcmp(surface->device_uuid, zero_uuid, sizeof(zero_uuid))
                    || surface->height || surface->format || surface->alpha_mode
                    || surface->resource_type || surface->sync_resource_type
                    || surface->sync_type || surface->memory_type_index
                    || surface->image_usage || surface->image_flags
                    || surface->sample_count || surface->mip_levels
                    || surface->array_layers || surface->front_buffer
                    || surface->buffer_count || surface->damage_left
                    || surface->damage_top || surface->damage_right
                    || surface->damage_bottom) return FALSE;
            continue;
        }
        if (!surface->width || !surface->height || surface->width > MAX_SURFACE_EXTENT
                || surface->height > MAX_SURFACE_EXTENT
                || surface->width > SIZE_MAX / surface->height
                || surface->width * (size_t)surface->height > SIZE_MAX / sizeof(uint32_t)) return FALSE;
        if (memcmp(surface->device_uuid, zero_uuid, sizeof(zero_uuid)) == 0
                || !surface->buffer_count || surface->buffer_count > 16
                || surface->front_buffer >= surface->buffer_count
                || surface->memory_type_index >= 32
                || surface->image_usage & ~(WINE_DCOMP_IMAGE_TRANSFER_SRC
                | WINE_DCOMP_IMAGE_TRANSFER_DST | WINE_DCOMP_IMAGE_SAMPLED
                | WINE_DCOMP_IMAGE_COLOR_ATTACHMENT)
                || !(surface->image_usage & WINE_DCOMP_IMAGE_TRANSFER_SRC)
                || surface->image_flags & ~(WINE_DCOMP_IMAGE_ALIAS
                | WINE_DCOMP_IMAGE_MUTABLE_FORMAT | WINE_DCOMP_IMAGE_OPTIMAL_TILING
                | WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION)
                || !(surface->image_flags & WINE_DCOMP_IMAGE_OPTIMAL_TILING)
                || !(surface->image_flags & WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION)
                || surface->sample_count != 1 || surface->mip_levels != 1
                || surface->array_layers != 1
                || (surface->format != 28 && surface->format != 87)
                || surface->alpha_mode < 1 || surface->alpha_mode > 3
                || surface->damage_left < 0 || surface->damage_top < 0
                || surface->damage_right < surface->damage_left
                || surface->damage_bottom < surface->damage_top
                || surface->damage_right > surface->width
                || surface->damage_bottom > surface->height) return FALSE;
        if (has_front)
        {
            if (surface->resource_type != WINE_DCOMP_RESOURCE_OPAQUE_FD
                    || surface->resource > INT_MAX
                    || fcntl((int)surface->resource, F_GETFD) == -1
                    || !surface->adapter_luid) return FALSE;
            if (surface->sync_type == WINE_DCOMP_SYNC_HOST_IDLE)
            {
                if (surface->sync_resource || surface->sync_resource_type
                        || surface->sync_value) return FALSE;
            }
            else if (surface->sync_resource_type != WINE_DCOMP_RESOURCE_OPAQUE_FD
                    || surface->sync_resource > INT_MAX
                    || fcntl((int)surface->sync_resource, F_GETFD) == -1
                    || surface->sync_type != WINE_DCOMP_SYNC_TIMELINE
                    || !surface->sync_value) return FALSE;
        }
        else if (surface->resource || surface->resource_type || surface->sync_resource
                || surface->sync_resource_type || surface->sync_type
                || surface->sync_value) return FALSE;
    }
    return scene->update_kind != WINE_DCOMP_UPDATE_COMMIT
            || validate_graph(scene, *targets, *visuals);
}

static uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties *properties,
        uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred)
{
    uint32_t fallback = UINT32_MAX, i;

    for (i = 0; i < properties->memoryTypeCount; ++i)
    {
        VkMemoryPropertyFlags flags = properties->memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback == UINT32_MAX) fallback = i;
    }
    return fallback;
}

static VkFormat vk_format_from_dxgi(uint32_t format, BOOL *rgba)
{
    *rgba = FALSE;
    switch (format)
    {
    case 28: *rgba = TRUE; return VK_FORMAT_R8G8B8A8_UNORM;
    case 87: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkImageUsageFlags vk_usage_from_dcomp(uint32_t usage)
{
    VkImageUsageFlags ret = 0;
    if (usage & WINE_DCOMP_IMAGE_TRANSFER_SRC) ret |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & WINE_DCOMP_IMAGE_TRANSFER_DST) ret |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & WINE_DCOMP_IMAGE_SAMPLED) ret |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (usage & WINE_DCOMP_IMAGE_COLOR_ATTACHMENT) ret |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return ret;
}

static VkImageCreateFlags vk_flags_from_dcomp(uint32_t flags)
{
    VkImageCreateFlags ret = 0;
    if (flags & WINE_DCOMP_IMAGE_ALIAS) ret |= VK_IMAGE_CREATE_ALIAS_BIT;
    if (flags & WINE_DCOMP_IMAGE_MUTABLE_FORMAT) ret |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    return ret;
}

static VkSampleCountFlagBits vk_samples_from_count(uint32_t count)
{
    switch (count)
    {
    case 1: return VK_SAMPLE_COUNT_1_BIT;
    case 2: return VK_SAMPLE_COUNT_2_BIT;
    case 4: return VK_SAMPLE_COUNT_4_BIT;
    case 8: return VK_SAMPLE_COUNT_8_BIT;
    case 16: return VK_SAMPLE_COUNT_16_BIT;
    case 32: return VK_SAMPLE_COUNT_32_BIT;
    case 64: return VK_SAMPLE_COUNT_64_BIT;
    default: return 0;
    }
}

#ifdef SONAME_LIBVULKAN
static BOOL load_device_functions(struct vk_context *context, PFN_vkGetDeviceProcAddr get_proc)
{
#define LOAD_DEVICE_FUNC(name) \
    if (!(context->p_##name = (void *)get_proc(context->device, #name))) return FALSE
    LOAD_DEVICE_FUNC(vkCreateImage);
    LOAD_DEVICE_FUNC(vkDestroyImage);
    LOAD_DEVICE_FUNC(vkGetImageMemoryRequirements);
    LOAD_DEVICE_FUNC(vkAllocateMemory);
    LOAD_DEVICE_FUNC(vkFreeMemory);
    LOAD_DEVICE_FUNC(vkBindImageMemory);
    LOAD_DEVICE_FUNC(vkCreateBuffer);
    LOAD_DEVICE_FUNC(vkDestroyBuffer);
    LOAD_DEVICE_FUNC(vkGetBufferMemoryRequirements);
    LOAD_DEVICE_FUNC(vkBindBufferMemory);
    LOAD_DEVICE_FUNC(vkCreateCommandPool);
    LOAD_DEVICE_FUNC(vkDestroyCommandPool);
    LOAD_DEVICE_FUNC(vkAllocateCommandBuffers);
    LOAD_DEVICE_FUNC(vkBeginCommandBuffer);
    LOAD_DEVICE_FUNC(vkEndCommandBuffer);
    LOAD_DEVICE_FUNC(vkCmdPipelineBarrier);
    LOAD_DEVICE_FUNC(vkCmdCopyImageToBuffer);
    LOAD_DEVICE_FUNC(vkQueueSubmit);
    LOAD_DEVICE_FUNC(vkQueueWaitIdle);
    LOAD_DEVICE_FUNC(vkMapMemory);
    LOAD_DEVICE_FUNC(vkUnmapMemory);
    LOAD_DEVICE_FUNC(vkInvalidateMappedMemoryRanges);
    LOAD_DEVICE_FUNC(vkCreateSemaphore);
    LOAD_DEVICE_FUNC(vkDestroySemaphore);
    LOAD_DEVICE_FUNC(vkImportSemaphoreFdKHR);
#undef LOAD_DEVICE_FUNC
    context->p_vkGetSemaphoreCounterValue =
            (void *)get_proc(context->device, "vkGetSemaphoreCounterValue");
    if (!context->p_vkGetSemaphoreCounterValue)
        context->p_vkGetSemaphoreCounterValue =
                (void *)get_proc(context->device, "vkGetSemaphoreCounterValueKHR");
    return TRUE;
}

static void init_vk_reader(void)
{
    static const char *device_extensions[] =
    {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
    };
    VkApplicationInfo app_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pApplicationInfo = &app_info};
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory_properties;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queue_properties;
    PFN_vkEnumeratePhysicalDevices enumerate_devices;
    PFN_vkCreateInstance create_instance;
    PFN_vkCreateDevice create_device;
    PFN_vkGetDeviceQueue get_device_queue;
    VkPhysicalDevice *physical_devices = NULL;
    uint32_t count = 0, i;

    if (!(vk_reader.module = dlopen(SONAME_LIBVULKAN, RTLD_NOW))) return;
    if (!(create_instance = dlsym(vk_reader.module, "vkCreateInstance"))) goto fail;
    if (!(vk_reader.p_vkGetDeviceProcAddr = dlsym(vk_reader.module, "vkGetDeviceProcAddr"))) goto fail;
    if (create_instance(&instance_info, NULL, &vk_reader.instance) != VK_SUCCESS) goto fail;
#define LOAD_INSTANCE_FUNC(var, name) \
    if (!(var = (void *)dlsym(vk_reader.module, #name))) goto fail
    LOAD_INSTANCE_FUNC(enumerate_devices, vkEnumeratePhysicalDevices);
    LOAD_INSTANCE_FUNC(create_device, vkCreateDevice);
    LOAD_INSTANCE_FUNC(get_memory_properties, vkGetPhysicalDeviceMemoryProperties);
    LOAD_INSTANCE_FUNC(get_properties2, vkGetPhysicalDeviceProperties2);
    LOAD_INSTANCE_FUNC(get_queue_properties, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD_INSTANCE_FUNC(get_device_queue, vkGetDeviceQueue);
    LOAD_INSTANCE_FUNC(vk_reader.p_vkDestroyDevice, vkDestroyDevice);
    LOAD_INSTANCE_FUNC(vk_reader.p_vkDestroyInstance, vkDestroyInstance);
#undef LOAD_INSTANCE_FUNC
    if (enumerate_devices(vk_reader.instance, &count, NULL) != VK_SUCCESS || !count
            || !(physical_devices = calloc(count, sizeof(*physical_devices)))) goto fail;
    if (enumerate_devices(vk_reader.instance, &count, physical_devices) != VK_SUCCESS) goto fail;
    if (!(vk_reader.contexts = calloc(count, sizeof(*vk_reader.contexts)))) goto fail;

    for (i = 0; i < count; ++i)
    {
        struct vk_context *context = vk_reader.contexts + vk_reader.context_count;
        VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline_features =
        {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
            .timelineSemaphore = VK_TRUE,
        };
        VkPhysicalDeviceIDProperties id = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &id};
        VkQueueFamilyProperties *queue_properties = NULL;
        uint32_t queue_count = 0, j;
        float priority = 1.0f;

        get_queue_properties(physical_devices[i], &queue_count, NULL);
        if (!queue_count || !(queue_properties = calloc(queue_count, sizeof(*queue_properties)))) continue;
        get_queue_properties(physical_devices[i], &queue_count, queue_properties);
        for (j = 0; j < queue_count; ++j)
            if (queue_properties[j].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) break;
        free(queue_properties);
        if (j == queue_count) continue;

        queue_info.queueFamilyIndex = j;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pNext = &timeline_features;
        device_info.enabledExtensionCount = ARRAY_SIZE(device_extensions);
        device_info.ppEnabledExtensionNames = device_extensions;
        if (create_device(physical_devices[i], &device_info, NULL, &context->device) != VK_SUCCESS) continue;
        context->physical_device = physical_devices[i];
        get_properties2(physical_devices[i], &properties);
        memcpy(context->device_uuid, id.deviceUUID, sizeof(context->device_uuid));
        if (id.deviceLUIDValid)
        {
            memcpy(&context->adapter_luid, id.deviceLUID, sizeof(context->adapter_luid));
            context->adapter_luid_valid = TRUE;
        }
        context->queue_family = j;
        get_device_queue(context->device, j, 0, &context->queue);
        get_memory_properties(physical_devices[i], &context->memory_properties);
        if (!load_device_functions(context, vk_reader.p_vkGetDeviceProcAddr))
        {
            vk_reader.p_vkDestroyDevice(context->device, NULL);
            memset(context, 0, sizeof(*context));
            continue;
        }
        ++vk_reader.context_count;
    }
    free(physical_devices);
    return;

fail:
    free(physical_devices);
    if (vk_reader.instance && vk_reader.p_vkDestroyInstance)
        vk_reader.p_vkDestroyInstance(vk_reader.instance, NULL);
    if (vk_reader.module) dlclose(vk_reader.module);
    memset(&vk_reader, 0, sizeof(vk_reader));
}
#else
static void init_vk_reader(void) {}
#endif

static BOOL read_surface_with_context(struct vk_context *context,
        const struct wine_dcomp_surface *surface, uint32_t *pixels)
{
    VkExternalMemoryImageCreateInfo external_info =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkImageCreateInfo image_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .extent.depth = 1,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryDedicatedAllocateInfo dedicated_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkImportMemoryFdInfoKHR import_info = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo memory_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBufferBeginInfo begin_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}};
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1};
    VkImageMemoryBarrier acquire = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkImageMemoryBarrier release = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    VkMappedMemoryRange mapped_range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .offset = 0, .size = VK_WHOLE_SIZE};
    VkSemaphoreTypeCreateInfo semaphore_type =
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
    };
    VkSemaphoreCreateInfo semaphore_info =
    {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &semaphore_type,
    };
    VkImportSemaphoreFdInfoKHR semaphore_import =
    {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkTimelineSemaphoreSubmitInfo timeline_submit =
    {
        .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .waitSemaphoreValueCount = 1,
        .pWaitSemaphoreValues = &surface->sync_value,
    };
    VkMemoryRequirements image_requirements, buffer_requirements;
    VkDeviceMemory image_memory = VK_NULL_HANDLE, buffer_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkSampleCountFlagBits samples;
    VkMemoryPropertyFlags buffer_flags;
    VkFormat format;
    VkResult vr;
    uint64_t counter = 0;
    BOOL rgba, ret = FALSE;
    void *mapped = NULL;
    uint32_t buffer_memory_type, x, y;
    size_t pixel_count = surface->width * (size_t)surface->height;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    int import_fd = -1, sync_fd = -1;

    if (!(surface->image_usage & WINE_DCOMP_IMAGE_TRANSFER_SRC)
            || surface->image_usage & ~(WINE_DCOMP_IMAGE_TRANSFER_SRC | WINE_DCOMP_IMAGE_TRANSFER_DST
            | WINE_DCOMP_IMAGE_SAMPLED | WINE_DCOMP_IMAGE_COLOR_ATTACHMENT)
            || surface->image_flags & ~(WINE_DCOMP_IMAGE_ALIAS | WINE_DCOMP_IMAGE_MUTABLE_FORMAT
            | WINE_DCOMP_IMAGE_OPTIMAL_TILING | WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION)
            || surface->mip_levels != 1 || surface->array_layers != 1
            || !(samples = vk_samples_from_count(surface->sample_count))
            || samples != VK_SAMPLE_COUNT_1_BIT
            || !(format = vk_format_from_dxgi(surface->format, &rgba))) return FALSE;

    if ((import_fd = fcntl((int)surface->resource, F_DUPFD_CLOEXEC, 0)) == -1) return FALSE;
    if (TRACE_ON(dcomp))
    {
        struct stat st;
        if (!fstat( import_fd, &st )) TRACE("duplicated memory fd %d dev %s ino %s size %s\n",
                import_fd, wine_dbgstr_longlong(st.st_dev), wine_dbgstr_longlong(st.st_ino),
                wine_dbgstr_longlong(st.st_size));
    }
    if (surface->sync_type != WINE_DCOMP_SYNC_HOST_IDLE
            && (sync_fd = fcntl((int)surface->sync_resource, F_DUPFD_CLOEXEC, 0)) == -1) goto done;

    image_info.flags = vk_flags_from_dcomp(surface->image_flags);
    image_info.format = format;
    image_info.extent.width = surface->width;
    image_info.extent.height = surface->height;
    image_info.mipLevels = surface->mip_levels;
    image_info.arrayLayers = surface->array_layers;
    image_info.samples = samples;
    image_info.tiling = surface->image_flags & WINE_DCOMP_IMAGE_OPTIMAL_TILING
            ? VK_IMAGE_TILING_OPTIMAL : VK_IMAGE_TILING_LINEAR;
    image_info.usage = vk_usage_from_dcomp(surface->image_usage);
    TRACE("image create format %u extent %ux%u flags %#x usage %#x tiling %u samples %#x "
            "mips %u layers %u memory type %u\n", image_info.format, image_info.extent.width,
            image_info.extent.height, image_info.flags, image_info.usage, image_info.tiling,
            image_info.samples, image_info.mipLevels, image_info.arrayLayers,
            surface->memory_type_index);
    if (context->p_vkCreateImage(context->device, &image_info, NULL, &image) != VK_SUCCESS) goto done;
    context->p_vkGetImageMemoryRequirements(context->device, image, &image_requirements);
    TRACE("image requirements size %s alignment %s memory bits %#x\n",
            wine_dbgstr_longlong(image_requirements.size),
            wine_dbgstr_longlong(image_requirements.alignment), image_requirements.memoryTypeBits);
    if (surface->memory_type_index >= context->memory_properties.memoryTypeCount
            || !(image_requirements.memoryTypeBits & (1u << surface->memory_type_index))) goto done;

    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    import_info.fd = import_fd;
    if (surface->image_flags & WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION)
    {
        dedicated_info.image = image;
        dedicated_info.pNext = &import_info;
        memory_info.pNext = &dedicated_info;
    }
    else memory_info.pNext = &import_info;
    memory_info.allocationSize = image_requirements.size;
    memory_info.memoryTypeIndex = surface->memory_type_index;
    if (context->p_vkAllocateMemory(context->device, &memory_info, NULL, &image_memory) != VK_SUCCESS)
        goto done;
    import_fd = -1; /* Vulkan consumed the fd on successful import. */
    if (context->p_vkBindImageMemory(context->device, image, image_memory, 0) != VK_SUCCESS) goto done;

    /* A producer that cannot export a shareable timeline semaphore publishes
     * without one, having waited for its own device to go idle first.  There is
     * then nothing to wait on here: the release barrier it recorded has already
     * completed by the time the publication became visible. */
    if (surface->sync_type != WINE_DCOMP_SYNC_HOST_IDLE)
    {
        vr = context->p_vkCreateSemaphore(context->device, &semaphore_info, NULL, &semaphore);
        TRACE("vkCreateSemaphore returned %d for publication value %s\n", vr,
                wine_dbgstr_longlong(surface->sync_value));
        if (vr != VK_SUCCESS) goto done;
        semaphore_import.semaphore = semaphore;
        semaphore_import.fd = sync_fd;
        vr = context->p_vkImportSemaphoreFdKHR(context->device, &semaphore_import);
        TRACE("vkImportSemaphoreFdKHR returned %d\n", vr);
        if (vr != VK_SUCCESS) goto done;
        sync_fd = -1; /* Vulkan consumed the fd on successful import. */
        if (context->p_vkGetSemaphoreCounterValue)
        {
            vr = context->p_vkGetSemaphoreCounterValue(context->device, semaphore, &counter);
            TRACE("vkGetSemaphoreCounterValue returned %d, value %s\n", vr,
                    wine_dbgstr_longlong(counter));
            if (vr != VK_SUCCESS) goto done;
        }
    }

    buffer_info.size = pixel_count * sizeof(*pixels);
    if (context->p_vkCreateBuffer(context->device, &buffer_info, NULL, &buffer) != VK_SUCCESS) goto done;
    context->p_vkGetBufferMemoryRequirements(context->device, buffer, &buffer_requirements);
    if ((buffer_memory_type = find_memory_type(&context->memory_properties,
            buffer_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == UINT32_MAX) goto done;
    buffer_flags = context->memory_properties.memoryTypes[buffer_memory_type].propertyFlags;
    memory_info.pNext = NULL;
    memory_info.allocationSize = buffer_requirements.size;
    memory_info.memoryTypeIndex = buffer_memory_type;
    if (context->p_vkAllocateMemory(context->device, &memory_info, NULL, &buffer_memory) != VK_SUCCESS
            || context->p_vkBindBufferMemory(context->device, buffer, buffer_memory, 0) != VK_SUCCESS) goto done;

    pool_info.queueFamilyIndex = context->queue_family;
    if (context->p_vkCreateCommandPool(context->device, &pool_info, NULL, &command_pool) != VK_SUCCESS) goto done;
    command_info.commandPool = command_pool;
    if (context->p_vkAllocateCommandBuffers(context->device, &command_info, &command_buffer) != VK_SUCCESS
            || context->p_vkBeginCommandBuffer(command_buffer, &begin_info) != VK_SUCCESS) goto done;

    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acquire.dstQueueFamilyIndex = context->queue_family;
    acquire.image = image;
    acquire.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    context->p_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &acquire);
    copy.imageExtent = (VkExtent3D){surface->width, surface->height, 1};
    context->p_vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            buffer, 1, &copy);
    release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    release.dstAccessMask = 0;
    release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    release.srcQueueFamilyIndex = context->queue_family;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    release.image = image;
    release.subresourceRange = acquire.subresourceRange;
    context->p_vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &release);
    if (context->p_vkEndCommandBuffer(command_buffer) != VK_SUCCESS) goto done;
    if (semaphore)
    {
        submit.pNext = &timeline_submit;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &semaphore;
        submit.pWaitDstStageMask = &wait_stage;
    }
    submit.pCommandBuffers = &command_buffer;
    vr = context->p_vkQueueSubmit(context->queue, 1, &submit, VK_NULL_HANDLE);
    TRACE("vkQueueSubmit wait value %s returned %d\n",
            wine_dbgstr_longlong(surface->sync_value), vr);
    if (vr != VK_SUCCESS) goto done;
    vr = context->p_vkQueueWaitIdle(context->queue);
    TRACE("vkQueueWaitIdle returned %d\n", vr);
    if (vr != VK_SUCCESS) goto done;
    if (context->p_vkMapMemory(context->device, buffer_memory, 0, buffer_info.size, 0, &mapped) != VK_SUCCESS)
        goto done;
    if (!(buffer_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        mapped_range.memory = buffer_memory;
        if (context->p_vkInvalidateMappedMemoryRanges(context->device, 1, &mapped_range) != VK_SUCCESS)
            goto done;
    }
    TRACE("staging first pixel after copy/invalidate %#x\n", *(const uint32_t *)mapped);

    for (y = 0; y < surface->height; ++y)
    {
        const uint32_t *src = (const uint32_t *)mapped + y * surface->width;
        uint32_t *dst = pixels + y * surface->width;
        for (x = 0; x < surface->width; ++x)
        {
            uint32_t pixel = src[x], a = pixel >> 24, r, g, b;
            if (rgba)
            {
                r = pixel & 0xff;
                g = (pixel >> 8) & 0xff;
                b = (pixel >> 16) & 0xff;
            }
            else
            {
                b = pixel & 0xff;
                g = (pixel >> 8) & 0xff;
                r = (pixel >> 16) & 0xff;
            }
            if (surface->alpha_mode == 2)
            {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }
            else if (surface->alpha_mode == 3) a = 255;
            else if (surface->alpha_mode != 1) goto done;
            dst[x] = a << 24 | r << 16 | g << 8 | b;
        }
    }
    ret = TRUE;

done:
    if (mapped) context->p_vkUnmapMemory(context->device, buffer_memory);
    if (command_pool) context->p_vkDestroyCommandPool(context->device, command_pool, NULL);
    if (semaphore) context->p_vkDestroySemaphore(context->device, semaphore, NULL);
    if (buffer) context->p_vkDestroyBuffer(context->device, buffer, NULL);
    if (buffer_memory) context->p_vkFreeMemory(context->device, buffer_memory, NULL);
    if (image) context->p_vkDestroyImage(context->device, image, NULL);
    if (image_memory) context->p_vkFreeMemory(context->device, image_memory, NULL);
    if (import_fd != -1) close(import_fd);
    if (sync_fd != -1) close(sync_fd);
    return ret;
}

static BOOL read_surface(const struct wine_dcomp_surface *surface, uint32_t *pixels)
{
    unsigned int i;
    BOOL ret = FALSE;

    pthread_once(&vk_once, init_vk_reader);
    pthread_mutex_lock(&vk_mutex);
    for (i = 0; i < vk_reader.context_count; ++i)
        if (!memcmp(vk_reader.contexts[i].device_uuid, surface->device_uuid, VK_UUID_SIZE)
                && (!vk_reader.contexts[i].adapter_luid_valid
                || vk_reader.contexts[i].adapter_luid == surface->adapter_luid))
        {
            ret = read_surface_with_context(vk_reader.contexts + i, surface, pixels);
            break;
        }
    pthread_mutex_unlock(&vk_mutex);
    return ret;
}

static void state_destroy(struct dcomp_state *state)
{
    unsigned int i;
    if (!state) return;
    for (i = 0; i < state->surface_count; ++i) free(state->surfaces[i].pixels);
    free(state->surfaces);
    free(state->visuals);
    free(state->target_live);
    free(state->targets);
    free(state);
}

static void state_addref(struct dcomp_state *state)
{
    InterlockedIncrement(&state->refs);
}

static void state_release(struct dcomp_state *state)
{
    if (!InterlockedDecrement(&state->refs)) state_destroy(state);
}

static BOOL copy_surfaces(struct dcomp_state *state, const struct wine_dcomp_surface *surfaces,
        uint32_t count)
{
    unsigned int i;

    if (!(state->surfaces = calloc(count, sizeof(*state->surfaces))) && count) return FALSE;
    state->surface_count = count;
    for (i = 0; i < count; ++i)
    {
        size_t bytes = surfaces[i].width * (size_t)surfaces[i].height * sizeof(uint32_t);
        state->surfaces[i].desc = surfaces[i];
        state->surfaces[i].desc.resource = 0;
        state->surfaces[i].desc.sync_resource = 0;
        if (!(surfaces[i].flags & WINE_DCOMP_SURFACE_HAS_FRONT)) continue;
        if (!(state->surfaces[i].pixels = malloc(bytes))
                || !read_surface(surfaces + i, state->surfaces[i].pixels)) return FALSE;
        TRACE("surface %u imported first premultiplied pixel %#x\n", i,
                state->surfaces[i].pixels[0]);
    }
    return TRUE;
}

static struct dcomp_state *build_state(const struct wine_dcomp_scene *scene,
        const struct wine_dcomp_target *targets, const struct wine_dcomp_visual *visuals,
        const struct wine_dcomp_surface *surfaces, const struct dcomp_state *base)
{
    struct dcomp_state *state;

    if (!(state = calloc(1, sizeof(*state)))) return NULL;
    state->refs = 1;
    state->device_id = scene->device_id;
    state->generation = scene->generation;
    if (scene->update_kind == WINE_DCOMP_UPDATE_SURFACES)
    {
        unsigned int i;
        if (!base) goto failed;
        if (base->surface_count != scene->surface_count) goto failed;
        for (i = 0; i < base->visual_count; ++i)
            if (base->visuals[i].content_kind != WINE_DCOMP_CONTENT_NONE
                    && base->visuals[i].content >= scene->surface_count) goto failed;
        for (i = 0; i < base->surface_count; ++i)
            if (surfaces[i].generation < base->surfaces[i].desc.generation) goto failed;
        state->target_count = base->target_count;
        state->visual_count = base->visual_count;
        if ((state->target_count && !(state->targets = malloc(state->target_count * sizeof(*state->targets))))
                || (state->target_count && !(state->target_live = malloc(state->target_count * sizeof(*state->target_live))))
                || (state->visual_count && !(state->visuals = malloc(state->visual_count * sizeof(*state->visuals)))))
            goto failed;
        if (state->target_count)
        {
            memcpy(state->targets, base->targets, state->target_count * sizeof(*state->targets));
            for (unsigned int i = 0; i < state->target_count; ++i)
                state->target_live[i] = InterlockedCompareExchange(base->target_live + i, 1, 1);
        }
        if (state->visual_count)
            memcpy(state->visuals, base->visuals, state->visual_count * sizeof(*state->visuals));
    }
    else
    {
        state->target_count = scene->target_count;
        state->visual_count = scene->visual_count;
        if ((state->target_count && !(state->targets = malloc(state->target_count * sizeof(*state->targets))))
                || (state->target_count && !(state->target_live = malloc(state->target_count * sizeof(*state->target_live))))
                || (state->visual_count && !(state->visuals = malloc(state->visual_count * sizeof(*state->visuals)))))
            goto failed;
        if (state->target_count)
        {
            memcpy(state->targets, targets, state->target_count * sizeof(*state->targets));
            for (unsigned int i = 0; i < state->target_count; ++i) state->target_live[i] = 1;
        }
        if (state->visual_count)
            memcpy(state->visuals, visuals, state->visual_count * sizeof(*state->visuals));
    }
    if (!copy_surfaces(state, surfaces, scene->surface_count)) goto failed;
    return state;

failed:
    state_destroy(state);
    return NULL;
}

/* Called with scene_mutex held immediately before publication.  target_id is
 * the stable identity; an HWND value alone is not sufficient because user
 * handles may be reused after destruction. */
static BOOL reconcile_target_liveness(struct dcomp_state *candidate,
        const struct dcomp_state *previous)
{
    unsigned int i, j;

    for (i = 0; i < candidate->target_count; ++i)
    {
        const struct wine_dcomp_target *target = candidate->targets + i;
        LONG live = NtUserIsWindow((HWND)(UINT_PTR)target->hwnd);

        for (j = 0; previous && j < previous->target_count; ++j)
            if (previous->targets[j].target_id == target->target_id)
            {
                if (previous->targets[j].hwnd != target->hwnd) return FALSE;
                live &= InterlockedCompareExchange(previous->target_live + j, 1, 1);
                break;
            }
        candidate->target_live[i] = live;
    }
    return TRUE;
}

static struct dcomp_state *find_state(uint64_t device_id)
{
    struct dcomp_state *state;
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry)
        if (state->device_id == device_id) return state;
    return NULL;
}

static void damage_state(const struct dcomp_state *state)
{
    unsigned int i;
    if (!state) return;
    for (i = 0; i < state->target_count; ++i)
        if (state->targets[i].hwnd
                && InterlockedCompareExchange(state->target_live + i, 1, 1))
        {
            HWND root = NtUserGetAncestor((HWND)(UINT_PTR)state->targets[i].hwnd, GA_ROOT);
            if (root) NtUserExposeWindowSurface(root,
                    RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN, NULL);
        }
}

BOOL X11DRV_DCompositionUpdate(const struct wine_dcomp_scene *scene, UINT size)
{
    const struct wine_dcomp_target *targets;
    const struct wine_dcomp_visual *visuals;
    const struct wine_dcomp_surface *surfaces;
    struct dcomp_state *base = NULL, *candidate = NULL, *old = NULL;
    BOOL ret = FALSE;

    if (!validate_scene(scene, size, &targets, &visuals, &surfaces)) return FALSE;
    TRACE("device %s generation %s kind %u targets %u visuals %u surfaces %u\n",
            wine_dbgstr_longlong(scene->device_id), wine_dbgstr_longlong(scene->generation),
            scene->update_kind, scene->target_count, scene->visual_count, scene->surface_count);
    if (scene->update_kind == WINE_DCOMP_UPDATE_DESTROY)
    {
        pthread_mutex_lock(&scene_mutex);
        if ((old = find_state(scene->device_id)) && scene->generation <= old->generation)
        {
            pthread_mutex_unlock(&scene_mutex);
            return FALSE;
        }
        if (old) list_remove(&old->entry);
        pthread_mutex_unlock(&scene_mutex);
        damage_state(old);
        if (old) state_release(old);
        return TRUE;
    }

    pthread_mutex_lock(&scene_mutex);
    if ((base = find_state(scene->device_id))) state_addref(base);
    pthread_mutex_unlock(&scene_mutex);
    if ((scene->update_kind == WINE_DCOMP_UPDATE_SURFACES && !base)
            || (base && scene->generation <= base->generation)) goto done;
    if (!(candidate = build_state(scene, targets, visuals, surfaces, base))) goto done;

    pthread_mutex_lock(&scene_mutex);
    old = find_state(scene->device_id);
    if (old != base || (old && scene->generation <= old->generation)
            || !reconcile_target_liveness(candidate, old))
    {
        pthread_mutex_unlock(&scene_mutex);
        goto done;
    }
    candidate->publication = ++publication;
    if (old) list_remove(&old->entry);
    list_add_tail(&states, &candidate->entry);
    pthread_mutex_unlock(&scene_mutex);
    damage_state(old);
    damage_state(candidate);
    if (old) state_release(old);
    candidate = NULL;
    ret = TRUE;

done:
    if (base) state_release(base);
    if (candidate) state_release(candidate);
    return ret;
}

struct matrix
{
    float a, b, c, d, x, y;
};

struct clip
{
    struct matrix inverse;
    float left, top, right, bottom;
};

struct render_context
{
    uint32_t *pixels;
    int width, height, stride;
    struct clip *clips;
    uint32_t clip_count;
    RECT *exclusions;
    uint32_t exclusion_count;
};

static struct matrix multiply_matrix(struct matrix parent, struct matrix child)
{
    return (struct matrix){
        parent.a * child.a + parent.c * child.b,
        parent.b * child.a + parent.d * child.b,
        parent.a * child.c + parent.c * child.d,
        parent.b * child.c + parent.d * child.d,
        parent.a * child.x + parent.c * child.y + parent.x,
        parent.b * child.x + parent.d * child.y + parent.y,
    };
}

static BOOL invert_matrix(struct matrix m, struct matrix *inverse)
{
    float determinant = m.a * m.d - m.b * m.c;
    if (!isfinite(determinant) || fabsf(determinant) < 1.0e-12f) return FALSE;
    inverse->a = m.d / determinant;
    inverse->b = -m.b / determinant;
    inverse->c = -m.c / determinant;
    inverse->d = m.a / determinant;
    inverse->x = -(inverse->a * m.x + inverse->c * m.y);
    inverse->y = -(inverse->b * m.x + inverse->d * m.y);
    return finite_matrix((const float *)inverse);
}

static void transform_point(struct matrix m, float x, float y, float *out_x, float *out_y)
{
    *out_x = m.a * x + m.c * y + m.x;
    *out_y = m.b * x + m.d * y + m.y;
}

static BOOL point_in_clips(const struct render_context *context, float x, float y)
{
    unsigned int i;
    for (i = 0; i < context->clip_count; ++i)
    {
        float local_x, local_y;
        transform_point(context->clips[i].inverse, x, y, &local_x, &local_y);
        if (local_x < context->clips[i].left || local_x >= context->clips[i].right
                || local_y < context->clips[i].top || local_y >= context->clips[i].bottom) return FALSE;
    }
    for (i = 0; i < context->exclusion_count; ++i)
        if (x >= context->exclusions[i].left && x < context->exclusions[i].right
                && y >= context->exclusions[i].top && y < context->exclusions[i].bottom) return FALSE;
    return TRUE;
}

static uint32_t source_over(uint32_t source, uint32_t destination)
{
    uint32_t sa = source >> 24, inverse = 255 - sa;
    uint32_t da = destination >> 24;
    uint32_t r = (source >> 16 & 0xff) + ((destination >> 16 & 0xff) * inverse + 127) / 255;
    uint32_t g = (source >> 8 & 0xff) + ((destination >> 8 & 0xff) * inverse + 127) / 255;
    uint32_t b = (source & 0xff) + ((destination & 0xff) * inverse + 127) / 255;
    uint32_t a = sa + (da * inverse + 127) / 255;
    return min(a, 255) << 24 | min(r, 255) << 16 | min(g, 255) << 8 | min(b, 255);
}

static uint32_t sample_linear(const struct dcomp_surface *surface, float x, float y)
{
    int left = floorf(x - 0.5f), top = floorf(y - 0.5f), i, j;
    unsigned int wx = min(max((int)floorf((x - 0.5f - left) * 256.0f + 0.5f), 0), 256);
    unsigned int wy = min(max((int)floorf((y - 0.5f - top) * 256.0f + 0.5f), 0), 256);
    uint64_t channels[4] = {0};

    for (j = 0; j < 2; ++j)
        for (i = 0; i < 2; ++i)
        {
            int sx = left + i, sy = top + j;
            unsigned int weight = (i ? wx : 256 - wx) * (j ? wy : 256 - wy);
            uint32_t pixel;
            if (sx < 0 || sy < 0 || sx >= surface->desc.width || sy >= surface->desc.height) continue;
            pixel = surface->pixels[sy * surface->desc.width + sx];
            channels[0] += (pixel >> 24) * weight;
            channels[1] += (pixel >> 16 & 0xff) * weight;
            channels[2] += (pixel >> 8 & 0xff) * weight;
            channels[3] += (pixel & 0xff) * weight;
        }
    return ((channels[0] + 32768) >> 16) << 24 | ((channels[1] + 32768) >> 16) << 16
            | ((channels[2] + 32768) >> 16) << 8 | ((channels[3] + 32768) >> 16);
}

static void render_surface(struct render_context *context, const struct dcomp_surface *surface,
        struct matrix transform, uint32_t interpolation)
{
    struct matrix inverse;
    float corners_x[4], corners_y[4], min_x, max_x, min_y, max_y;
    int left, top, right, bottom, x, y;

    if (!surface->pixels || !invert_matrix(transform, &inverse)) return;
    transform_point(transform, 0, 0, &corners_x[0], &corners_y[0]);
    transform_point(transform, surface->desc.width, 0, &corners_x[1], &corners_y[1]);
    transform_point(transform, 0, surface->desc.height, &corners_x[2], &corners_y[2]);
    transform_point(transform, surface->desc.width, surface->desc.height, &corners_x[3], &corners_y[3]);
    min_x = max_x = corners_x[0];
    min_y = max_y = corners_y[0];
    for (x = 1; x < 4; ++x)
    {
        min_x = min(min_x, corners_x[x]); max_x = max(max_x, corners_x[x]);
        min_y = min(min_y, corners_y[x]); max_y = max(max_y, corners_y[x]);
    }
    if (!isfinite(min_x) || !isfinite(max_x) || !isfinite(min_y) || !isfinite(max_y)) return;
    if (max_x <= 0.0f || max_y <= 0.0f || min_x >= context->width || min_y >= context->height)
        return;
    left = min_x <= 0.0f ? 0 : (int)floorf(min_x);
    top = min_y <= 0.0f ? 0 : (int)floorf(min_y);
    right = max_x >= context->width ? context->width : (int)ceilf(max_x);
    bottom = max_y >= context->height ? context->height : (int)ceilf(max_y);
    for (y = top; y < bottom; ++y)
        for (x = left; x < right; ++x)
        {
            float source_x, source_y;
            int sx, sy;
            transform_point(inverse, x + 0.5f, y + 0.5f, &source_x, &source_y);
            if (!isfinite(source_x) || !isfinite(source_y)
                    || (interpolation && (source_x <= -0.5f || source_y <= -0.5f
                    || source_x >= surface->desc.width + 0.5f
                    || source_y >= surface->desc.height + 0.5f))
                    || (!interpolation && (source_x < 0.0f || source_y < 0.0f
                    || source_x >= surface->desc.width || source_y >= surface->desc.height))
                    || !point_in_clips(context, x + 0.5f, y + 0.5f)) continue;
            sx = floorf(source_x); sy = floorf(source_y);
            context->pixels[y * context->stride + x] = source_over(interpolation
                    ? sample_linear(surface, source_x, source_y)
                    : surface->pixels[sy * surface->desc.width + sx],
                    context->pixels[y * context->stride + x]);
        }
}

static void render_visual(const struct dcomp_state *state, uint32_t index, struct matrix parent,
        struct render_context *context, uint8_t *active, uint32_t depth,
        uint32_t inherited_interpolation, BOOL ignore_opacity)
{
    const struct wine_dcomp_visual *visual;
    struct matrix local, transform, inverse;
    uint32_t child, sibling_count = 0, interpolation;
    BOOL pushed = FALSE;

    if (index >= state->visual_count || depth > state->visual_count || active[index]) return;
    visual = state->visuals + index;
    interpolation = visual->interpolation_mode == UINT32_MAX
            ? inherited_interpolation : visual->interpolation_mode;
    if (!ignore_opacity && visual->opacity < 1.0f)
    {
        uint32_t *layer;
        size_t pixel_count;
        unsigned int factor = floorf(visual->opacity * 255.0f + 0.5f);
        int x, y;
        struct render_context layer_context = *context;

        if (!factor || context->width <= 0 || context->height <= 0
                || context->width > SIZE_MAX / context->height
                || (pixel_count = context->width * (size_t)context->height) > SIZE_MAX / sizeof(*layer)
                || !(layer = calloc(pixel_count, sizeof(*layer)))) return;
        layer_context.pixels = layer;
        layer_context.stride = context->width;
        render_visual(state, index, parent, &layer_context, active, depth,
                inherited_interpolation, TRUE);
        for (y = 0; y < context->height; ++y)
            for (x = 0; x < context->width; ++x)
            {
                uint32_t source = layer[y * context->width + x];
                uint32_t a = (source >> 24) * factor + 127;
                uint32_t r = (source >> 16 & 0xff) * factor + 127;
                uint32_t g = (source >> 8 & 0xff) * factor + 127;
                uint32_t b = (source & 0xff) * factor + 127;
                source = (a / 255) << 24 | (r / 255) << 16 | (g / 255) << 8 | b / 255;
                context->pixels[y * context->stride + x] = source_over(source,
                        context->pixels[y * context->stride + x]);
            }
        free(layer);
        return;
    }
    active[index] = 1;
    local = (struct matrix){1, 0, 0, 1, visual->offset_x, visual->offset_y};
    if (visual->flags & WINE_DCOMP_VISUAL_HAS_TRANSFORM)
    {
        local = (struct matrix){visual->transform[0], visual->transform[1], visual->transform[2],
                visual->transform[3], visual->transform[4] + visual->offset_x,
                visual->transform[5] + visual->offset_y};
    }
    transform = multiply_matrix(parent, local);
    if ((visual->flags & WINE_DCOMP_VISUAL_HAS_CLIP) && invert_matrix(transform, &inverse))
    {
        struct clip *clip = context->clips + context->clip_count++;
        clip->inverse = inverse;
        clip->left = visual->clip[0]; clip->top = visual->clip[1];
        clip->right = visual->clip[2]; clip->bottom = visual->clip[3];
        pushed = TRUE;
    }
    if (visual->content_kind != WINE_DCOMP_CONTENT_NONE && visual->content < state->surface_count)
        render_surface(context, state->surfaces + visual->content, transform, interpolation);
    for (child = visual->first_child; child != WINE_DCOMP_INVALID_INDEX && sibling_count++ < state->visual_count;
            child = state->visuals[child].next_sibling)
    {
        if (child >= state->visual_count || state->visuals[child].parent != index) break;
        render_visual(state, child, transform, context, active, depth + 1, interpolation, FALSE);
    }
    if (pushed) --context->clip_count;
    active[index] = 0;
}

static BOOL surface_client_offset(HWND surface, const RECT *surface_rect, POINT *offset)
{
    struct x11drv_win_data *data;
    if (!(data = get_win_data(surface))) return FALSE;
    offset->x = data->rects.client.left - data->rects.visible.left - surface_rect->left;
    offset->y = data->rects.client.top - data->rects.visible.top - surface_rect->top;
    release_win_data(data);
    return TRUE;
}

static BOOL target_origin(HWND target, HWND surface, POINT surface_offset, POINT *origin)
{
    HWND target_root = NtUserGetAncestor(target, GA_ROOT);
    HWND surface_root = NtUserGetAncestor(surface, GA_ROOT);
    if (!target_root || target_root != surface_root) return FALSE;
    origin->x = origin->y = 0;
    NtUserMapWindowPoints(target, surface, origin, 1, 0);
    origin->x += surface_offset.x;
    origin->y += surface_offset.y;
    return TRUE;
}

static void collect_window_shape_rects(HWND window, HWND surface, POINT surface_offset,
        RECT *rects, uint32_t *count)
{
    HRGN region = NtGdiCreateRectRgn(0, 0, 0, 0);
    RGNDATA *data = NULL;
    RECT window_rect;
    POINT origin;
    DWORD size;
    unsigned int i;

    if (!NtUserGetWindowRect(window, &window_rect, 0)) goto done;
    origin.x = window_rect.left; origin.y = window_rect.top;
    NtUserMapWindowPoints(0, surface, &origin, 1, 0);
    origin.x += surface_offset.x; origin.y += surface_offset.y;
    if (!region || NtUserGetWindowRgnEx(window, region, 0) == ERROR
            || !(size = NtGdiGetRegionData(region, 0, NULL))
            || !(data = malloc(size)) || !NtGdiGetRegionData(region, size, data))
    {
        NtUserMapWindowPoints(0, surface, (POINT *)&window_rect, 2, 0);
        OffsetRect(&window_rect, surface_offset.x, surface_offset.y);
        if (*count < MAX_SCENE_ITEMS) rects[(*count)++] = window_rect;
        goto done;
    }
    for (i = 0; i < data->rdh.nCount && *count < MAX_SCENE_ITEMS; ++i)
    {
        RECT rect = ((RECT *)data->Buffer)[i];
        OffsetRect(&rect, origin.x, origin.y);
        rects[(*count)++] = rect;
    }

done:
    free(data);
    if (region) NtGdiDeleteObjectApp(region);
}

static void collect_child_rects(HWND parent, HWND surface, POINT surface_offset,
        RECT *rects, uint32_t *count, uint32_t depth)
{
    HWND child;

    if (depth > MAX_SCENE_ITEMS) return;
    for (child = NtUserGetWindowRelative(parent, GW_CHILD); child && *count < MAX_SCENE_ITEMS;
            child = NtUserGetWindowRelative(child, GW_HWNDNEXT))
    {
        if (!(NtUserGetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        collect_window_shape_rects(child, surface, surface_offset, rects, count);
        collect_child_rects(child, surface, surface_offset, rects, count, depth + 1);
    }
}

static void collect_higher_window_rects(HWND target, HWND surface, POINT surface_offset,
        RECT *rects, uint32_t *count)
{
    HWND current = target;
    uint32_t depth = 0;

    while (current && current != surface && depth++ < MAX_SCENE_ITEMS)
    {
        HWND sibling;
        for (sibling = NtUserGetWindowRelative(current, GW_HWNDPREV);
                sibling && *count < MAX_SCENE_ITEMS;
                sibling = NtUserGetWindowRelative(sibling, GW_HWNDPREV))
        {
            if (!(NtUserGetWindowLongW(sibling, GWL_STYLE) & WS_VISIBLE)) continue;
            collect_window_shape_rects(sibling, surface, surface_offset, rects, count);
            collect_child_rects(sibling, surface, surface_offset, rects, count, 0);
        }
        current = NtUserGetAncestor(current, GA_PARENT);
    }
}

struct target_work
{
    struct dcomp_state *state;
    uint32_t index;
    HWND parent;
    uint32_t windows_above;
    BOOL topmost;
};

static int compare_target_work(const void *left, const void *right)
{
    const struct target_work *a = left, *b = right;
    if (a->topmost != b->topmost) return a->topmost - b->topmost;
    if (a->parent == b->parent && a->windows_above != b->windows_above)
        return a->windows_above < b->windows_above ? 1 : -1; /* bottom to top */
    if (a->state->publication != b->state->publication)
        return a->state->publication < b->state->publication ? -1 : 1;
    if (a->index != b->index) return a->index < b->index ? -1 : 1;
    return 0;
}

static BOOL render_target_work(const struct target_work *work, HWND hwnd, POINT surface_offset,
        uint32_t *pixels, int width, int height, int stride, POINT *trace_sample)
{
    const struct dcomp_state *state = work->state;
    const struct wine_dcomp_target *target = state->targets + work->index;
    struct render_context context = {.pixels = pixels, .width = width, .height = height, .stride = stride};
    struct matrix origin_matrix = {1, 0, 0, 1, 0, 0};
    uint8_t *active = NULL;
    RECT client;
    POINT origin;
    BOOL ret = FALSE;

    if (!target->hwnd || !InterlockedCompareExchange(state->target_live + work->index, 1, 1)
            || target->root_visual == WINE_DCOMP_INVALID_INDEX
            || !target_origin((HWND)(UINT_PTR)target->hwnd, hwnd, surface_offset, &origin)
            || !(active = calloc(state->visual_count, 1))
            || !(context.clips = calloc(state->visual_count + 1, sizeof(*context.clips)))) goto done;
    origin_matrix.x = origin.x; origin_matrix.y = origin.y;
    if (!NtUserGetClientRect((HWND)(UINT_PTR)target->hwnd, &client, 0)) goto done;
    OffsetRect(&client, origin.x, origin.y);
    if (TRACE_ON(dcomp))
    {
        struct x11drv_win_data *data = get_win_data((HWND)(UINT_PTR)target->hwnd);
        TRACE("target hwnd %p target_id %s origin %s client %s X whole %#lx client %#lx\n",
                (HWND)(UINT_PTR)target->hwnd, wine_dbgstr_longlong(target->target_id),
                wine_dbgstr_point(&origin), wine_dbgstr_rect(&client),
                data ? data->whole_window : 0, data ? data->client_window : 0);
        if (data) release_win_data(data);
    }
    context.clips[0] = (struct clip){{1, 0, 0, 1, 0, 0},
            client.left, client.top, client.right, client.bottom};
    context.clip_count = 1;
    if (!work->topmost && (context.exclusions = malloc(MAX_SCENE_ITEMS * sizeof(*context.exclusions))))
    {
        collect_child_rects((HWND)(UINT_PTR)target->hwnd, hwnd, surface_offset, context.exclusions,
                &context.exclusion_count, 0);
        collect_higher_window_rects((HWND)(UINT_PTR)target->hwnd, hwnd, surface_offset, context.exclusions,
                &context.exclusion_count);
    }
    if (trace_sample && trace_sample->x < 0 && client.left >= 0 && client.top >= 0
            && client.left < width && client.top < height)
    {
        trace_sample->x = client.left;
        trace_sample->y = client.top;
        TRACE("target-derived sample %s before render %#x\n", wine_dbgstr_point(trace_sample),
                pixels[trace_sample->y * stride + trace_sample->x]);
    }
    render_visual(state, target->root_visual, origin_matrix, &context, active, 0, 1, FALSE);
    if (trace_sample && trace_sample->x == client.left && trace_sample->y == client.top)
        TRACE("target-derived sample %s after target render %#x\n", wine_dbgstr_point(trace_sample),
                pixels[trace_sample->y * stride + trace_sample->x]);
    ret = TRUE;

done:
    free(context.exclusions);
    free(context.clips);
    free(active);
    return ret;
}

BOOL x11drv_dcomp_compose(HWND hwnd, const RECT *surface_rect,
        UINT32 *pixels, int width, int height, int stride, POINT *trace_sample)
{
    struct dcomp_state **snapshot = NULL, *state;
    struct target_work *work = NULL;
    uint32_t count = 0, work_count = 0, i, j;
    POINT surface_offset;
    BOOL rendered = FALSE;

    if (trace_sample) trace_sample->x = trace_sample->y = -1;

    TRACE("compose surface %p root %p rect %s extent %dx%d stride %d\n", hwnd,
            NtUserGetAncestor(hwnd, GA_ROOT), wine_dbgstr_rect(surface_rect), width, height, stride);
    if (NtUserGetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;
    if (!surface_client_offset(hwnd, surface_rect, &surface_offset)) return FALSE;

    pthread_mutex_lock(&scene_mutex);
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry) ++count;
    if (count && !(snapshot = calloc(count, sizeof(*snapshot)))) count = 0;
    i = 0;
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry)
    {
        if (i == count) break;
        state_addref(state);
        snapshot[i++] = state;
    }
    pthread_mutex_unlock(&scene_mutex);

    for (i = 0; i < count; ++i)
    {
        state = snapshot[i];
        if (state->target_count > UINT32_MAX - work_count) goto done;
        work_count += state->target_count;
    }
    if (work_count && !(work = calloc(work_count, sizeof(*work)))) goto done;
    work_count = 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < snapshot[i]->target_count; ++j)
        {
            const struct wine_dcomp_target *target = snapshot[i]->targets + j;
            HWND current, target_hwnd = (HWND)(UINT_PTR)target->hwnd;
            struct target_work *item = work + work_count++;
            item->state = snapshot[i]; item->index = j;
            item->topmost = !!(target->flags & WINE_DCOMP_TARGET_TOPMOST);
            item->parent = NtUserGetAncestor(target_hwnd, GA_PARENT);
            for (current = NtUserGetWindowRelative(target_hwnd, GW_HWNDPREV);
                    current && item->windows_above < MAX_SCENE_ITEMS;
                    current = NtUserGetWindowRelative(current, GW_HWNDPREV))
                ++item->windows_above;
        }
    qsort(work, work_count, sizeof(*work), compare_target_work);
    for (i = 0; i < work_count; ++i)
        rendered |= render_target_work(work + i, hwnd, surface_offset, pixels, width, height, stride,
                trace_sample);

done:
    free(work);
    for (i = 0; i < count; ++i) state_release(snapshot[i]);
    free(snapshot);
    TRACE("compose surface %p considered %u targets, rendered %u\n", hwnd, work_count, rendered);
    return rendered;
}

BOOL x11drv_dcomp_has_targets(HWND hwnd)
{
    struct dcomp_state **snapshot = NULL, *state;
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    uint32_t count = 0, i = 0, j;
    BOOL found = FALSE;

    /* Child HWND painting is already flattened into the top-level backing
     * surface.  Applying the retained scene to a child surface as well would
     * double-compose child targets and let a layered child occlude the result. */
    if (!root || hwnd != root)
    {
        TRACE("surface %p is not root %p; retained composition skipped\n", hwnd, root);
        return FALSE;
    }
    pthread_mutex_lock(&scene_mutex);
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry) ++count;
    if (count && !(snapshot = calloc(count, sizeof(*snapshot)))) count = 0;
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry)
    {
        if (i == count) break;
        state_addref(state);
        snapshot[i++] = state;
    }
    pthread_mutex_unlock(&scene_mutex);

    for (i = 0; i < count && !found; ++i)
        for (j = 0; j < snapshot[i]->target_count; ++j)
            if (snapshot[i]->targets[j].hwnd
                    && InterlockedCompareExchange(snapshot[i]->target_live + j, 1, 1)
                    && snapshot[i]->targets[j].root_visual != WINE_DCOMP_INVALID_INDEX
                    && NtUserGetAncestor((HWND)(UINT_PTR)snapshot[i]->targets[j].hwnd, GA_ROOT) == root)
            {
                TRACE("root surface %p has live target %p id %s\n", hwnd,
                        (HWND)(UINT_PTR)snapshot[i]->targets[j].hwnd,
                        wine_dbgstr_longlong(snapshot[i]->targets[j].target_id));
                found = TRUE;
                break;
            }
    for (i = 0; i < count; ++i) state_release(snapshot[i]);
    free(snapshot);
    return found;
}

void x11drv_dcomp_window_changed(HWND hwnd)
{
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    if (root && x11drv_dcomp_has_targets(root))
        NtUserExposeWindowSurface(root, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN, NULL);
}

void x11drv_dcomp_window_destroyed(HWND hwnd)
{
    struct dcomp_state *state;
    HWND root = NtUserGetAncestor(hwnd, GA_ROOT);
    BOOL relevant = root && x11drv_dcomp_has_targets(root);
    unsigned int i;

    pthread_mutex_lock(&scene_mutex);
    LIST_FOR_EACH_ENTRY(state, &states, struct dcomp_state, entry)
        for (i = 0; i < state->target_count; ++i)
            if (state->targets[i].hwnd == (UINT_PTR)hwnd)
                InterlockedExchange(state->target_live + i, 0);
    pthread_mutex_unlock(&scene_mutex);
    if (relevant)
        NtUserExposeWindowSurface(root, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN, NULL);
}
