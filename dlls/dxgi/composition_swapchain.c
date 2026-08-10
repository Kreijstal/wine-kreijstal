/*
 * Headless DXGI flip swapchains for DirectComposition surface handles.
 *
 * Copyright 2026 OpenTerminal contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "dxgi_private.h"
#include "winternl.h"
#include "wine/server.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

struct composition_swapchain
{
    IDXGISwapChain2 IDXGISwapChain2_iface;
    IWineDXGICompositionSwapChain IWineDXGICompositionSwapChain_iface;
    LONG refcount;
    CRITICAL_SECTION lock;
    struct wined3d_private_store private_store;
    IWineDXGIFactory *factory;
    IDXGIDevice *device;
    IDXGIOutput *restrict_to_output;
    IDXGISurface *buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    HANDLE shared_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ULONG buffer_ref_baseline[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    DXGI_SWAP_CHAIN_DESC1 desc;
    DXGI_RGBA background;
    DXGI_MATRIX_3X2_F matrix;
    DXGI_MODE_ROTATION rotation;
    HANDLE binding;
    HANDLE available_event;
    HANDLE surface;
    HANDLE latency_semaphore;
    UINT source_width;
    UINT source_height;
    UINT current_buffer;
    UINT present_count;
    UINT maximum_frame_latency;
    UINT memory_type_index;
    LUID adapter_luid;
    GUID device_uuid;
};

static inline struct composition_swapchain *impl_from_IDXGISwapChain2(IDXGISwapChain2 *iface)
{
    return CONTAINING_RECORD(iface, struct composition_swapchain, IDXGISwapChain2_iface);
}

static inline struct composition_swapchain *impl_from_IWineDXGICompositionSwapChain(
        IWineDXGICompositionSwapChain *iface)
{
    return CONTAINING_RECORD(iface, struct composition_swapchain,
            IWineDXGICompositionSwapChain_iface);
}

static HRESULT composition_status_to_hresult(NTSTATUS status)
{
    if (status == STATUS_DEVICE_BUSY)
        return DXGI_ERROR_INVALID_CALL;
    return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
}

static HRESULT composition_surface_bind(HANDLE surface, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const LUID *adapter_luid, const GUID *device_uuid, UINT memory_type_index,
        const HANDLE *buffers,
        HANDLE *binding, HANDLE *available_event)
{
    const UINT *uuid = (const UINT *)device_uuid;
    struct dcomp_surface_info info;
    obj_handle_t server_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    NTSTATUS status;
    UINT i;

    for (i = 0; i < desc->BufferCount; ++i)
        server_buffers[i] = wine_server_obj_handle(buffers[i]);
    memset(&info, 0, sizeof(info));
    info.width = desc->Width;
    info.height = desc->Height;
    info.format = desc->Format;
    info.alpha_mode = desc->AlphaMode;
    info.adapter_luid.low_part = adapter_luid->LowPart;
    info.adapter_luid.high_part = adapter_luid->HighPart;
    info.device_uuid0 = uuid[0];
    info.device_uuid1 = uuid[1];
    info.device_uuid2 = uuid[2];
    info.device_uuid3 = uuid[3];
    info.memory_type_index = memory_type_index;
    info.buffer_count = desc->BufferCount;

    *binding = NULL;
    *available_event = NULL;
    SERVER_START_REQ(dcomp_bind_surface)
    {
        req->surface = wine_server_obj_handle(surface);
        req->payload_size = sizeof(info) + desc->BufferCount * sizeof(*server_buffers);
        wine_server_add_data(req, &info, sizeof(info));
        wine_server_add_data(req, server_buffers,
                desc->BufferCount * sizeof(*server_buffers));
        if (!(status = wine_server_call(req)))
        {
            *binding = wine_server_ptr_handle(reply->binding);
            *available_event = wine_server_ptr_handle(reply->available_event);
        }
    }
    SERVER_END_REQ;

    return status ? composition_status_to_hresult(status) : S_OK;
}

static HRESULT composition_surface_update(HANDLE binding, const DXGI_SWAP_CHAIN_DESC1 *desc,
        const LUID *adapter_luid, const GUID *device_uuid, UINT memory_type_index,
        UINT front_buffer, BOOL resize,
        const HANDLE *buffers, HANDLE sync_handle, HANDLE available_event,
        UINT present_flags, UINT *next_buffer)
{
    const UINT *uuid = (const UINT *)device_uuid;
    struct dcomp_surface_info info;
    obj_handle_t server_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    NTSTATUS status;
    UINT i;

    if (resize)
        for (i = 0; i < desc->BufferCount; ++i)
            server_buffers[i] = wine_server_obj_handle(buffers[i]);
    memset(&info, 0, sizeof(info));
    info.width = desc->Width;
    info.height = desc->Height;
    info.format = desc->Format;
    info.alpha_mode = desc->AlphaMode;
    info.adapter_luid.low_part = adapter_luid->LowPart;
    info.adapter_luid.high_part = adapter_luid->HighPart;
    info.device_uuid0 = uuid[0];
    info.device_uuid1 = uuid[1];
    info.device_uuid2 = uuid[2];
    info.device_uuid3 = uuid[3];
    info.memory_type_index = memory_type_index;
    info.buffer_count = desc->BufferCount;
    info.front_buffer = front_buffer;
    info.resize = resize;
    info.sync_resource = wine_server_obj_handle(sync_handle);

    for (;;)
    {
        SERVER_START_REQ(dcomp_update_surface)
        {
            req->binding = wine_server_obj_handle(binding);
            req->payload_size = sizeof(info)
                    + (resize ? desc->BufferCount * sizeof(*server_buffers) : 0);
            wine_server_add_data(req, &info, sizeof(info));
            if (resize) wine_server_add_data(req, server_buffers,
                    desc->BufferCount * sizeof(*server_buffers));
            if (!(status = wine_server_call(req)) && next_buffer)
                *next_buffer = reply->next_buffer;
        }
        SERVER_END_REQ;
        if (status != STATUS_DEVICE_BUSY) break;
        if (present_flags & DXGI_PRESENT_DO_NOT_WAIT) return DXGI_ERROR_WAS_STILL_DRAWING;
        if (WaitForSingleObject(available_event, INFINITE) != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(GetLastError());
    }

    return status ? composition_status_to_hresult(status) : S_OK;
}

static void composition_swapchain_release_buffers(IDXGISurface **buffers, UINT count);

static HRESULT composition_swapchain_create_buffers(struct composition_swapchain *swapchain,
        const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGISurface **buffers, HANDLE *shared_buffers,
        ULONG *baselines, UINT *memory_type_index)
{
    IWineDXGIDevice *wine_device;
    DXGI_SURFACE_DESC surface_desc;
    HRESULT hr;
    UINT index, i = 0;

    surface_desc.Width = desc->Width;
    surface_desc.Height = desc->Height;
    surface_desc.Format = desc->Format;
    surface_desc.SampleDesc = desc->SampleDesc;
    memset(buffers, 0, sizeof(*buffers) * DXGI_MAX_SWAP_CHAIN_BUFFERS);
    memset(shared_buffers, 0, sizeof(*shared_buffers) * DXGI_MAX_SWAP_CHAIN_BUFFERS);

    if (FAILED(hr = IDXGIDevice_QueryInterface(swapchain->device, &IID_IWineDXGIDevice,
            (void **)&wine_device)))
        return hr;

    hr = IWineDXGIDevice_create_composition_surfaces(wine_device, &surface_desc,
            desc->BufferCount, desc->BufferUsage | DXGI_USAGE_BACK_BUFFER
            | DXGI_USAGE_SHADER_INPUT, buffers);
    if (FAILED(hr))
    {
        IWineDXGIDevice_Release(wine_device);
        return hr;
    }

    for (i = 0; i < desc->BufferCount; ++i)
    {
        if (FAILED(hr = IWineDXGIDevice_create_composition_shared_handle(wine_device,
                buffers[i], &shared_buffers[i], &index)))
            break;
        if (i && index != *memory_type_index)
        {
            CloseHandle(shared_buffers[i]);
            shared_buffers[i] = NULL;
            hr = E_NOTIMPL;
            break;
        }
        *memory_type_index = index;
        baselines[i] = IDXGISurface_AddRef(buffers[i]);
        IDXGISurface_Release(buffers[i]);
    }
    IWineDXGIDevice_Release(wine_device);
    if (i == desc->BufferCount) return S_OK;

    while (i--)
    {
        CloseHandle(shared_buffers[i]);
        shared_buffers[i] = NULL;
    }
    composition_swapchain_release_buffers(buffers, desc->BufferCount);
    return hr;
}

/* Both release helpers clear as they go. Buffer creation releases what it
 * already made when it gives up part way, and the caller's own failure path
 * then runs over the same arrays; leaving the released pointers behind would
 * release and close them a second time. */
