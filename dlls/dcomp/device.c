/*
 * Copyright 2020 Nikolay Sivov for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "initguid.h"
#include "objidl.h"
#include "dcomp.h"
#include "dxgi.h"
#include "ntuser.h"
struct wined3d_resource;
struct wined3d_texture;
#include "wine/server.h"
#include "wine/list.h"
#include "wine/dcomp_driver.h"
#include "wine/debug.h"
#include "wine/winedxgi.h"

WINE_DEFAULT_DEBUG_CHANNEL(dcomp);

HRESULT WINAPI DCompositionCreateSurfaceHandle(DWORD access,
        SECURITY_ATTRIBUTES *security_attributes, HANDLE *surface_handle);

struct composition_device
{
    IDCompositionDevice IDCompositionDevice_iface;
    IDCompositionDesktopDevice IDCompositionDesktopDevice_iface;
    IUnknown *rendering_device;
    CRITICAL_SECTION lock;
    CRITICAL_SECTION commit_lock;
    LONG object_refs;
    LONG destroying;
    LONG ref;
    struct list targets;
    UINT64 generation;
    UINT64 device_id;
    HANDLE subscription_event;
    HANDLE subscription;
    HANDLE stop_event;
    HANDLE worker;
    HANDLE *committed_surfaces;
    IUnknown **committed_surface_objects;
    BOOL *committed_surface_local;
    UINT committed_surface_count;
    struct wine_dcomp_scene *committed_scene;
};

struct composition_target;

enum composition_content_kind
{
    COMPOSITION_CONTENT_NONE,
    COMPOSITION_CONTENT_HANDLE_SURFACE,
    COMPOSITION_CONTENT_LOCAL_SURFACE,
    COMPOSITION_CONTENT_SWAPCHAIN,
};

struct composition_surface
{
    IDCompositionSurface IDCompositionSurface_iface;
    struct composition_device *device;
    IDXGISurface *dxgi_surface;
    IWineDXGIDevice *wine_device;
    HANDLE composition_handle;
    HANDLE binding;
    HANDLE available_event;
    HANDLE shared_handle;
    struct dcomp_surface_info producer_info;
    CRITICAL_SECTION lock;
    RECT pending_damage;
    RECT committed_damage;
    UINT width;
    UINT height;
    DXGI_ALPHA_MODE alpha_mode;
    UINT generation;
    BOOL has_committed_damage;
    BOOL drawing;
    BOOL suspended;
    LONG committed_refs;
    LONG ref;
};

struct composition_visual
{
    IDCompositionVisual2 IDCompositionVisual2_iface;
    struct composition_device *device;
    struct composition_visual *parent;
    struct composition_visual *first_child;
    struct composition_visual *last_child;
    struct composition_visual *previous;
    struct composition_visual *next;
    struct composition_target *root_target;
    IUnknown *content;
    HANDLE content_handle;
    IDCompositionEffectGroup *effect;
    enum composition_content_kind content_kind;
    D2D_MATRIX_3X2_F transform;
    D2D_RECT_F clip;
    float opacity;
    float offset_x;
    float offset_y;
    enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode;
    enum DCOMPOSITION_BORDER_MODE border_mode;
    enum DCOMPOSITION_COMPOSITE_MODE composite_mode;
    enum DCOMPOSITION_OPACITY_MODE opacity_mode;
    enum DCOMPOSITION_BACKFACE_VISIBILITY backface_visibility;
    BOOL has_transform;
    BOOL has_clip;
    LONG graph_refs;
    LONG ref;
};

struct composition_effect_group
{
    IDCompositionEffectGroup IDCompositionEffectGroup_iface;
    struct composition_device *device;
    float opacity;
    LONG ref;
};

struct composition_target
{
    IDCompositionTarget IDCompositionTarget_iface;
    struct composition_device *device;
    struct composition_visual *root;
    HWND hwnd;
    UINT64 target_id;
    BOOL topmost;
    LONG ref;
    struct list entry;
};

struct composition_handle_surface
{
    IUnknown IUnknown_iface;
    HANDLE handle;
    LONG ref;
};

static inline struct composition_surface *surface_from_IDCompositionSurface(
        IDCompositionSurface *iface);

static inline struct composition_device *device_from_IDCompositionDevice(IDCompositionDevice *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDevice_iface);
}

static inline struct composition_device *device_from_IDCompositionDesktopDevice(
        IDCompositionDesktopDevice *iface)
{
    return CONTAINING_RECORD(iface, struct composition_device, IDCompositionDesktopDevice_iface);
}

static inline struct composition_handle_surface *surface_from_IUnknown(IUnknown *iface)
{
    return CONTAINING_RECORD(iface, struct composition_handle_surface, IUnknown_iface);
}

static HRESULT STDMETHODCALLTYPE handle_surface_QueryInterface(IUnknown *iface, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualIID(iid, &IID_IUnknown))
    {
        IUnknown_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE handle_surface_AddRef(IUnknown *iface)
{
    struct composition_handle_surface *surface = surface_from_IUnknown(iface);
    return InterlockedIncrement(&surface->ref);
}

static ULONG STDMETHODCALLTYPE handle_surface_Release(IUnknown *iface)
{
    struct composition_handle_surface *surface = surface_from_IUnknown(iface);
    ULONG ref = InterlockedDecrement(&surface->ref);

    if (!ref)
    {
        CloseHandle(surface->handle);
        free(surface);
    }
    return ref;
}

static const IUnknownVtbl handle_surface_vtbl =
{
    handle_surface_QueryInterface,
    handle_surface_AddRef,
    handle_surface_Release,
};

static HRESULT create_handle_surface(HANDLE handle, IUnknown **out)
{
    struct composition_handle_surface *surface;
    HANDLE retained = NULL;
    NTSTATUS status;

    if (!out) return E_INVALIDARG;
    *out = NULL;

    SERVER_START_REQ(dcomp_open_surface)
    {
        req->handle = wine_server_obj_handle(handle);
        if (!(status = wine_server_call(req)))
            retained = wine_server_ptr_handle(reply->handle);
    }
    SERVER_END_REQ;

    if (status)
        return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));

    if (!(surface = calloc(1, sizeof(*surface))))
    {
        CloseHandle(retained);
        return E_OUTOFMEMORY;
    }

    surface->IUnknown_iface.lpVtbl = &handle_surface_vtbl;
    surface->handle = retained;
    surface->ref = 1;
    *out = &surface->IUnknown_iface;
    return S_OK;
}

static void device_try_destroy(struct composition_device *device)
{
    UINT committed_local_count = 0, i;
    BOOL destroy = FALSE;

    if (device->ref || device->destroying) return;

    EnterCriticalSection(&device->commit_lock);
    if (!device->ref && !device->destroying)
    {
        for (i = 0; i < device->committed_surface_count; ++i)
        {
            struct composition_surface *surface;
            LONG committed_refs, refs;

            if (!device->committed_surface_local[i]) continue;
            surface = surface_from_IDCompositionSurface(
                    (IDCompositionSurface *)device->committed_surface_objects[i]);
            committed_refs = InterlockedCompareExchange(&surface->committed_refs, 0, 0);
            refs = InterlockedCompareExchange(&surface->ref, 0, 0);
            if (!committed_refs || refs != committed_refs) break;
            ++committed_local_count;
        }
        if (i == device->committed_surface_count
                && device->object_refs == committed_local_count
                && !InterlockedCompareExchange(&device->destroying, 1, 0))
            destroy = TRUE;
    }
    LeaveCriticalSection(&device->commit_lock);
    if (!destroy) return;

    if (device->stop_event) SetEvent(device->stop_event);
    if (device->worker)
    {
        WaitForSingleObject(device->worker, INFINITE);
        CloseHandle(device->worker);
    }
    if (device->subscription) CloseHandle(device->subscription);
    if (device->subscription_event) CloseHandle(device->subscription_event);
    if (device->stop_event) CloseHandle(device->stop_event);
    while (device->committed_surface_count)
    {
        --device->committed_surface_count;
        CloseHandle(device->committed_surfaces[device->committed_surface_count]);
        if (device->committed_surface_local[device->committed_surface_count])
        {
            struct composition_surface *surface = surface_from_IDCompositionSurface(
                    (IDCompositionSurface *)device->committed_surface_objects[
                    device->committed_surface_count]);
            InterlockedDecrement(&surface->committed_refs);
        }
        IUnknown_Release(device->committed_surface_objects[device->committed_surface_count]);
    }
    free(device->committed_surfaces);
    free(device->committed_surface_objects);
    free(device->committed_surface_local);
    free(device->committed_scene);
    DeleteCriticalSection(&device->commit_lock);
    DeleteCriticalSection(&device->lock);
    if (device->rendering_device) IUnknown_Release(device->rendering_device);
    free(device);
}

static void device_object_addref(struct composition_device *device)
{
    InterlockedIncrement(&device->object_refs);
}

static void device_object_release(struct composition_device *device, ULONG count)
{
    while (count--) InterlockedDecrement(&device->object_refs);
    device_try_destroy(device);
}

static inline struct composition_surface *surface_from_IDCompositionSurface(
        IDCompositionSurface *iface)
{
    return CONTAINING_RECORD(iface, struct composition_surface, IDCompositionSurface_iface);
}

static HRESULT STDMETHODCALLTYPE surface_QueryInterface(IDCompositionSurface *iface,
        REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDCompositionSurface))
    {
        IDCompositionSurface_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE surface_AddRef(IDCompositionSurface *iface)
{
    return InterlockedIncrement(&surface_from_IDCompositionSurface(iface)->ref);
}

static ULONG STDMETHODCALLTYPE surface_Release(IDCompositionSurface *iface)
{
    struct composition_surface *surface = surface_from_IDCompositionSurface(iface);
    struct composition_device *device = surface->device;
    ULONG ref = InterlockedDecrement(&surface->ref);

    if (!ref)
    {
        if (surface->binding) CloseHandle(surface->binding);
        if (surface->available_event) CloseHandle(surface->available_event);
        if (surface->composition_handle) CloseHandle(surface->composition_handle);
        if (surface->shared_handle) CloseHandle(surface->shared_handle);
        if (surface->wine_device) IWineDXGIDevice_Release(surface->wine_device);
        IDXGISurface_Release(surface->dxgi_surface);
        DeleteCriticalSection(&surface->lock);
        free(surface);
        device_object_release(device, 1);
    }
    else if (ref == InterlockedCompareExchange(&surface->committed_refs, 0, 0))
        device_try_destroy(device);
    return ref;
}

static HRESULT STDMETHODCALLTYPE surface_BeginDraw(IDCompositionSurface *iface, const RECT *rect,
        REFIID iid, void **object, POINT *offset)
{
    struct composition_surface *surface = surface_from_IDCompositionSurface(iface);
    RECT update;
    HRESULT hr;

    if (!object || !offset) return E_INVALIDARG;
    *object = NULL;
    offset->x = offset->y = 0;
    if (rect) update = *rect;
    else SetRect(&update, 0, 0, surface->width, surface->height);
    if (update.left < 0 || update.top < 0 || update.right > surface->width
            || update.bottom > surface->height || IsRectEmpty(&update))
        return E_INVALIDARG;

    EnterCriticalSection(&surface->lock);
    if (surface->drawing)
        hr = DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
    else if (SUCCEEDED(hr = IDXGISurface_QueryInterface(surface->dxgi_surface, iid, object)))
    {
        surface->pending_damage = update;
        surface->drawing = TRUE;
        surface->suspended = FALSE;
    }
    LeaveCriticalSection(&surface->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_EndDraw(IDCompositionSurface *iface)
{
    struct composition_surface *surface = surface_from_IDCompositionSurface(iface);
    HANDLE sync_handle = NULL;
    HRESULT hr = S_OK;

    EnterCriticalSection(&surface->lock);
    if (!surface->drawing || surface->suspended)
        hr = DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    else
    {
        if (FAILED(hr = IWineDXGIDevice_publish_composition_surface(surface->wine_device,
                surface->dxgi_surface, &sync_handle)))
        {
            surface->drawing = FALSE;
            LeaveCriticalSection(&surface->lock);
            return hr;
        }
        SERVER_START_REQ(dcomp_update_surface)
        {
            NTSTATUS status;

            surface->producer_info.front_buffer = 0;
            surface->producer_info.resize = 0;
            surface->producer_info.sync_resource = wine_server_obj_handle(sync_handle);
            req->binding = wine_server_obj_handle(surface->binding);
            req->payload_size = sizeof(surface->producer_info);
            wine_server_add_data(req, &surface->producer_info,
                    sizeof(surface->producer_info));
            if ((status = wine_server_call(req)))
                hr = HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
        }
        SERVER_END_REQ;
        CloseHandle(sync_handle);
        sync_handle = NULL;
        if (FAILED(hr))
        {
            surface->drawing = FALSE;
            LeaveCriticalSection(&surface->lock);
            return hr;
        }
        if (surface->has_committed_damage)
            UnionRect(&surface->committed_damage, &surface->committed_damage,
                    &surface->pending_damage);
        else
        {
            surface->committed_damage = surface->pending_damage;
            surface->has_committed_damage = TRUE;
        }
        ++surface->generation;
        surface->drawing = FALSE;
    }
    LeaveCriticalSection(&surface->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_SuspendDraw(IDCompositionSurface *iface)
{
    struct composition_surface *surface = surface_from_IDCompositionSurface(iface);
    HRESULT hr = S_OK;

    EnterCriticalSection(&surface->lock);
    if (!surface->drawing || surface->suspended)
        hr = DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    else
        surface->suspended = TRUE;
    LeaveCriticalSection(&surface->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_ResumeDraw(IDCompositionSurface *iface)
{
    struct composition_surface *surface = surface_from_IDCompositionSurface(iface);
    HRESULT hr = S_OK;

    EnterCriticalSection(&surface->lock);
    if (!surface->drawing || !surface->suspended)
        hr = DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED;
    else
        surface->suspended = FALSE;
    LeaveCriticalSection(&surface->lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE surface_Scroll(IDCompositionSurface *iface, const RECT *scroll,
        const RECT *clip, int offset_x, int offset_y)
{
    FIXME("iface %p, scroll %s, clip %s, offset %d,%d, no GPU copy backend.\n",
            iface, wine_dbgstr_rect(scroll), wine_dbgstr_rect(clip), offset_x, offset_y);
    return E_NOTIMPL;
}

static const IDCompositionSurfaceVtbl surface_vtbl =
{
    surface_QueryInterface,
    surface_AddRef,
    surface_Release,
    surface_BeginDraw,
    surface_EndDraw,
    surface_SuspendDraw,
    surface_ResumeDraw,
    surface_Scroll,
};

static HRESULT create_surface(struct composition_device *device, UINT width, UINT height,
        DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **out)
{
    struct composition_surface *surface;
    DXGI_SURFACE_DESC desc;
    struct wine_dxgi_adapter_info adapter_info;
    IWineDXGIAdapter *wine_adapter = NULL;
    IDXGIAdapter *adapter = NULL;
    obj_handle_t buffer;
    IDXGIDevice *dxgi_device;
    UINT memory_type_index;
    NTSTATUS status;
    HRESULT hr;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!width || !height || (alpha_mode != DXGI_ALPHA_MODE_PREMULTIPLIED
            && alpha_mode != DXGI_ALPHA_MODE_IGNORE))
        return E_INVALIDARG;
    if (format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM)
        return E_INVALIDARG;
    if (!device->rendering_device
            || FAILED(hr = IUnknown_QueryInterface(device->rendering_device,
            &IID_IDXGIDevice, (void **)&dxgi_device)))
        return E_NOINTERFACE;

    if (!(surface = calloc(1, sizeof(*surface))))
    {
        IDXGIDevice_Release(dxgi_device);
        return E_OUTOFMEMORY;
    }
    desc.Width = width;
    desc.Height = height;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    hr = IDXGIDevice_QueryInterface(dxgi_device, &IID_IWineDXGIDevice,
            (void **)&surface->wine_device);
    if (SUCCEEDED(hr)) hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    if (SUCCEEDED(hr)) hr = IDXGIAdapter_QueryInterface(adapter, &IID_IWineDXGIAdapter,
            (void **)&wine_adapter);
    if (SUCCEEDED(hr)) hr = IWineDXGIAdapter_get_adapter_info(wine_adapter, &adapter_info);
    if (SUCCEEDED(hr)) hr = IWineDXGIDevice_create_composition_surfaces(surface->wine_device,
            &desc, 1, DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT,
            &surface->dxgi_surface);
    if (SUCCEEDED(hr)) hr = IWineDXGIDevice_create_composition_shared_handle(
            surface->wine_device, surface->dxgi_surface, &surface->shared_handle,
            &memory_type_index);
    if (SUCCEEDED(hr)) hr = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS,
            NULL, &surface->composition_handle);
    if (wine_adapter) IWineDXGIAdapter_Release(wine_adapter);
    if (adapter) IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_device);
    if (FAILED(hr))
    {
        if (surface->composition_handle) CloseHandle(surface->composition_handle);
        if (surface->shared_handle) CloseHandle(surface->shared_handle);
        if (surface->dxgi_surface) IDXGISurface_Release(surface->dxgi_surface);
        if (surface->wine_device) IWineDXGIDevice_Release(surface->wine_device);
        free(surface);
        return hr;
    }

    memset(&surface->producer_info, 0, sizeof(surface->producer_info));
    surface->producer_info.width = width;
    surface->producer_info.height = height;
    surface->producer_info.format = format;
    surface->producer_info.alpha_mode = alpha_mode;
    surface->producer_info.adapter_luid.low_part = adapter_info.luid.LowPart;
    surface->producer_info.adapter_luid.high_part = adapter_info.luid.HighPart;
    memcpy(&surface->producer_info.device_uuid0, &adapter_info.device_uuid,
            sizeof(adapter_info.device_uuid));
    surface->producer_info.memory_type_index = memory_type_index;
    surface->producer_info.buffer_count = 1;
    buffer = wine_server_obj_handle(surface->shared_handle);
    SERVER_START_REQ(dcomp_bind_surface)
    {
        req->surface = wine_server_obj_handle(surface->composition_handle);
        req->payload_size = sizeof(surface->producer_info) + sizeof(buffer);
        wine_server_add_data(req, &surface->producer_info, sizeof(surface->producer_info));
        wine_server_add_data(req, &buffer, sizeof(buffer));
        if (!(status = wine_server_call(req)))
        {
            surface->binding = wine_server_ptr_handle(reply->binding);
            surface->available_event = wine_server_ptr_handle(reply->available_event);
        }
    }
    SERVER_END_REQ;
    if (status)
    {
        CloseHandle(surface->composition_handle);
        if (surface->available_event) CloseHandle(surface->available_event);
        CloseHandle(surface->shared_handle);
        IDXGISurface_Release(surface->dxgi_surface);
        IWineDXGIDevice_Release(surface->wine_device);
        free(surface);
        return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
    }

    surface->IDCompositionSurface_iface.lpVtbl = &surface_vtbl;
    surface->device = device;
    surface->width = width;
    surface->height = height;
    surface->alpha_mode = alpha_mode;
    surface->ref = 1;
    InitializeCriticalSection(&surface->lock);
    device_object_addref(device);
    *out = &surface->IDCompositionSurface_iface;
    return S_OK;
}

static inline struct composition_effect_group *effect_group_from_IDCompositionEffectGroup(
        IDCompositionEffectGroup *iface)
{
    return CONTAINING_RECORD(iface, struct composition_effect_group,
            IDCompositionEffectGroup_iface);
}

static HRESULT STDMETHODCALLTYPE effect_group_QueryInterface(IDCompositionEffectGroup *iface,
        REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDCompositionEffect)
            || IsEqualIID(iid, &IID_IDCompositionEffectGroup))
    {
        IDCompositionEffectGroup_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE effect_group_AddRef(IDCompositionEffectGroup *iface)
{
    return InterlockedIncrement(&effect_group_from_IDCompositionEffectGroup(iface)->ref);
}

static ULONG STDMETHODCALLTYPE effect_group_Release(IDCompositionEffectGroup *iface)
{
    struct composition_effect_group *group = effect_group_from_IDCompositionEffectGroup(iface);
    struct composition_device *device = group->device;
    ULONG ref = InterlockedDecrement(&group->ref);

    if (!ref)
    {
        free(group);
        device_object_release(device, 1);
    }
    return ref;
}

static HRESULT STDMETHODCALLTYPE effect_group_SetOpacityAnimation(IDCompositionEffectGroup *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p, animations are not retained yet.\n", iface, animation);
    return animation ? E_NOTIMPL : S_OK;
}

static HRESULT STDMETHODCALLTYPE effect_group_SetOpacity(IDCompositionEffectGroup *iface,
        float opacity)
{
    struct composition_effect_group *group = effect_group_from_IDCompositionEffectGroup(iface);

    if (!isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) return E_INVALIDARG;
    EnterCriticalSection(&group->device->lock);
    group->opacity = opacity;
    LeaveCriticalSection(&group->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE effect_group_SetTransform3D(IDCompositionEffectGroup *iface,
        IDCompositionTransform3D *transform)
{
    FIXME("iface %p, transform %p, effect transforms are not retained yet.\n", iface, transform);
    return transform ? E_NOTIMPL : S_OK;
}

static const IDCompositionEffectGroupVtbl effect_group_vtbl =
{
    effect_group_QueryInterface,
    effect_group_AddRef,
    effect_group_Release,
    effect_group_SetOpacityAnimation,
    effect_group_SetOpacity,
    effect_group_SetTransform3D,
};

static HRESULT create_effect_group(struct composition_device *device,
        IDCompositionEffectGroup **out)
{
    struct composition_effect_group *group;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(group = calloc(1, sizeof(*group)))) return E_OUTOFMEMORY;
    group->IDCompositionEffectGroup_iface.lpVtbl = &effect_group_vtbl;
    group->device = device;
    group->opacity = 1.0f;
    group->ref = 1;
    device_object_addref(device);
    *out = &group->IDCompositionEffectGroup_iface;
    return S_OK;
}

static const IDCompositionVisual2Vtbl visual_vtbl;

static inline struct composition_visual *visual_from_IDCompositionVisual2(IDCompositionVisual2 *iface)
{
    return CONTAINING_RECORD(iface, struct composition_visual, IDCompositionVisual2_iface);
}

static struct composition_visual *visual_from_public(IDCompositionVisual *iface)
{
    if (!iface || iface->lpVtbl != (IDCompositionVisualVtbl *)&visual_vtbl) return NULL;
    return CONTAINING_RECORD((IDCompositionVisual2 *)iface,
            struct composition_visual, IDCompositionVisual2_iface);
}

static ULONG visual_destroy_locked(struct composition_visual *visual)
{
    struct composition_visual *child, *next;
    ULONG destroyed = 1;

    for (child = visual->first_child; child; child = next)
    {
        next = child->next;
        child->parent = NULL;
        child->previous = NULL;
        child->next = NULL;
        if (!--child->graph_refs && !child->ref)
            destroyed += visual_destroy_locked(child);
    }

    if (visual->content) IUnknown_Release(visual->content);
    if (visual->content_handle) CloseHandle(visual->content_handle);
    if (visual->effect) IDCompositionEffectGroup_Release(visual->effect);
    free(visual);
    return destroyed;
}

static ULONG visual_release_graph_locked(struct composition_visual *visual)
{
    if (!--visual->graph_refs && !visual->ref)
        return visual_destroy_locked(visual);
    return 0;
}

static HRESULT STDMETHODCALLTYPE visual_QueryInterface(IDCompositionVisual2 *iface,
        REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDCompositionVisual)
            || IsEqualIID(iid, &IID_IDCompositionVisual2))
    {
        IDCompositionVisual2_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE visual_AddRef(IDCompositionVisual2 *iface)
{
    return InterlockedIncrement(&visual_from_IDCompositionVisual2(iface)->ref);
}

static ULONG STDMETHODCALLTYPE visual_Release(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    struct composition_device *device = visual->device;
    ULONG destroyed = 0;
    ULONG ref;

    EnterCriticalSection(&device->lock);
    ref = InterlockedDecrement(&visual->ref);
    if (!ref)
        if (!visual->graph_refs) destroyed = visual_destroy_locked(visual);
    LeaveCriticalSection(&device->lock);
    if (destroyed) device_object_release(device, destroyed);
    return ref;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetXAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p, animations are not retained yet.\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetX(IDCompositionVisual2 *iface, float offset)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    EnterCriticalSection(&visual->device->lock);
    visual->offset_x = offset;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetYAnimation(IDCompositionVisual2 *iface,
        IDCompositionAnimation *animation)
{
    FIXME("iface %p, animation %p, animations are not retained yet.\n", iface, animation);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE visual_SetOffsetY(IDCompositionVisual2 *iface, float offset)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    EnterCriticalSection(&visual->device->lock);
    visual->offset_y = offset;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformObject(IDCompositionVisual2 *iface,
        IDCompositionTransform *transform)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (transform) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->has_transform = FALSE;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransform(IDCompositionVisual2 *iface,
        const D2D_MATRIX_3X2_F *matrix)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (!matrix) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->transform = *matrix;
    visual->has_transform = TRUE;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetTransformParent(IDCompositionVisual2 *iface,
        IDCompositionVisual *parent)
{
    FIXME("iface %p, parent %p, transform-parent dependencies are not retained yet.\n", iface, parent);
    return parent ? E_INVALIDARG : S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetEffect(IDCompositionVisual2 *iface,
        IDCompositionEffect *effect)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    IDCompositionEffectGroup *group = NULL, *previous;

    if (effect && FAILED(IDCompositionEffect_QueryInterface(effect,
            &IID_IDCompositionEffectGroup, (void **)&group)))
        return E_INVALIDARG;
    if (group && (group->lpVtbl != &effect_group_vtbl
            || effect_group_from_IDCompositionEffectGroup(group)->device != visual->device))
    {
        IDCompositionEffectGroup_Release(group);
        return E_INVALIDARG;
    }

    EnterCriticalSection(&visual->device->lock);
    previous = visual->effect;
    visual->effect = group;
    LeaveCriticalSection(&visual->device->lock);
    if (previous) IDCompositionEffectGroup_Release(previous);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetBitmapInterpolationMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE mode)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
            && mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR
            && mode != DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->interpolation_mode = mode;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetBorderMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BORDER_MODE mode)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (mode != DCOMPOSITION_BORDER_MODE_SOFT && mode != DCOMPOSITION_BORDER_MODE_HARD
            && mode != DCOMPOSITION_BORDER_MODE_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->border_mode = mode;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetClipObject(IDCompositionVisual2 *iface,
        IDCompositionClip *clip)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (clip) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->has_clip = FALSE;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetClip(IDCompositionVisual2 *iface, const D2D_RECT_F *rect)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (!rect) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->clip = *rect;
    visual->has_clip = TRUE;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetContent(IDCompositionVisual2 *iface, IUnknown *content)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    enum composition_content_kind content_kind = COMPOSITION_CONTENT_NONE;
    IDCompositionSurface *local_surface = NULL;
    IWineDXGICompositionSwapChain *composition_swapchain = NULL;
    IDXGISwapChain *swapchain = NULL;
    HANDLE content_handle = NULL, previous_handle;
    IUnknown *previous;

    if (content)
    {
        if (content->lpVtbl == &handle_surface_vtbl)
            content_kind = COMPOSITION_CONTENT_HANDLE_SURFACE;
        else if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDCompositionSurface,
                (void **)&local_surface)))
        {
            if (local_surface->lpVtbl != &surface_vtbl
                    || surface_from_IDCompositionSurface(local_surface)->device != visual->device)
            {
                IDCompositionSurface_Release(local_surface);
                return E_INVALIDARG;
            }
            IDCompositionSurface_Release(local_surface);
            content_kind = COMPOSITION_CONTENT_LOCAL_SURFACE;
        }
        else if (SUCCEEDED(IUnknown_QueryInterface(content, &IID_IDXGISwapChain,
                (void **)&swapchain)))
        {
            IDXGISwapChain_Release(swapchain);
            if (FAILED(IUnknown_QueryInterface(content, &IID_IWineDXGICompositionSwapChain,
                    (void **)&composition_swapchain)))
                return E_INVALIDARG;
            if (FAILED(IWineDXGICompositionSwapChain_get_composition_surface_handle(
                    composition_swapchain, &content_handle)))
            {
                IWineDXGICompositionSwapChain_Release(composition_swapchain);
                return E_INVALIDARG;
            }
            IWineDXGICompositionSwapChain_Release(composition_swapchain);
            content_kind = COMPOSITION_CONTENT_SWAPCHAIN;
        }
        else
            return E_INVALIDARG;
    }
    if (content) IUnknown_AddRef(content);

    EnterCriticalSection(&visual->device->lock);
    previous = visual->content;
    previous_handle = visual->content_handle;
    visual->content = content;
    visual->content_handle = content_handle;
    visual->content_kind = content_kind;
    LeaveCriticalSection(&visual->device->lock);

    if (previous) IUnknown_Release(previous);
    if (previous_handle) CloseHandle(previous_handle);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_AddVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *child_iface, BOOL insert_above, IDCompositionVisual *reference_iface)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    struct composition_visual *child = visual_from_public(child_iface);
    struct composition_visual *reference = visual_from_public(reference_iface);
    struct composition_visual *ancestor;
    HRESULT hr = S_OK;

    if (!child || (reference_iface && !reference)) return E_INVALIDARG;
    if (child->device != visual->device || (reference && reference->device != visual->device))
        return E_INVALIDARG;

    EnterCriticalSection(&visual->device->lock);
    if (child->parent || child->root_target)
        hr = E_INVALIDARG;
    else if (reference && reference->parent != visual)
        hr = E_INVALIDARG;
    else
    {
        for (ancestor = visual; ancestor; ancestor = ancestor->parent)
            if (ancestor == child) break;
        if (ancestor)
            hr = E_INVALIDARG;
    }

    if (SUCCEEDED(hr))
    {
        if (reference)
        {
            if (insert_above)
            {
                child->previous = reference;
                child->next = reference->next;
            }
            else
            {
                child->previous = reference->previous;
                child->next = reference;
            }
        }
        else if (insert_above)
        {
            child->previous = visual->last_child;
            child->next = NULL;
        }
        else
        {
            child->previous = NULL;
            child->next = visual->first_child;
        }

        if (child->previous) child->previous->next = child;
        else visual->first_child = child;
        if (child->next) child->next->previous = child;
        else visual->last_child = child;
        child->parent = visual;
        ++child->graph_refs;
    }
    LeaveCriticalSection(&visual->device->lock);
    return hr;
}

static ULONG visual_detach_locked(struct composition_visual *visual, struct composition_visual *child)
{
    if (child->previous) child->previous->next = child->next;
    else visual->first_child = child->next;
    if (child->next) child->next->previous = child->previous;
    else visual->last_child = child->previous;
    child->parent = NULL;
    child->previous = NULL;
    child->next = NULL;
    return visual_release_graph_locked(child);
}

static HRESULT STDMETHODCALLTYPE visual_RemoveVisual(IDCompositionVisual2 *iface,
        IDCompositionVisual *child_iface)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    struct composition_visual *child = visual_from_public(child_iface);
    ULONG destroyed = 0;
    HRESULT hr = S_OK;

    if (!child || child->device != visual->device) return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    if (child->parent != visual) hr = E_INVALIDARG;
    else destroyed = visual_detach_locked(visual, child);
    LeaveCriticalSection(&visual->device->lock);
    if (destroyed) device_object_release(visual->device, destroyed);
    return hr;
}

static HRESULT STDMETHODCALLTYPE visual_RemoveAllVisuals(IDCompositionVisual2 *iface)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);
    struct composition_device *device = visual->device;
    ULONG destroyed = 0;

    EnterCriticalSection(&device->lock);
    while (visual->first_child)
        destroyed += visual_detach_locked(visual, visual->first_child);
    LeaveCriticalSection(&device->lock);
    if (destroyed) device_object_release(device, destroyed);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetCompositeMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_COMPOSITE_MODE mode)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (mode != DCOMPOSITION_COMPOSITE_MODE_SOURCE_OVER
            && mode != DCOMPOSITION_COMPOSITE_MODE_DESTINATION_INVERT
            && mode != DCOMPOSITION_COMPOSITE_MODE_MIN_BLEND
            && mode != DCOMPOSITION_COMPOSITE_MODE_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->composite_mode = mode;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetOpacityMode(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_OPACITY_MODE mode)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (mode != DCOMPOSITION_OPACITY_MODE_LAYER && mode != DCOMPOSITION_OPACITY_MODE_MULTIPLY
            && mode != DCOMPOSITION_OPACITY_MODE_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->opacity_mode = mode;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE visual_SetBackFaceVisibility(IDCompositionVisual2 *iface,
        enum DCOMPOSITION_BACKFACE_VISIBILITY visibility)
{
    struct composition_visual *visual = visual_from_IDCompositionVisual2(iface);

    if (visibility != DCOMPOSITION_BACKFACE_VISIBILITY_VISIBLE
            && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN
            && visibility != DCOMPOSITION_BACKFACE_VISIBILITY_INHERIT)
        return E_INVALIDARG;
    EnterCriticalSection(&visual->device->lock);
    visual->backface_visibility = visibility;
    LeaveCriticalSection(&visual->device->lock);
    return S_OK;
}

static const IDCompositionVisual2Vtbl visual_vtbl =
{
    visual_QueryInterface,
    visual_AddRef,
    visual_Release,
    visual_SetOffsetXAnimation,
    visual_SetOffsetX,
    visual_SetOffsetYAnimation,
    visual_SetOffsetY,
    visual_SetTransformObject,
    visual_SetTransform,
    visual_SetTransformParent,
    visual_SetEffect,
    visual_SetBitmapInterpolationMode,
    visual_SetBorderMode,
    visual_SetClipObject,
    visual_SetClip,
    visual_SetContent,
    visual_AddVisual,
    visual_RemoveVisual,
    visual_RemoveAllVisuals,
    visual_SetCompositeMode,
    visual_SetOpacityMode,
    visual_SetBackFaceVisibility,
};

static HRESULT create_visual(struct composition_device *device, IDCompositionVisual2 **out)
{
    struct composition_visual *visual;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!(visual = calloc(1, sizeof(*visual)))) return E_OUTOFMEMORY;

    visual->IDCompositionVisual2_iface.lpVtbl = &visual_vtbl;
    visual->device = device;
    visual->transform._11 = 1.0f;
    visual->transform._22 = 1.0f;
    visual->opacity = 1.0f;
    visual->interpolation_mode = DCOMPOSITION_BITMAP_INTERPOLATION_MODE_INHERIT;
    visual->border_mode = DCOMPOSITION_BORDER_MODE_INHERIT;
    visual->composite_mode = DCOMPOSITION_COMPOSITE_MODE_INHERIT;
    visual->opacity_mode = DCOMPOSITION_OPACITY_MODE_INHERIT;
    visual->backface_visibility = DCOMPOSITION_BACKFACE_VISIBILITY_INHERIT;
    visual->ref = 1;
    device_object_addref(device);
    *out = &visual->IDCompositionVisual2_iface;
    return S_OK;
}

static const WCHAR target_topmost_property[] = L"WineDirectCompositionTargetTopmost";
static const WCHAR target_normal_property[] = L"WineDirectCompositionTargetNormal";
static SRWLOCK target_property_lock = SRWLOCK_INIT;

static inline struct composition_target *target_from_IDCompositionTarget(IDCompositionTarget *iface)
{
    return CONTAINING_RECORD(iface, struct composition_target, IDCompositionTarget_iface);
}

static HRESULT STDMETHODCALLTYPE target_QueryInterface(IDCompositionTarget *iface,
        REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDCompositionTarget))
    {
        IDCompositionTarget_AddRef(iface);
        *out = iface;
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE target_AddRef(IDCompositionTarget *iface)
{
    return InterlockedIncrement(&target_from_IDCompositionTarget(iface)->ref);
}

static ULONG STDMETHODCALLTYPE target_Release(IDCompositionTarget *iface)
{
    struct composition_target *target = target_from_IDCompositionTarget(iface);
    struct composition_device *device = target->device;
    const WCHAR *property = target->topmost ? target_topmost_property : target_normal_property;
    ULONG destroyed = 0;
    ULONG ref = InterlockedDecrement(&target->ref);

    if (ref) return ref;
    EnterCriticalSection(&device->lock);
    list_remove(&target->entry);
    if (target->root)
    {
        target->root->root_target = NULL;
        destroyed = visual_release_graph_locked(target->root);
        target->root = NULL;
    }
    LeaveCriticalSection(&device->lock);

    AcquireSRWLockExclusive(&target_property_lock);
    if (GetPropW(target->hwnd, property) == (HANDLE)target)
        RemovePropW(target->hwnd, property);
    ReleaseSRWLockExclusive(&target_property_lock);
    free(target);
    device_object_release(device, destroyed + 1);
    return 0;
}

static HRESULT STDMETHODCALLTYPE target_SetRoot(IDCompositionTarget *iface,
        IDCompositionVisual *root_iface)
{
    struct composition_target *target = target_from_IDCompositionTarget(iface);
    struct composition_visual *root = visual_from_public(root_iface);
    struct composition_visual *previous;
    ULONG destroyed = 0;

    if (root_iface && (!root || root->device != target->device)) return E_INVALIDARG;
    EnterCriticalSection(&target->device->lock);
    if (root && root != target->root && (root->parent || root->root_target))
    {
        LeaveCriticalSection(&target->device->lock);
        return E_INVALIDARG;
    }
    if (root == target->root)
    {
        LeaveCriticalSection(&target->device->lock);
        return S_OK;
    }

    previous = target->root;
    target->root = root;
    if (root)
    {
        root->root_target = target;
        ++root->graph_refs;
    }
    if (previous)
    {
        previous->root_target = NULL;
        destroyed = visual_release_graph_locked(previous);
    }
    LeaveCriticalSection(&target->device->lock);
    if (destroyed) device_object_release(target->device, destroyed);
    return S_OK;
}

static const IDCompositionTargetVtbl target_vtbl =
{
    target_QueryInterface,
    target_AddRef,
    target_Release,
    target_SetRoot,
};

static HRESULT create_target(struct composition_device *device, HWND hwnd, BOOL topmost,
        IDCompositionTarget **out)
{
    static LONG64 next_target_id;
    struct composition_target *target;
    const WCHAR *property = topmost ? target_topmost_property : target_normal_property;
    DWORD process_id = 0;

    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!hwnd || hwnd == GetDesktopWindow() || !IsWindow(hwnd)) return E_INVALIDARG;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != GetCurrentProcessId()) return E_ACCESSDENIED;
    if (!(target = calloc(1, sizeof(*target)))) return E_OUTOFMEMORY;

    target->IDCompositionTarget_iface.lpVtbl = &target_vtbl;
    target->device = device;
    target->hwnd = hwnd;
    target->target_id = InterlockedIncrement64(&next_target_id);
    target->topmost = topmost;
    target->ref = 1;
    AcquireSRWLockExclusive(&target_property_lock);
    if (GetPropW(hwnd, property))
    {
        ReleaseSRWLockExclusive(&target_property_lock);
        free(target);
        return DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED;
    }
    if (!SetPropW(hwnd, property, (HANDLE)target))
    {
        ReleaseSRWLockExclusive(&target_property_lock);
        free(target);
        return HRESULT_FROM_WIN32(GetLastError());
    }
    ReleaseSRWLockExclusive(&target_property_lock);
    EnterCriticalSection(&device->lock);
    list_add_tail(&device->targets, &target->entry);
    LeaveCriticalSection(&device->lock);
    device_object_addref(device);
    *out = &target->IDCompositionTarget_iface;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE device_QueryInterface(IDCompositionDevice *iface, REFIID iid, void **out)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);

    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDCompositionDevice))
        *out = &device->IDCompositionDevice_iface;
    else if (IsEqualIID(iid, &IID_IDCompositionDevice2)
            || IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        *out = &device->IDCompositionDesktopDevice_iface;
    else
        return E_NOINTERFACE;

    IDCompositionDevice_AddRef(&device->IDCompositionDevice_iface);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE device_AddRef(IDCompositionDevice *iface)
{
    return InterlockedIncrement(&device_from_IDCompositionDevice(iface)->ref);
}

static ULONG STDMETHODCALLTYPE device_Release(IDCompositionDevice *iface)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);
    ULONG ref = InterlockedDecrement(&device->ref);

    if (!ref) device_try_destroy(device);
    return ref;
}

struct commit_target
{
    HWND hwnd;
    UINT64 target_id;
    BOOL topmost;
    const struct composition_visual *root;
};

struct commit_visual
{
    const struct composition_visual *identity;
    const struct composition_visual *parent;
    const struct composition_visual *first_child;
    const struct composition_visual *next;
    enum composition_content_kind content_kind;
    UINT content;
    D2D_MATRIX_3X2_F transform;
    D2D_RECT_F clip;
    float opacity;
    float offset_x;
    float offset_y;
    enum DCOMPOSITION_BITMAP_INTERPOLATION_MODE interpolation_mode;
    enum DCOMPOSITION_BORDER_MODE border_mode;
    enum DCOMPOSITION_COMPOSITE_MODE composite_mode;
    enum DCOMPOSITION_OPACITY_MODE opacity_mode;
    enum DCOMPOSITION_BACKFACE_VISIBILITY backface_visibility;
    BOOL has_transform;
    BOOL has_clip;
};

struct commit_graph
{
    struct commit_target *targets;
    struct commit_visual *visuals;
    HANDLE *surface_handles;
    IUnknown **surface_identities;
    BOOL *surface_is_local;
    struct composition_surface **locked_surfaces;
    UINT target_count;
    UINT visual_count;
    UINT surface_count;
};

static HRESULT count_commit_visual(struct composition_visual *visual, UINT depth,
        UINT *visual_count, UINT *surface_count)
{
    struct composition_visual *child;
    HRESULT hr;

    if (depth > 4096 || *visual_count == 4096) return E_INVALIDARG;
    ++*visual_count;
    if (visual->content_kind == COMPOSITION_CONTENT_HANDLE_SURFACE
            || visual->content_kind == COMPOSITION_CONTENT_LOCAL_SURFACE
            || visual->content_kind == COMPOSITION_CONTENT_SWAPCHAIN)
        ++*surface_count;
    else if (visual->content_kind != COMPOSITION_CONTENT_NONE)
        return E_NOTIMPL;
    for (child = visual->first_child; child; child = child->next)
        if (FAILED(hr = count_commit_visual(child, depth + 1, visual_count, surface_count)))
            return hr;
    return S_OK;
}

static HRESULT fill_commit_visual(struct composition_visual *visual,
        struct commit_graph *graph, UINT *visual_index, UINT *surface_index)
{
    struct commit_visual *snapshot = &graph->visuals[(*visual_index)++];
    struct composition_visual *child;
    HANDLE source, duplicate;
    UINT i;

    snapshot->identity = visual;
    snapshot->parent = visual->parent;
    snapshot->first_child = visual->first_child;
    snapshot->next = visual->next;
    snapshot->content_kind = visual->content_kind;
    snapshot->content = WINE_DCOMP_INVALID_INDEX;
    snapshot->transform = visual->transform;
    snapshot->clip = visual->clip;
    snapshot->opacity = visual->effect
            ? effect_group_from_IDCompositionEffectGroup(visual->effect)->opacity : 1.0f;
    snapshot->offset_x = visual->offset_x;
    snapshot->offset_y = visual->offset_y;
    snapshot->interpolation_mode = visual->interpolation_mode;
    snapshot->border_mode = visual->border_mode;
    snapshot->composite_mode = visual->composite_mode;
    snapshot->opacity_mode = visual->opacity_mode;
    snapshot->backface_visibility = visual->backface_visibility;
    snapshot->has_transform = visual->has_transform;
    snapshot->has_clip = visual->has_clip;
    if (visual->content_kind != COMPOSITION_CONTENT_NONE)
        for (i = 0; i < *surface_index; ++i)
            if (graph->surface_identities[i] == visual->content)
            {
                snapshot->content = i;
                break;
            }
    if (snapshot->content == WINE_DCOMP_INVALID_INDEX
            && visual->content_kind == COMPOSITION_CONTENT_HANDLE_SURFACE)
    {
        snapshot->content = *surface_index;
        graph->surface_identities[*surface_index] = visual->content;
        IUnknown_AddRef(visual->content);
        source = surface_from_IUnknown(visual->content)->handle;
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                0, FALSE, DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        graph->surface_handles[(*surface_index)++] = duplicate;
    }
    else if (snapshot->content == WINE_DCOMP_INVALID_INDEX
            && visual->content_kind == COMPOSITION_CONTENT_LOCAL_SURFACE)
    {
        struct composition_surface *surface = surface_from_IDCompositionSurface(
                (IDCompositionSurface *)visual->content);

        snapshot->content = *surface_index;
        graph->surface_identities[*surface_index] = visual->content;
        graph->surface_is_local[*surface_index] = TRUE;
        IUnknown_AddRef(visual->content);
        EnterCriticalSection(&surface->lock);
        graph->locked_surfaces[*surface_index] = surface;
        if (surface->drawing) return DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED;
        source = surface->composition_handle;
        if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &duplicate,
                0, FALSE, DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        graph->surface_handles[(*surface_index)++] = duplicate;
    }
    else if (snapshot->content == WINE_DCOMP_INVALID_INDEX
            && visual->content_kind == COMPOSITION_CONTENT_SWAPCHAIN)
    {
        snapshot->content = *surface_index;
        graph->surface_identities[*surface_index] = visual->content;
        IUnknown_AddRef(visual->content);
        if (!DuplicateHandle(GetCurrentProcess(), visual->content_handle, GetCurrentProcess(),
                &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        graph->surface_handles[(*surface_index)++] = duplicate;
    }
    for (child = visual->first_child; child; child = child->next)
    {
        HRESULT hr = fill_commit_visual(child, graph, visual_index, surface_index);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

static void free_commit_graph(struct commit_graph *graph)
{
    UINT i;

    for (i = 0; i < graph->surface_count; ++i)
        if (graph->locked_surfaces && graph->locked_surfaces[i])
            LeaveCriticalSection(&graph->locked_surfaces[i]->lock);
    for (i = 0; i < graph->surface_count; ++i)
        if (graph->surface_handles && graph->surface_handles[i])
            CloseHandle(graph->surface_handles[i]);
    for (i = 0; i < graph->surface_count; ++i)
        if (graph->surface_identities && graph->surface_identities[i])
            IUnknown_Release(graph->surface_identities[i]);
    free(graph->locked_surfaces);
    free(graph->surface_handles);
    free(graph->surface_identities);
    free(graph->surface_is_local);
    free(graph->visuals);
    free(graph->targets);
    memset(graph, 0, sizeof(*graph));
}

static HRESULT snapshot_commit_graph(struct composition_device *device,
        struct commit_graph *graph)
{
    struct composition_target *target;
    UINT target_index = 0, visual_index = 0, surface_index = 0;
    HRESULT hr = S_OK;

    memset(graph, 0, sizeof(*graph));
    EnterCriticalSection(&device->lock);
    LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
    {
        const WCHAR *property = target->topmost
                ? target_topmost_property : target_normal_property;

        AcquireSRWLockShared(&target_property_lock);
        if (GetPropW(target->hwnd, property) != (HANDLE)target)
            hr = E_INVALIDARG;
        ReleaseSRWLockShared(&target_property_lock);
        if (FAILED(hr)) goto done;
        if (graph->target_count == 4096)
        {
            hr = E_INVALIDARG;
            goto done;
        }
        ++graph->target_count;
        if (target->root && FAILED(hr = count_commit_visual(target->root, 0,
                &graph->visual_count, &graph->surface_count)))
            goto done;
    }
    if ((graph->target_count && !(graph->targets = calloc(graph->target_count,
            sizeof(*graph->targets))))
            || (graph->visual_count && !(graph->visuals = calloc(graph->visual_count,
            sizeof(*graph->visuals))))
            || (graph->surface_count && !(graph->surface_handles = calloc(graph->surface_count,
            sizeof(*graph->surface_handles))))
            || (graph->surface_count && !(graph->locked_surfaces = calloc(graph->surface_count,
            sizeof(*graph->locked_surfaces))))
            || (graph->surface_count && !(graph->surface_identities = calloc(graph->surface_count,
            sizeof(*graph->surface_identities))))
            || (graph->surface_count && !(graph->surface_is_local = calloc(graph->surface_count,
            sizeof(*graph->surface_is_local)))))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    LIST_FOR_EACH_ENTRY(target, &device->targets, struct composition_target, entry)
    {
        graph->targets[target_index].hwnd = target->hwnd;
        graph->targets[target_index].target_id = target->target_id;
        graph->targets[target_index].topmost = target->topmost;
        graph->targets[target_index].root = target->root;
        ++target_index;
        if (target->root && FAILED(hr = fill_commit_visual(target->root, graph,
                &visual_index, &surface_index)))
            goto done;
    }
    graph->surface_count = surface_index;

done:
    LeaveCriticalSection(&device->lock);
    if (FAILED(hr)) free_commit_graph(graph);
    return hr;
}

static UINT commit_visual_index(const struct commit_graph *graph,
        const struct composition_visual *visual)
{
    UINT i;

    if (!visual) return WINE_DCOMP_INVALID_INDEX;
    for (i = 0; i < graph->visual_count; ++i)
        if (graph->visuals[i].identity == visual) return i;
    return WINE_DCOMP_INVALID_INDEX;
}

static HRESULT subscribe_surface_handles(struct composition_device *device,
        const HANDLE *surface_handles, UINT surface_count,
        struct dcomp_surface_snapshot *snapshots,
        HANDLE *subscription)
{
    struct dcomp_subscription_input *inputs = NULL;
    NTSTATUS status;
    UINT i;

    *subscription = NULL;
    if (surface_count && !(inputs = calloc(surface_count, sizeof(*inputs))))
        return E_OUTOFMEMORY;
    for (i = 0; i < surface_count; ++i)
        inputs[i].surface = wine_server_obj_handle(surface_handles[i]);
    SERVER_START_REQ(dcomp_subscribe_surfaces)
    {
        req->event = wine_server_obj_handle(device->subscription_event);
        req->surfaces_size = surface_count * sizeof(*inputs);
        if (req->surfaces_size) wine_server_add_data(req, inputs, req->surfaces_size);
        if (surface_count)
            wine_server_set_reply(req, snapshots,
                    surface_count * sizeof(*snapshots));
        if (!(status = wine_server_call(req)))
        {
            if (reply->snapshots_size != surface_count * sizeof(*snapshots))
                status = STATUS_INFO_LENGTH_MISMATCH;
            else
                *subscription = wine_server_ptr_handle(reply->subscription);
        }
    }
    SERVER_END_REQ;
    free(inputs);
    return status ? HRESULT_FROM_WIN32(RtlNtStatusToDosError(status)) : S_OK;
}

static HRESULT subscribe_commit_surfaces(struct composition_device *device,
        const struct commit_graph *graph, struct dcomp_surface_snapshot *snapshots,
        HANDLE *subscription)
{
    return subscribe_surface_handles(device, graph->surface_handles, graph->surface_count,
            snapshots, subscription);
}

static void close_commit_snapshots(struct dcomp_surface_snapshot *snapshots, UINT count)
{
    UINT i;

    for (i = 0; i < count; ++i)
    {
        if (snapshots[i].resource) CloseHandle(wine_server_ptr_handle(snapshots[i].resource));
        if (snapshots[i].sync_resource)
            CloseHandle(wine_server_ptr_handle(snapshots[i].sync_resource));
        if (snapshots[i].lease) CloseHandle(wine_server_ptr_handle(snapshots[i].lease));
    }
}

static void fill_scene_surface(const struct dcomp_surface_snapshot *snapshot,
        struct wine_dcomp_surface *surface)
{
    memset(surface, 0, sizeof(*surface));
    surface->generation = snapshot->generation;

    /* A composition surface handle may be placed in a committed visual
     * before a producer binds a swapchain to it.  Preserve that graph as
     * detached content; the retained subscription will publish the real
     * descriptor/front later. */
    if (!snapshot->width) return;

    surface->resource = (UINT_PTR)wine_server_ptr_handle(snapshot->resource);
    surface->sync_resource = (UINT_PTR)wine_server_ptr_handle(snapshot->sync_resource);
    surface->sync_value = snapshot->has_front && snapshot->sync_resource ? 1 : 0;
    surface->adapter_luid = ((UINT64)(UINT32)snapshot->adapter_luid.high_part << 32)
            | snapshot->adapter_luid.low_part;
    memcpy(surface->device_uuid, &snapshot->device_uuid0, sizeof(surface->device_uuid));
    surface->width = snapshot->width;
    surface->height = snapshot->height;
    surface->format = snapshot->format;
    surface->alpha_mode = snapshot->alpha_mode;
    surface->resource_type = snapshot->has_front ? WINE_DCOMP_RESOURCE_WIN32_HANDLE : 0;
    surface->sync_resource_type = snapshot->has_front && snapshot->sync_resource
            ? WINE_DCOMP_RESOURCE_WIN32_HANDLE : 0;
    if (!snapshot->has_front)
        surface->sync_type = 0;
    else
        surface->sync_type = snapshot->sync_resource ? WINE_DCOMP_SYNC_TIMELINE
                : WINE_DCOMP_SYNC_HOST_IDLE;
    surface->memory_type_index = snapshot->memory_type_index;
    surface->image_usage = WINE_DCOMP_IMAGE_TRANSFER_SRC | WINE_DCOMP_IMAGE_TRANSFER_DST
            | WINE_DCOMP_IMAGE_SAMPLED | WINE_DCOMP_IMAGE_COLOR_ATTACHMENT;
    surface->image_flags = WINE_DCOMP_IMAGE_ALIAS | WINE_DCOMP_IMAGE_OPTIMAL_TILING
            | WINE_DCOMP_IMAGE_DEDICATED_ALLOCATION;
    surface->sample_count = 1;
    surface->mip_levels = 1;
    surface->array_layers = 1;
    surface->front_buffer = snapshot->front_buffer;
    surface->buffer_count = snapshot->buffer_count;
    if (snapshot->has_front) surface->flags |= WINE_DCOMP_SURFACE_HAS_FRONT;
    surface->damage_right = snapshot->width;
    surface->damage_bottom = snapshot->height;
}

