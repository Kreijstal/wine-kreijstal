/*
 * DirectComposition display-driver transaction ABI.
 *
 * The blob is immutable and self-contained for the duration of the callback.
 * Resource values delivered to the display driver are Vulkan OPAQUE_FD file
 * descriptors, not dma-bufs or linear mappings. The producer buffer is leased
 * for the callback, and the descriptor is valid only for the callback. The
 * driver must finish importing and consuming the source before returning; a
 * duplicated descriptor does not extend the source-buffer lease. A driver must
 * publish the complete update atomically or return FALSE without changing its
 * scene.
 */
#ifndef __WINE_DCOMP_DRIVER_H
#define __WINE_DCOMP_DRIVER_H

#include <stdint.h>

#define WINE_DCOMP_DRIVER_ABI_VERSION 2
#define WINE_DCOMP_INVALID_INDEX UINT32_MAX

enum wine_dcomp_update_kind
{
    WINE_DCOMP_UPDATE_COMMIT = 1,
    WINE_DCOMP_UPDATE_SURFACES = 2,
    WINE_DCOMP_UPDATE_DESTROY = 3,
};

enum wine_dcomp_target_flags
{
    WINE_DCOMP_TARGET_TOPMOST = 0x1,
};

enum wine_dcomp_visual_flags
{
    WINE_DCOMP_VISUAL_HAS_TRANSFORM = 0x1,
    WINE_DCOMP_VISUAL_HAS_CLIP = 0x2,
};

enum wine_dcomp_content_kind
{
    WINE_DCOMP_CONTENT_NONE = 0,
    WINE_DCOMP_CONTENT_HANDLE_SURFACE = 1,
    WINE_DCOMP_CONTENT_LOCAL_SURFACE = 2,
    WINE_DCOMP_CONTENT_SWAPCHAIN = 3,
};

enum wine_dcomp_surface_flags
{
    WINE_DCOMP_SURFACE_HAS_FRONT = 0x1,
};

enum wine_dcomp_resource_type
{
    /* Private PE-to-win32u ingress only; never delivered to a display driver. */
    WINE_DCOMP_RESOURCE_WIN32_HANDLE = 1,
    /* Vulkan VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, not a dma-buf. */
    WINE_DCOMP_RESOURCE_OPAQUE_FD = 2,
};

enum wine_dcomp_sync_type
{
    /* Exported Vulkan timeline semaphore; sync_value is the publication value. */
    WINE_DCOMP_SYNC_TIMELINE = 1,
    /* No sync object at all: the producer waited for its device to go idle on
     * the host before the publication reached the server, so the buffer holds
     * the finished contents by the time a consumer can see it.  This is what a
     * driver that cannot export a shareable timeline semaphore falls back to;
     * sync_resource and sync_value are zero. */
    WINE_DCOMP_SYNC_HOST_IDLE = 2,
};

enum wine_dcomp_image_usage
{
    WINE_DCOMP_IMAGE_TRANSFER_SRC = 0x1,
    WINE_DCOMP_IMAGE_TRANSFER_DST = 0x2,
    WINE_DCOMP_IMAGE_SAMPLED = 0x4,
    WINE_DCOMP_IMAGE_COLOR_ATTACHMENT = 0x8,
};

enum wine_dcomp_image_flags
{
    WINE_DCOMP_IMAGE_ALIAS = 0x1,
    WINE_DCOMP_IMAGE_MUTABLE_FORMAT = 0x2,
    WINE_DCOMP_IMAGE_OPTIMAL_TILING = 0x4,
    WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION = 0x8,
};

#include "pshpack8.h"

struct wine_dcomp_scene
{
    uint32_t abi_version;
    uint32_t byte_size;
    uint32_t update_kind;
    uint32_t reserved;
    uint64_t device_id;
    uint64_t generation;
    uint32_t target_count;
    uint32_t target_offset;
    uint32_t visual_count;
    uint32_t visual_offset;
    uint32_t surface_count;
    uint32_t surface_offset;
};

struct wine_dcomp_target
{
    uint64_t hwnd;
    uint64_t target_id;
    uint32_t root_visual;
    uint32_t flags;
};

struct wine_dcomp_visual
{
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t content;
    uint32_t content_kind;
    uint32_t flags;
    uint32_t interpolation_mode;
    uint32_t border_mode;
    uint32_t composite_mode;
    uint32_t opacity_mode;
    uint32_t backface_visibility;
    uint32_t reserved;
    float opacity;
    float offset_x;
    float offset_y;
    float transform[6];
    float clip[4];
};

struct wine_dcomp_surface
{
    uint64_t resource;
    uint64_t sync_resource;
    uint64_t sync_value;
    uint64_t adapter_luid;
    uint8_t device_uuid[16];
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t alpha_mode;
    uint32_t resource_type;
    uint32_t sync_resource_type;
    uint32_t sync_type;
    uint32_t memory_type_index;
    uint32_t image_usage;
    uint32_t image_flags;
    uint32_t sample_count;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t front_buffer;
    uint32_t buffer_count;
    uint32_t generation;
    uint32_t flags;
    int32_t damage_left;
    int32_t damage_top;
    int32_t damage_right;
    int32_t damage_bottom;
};

#include "poppack.h"

#endif /* __WINE_DCOMP_DRIVER_H */