static void composition_swapchain_release_buffers(IDXGISurface **buffers, UINT count)
{
    UINT i;

    for (i = 0; i < count; ++i)
    {
        if (!buffers[i]) continue;
        IDXGISurface_Release(buffers[i]);
        buffers[i] = NULL;
    }
}

static void composition_swapchain_release_shared_buffers(HANDLE *buffers, UINT count)
{
    UINT i;

    for (i = 0; i < count; ++i)
    {
        if (!buffers[i]) continue;
        CloseHandle(buffers[i]);
        buffers[i] = NULL;
    }
}

static BOOL composition_swapchain_buffers_referenced(struct composition_swapchain *swapchain)
{
    ULONG refcount;
    UINT i;

    for (i = 0; i < swapchain->desc.BufferCount; ++i)
    {
        refcount = IDXGISurface_AddRef(swapchain->buffers[i]);
        IDXGISurface_Release(swapchain->buffers[i]);
        if (refcount != swapchain->buffer_ref_baseline[i])
            return TRUE;
    }
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_QueryInterface(IDXGISwapChain2 *iface,
        REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDXGIObject)
            || IsEqualIID(iid, &IID_IDXGIDeviceSubObject)
            || IsEqualIID(iid, &IID_IDXGISwapChain)
            || IsEqualIID(iid, &IID_IDXGISwapChain1)
            || IsEqualIID(iid, &IID_IDXGISwapChain2))
    {
        IDXGISwapChain2_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    if (IsEqualIID(iid, &IID_IWineDXGICompositionSwapChain))
    {
        IDXGISwapChain2_AddRef(iface);
        *out = &impl_from_IDXGISwapChain2(iface)->IWineDXGICompositionSwapChain_iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE composition_swapchain_AddRef(IDXGISwapChain2 *iface)
{
    return InterlockedIncrement(&impl_from_IDXGISwapChain2(iface)->refcount);
}

static ULONG STDMETHODCALLTYPE composition_swapchain_Release(IDXGISwapChain2 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);
    ULONG refcount = InterlockedDecrement(&swapchain->refcount);

    if (!refcount)
    {
        CloseHandle(swapchain->binding);
        CloseHandle(swapchain->available_event);
        CloseHandle(swapchain->surface);
        composition_swapchain_release_buffers(swapchain->buffers, swapchain->desc.BufferCount);
        composition_swapchain_release_shared_buffers(swapchain->shared_buffers,
                swapchain->desc.BufferCount);
        if (swapchain->restrict_to_output) IDXGIOutput_Release(swapchain->restrict_to_output);
        IDXGIDevice_Release(swapchain->device);
        IWineDXGIFactory_Release(swapchain->factory);
        CloseHandle(swapchain->latency_semaphore);
        wined3d_private_store_cleanup(&swapchain->private_store);
        DeleteCriticalSection(&swapchain->lock);
        free(swapchain);
    }
    return refcount;
}

static HRESULT STDMETHODCALLTYPE composition_private_QueryInterface(
        IWineDXGICompositionSwapChain *iface, REFIID iid, void **out)
{
    return composition_swapchain_QueryInterface(
            &impl_from_IWineDXGICompositionSwapChain(iface)->IDXGISwapChain2_iface, iid, out);
}

static ULONG STDMETHODCALLTYPE composition_private_AddRef(IWineDXGICompositionSwapChain *iface)
{
    return composition_swapchain_AddRef(
            &impl_from_IWineDXGICompositionSwapChain(iface)->IDXGISwapChain2_iface);
}

static ULONG STDMETHODCALLTYPE composition_private_Release(IWineDXGICompositionSwapChain *iface)
{
    return composition_swapchain_Release(
            &impl_from_IWineDXGICompositionSwapChain(iface)->IDXGISwapChain2_iface);
}

static HRESULT STDMETHODCALLTYPE composition_private_get_composition_surface_handle(
        IWineDXGICompositionSwapChain *iface, HANDLE *handle)
{
    struct composition_swapchain *swapchain = impl_from_IWineDXGICompositionSwapChain(iface);

    if (!handle) return E_INVALIDARG;
    *handle = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), swapchain->surface, GetCurrentProcess(), handle,
            0, FALSE, DUPLICATE_SAME_ACCESS)) return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