static HRESULT build_commit_scene(struct composition_device *device,
        const struct commit_graph *graph, const struct dcomp_surface_snapshot *snapshots,
        struct wine_dcomp_scene **out)
{
    struct wine_dcomp_scene *scene;
    struct wine_dcomp_surface *surfaces;
    struct wine_dcomp_visual *visuals;
    struct wine_dcomp_target *targets;
    size_t target_offset, visual_offset, surface_offset, size;
    UINT i;

    target_offset = (sizeof(*scene) + 7) & ~(size_t)7;
    visual_offset = (target_offset + graph->target_count * sizeof(*targets) + 7) & ~(size_t)7;
    surface_offset = (visual_offset + graph->visual_count * sizeof(*visuals) + 7) & ~(size_t)7;
    size = surface_offset + graph->surface_count * sizeof(*surfaces);
    if (size > UINT_MAX || !(scene = calloc(1, size))) return E_OUTOFMEMORY;

    scene->abi_version = WINE_DCOMP_DRIVER_ABI_VERSION;
    scene->byte_size = size;
    scene->update_kind = WINE_DCOMP_UPDATE_COMMIT;
    scene->device_id = device->device_id;
    scene->generation = ++device->generation;
    scene->target_count = graph->target_count;
    scene->target_offset = target_offset;
    scene->visual_count = graph->visual_count;
    scene->visual_offset = visual_offset;
    scene->surface_count = graph->surface_count;
    scene->surface_offset = surface_offset;
    targets = (struct wine_dcomp_target *)((char *)scene + target_offset);
    visuals = (struct wine_dcomp_visual *)((char *)scene + visual_offset);
    surfaces = (struct wine_dcomp_surface *)((char *)scene + surface_offset);