static const IWineDXGICompositionSwapChainVtbl composition_private_vtbl =
{
    composition_private_QueryInterface,
    composition_private_AddRef,
    composition_private_Release,
    composition_private_get_composition_surface_handle,
};

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetPrivateData(IDXGISwapChain2 *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    return dxgi_set_private_data(&impl_from_IDXGISwapChain2(iface)->private_store,
            guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetPrivateDataInterface(IDXGISwapChain2 *iface,
        REFGUID guid, const IUnknown *object)
{
    return dxgi_set_private_data_interface(&impl_from_IDXGISwapChain2(iface)->private_store,
            guid, object);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetPrivateData(IDXGISwapChain2 *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    return dxgi_get_private_data(&impl_from_IDXGISwapChain2(iface)->private_store,
            guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetParent(IDXGISwapChain2 *iface,
        REFIID iid, void **parent)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    return IWineDXGIFactory_QueryInterface(swapchain->factory, iid, parent);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDevice(IDXGISwapChain2 *iface,
        REFIID iid, void **device)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    return IDXGIDevice_QueryInterface(swapchain->device, iid, device);
}

static HRESULT composition_swapchain_present(struct composition_swapchain *swapchain,
        UINT sync_interval, UINT flags)
{
    IWineDXGIDevice *wine_device;
    HANDLE sync_handle = NULL;
    UINT next_buffer;
    HRESULT hr;

    if (sync_interval > 4 || flags & ~(DXGI_PRESENT_TEST | DXGI_PRESENT_DO_NOT_SEQUENCE
            | DXGI_PRESENT_DO_NOT_WAIT))
        return DXGI_ERROR_INVALID_CALL;
    if (flags & DXGI_PRESENT_TEST)
        return S_OK;

    EnterCriticalSection(&swapchain->lock);
    if (SUCCEEDED(hr = IDXGIDevice_QueryInterface(swapchain->device,
            &IID_IWineDXGIDevice, (void **)&wine_device)))
    {
        hr = IWineDXGIDevice_publish_composition_surface(wine_device,
                swapchain->buffers[swapchain->current_buffer], &sync_handle);
        IWineDXGIDevice_Release(wine_device);
    }
    if (SUCCEEDED(hr))
        hr = composition_surface_update(swapchain->binding, &swapchain->desc,
                &swapchain->adapter_luid, &swapchain->device_uuid,
                swapchain->memory_type_index,
                swapchain->current_buffer, FALSE, NULL, sync_handle,
                swapchain->available_event,
                flags, &next_buffer);
    if (sync_handle) CloseHandle(sync_handle);
    if (SUCCEEDED(hr))
    {
        swapchain->current_buffer = next_buffer;
        ++swapchain->present_count;
        if (!ReleaseSemaphore(swapchain->latency_semaphore, 1, NULL)
                && GetLastError() != ERROR_TOO_MANY_POSTS)
            WARN("Failed to signal frame latency semaphore, error %lu.\n", GetLastError());
    }
    LeaveCriticalSection(&swapchain->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_Present(IDXGISwapChain2 *iface,
        UINT sync_interval, UINT flags)
{
    return composition_swapchain_present(impl_from_IDXGISwapChain2(iface), sync_interval, flags);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetBuffer(IDXGISwapChain2 *iface,
        UINT buffer_idx, REFIID iid, void **surface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);
    HRESULT hr;

    if (!surface) return DXGI_ERROR_INVALID_CALL;
    *surface = NULL;

    EnterCriticalSection(&swapchain->lock);
    if (buffer_idx >= swapchain->desc.BufferCount)
        hr = DXGI_ERROR_INVALID_CALL;
    else
        hr = IDXGISurface_QueryInterface(swapchain->buffers[
                (swapchain->current_buffer + buffer_idx) % swapchain->desc.BufferCount], iid, surface);
    LeaveCriticalSection(&swapchain->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetFullscreenState(IDXGISwapChain2 *iface,
        BOOL fullscreen, IDXGIOutput *target)
{
    return fullscreen ? DXGI_ERROR_INVALID_CALL : S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFullscreenState(IDXGISwapChain2 *iface,
        BOOL *fullscreen, IDXGIOutput **target)
{
    if (!fullscreen && !target) return DXGI_ERROR_INVALID_CALL;
    if (fullscreen) *fullscreen = FALSE;
    if (target) *target = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDesc1(IDXGISwapChain2 *iface,
        DXGI_SWAP_CHAIN_DESC1 *desc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!desc) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *desc = swapchain->desc;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetDesc(IDXGISwapChain2 *iface,
        DXGI_SWAP_CHAIN_DESC *desc)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!desc) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    memset(desc, 0, sizeof(*desc));
    desc->BufferDesc.Width = swapchain->desc.Width;
    desc->BufferDesc.Height = swapchain->desc.Height;
    desc->BufferDesc.Format = swapchain->desc.Format;
    desc->SampleDesc = swapchain->desc.SampleDesc;
    desc->BufferUsage = swapchain->desc.BufferUsage;
    desc->BufferCount = swapchain->desc.BufferCount;
    desc->OutputWindow = NULL;
    desc->Windowed = TRUE;
    desc->SwapEffect = swapchain->desc.SwapEffect;
    desc->Flags = swapchain->desc.Flags;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_ResizeBuffers(IDXGISwapChain2 *iface,
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);
    IDXGISurface *new_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    IDXGISurface *old_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    HANDLE new_shared_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    HANDLE old_shared_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    ULONG new_baselines[DXGI_MAX_SWAP_CHAIN_BUFFERS];
    DXGI_SWAP_CHAIN_DESC1 desc;
    UINT new_memory_type_index, old_count;
    HRESULT hr;

    EnterCriticalSection(&swapchain->lock);
    if (composition_swapchain_buffers_referenced(swapchain))
    {
        LeaveCriticalSection(&swapchain->lock);
        return DXGI_ERROR_INVALID_CALL;
    }

    desc = swapchain->desc;
    if (buffer_count) desc.BufferCount = buffer_count;
    if (width) desc.Width = width;
    if (height) desc.Height = height;
    if (format != DXGI_FORMAT_UNKNOWN) desc.Format = format;
    desc.Flags = flags;
    if (!dxgi_validate_swapchain_desc(&desc) || !desc.Width || !desc.Height
            || desc.SwapEffect < DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
    {
        LeaveCriticalSection(&swapchain->lock);
        return DXGI_ERROR_INVALID_CALL;
    }

    if (FAILED(hr = composition_swapchain_create_buffers(swapchain, &desc,
            new_buffers, new_shared_buffers, new_baselines, &new_memory_type_index)))
    {
        LeaveCriticalSection(&swapchain->lock);
        return hr;
    }
    if (FAILED(hr = composition_surface_update(swapchain->binding, &desc,
            &swapchain->adapter_luid, &swapchain->device_uuid, new_memory_type_index, 0, TRUE,
            new_shared_buffers, NULL, swapchain->available_event, 0,
            &swapchain->current_buffer)))
    {
        composition_swapchain_release_buffers(new_buffers, desc.BufferCount);
        composition_swapchain_release_shared_buffers(new_shared_buffers, desc.BufferCount);
        LeaveCriticalSection(&swapchain->lock);
        return hr;
    }

    old_count = swapchain->desc.BufferCount;
    memcpy(old_buffers, swapchain->buffers, sizeof(old_buffers));
    memcpy(old_shared_buffers, swapchain->shared_buffers, sizeof(old_shared_buffers));
    memcpy(swapchain->buffers, new_buffers, sizeof(new_buffers));
    memcpy(swapchain->shared_buffers, new_shared_buffers, sizeof(new_shared_buffers));
    memcpy(swapchain->buffer_ref_baseline, new_baselines, sizeof(new_baselines));
    swapchain->desc = desc;
    swapchain->memory_type_index = new_memory_type_index;
    swapchain->source_width = desc.Width;
    swapchain->source_height = desc.Height;
    LeaveCriticalSection(&swapchain->lock);

    composition_swapchain_release_buffers(old_buffers, old_count);
    composition_swapchain_release_shared_buffers(old_shared_buffers, old_count);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_ResizeTarget(IDXGISwapChain2 *iface,
        const DXGI_MODE_DESC *target_mode_desc)
{
    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetContainingOutput(IDXGISwapChain2 *iface,
        IDXGIOutput **output)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!output) return DXGI_ERROR_INVALID_CALL;
    *output = swapchain->restrict_to_output;
    if (!*output) return DXGI_ERROR_UNSUPPORTED;
    IDXGIOutput_AddRef(*output);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFrameStatistics(IDXGISwapChain2 *iface,
        DXGI_FRAME_STATISTICS *stats)
{
    if (!stats) return DXGI_ERROR_INVALID_CALL;
    memset(stats, 0, sizeof(*stats));
    return DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetLastPresentCount(IDXGISwapChain2 *iface,
        UINT *present_count)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!present_count) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *present_count = swapchain->present_count;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetFullscreenDesc(IDXGISwapChain2 *iface,
        DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc)
{
    if (!desc) return DXGI_ERROR_INVALID_CALL;
    memset(desc, 0, sizeof(*desc));
    desc->Windowed = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetHwnd(IDXGISwapChain2 *iface, HWND *hwnd)
{
    if (!hwnd) return DXGI_ERROR_INVALID_CALL;
    *hwnd = NULL;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetCoreWindow(IDXGISwapChain2 *iface,
        REFIID iid, void **window)
{
    if (!window) return DXGI_ERROR_INVALID_CALL;
    *window = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_Present1(IDXGISwapChain2 *iface,
        UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS *parameters)
{
    return composition_swapchain_present(impl_from_IDXGISwapChain2(iface), sync_interval, flags);
}

static BOOL STDMETHODCALLTYPE composition_swapchain_IsTemporaryMonoSupported(IDXGISwapChain2 *iface)
{
    return FALSE;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetRestrictToOutput(IDXGISwapChain2 *iface,
        IDXGIOutput **output)
{
    return composition_swapchain_GetContainingOutput(iface, output);
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetBackgroundColor(IDXGISwapChain2 *iface,
        const DXGI_RGBA *color)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!color) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    swapchain->background = *color;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetBackgroundColor(IDXGISwapChain2 *iface,
        DXGI_RGBA *color)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!color) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *color = swapchain->background;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetRotation(IDXGISwapChain2 *iface,
        DXGI_MODE_ROTATION rotation)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (rotation < DXGI_MODE_ROTATION_IDENTITY || rotation > DXGI_MODE_ROTATION_ROTATE270)
        return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    swapchain->rotation = rotation;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetRotation(IDXGISwapChain2 *iface,
        DXGI_MODE_ROTATION *rotation)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!rotation) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *rotation = swapchain->rotation;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetSourceSize(IDXGISwapChain2 *iface,
        UINT width, UINT height)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    EnterCriticalSection(&swapchain->lock);
    if (!width || !height || width > swapchain->desc.Width || height > swapchain->desc.Height)
    {
        LeaveCriticalSection(&swapchain->lock);
        return DXGI_ERROR_INVALID_CALL;
    }
    swapchain->source_width = width;
    swapchain->source_height = height;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetSourceSize(IDXGISwapChain2 *iface,
        UINT *width, UINT *height)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!width || !height) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *width = swapchain->source_width;
    *height = swapchain->source_height;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetMaximumFrameLatency(IDXGISwapChain2 *iface,
        UINT maximum_latency)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);
    UINT old_latency;
    UINT i;

    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
            || !maximum_latency || maximum_latency > DXGI_FRAME_LATENCY_MAX)
        return DXGI_ERROR_INVALID_CALL;

    EnterCriticalSection(&swapchain->lock);
    old_latency = swapchain->maximum_frame_latency;
    if (maximum_latency > old_latency)
        ReleaseSemaphore(swapchain->latency_semaphore, maximum_latency - old_latency, NULL);
    else
        for (i = maximum_latency; i < old_latency; ++i)
            WaitForSingleObject(swapchain->latency_semaphore, 0);
    swapchain->maximum_frame_latency = maximum_latency;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetMaximumFrameLatency(IDXGISwapChain2 *iface,
        UINT *maximum_latency)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!maximum_latency) return DXGI_ERROR_INVALID_CALL;
    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *maximum_latency = swapchain->maximum_frame_latency;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HANDLE STDMETHODCALLTYPE composition_swapchain_GetFrameLatencyWaitableObject(
        IDXGISwapChain2 *iface)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);
    HANDLE handle = NULL;

    if (!(swapchain->desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
        return NULL;
    if (!DuplicateHandle(GetCurrentProcess(), swapchain->latency_semaphore, GetCurrentProcess(),
            &handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
        WARN("Failed to duplicate frame latency semaphore, error %lu.\n", GetLastError());
    return handle;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_SetMatrixTransform(IDXGISwapChain2 *iface,
        const DXGI_MATRIX_3X2_F *matrix)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!matrix) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    swapchain->matrix = *matrix;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE composition_swapchain_GetMatrixTransform(IDXGISwapChain2 *iface,
        DXGI_MATRIX_3X2_F *matrix)
{
    struct composition_swapchain *swapchain = impl_from_IDXGISwapChain2(iface);

    if (!matrix) return DXGI_ERROR_INVALID_CALL;
    EnterCriticalSection(&swapchain->lock);
    *matrix = swapchain->matrix;
    LeaveCriticalSection(&swapchain->lock);
    return S_OK;
}

static const IDXGISwapChain2Vtbl composition_swapchain_vtbl =
{
    composition_swapchain_QueryInterface,
    composition_swapchain_AddRef,
    composition_swapchain_Release,
    composition_swapchain_SetPrivateData,
    composition_swapchain_SetPrivateDataInterface,
    composition_swapchain_GetPrivateData,
    composition_swapchain_GetParent,
    composition_swapchain_GetDevice,
    composition_swapchain_Present,
    composition_swapchain_GetBuffer,
    composition_swapchain_SetFullscreenState,
    composition_swapchain_GetFullscreenState,
    composition_swapchain_GetDesc,
    composition_swapchain_ResizeBuffers,
    composition_swapchain_ResizeTarget,
    composition_swapchain_GetContainingOutput,
    composition_swapchain_GetFrameStatistics,
    composition_swapchain_GetLastPresentCount,
    composition_swapchain_GetDesc1,
    composition_swapchain_GetFullscreenDesc,
    composition_swapchain_GetHwnd,
    composition_swapchain_GetCoreWindow,
    composition_swapchain_Present1,
    composition_swapchain_IsTemporaryMonoSupported,
    composition_swapchain_GetRestrictToOutput,
    composition_swapchain_SetBackgroundColor,
    composition_swapchain_GetBackgroundColor,
    composition_swapchain_SetRotation,
    composition_swapchain_GetRotation,
    composition_swapchain_SetSourceSize,
    composition_swapchain_GetSourceSize,
    composition_swapchain_SetMaximumFrameLatency,
    composition_swapchain_GetMaximumFrameLatency,
    composition_swapchain_GetFrameLatencyWaitableObject,
    composition_swapchain_SetMatrixTransform,
    composition_swapchain_GetMatrixTransform,
};

HRESULT composition_swapchain_create(IWineDXGIFactory *factory, IUnknown *device, HANDLE surface,
        const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to_output,
        IDXGISwapChain1 **swapchain)
{
    struct composition_swapchain *object;
    struct wine_dxgi_adapter_info adapter_info;
    IWineDXGIAdapter *wine_adapter = NULL;
    IDXGIAdapter *adapter = NULL;
    HRESULT hr;

    if (!swapchain) return DXGI_ERROR_INVALID_CALL;
    *swapchain = NULL;
    if (!device || !surface || !desc || !desc->Width || !desc->Height
            || !dxgi_validate_swapchain_desc(desc)
            || desc->SwapEffect < DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL)
        return DXGI_ERROR_INVALID_CALL;

    if (!(object = calloc(1, sizeof(*object)))) return E_OUTOFMEMORY;
    object->IDXGISwapChain2_iface.lpVtbl = &composition_swapchain_vtbl;
    object->IWineDXGICompositionSwapChain_iface.lpVtbl = &composition_private_vtbl;
    object->refcount = 1;
    object->factory = factory;
    object->desc = *desc;
    object->source_width = desc->Width;
    object->source_height = desc->Height;
    object->maximum_frame_latency = 1;
    object->matrix._11 = 1.0f;
    object->matrix._22 = 1.0f;
    object->rotation = DXGI_MODE_ROTATION_IDENTITY;
    InitializeCriticalSection(&object->lock);
    wined3d_private_store_init(&object->private_store);
    IWineDXGIFactory_AddRef(object->factory);
    if (!DuplicateHandle(GetCurrentProcess(), surface, GetCurrentProcess(), &object->surface,
            0, FALSE, DUPLICATE_SAME_ACCESS))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }

    if (FAILED(hr = IUnknown_QueryInterface(device, &IID_IDXGIDevice, (void **)&object->device)))
        goto fail;
    if (FAILED(hr = IDXGIDevice_GetAdapter(object->device, &adapter))) goto fail;
    hr = IDXGIAdapter_QueryInterface(adapter, &IID_IWineDXGIAdapter,
            (void **)&wine_adapter);
    if (SUCCEEDED(hr)) hr = IWineDXGIAdapter_get_adapter_info(wine_adapter, &adapter_info);
    if (wine_adapter) IWineDXGIAdapter_Release(wine_adapter);
    IDXGIAdapter_Release(adapter);
    adapter = NULL;
    if (FAILED(hr)) goto fail;
    object->adapter_luid = adapter_info.luid;
    object->device_uuid = adapter_info.device_uuid;
    if (restrict_to_output)
    {
        object->restrict_to_output = restrict_to_output;
        IDXGIOutput_AddRef(restrict_to_output);
    }
    if (!(object->latency_semaphore = CreateSemaphoreW(NULL, 1,
            DXGI_FRAME_LATENCY_MAX, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto fail;
    }
    if (FAILED(hr = composition_swapchain_create_buffers(object, desc,
            object->buffers, object->shared_buffers, object->buffer_ref_baseline,
            &object->memory_type_index)))
        goto fail;
    if (FAILED(hr = composition_surface_bind(surface, desc, &object->adapter_luid,
            &object->device_uuid, object->memory_type_index,
            object->shared_buffers,
            &object->binding, &object->available_event)))
        goto fail;

    *swapchain = (IDXGISwapChain1 *)&object->IDXGISwapChain2_iface;
    return S_OK;

fail:
    if (adapter) IDXGIAdapter_Release(adapter);
    if (object->binding) CloseHandle(object->binding);
    if (object->available_event) CloseHandle(object->available_event);
    if (object->surface) CloseHandle(object->surface);
    composition_swapchain_release_buffers(object->buffers, object->desc.BufferCount);
    composition_swapchain_release_shared_buffers(object->shared_buffers,
            object->desc.BufferCount);
    if (object->latency_semaphore) CloseHandle(object->latency_semaphore);
    if (object->restrict_to_output) IDXGIOutput_Release(object->restrict_to_output);
    if (object->device) IDXGIDevice_Release(object->device);
    IWineDXGIFactory_Release(object->factory);
    wined3d_private_store_cleanup(&object->private_store);
    DeleteCriticalSection(&object->lock);
    free(object);
    return hr;
}