    for (i = 0; i < graph->target_count; ++i)
    {
        targets[i].hwnd = (UINT_PTR)graph->targets[i].hwnd;
        targets[i].target_id = graph->targets[i].target_id;
        targets[i].root_visual = commit_visual_index(graph, graph->targets[i].root);
        if (graph->targets[i].topmost) targets[i].flags |= WINE_DCOMP_TARGET_TOPMOST;
    }
    for (i = 0; i < graph->visual_count; ++i)
    {
        const struct commit_visual *visual = &graph->visuals[i];

        visuals[i].parent = commit_visual_index(graph, visual->parent);
        visuals[i].first_child = commit_visual_index(graph, visual->first_child);
        visuals[i].next_sibling = commit_visual_index(graph, visual->next);
        visuals[i].content = visual->content;
        visuals[i].content_kind = visual->content_kind;
        if (visual->has_transform) visuals[i].flags |= WINE_DCOMP_VISUAL_HAS_TRANSFORM;
        if (visual->has_clip) visuals[i].flags |= WINE_DCOMP_VISUAL_HAS_CLIP;
        visuals[i].interpolation_mode = visual->interpolation_mode;
        visuals[i].border_mode = visual->border_mode;
        visuals[i].composite_mode = visual->composite_mode;
        visuals[i].opacity_mode = visual->opacity_mode;
        visuals[i].backface_visibility = visual->backface_visibility;
        visuals[i].opacity = visual->opacity;
        visuals[i].offset_x = visual->offset_x;
        visuals[i].offset_y = visual->offset_y;
        memcpy(visuals[i].transform, &visual->transform, sizeof(visuals[i].transform));
        memcpy(visuals[i].clip, &visual->clip, sizeof(visuals[i].clip));
    }
    for (i = 0; i < graph->surface_count; ++i)
        fill_scene_surface(snapshots + i, surfaces + i);
    *out = scene;
    return S_OK;
}

static HRESULT build_surface_update(struct composition_device *device,
        const struct wine_dcomp_scene *committed,
        const struct dcomp_surface_snapshot *snapshots, struct wine_dcomp_scene **out)
{
    struct wine_dcomp_surface *surfaces;
    struct wine_dcomp_scene *scene;
    size_t surface_offset, size;
    UINT i;

    surface_offset = (sizeof(*scene) + 7) & ~(size_t)7;
    size = surface_offset + committed->surface_count * sizeof(*surfaces);
    if (size > UINT_MAX || !(scene = calloc(1, size))) return E_OUTOFMEMORY;
    scene->abi_version = WINE_DCOMP_DRIVER_ABI_VERSION;
    scene->byte_size = size;
    scene->update_kind = WINE_DCOMP_UPDATE_SURFACES;
    scene->device_id = device->device_id;
    scene->generation = ++device->generation;
    scene->target_offset = surface_offset;
    scene->visual_offset = surface_offset;
    scene->surface_count = committed->surface_count;
    scene->surface_offset = surface_offset;
    surfaces = (void *)((char *)scene + surface_offset);
    for (i = 0; i < committed->surface_count; ++i)
        fill_scene_surface(snapshots + i, surfaces + i);
    *out = scene;
    return S_OK;
}

static DWORD WINAPI device_refresh_worker(void *arg)
{
    struct composition_device *device = arg;
    HANDLE waits[2] = {device->stop_event, device->subscription_event};
    DWORD retry_delay = 10;

    while (WaitForMultipleObjects(ARRAY_SIZE(waits), waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1)
    {
        struct dcomp_surface_snapshot *snapshots = NULL;
        struct wine_dcomp_scene *scene = NULL;
        HANDLE subscription = NULL, previous;
        BOOL retry = FALSE;
        UINT count, lock_limit = 0, i;

        EnterCriticalSection(&device->commit_lock);
        count = device->committed_surface_count;
        if (!count) goto done;
        retry = TRUE;
        for (i = 0; i < count; ++i)
        {
            struct composition_surface *surface;

            if (!device->committed_surface_local[i]) continue;
            surface = surface_from_IDCompositionSurface(
                    (IDCompositionSurface *)device->committed_surface_objects[i]);
            EnterCriticalSection(&surface->lock);
            lock_limit = i + 1;
            if (surface->drawing) goto done;
        }
        if (!(snapshots = calloc(count, sizeof(*snapshots)))) goto done;
        if (FAILED(subscribe_surface_handles(device, device->committed_surfaces, count,
                snapshots, &subscription))) goto done;
        if (FAILED(build_surface_update(device, device->committed_scene, snapshots, &scene)))
            goto done;
        if (!NtUserCallTwoParam((UINT_PTR)scene, scene->byte_size,
                NtUserCallTwoParam_DCompositionUpdate)) goto done;
        previous = device->subscription;
        device->subscription = subscription;
        subscription = NULL;
        if (previous) CloseHandle(previous);
        retry = FALSE;

done:
        if (lock_limit)
            for (i = lock_limit; i-- > 0;)
                if (device->committed_surface_local[i])
                {
                    struct composition_surface *surface = surface_from_IDCompositionSurface(
                            (IDCompositionSurface *)device->committed_surface_objects[i]);
                    LeaveCriticalSection(&surface->lock);
                }
        if (subscription) CloseHandle(subscription);
        if (snapshots) close_commit_snapshots(snapshots, count);
        free(scene);
        free(snapshots);
        LeaveCriticalSection(&device->commit_lock);
        if (retry)
        {
            if (WaitForSingleObject(device->stop_event, retry_delay) == WAIT_OBJECT_0) break;
            SetEvent(device->subscription_event);
            if (retry_delay < 1000) retry_delay *= 2;
            if (retry_delay > 1000) retry_delay = 1000;
        }
        else
            retry_delay = 10;
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE device_Commit(IDCompositionDevice *iface)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);
    struct dcomp_surface_snapshot *snapshots = NULL;
    struct wine_dcomp_scene *scene = NULL;
    struct wine_dcomp_scene *previous_scene;
    struct commit_graph graph;
    HANDLE *previous_surfaces;
    IUnknown **previous_surface_objects;
    BOOL *previous_surface_local;
    UINT i, previous_surface_count;
    HANDLE subscription = NULL, previous;
    HRESULT hr;

    EnterCriticalSection(&device->commit_lock);
    if (FAILED(hr = snapshot_commit_graph(device, &graph))) goto done;
    if (graph.surface_count && !(snapshots = calloc(graph.surface_count, sizeof(*snapshots))))
    {
        hr = E_OUTOFMEMORY;
        goto done_graph;
    }
    if (FAILED(hr = subscribe_commit_surfaces(device, &graph, snapshots, &subscription)))
        goto done_graph;
    if (FAILED(hr = build_commit_scene(device, &graph, snapshots, &scene))) goto done_graph;
    if (!NtUserCallTwoParam((UINT_PTR)scene, scene->byte_size,
            NtUserCallTwoParam_DCompositionUpdate))
    {
        hr = E_NOTIMPL;
        goto done_graph;
    }
    previous = device->subscription;
    device->subscription = subscription;
    subscription = NULL;
    if (previous) CloseHandle(previous);
    previous_scene = device->committed_scene;
    previous_surfaces = device->committed_surfaces;
    previous_surface_objects = device->committed_surface_objects;
    previous_surface_local = device->committed_surface_local;
    previous_surface_count = device->committed_surface_count;
    device->committed_scene = scene;
    scene = NULL;
    device->committed_surfaces = graph.surface_handles;
    device->committed_surface_objects = graph.surface_identities;
    device->committed_surface_local = graph.surface_is_local;
    device->committed_surface_count = graph.surface_count;
    for (i = 0; i < graph.surface_count; ++i)
        if (graph.surface_is_local[i])
        {
            struct composition_surface *surface = surface_from_IDCompositionSurface(
                    (IDCompositionSurface *)graph.surface_identities[i]);
            InterlockedIncrement(&surface->committed_refs);
        }
    graph.surface_handles = NULL;
    graph.surface_identities = NULL;
    graph.surface_is_local = NULL;
    while (previous_surface_count)
    {
        --previous_surface_count;
        CloseHandle(previous_surfaces[previous_surface_count]);
        if (previous_surface_local[previous_surface_count])
        {
            struct composition_surface *surface = surface_from_IDCompositionSurface(
                    (IDCompositionSurface *)previous_surface_objects[previous_surface_count]);
            InterlockedDecrement(&surface->committed_refs);
        }
        IUnknown_Release(previous_surface_objects[previous_surface_count]);
    }
    free(previous_surfaces);
    free(previous_surface_objects);
    free(previous_surface_local);
    free(previous_scene);
    hr = S_OK;

done_graph:
    if (subscription) CloseHandle(subscription);
    if (snapshots) close_commit_snapshots(snapshots, graph.surface_count);
    free(scene);
    free(snapshots);
    free_commit_graph(&graph);
done:
    LeaveCriticalSection(&device->commit_lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_WaitForCommitCompletion(IDCompositionDevice *iface)
{
    FIXME("iface %p, no compositor backend.\n", iface);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_GetFrameStatistics(IDCompositionDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{
    FIXME("iface %p, statistics %p, no compositor backend.\n", iface, statistics);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTargetForHwnd(IDCompositionDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);

    TRACE("iface %p, hwnd %p, topmost %d, target %p.\n", iface, hwnd, topmost, target);
    return create_target(device, hwnd, topmost, target);
}

static HRESULT STDMETHODCALLTYPE device_CreateVisual(IDCompositionDevice *iface,
        IDCompositionVisual **visual)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);
    IDCompositionVisual2 *visual2;
    HRESULT hr;

    if (!visual) return E_INVALIDARG;
    *visual = NULL;
    if (SUCCEEDED(hr = create_visual(device, &visual2)))
        *visual = (IDCompositionVisual *)visual2;
    return hr;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurface(IDCompositionDevice *iface, UINT width,
        UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{
    struct composition_device *device = device_from_IDCompositionDevice(iface);

    TRACE("iface %p, size %ux%u, format %#x, alpha %#x, surface %p.\n",
            iface, width, height, format, alpha_mode, surface);
    return create_surface(device, width, height, format, alpha_mode, surface);
}

static HRESULT STDMETHODCALLTYPE device_CreateVirtualSurface(IDCompositionDevice *iface, UINT width,
        UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{
    FIXME("iface %p, size %ux%u, format %#x, alpha %#x, surface %p, no GPU association.\n",
            iface, width, height, format, alpha_mode, surface);
    if (surface) *surface = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHandle(IDCompositionDevice *iface,
        HANDLE handle, IUnknown **surface)
{
    TRACE("iface %p, handle %p, surface %p.\n", iface, handle, surface);
    return create_handle_surface(handle, surface);
}

static HRESULT STDMETHODCALLTYPE device_CreateSurfaceFromHwnd(IDCompositionDevice *iface,
        HWND hwnd, IUnknown **surface)
{
    FIXME("iface %p, hwnd %p, surface %p, no compositor backend.\n", iface, hwnd, surface);
    if (surface) *surface = NULL;
    return E_NOTIMPL;
}

#define DEVICE_TRANSFORM_STUB(name, type) \
static HRESULT STDMETHODCALLTYPE device_Create##name(IDCompositionDevice *iface, type **out) \
{ \
    FIXME("iface %p, out %p, no compositor backend.\n", iface, out); \
    if (out) *out = NULL; \
    return E_NOTIMPL; \
}

DEVICE_TRANSFORM_STUB(TranslateTransform, IDCompositionTranslateTransform)
DEVICE_TRANSFORM_STUB(ScaleTransform, IDCompositionScaleTransform)
DEVICE_TRANSFORM_STUB(RotateTransform, IDCompositionRotateTransform)
DEVICE_TRANSFORM_STUB(SkewTransform, IDCompositionSkewTransform)
DEVICE_TRANSFORM_STUB(MatrixTransform, IDCompositionMatrixTransform)
DEVICE_TRANSFORM_STUB(TranslateTransform3D, IDCompositionTranslateTransform3D)
DEVICE_TRANSFORM_STUB(ScaleTransform3D, IDCompositionScaleTransform3D)
DEVICE_TRANSFORM_STUB(RotateTransform3D, IDCompositionRotateTransform3D)
DEVICE_TRANSFORM_STUB(MatrixTransform3D, IDCompositionMatrixTransform3D)
DEVICE_TRANSFORM_STUB(RectangleClip, IDCompositionRectangleClip)
DEVICE_TRANSFORM_STUB(Animation, IDCompositionAnimation)

static HRESULT STDMETHODCALLTYPE device_CreateEffectGroup(IDCompositionDevice *iface,
        IDCompositionEffectGroup **out)
{
    return create_effect_group(device_from_IDCompositionDevice(iface), out);
}

static HRESULT STDMETHODCALLTYPE device_CreateTransformGroup(IDCompositionDevice *iface,
        IDCompositionTransform **transforms, UINT count, IDCompositionTransform **group)
{
    FIXME("iface %p, transforms %p, count %u, group %p, no compositor backend.\n",
            iface, transforms, count, group);
    if (group) *group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CreateTransform3DGroup(IDCompositionDevice *iface,
        IDCompositionTransform3D **transforms, UINT count, IDCompositionTransform3D **group)
{
    FIXME("iface %p, transforms %p, count %u, group %p, no compositor backend.\n",
            iface, transforms, count, group);
    if (group) *group = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE device_CheckDeviceState(IDCompositionDevice *iface, BOOL *valid)
{
    FIXME("iface %p, valid %p, no compositor backend.\n", iface, valid);
    if (valid) *valid = FALSE;
    return E_NOTIMPL;
}

static const IDCompositionDeviceVtbl device_vtbl =
{
    device_QueryInterface,
    device_AddRef,
    device_Release,
    device_Commit,
    device_WaitForCommitCompletion,
    device_GetFrameStatistics,
    device_CreateTargetForHwnd,
    device_CreateVisual,
    device_CreateSurface,
    device_CreateVirtualSurface,
    device_CreateSurfaceFromHandle,
    device_CreateSurfaceFromHwnd,
    device_CreateTranslateTransform,
    device_CreateScaleTransform,
    device_CreateRotateTransform,
    device_CreateSkewTransform,
    device_CreateMatrixTransform,
    device_CreateTransformGroup,
    device_CreateTranslateTransform3D,
    device_CreateScaleTransform3D,
    device_CreateRotateTransform3D,
    device_CreateMatrixTransform3D,
    device_CreateTransform3DGroup,
    device_CreateEffectGroup,
    device_CreateRectangleClip,
    device_CreateAnimation,
    device_CheckDeviceState,
};

static HRESULT STDMETHODCALLTYPE desktop_QueryInterface(IDCompositionDesktopDevice *iface,
        REFIID iid, void **out)
{
    return device_QueryInterface(&device_from_IDCompositionDesktopDevice(iface)->IDCompositionDevice_iface,
            iid, out);
}

static ULONG STDMETHODCALLTYPE desktop_AddRef(IDCompositionDesktopDevice *iface)
{
    return device_AddRef(&device_from_IDCompositionDesktopDevice(iface)->IDCompositionDevice_iface);
}

static ULONG STDMETHODCALLTYPE desktop_Release(IDCompositionDesktopDevice *iface)
{
    return device_Release(&device_from_IDCompositionDesktopDevice(iface)->IDCompositionDevice_iface);
}

static IDCompositionDevice *desktop_device1(IDCompositionDesktopDevice *iface)
{
    return &device_from_IDCompositionDesktopDevice(iface)->IDCompositionDevice_iface;
}

static HRESULT STDMETHODCALLTYPE desktop_Commit(IDCompositionDesktopDevice *iface)
{ return device_Commit(desktop_device1(iface)); }
static HRESULT STDMETHODCALLTYPE desktop_WaitForCommitCompletion(IDCompositionDesktopDevice *iface)
{ return device_WaitForCommitCompletion(desktop_device1(iface)); }
static HRESULT STDMETHODCALLTYPE desktop_GetFrameStatistics(IDCompositionDesktopDevice *iface,
        DCOMPOSITION_FRAME_STATISTICS *statistics)
{ return device_GetFrameStatistics(desktop_device1(iface), statistics); }
static HRESULT STDMETHODCALLTYPE desktop_CreateVisual(IDCompositionDesktopDevice *iface,
        IDCompositionVisual2 **visual)
{
    return create_visual(device_from_IDCompositionDesktopDevice(iface), visual);
}
static HRESULT STDMETHODCALLTYPE desktop_CreateSurfaceFactory(IDCompositionDesktopDevice *iface,
        IUnknown *rendering_device, IDCompositionSurfaceFactory **factory)
{
    FIXME("iface %p, rendering_device %p, factory %p, no GPU association.\n",
            iface, rendering_device, factory);
    if (factory) *factory = NULL;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE desktop_CreateSurface(IDCompositionDesktopDevice *iface, UINT width,
        UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode, IDCompositionSurface **surface)
{ return device_CreateSurface(desktop_device1(iface), width, height, format, alpha_mode, surface); }
static HRESULT STDMETHODCALLTYPE desktop_CreateVirtualSurface(IDCompositionDesktopDevice *iface,
        UINT width, UINT height, DXGI_FORMAT format, DXGI_ALPHA_MODE alpha_mode,
        IDCompositionVirtualSurface **surface)
{ return device_CreateVirtualSurface(desktop_device1(iface), width, height, format, alpha_mode, surface); }

#define DESKTOP_TRANSFORM_FORWARD(name, type) \
static HRESULT STDMETHODCALLTYPE desktop_Create##name(IDCompositionDesktopDevice *iface, type **out) \
{ return device_Create##name(desktop_device1(iface), out); }

DESKTOP_TRANSFORM_FORWARD(TranslateTransform, IDCompositionTranslateTransform)
DESKTOP_TRANSFORM_FORWARD(ScaleTransform, IDCompositionScaleTransform)
DESKTOP_TRANSFORM_FORWARD(RotateTransform, IDCompositionRotateTransform)
DESKTOP_TRANSFORM_FORWARD(SkewTransform, IDCompositionSkewTransform)
DESKTOP_TRANSFORM_FORWARD(MatrixTransform, IDCompositionMatrixTransform)
DESKTOP_TRANSFORM_FORWARD(TranslateTransform3D, IDCompositionTranslateTransform3D)
DESKTOP_TRANSFORM_FORWARD(ScaleTransform3D, IDCompositionScaleTransform3D)
DESKTOP_TRANSFORM_FORWARD(RotateTransform3D, IDCompositionRotateTransform3D)
DESKTOP_TRANSFORM_FORWARD(MatrixTransform3D, IDCompositionMatrixTransform3D)
DESKTOP_TRANSFORM_FORWARD(EffectGroup, IDCompositionEffectGroup)
DESKTOP_TRANSFORM_FORWARD(RectangleClip, IDCompositionRectangleClip)
DESKTOP_TRANSFORM_FORWARD(Animation, IDCompositionAnimation)

static HRESULT STDMETHODCALLTYPE desktop_CreateTransformGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform **transforms, UINT count, IDCompositionTransform **group)
{ return device_CreateTransformGroup(desktop_device1(iface), transforms, count, group); }
static HRESULT STDMETHODCALLTYPE desktop_CreateTransform3DGroup(IDCompositionDesktopDevice *iface,
        IDCompositionTransform3D **transforms, UINT count, IDCompositionTransform3D **group)
{ return device_CreateTransform3DGroup(desktop_device1(iface), transforms, count, group); }
static HRESULT STDMETHODCALLTYPE desktop_CreateTargetForHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, BOOL topmost, IDCompositionTarget **target)
{ return device_CreateTargetForHwnd(desktop_device1(iface), hwnd, topmost, target); }
static HRESULT STDMETHODCALLTYPE desktop_CreateSurfaceFromHandle(IDCompositionDesktopDevice *iface,
        HANDLE handle, IUnknown **surface)
{ return create_handle_surface(handle, surface); }
static HRESULT STDMETHODCALLTYPE desktop_CreateSurfaceFromHwnd(IDCompositionDesktopDevice *iface,
        HWND hwnd, IUnknown **surface)
{ return device_CreateSurfaceFromHwnd(desktop_device1(iface), hwnd, surface); }

static const IDCompositionDesktopDeviceVtbl desktop_vtbl =
{
    desktop_QueryInterface,
    desktop_AddRef,
    desktop_Release,
    desktop_Commit,
    desktop_WaitForCommitCompletion,
    desktop_GetFrameStatistics,
    desktop_CreateVisual,
    desktop_CreateSurfaceFactory,
    desktop_CreateSurface,
    desktop_CreateVirtualSurface,
    desktop_CreateTranslateTransform,
    desktop_CreateScaleTransform,
    desktop_CreateRotateTransform,
    desktop_CreateSkewTransform,
    desktop_CreateMatrixTransform,
    desktop_CreateTransformGroup,
    desktop_CreateTranslateTransform3D,
    desktop_CreateScaleTransform3D,
    desktop_CreateRotateTransform3D,
    desktop_CreateMatrixTransform3D,
    desktop_CreateTransform3DGroup,
    desktop_CreateEffectGroup,
    desktop_CreateRectangleClip,
    desktop_CreateAnimation,
    desktop_CreateTargetForHwnd,
    desktop_CreateSurfaceFromHandle,
    desktop_CreateSurfaceFromHwnd,
};

static HRESULT create_device2(IUnknown *rendering_device, REFIID iid, void **out)
{
    static LONG64 next_device_id;
    struct composition_device *device;
    HRESULT hr;

    if (!out) return E_INVALIDARG;
    *out = NULL;

    if (!IsEqualIID(iid, &IID_IDCompositionDevice)
            && !IsEqualIID(iid, &IID_IDCompositionDesktopDevice))
        return E_NOINTERFACE;

    if (!(device = calloc(1, sizeof(*device)))) return E_OUTOFMEMORY;
    device->IDCompositionDevice_iface.lpVtbl = &device_vtbl;
    device->IDCompositionDesktopDevice_iface.lpVtbl = &desktop_vtbl;
    device->rendering_device = rendering_device;
    if (rendering_device) IUnknown_AddRef(rendering_device);
    InitializeCriticalSection(&device->lock);
    InitializeCriticalSection(&device->commit_lock);
    list_init(&device->targets);
    device->ref = 1;
    device->device_id = InterlockedIncrement64(&next_device_id);
    if (!(device->subscription_event = CreateEventW(NULL, FALSE, FALSE, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        device_Release(&device->IDCompositionDevice_iface);
        return hr;
    }
    if (!(device->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL))
            || !(device->worker = CreateThread(NULL, 0, device_refresh_worker, device, 0, NULL)))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        device_Release(&device->IDCompositionDevice_iface);
        return hr;
    }

    hr = device_QueryInterface(&device->IDCompositionDevice_iface, iid, out);
    device_Release(&device->IDCompositionDevice_iface);
    return hr;
}

HRESULT WINAPI DCompositionCreateDevice(IDXGIDevice *dxgi_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", dxgi_device, debugstr_guid(iid), device);
    return create_device2((IUnknown *)dxgi_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice2(IUnknown *rendering_device, REFIID iid, void **device)
{
    TRACE("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return create_device2(rendering_device, iid, device);
}

HRESULT WINAPI DCompositionCreateDevice3(IUnknown *rendering_device, REFIID iid, void **device)
{
    FIXME("%p, %s, %p.\n", rendering_device, debugstr_guid(iid), device);

    return E_NOTIMPL;
}

HRESULT WINAPI DCompositionCreateSurfaceHandle(DWORD access, SECURITY_ATTRIBUTES *security_attributes,
        HANDLE *surface_handle)
{
    unsigned int attributes = 0;
    NTSTATUS status;

    TRACE("access %#lx, security_attributes %p, surface_handle %p.\n",
            access, security_attributes, surface_handle);

    if (!surface_handle)
        return E_INVALIDARG;
    *surface_handle = NULL;

    if (security_attributes)
    {
        if (security_attributes->nLength != sizeof(*security_attributes))
            return E_INVALIDARG;
        if (security_attributes->lpSecurityDescriptor)
        {
            FIXME("Security descriptors are not supported yet.\n");
            return E_NOTIMPL;
        }
        if (security_attributes->bInheritHandle)
            attributes |= OBJ_INHERIT;
    }

    SERVER_START_REQ(dcomp_create_surface)
    {
        req->access = access;
        req->attributes = attributes;
        if (!(status = wine_server_call(req)))
            *surface_handle = wine_server_ptr_handle(reply->handle);
    }
    SERVER_END_REQ;

    if (status)
        return HRESULT_FROM_WIN32(RtlNtStatusToDosError(status));
    return S_OK;
}
