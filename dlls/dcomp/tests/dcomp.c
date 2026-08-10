/*
 * DirectComposition tests
 *
 * Copyright 2026 OpenTerminal contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define COBJMACROS
#include "initguid.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "dcomp.h"
#include "d3d11.h"
#include "dxgi1_3.h"

struct wined3d_resource;
struct wined3d_texture;

#include "wine/server.h"
#include "wine/test.h"
#include "wine/winedxgi.h"

static HRESULT (WINAPI *pDCompositionCreateSurfaceHandle)(DWORD access,
        SECURITY_ATTRIBUTES *security_attributes, HANDLE *surface_handle);
static HRESULT (WINAPI *pDCompositionCreateDevice)(IDXGIDevice *dxgi_device,
        REFIID iid, void **device);
static HRESULT (WINAPI *pDCompositionCreateDevice2)(IUnknown *rendering_device,
        REFIID iid, void **device);

static void flush_events(void)
{
    DWORD end = GetTickCount() + 250;
    int remaining = 250;
    MSG msg;

    while (remaining > 0)
    {
        if (MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT) == WAIT_TIMEOUT)
            break;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        remaining = end - GetTickCount();
    }
}

static COLORREF get_screen_pixel(int x, int y)
{
    COLORREF color;
    HDC dc;

    if (!(dc = GetDC(NULL))) return CLR_INVALID;
    color = GetPixel(dc, x, y);
    ReleaseDC(NULL, dc);
    return color;
}

static COLORREF get_client_screen_pixel(HWND hwnd, int x, int y)
{
    POINT point = {x, y};

    if (!ClientToScreen(hwnd, &point)) return CLR_INVALID;
    return get_screen_pixel(point.x, point.y);
}

static COLORREF get_window_pixel(HWND hwnd, int x, int y)
{
    COLORREF color;
    HDC dc;

    if (!(dc = GetDC(hwnd))) return CLR_INVALID;
    color = GetPixel(dc, x, y);
    ReleaseDC(hwnd, dc);
    return color;
}

static BOOL color_near(COLORREF actual, COLORREF expected, BYTE tolerance)
{
    return actual != CLR_INVALID
            && abs((int)GetRValue(actual) - GetRValue(expected)) <= tolerance
            && abs((int)GetGValue(actual) - GetGValue(expected)) <= tolerance
            && abs((int)GetBValue(actual) - GetBValue(expected)) <= tolerance;
}

static ID3D11Device *create_d3d11_device(void)
{
    ID3D11Device *device = NULL;
    HRESULT hr;

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
            D3D11_SDK_VERSION, &device, NULL, NULL);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                D3D11_SDK_VERSION, &device, NULL, NULL);
    if (FAILED(hr))
        skip("Failed to create a D3D11 device, hr %#lx.\n", hr);
    return device;
}

static HRESULT query_composition_surface(HANDLE surface, HANDLE *resource,
        DXGI_SURFACE_DESC *desc, DXGI_ALPHA_MODE *alpha_mode, BOOL *has_front,
        UINT *front_buffer, UINT *generation)
{
    NTSTATUS status;

    *resource = NULL;
    SERVER_START_REQ(dcomp_query_surface)
    {
        req->surface = wine_server_obj_handle(surface);
        if (!(status = wine_server_call(req)))
        {
            *resource = wine_server_ptr_handle(reply->resource);
            if (reply->sync_resource)
                CloseHandle(wine_server_ptr_handle(reply->sync_resource));
            desc->Width = reply->width;
            desc->Height = reply->height;
            desc->Format = reply->format;
            *alpha_mode = reply->alpha_mode;
            *has_front = reply->has_front;
            desc->SampleDesc.Count = 1;
            desc->SampleDesc.Quality = 0;
            *front_buffer = reply->front_buffer;
            *generation = reply->generation;
        }
    }
    SERVER_END_REQ;

    return status ? HRESULT_FROM_WIN32(RtlNtStatusToDosError(status)) : S_OK;
}

static HRESULT subscribe_composition_surface(HANDLE surface, UINT generation, HANDLE event,
        HANDLE *subscription, struct dcomp_surface_snapshot *snapshot)
{
    struct dcomp_subscription_input input;
    NTSTATUS status;

    input.surface = wine_server_obj_handle(surface);
    input.generation = generation;
    *subscription = NULL;
    memset(snapshot, 0, sizeof(*snapshot));
    SERVER_START_REQ(dcomp_subscribe_surfaces)
    {
        req->event = wine_server_obj_handle(event);
        req->surfaces_size = sizeof(input);
        wine_server_add_data(req, &input, sizeof(input));
        wine_server_set_reply(req, snapshot, sizeof(*snapshot));
        if (!(status = wine_server_call(req)))
        {
            ok(reply->snapshots_size == sizeof(*snapshot),
                    "Got snapshot size %u.\n", reply->snapshots_size);
            *subscription = wine_server_ptr_handle(reply->subscription);
        }
    }
    SERVER_END_REQ;

    return status ? HRESULT_FROM_WIN32(RtlNtStatusToDosError(status)) : S_OK;
}

static HRESULT clear_swapchain_buffer(ID3D11Device *device, ID3D11DeviceContext *context,
        IDXGISwapChain1 *swapchain, const float color[4])
{
    ID3D11RenderTargetView *view = NULL;
    ID3D11Texture2D *texture = NULL;
    HRESULT hr;

    if (FAILED(hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D,
            (void **)&texture)))
        return hr;
    if (SUCCEEDED(hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)texture, NULL, &view)))
    {
        ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
        ID3D11DeviceContext_Flush(context);
        ID3D11RenderTargetView_Release(view);
    }
    ID3D11Texture2D_Release(texture);
    return hr;
}

static HRESULT read_surface_pixel(ID3D11Device *device, ID3D11DeviceContext *context,
        IDXGISurface *surface, DWORD *pixel)
{
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE map;
    HRESULT hr;

    if (FAILED(hr = IDXGISurface_QueryInterface(surface, &IID_ID3D11Texture2D,
            (void **)&texture)))
        return hr;
    ID3D11Texture2D_GetDesc(texture, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (SUCCEEDED(hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging)))
    {
        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                (ID3D11Resource *)texture);
        ID3D11DeviceContext_Flush(context);
        if (SUCCEEDED(hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging,
                0, D3D11_MAP_READ, 0, &map)))
        {
            *pixel = *(const DWORD *)map.pData;
            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(texture);
    return hr;
}

static HRESULT read_swapchain_pixel(ID3D11Device *device, ID3D11DeviceContext *context,
        IDXGISwapChain1 *swapchain, DWORD *pixel)
{
    IDXGISurface *surface;
    HRESULT hr;

    if (FAILED(hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_IDXGISurface,
            (void **)&surface)))
        return hr;
    hr = read_surface_pixel(device, context, surface, pixel);
    IDXGISurface_Release(surface);
    return hr;
}

static HRESULT import_composition_surface(IDXGIDevice *dxgi_device, HANDLE surface,
        IDXGISurface **imported, DXGI_ALPHA_MODE *alpha_mode, BOOL *has_front,
        UINT *front_buffer, UINT *generation)
{
    IWineDXGIDevice *wine_device = NULL;
    DXGI_SURFACE_DESC desc;
    HANDLE resource = NULL, duplicate = NULL;
    HRESULT hr;

    *imported = NULL;
    if (FAILED(hr = query_composition_surface(surface, &resource, &desc,
            alpha_mode, has_front, front_buffer, generation)))
        return hr;
    /* The imported allocation must outlive the query handle independently. */
    if (!DuplicateHandle(GetCurrentProcess(), resource, GetCurrentProcess(), &duplicate,
            0, FALSE, DUPLICATE_SAME_ACCESS))
        hr = HRESULT_FROM_WIN32(GetLastError());
    CloseHandle(resource);
    if (FAILED(hr)) return hr;

    if (SUCCEEDED(hr = IDXGIDevice_QueryInterface(dxgi_device, &IID_IWineDXGIDevice,
            (void **)&wine_device)))
    {
        hr = IWineDXGIDevice_open_composition_shared_surface(wine_device, duplicate,
                &desc, DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT,
                imported);
        IWineDXGIDevice_Release(wine_device);
    }
    CloseHandle(duplicate);
    return hr;
}

static HRESULT create_filled_composition_surface(IDCompositionDevice *composition_device,
        ID3D11Device *device, ID3D11DeviceContext *context, UINT width, UINT height,
        const float color[4], IDCompositionSurface **out)
{
    ID3D11RenderTargetView *view = NULL;
    ID3D11Texture2D *texture = NULL;
    IDXGISurface *draw_surface = NULL;
    POINT offset;
    HRESULT end_hr, hr;
    BOOL drawing = FALSE;

    *out = NULL;
    if (FAILED(hr = IDCompositionDevice_CreateSurface(composition_device, width, height,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, out)))
        return hr;
    if (SUCCEEDED(hr = IDCompositionSurface_BeginDraw(*out, NULL, &IID_IDXGISurface,
            (void **)&draw_surface, &offset))) drawing = TRUE;
    if (SUCCEEDED(hr)
            && SUCCEEDED(hr = IDXGISurface_QueryInterface(draw_surface, &IID_ID3D11Texture2D,
            (void **)&texture))
            && SUCCEEDED(hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)texture, NULL, &view)))
    {
        ID3D11DeviceContext_ClearRenderTargetView(context, view, color);
        ID3D11DeviceContext_Flush(context);
    }
    if (view) ID3D11RenderTargetView_Release(view);
    if (texture) ID3D11Texture2D_Release(texture);
    if (draw_surface) IDXGISurface_Release(draw_surface);
    if (drawing)
    {
        end_hr = IDCompositionSurface_EndDraw(*out);
        if (SUCCEEDED(hr)) hr = end_hr;
    }
    if (FAILED(hr))
    {
        IDCompositionSurface_Release(*out);
        *out = NULL;
    }
    return hr;
}

struct present_thread_args
{
    IDXGISwapChain1 *swapchain;
    HRESULT hr;
};

static DWORD WINAPI present_thread(void *context)
{
    struct present_thread_args *args = context;

    args->hr = IDXGISwapChain1_Present(args->swapchain, 0, 0);
    return 0;
}

static void test_composition_swapchain(void)
{
    static const float first_color[4] = {0.125f, 0.25f, 0.5f, 1.0f};
    static const float second_color[4] = {0.75f, 0.5f, 0.25f, 1.0f};
    static const float base_color[4] = {16.0f / 255.0f, 32.0f / 255.0f,
            48.0f / 255.0f, 1.0f};
    static const float overlay_color[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    DXGI_SWAP_CHAIN_DESC1 desc = {64, 32, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE,
            {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3, DXGI_SCALING_NONE,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT};
    DXGI_PRESENT_PARAMETERS present_parameters = {0};
    DXGI_MATRIX_3X2_F matrix = {0}, result_matrix;
    ID3D11Texture2D *texture = NULL;
    ID3D11DeviceContext *context = NULL;
    IDCompositionDevice *composition_device = NULL;
    IDCompositionDevice *second_composition_device = NULL;
    IDCompositionEffectGroup *composition_effect = NULL;
    IDCompositionEffectGroup *second_composition_effect = NULL;
    IDCompositionEffectGroup *overlay_effect = NULL;
    IDCompositionTarget *composition_target = NULL;
    IDCompositionTarget *second_composition_target = NULL;
    IDCompositionVisual *composition_visual = NULL, *below_visual = NULL;
    IDCompositionVisual *second_composition_visual = NULL;
    IDCompositionVisual *swapchain_visual = NULL, *overlay_visual = NULL;
    IDCompositionSurface *below_surface = NULL, *overlay_surface = NULL;
    D3D11_TEXTURE2D_DESC texture_desc;
    IDXGISwapChain1 *swapchain1 = NULL, *other = NULL;
    IDXGISwapChain2 *swapchain2 = NULL;
    IDXGIFactoryMedia *factory_media = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi_device = NULL;
    IDXGISurface *imported = NULL;
    ID3D11Device *device;
    struct dcomp_surface_snapshot snapshot = {0}, second_snapshot = {0};
    HANDLE surface, readonly_surface, event, latency, subscription = NULL, subscription_event = NULL;
    HANDLE second_subscription = NULL, second_subscription_event = NULL;
    HANDLE thread = NULL;
    struct present_thread_args present_args;
    UINT present_count, first_front = 0, second_front = 0;
    UINT first_generation = 0, second_generation = 0;
    DXGI_ALPHA_MODE alpha_mode = DXGI_ALPHA_MODE_UNSPECIFIED;
    BOOL has_front = FALSE;
    DWORD pixel;
    HRESULT hr;
    HWND composition_parent = NULL;
    HWND composition_hwnd = NULL;
    HWND second_composition_parent = NULL;
    HWND second_composition_hwnd = NULL;
    HBRUSH composition_brush = NULL;
    ATOM composition_class = 0, composition_child_class = 0;
    COLORREF color, first_composed_color = CLR_INVALID;
    POINT old_point;
    BOOL screen_capture_supported = FALSE;

    if (!(device = create_d3d11_device())) return;
    ID3D11Device_GetImmediateContext(device, &context);
    ok(!!context, "Failed to get the immediate context.\n");
    if (!context) goto done;
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "IDXGIDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    ok(hr == S_OK, "GetAdapter failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    ok(hr == S_OK, "Factory query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactoryMedia,
            (void **)&factory_media);
    ok(hr == S_OK, "IDXGIFactoryMedia query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(factory_media,
            (IUnknown *)device, event, &desc, NULL, &swapchain1);
    ok(FAILED(hr), "Non-composition handle unexpectedly succeeded.\n");
    ok(!swapchain1, "Got unexpected swapchain %p.\n", swapchain1);
    CloseHandle(event);

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_READ, NULL, &readonly_surface);
    ok(hr == S_OK, "Read-only surface creation failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(factory_media,
                (IUnknown *)device, readonly_surface, &desc, NULL, &swapchain1);
        ok(hr == E_ACCESSDENIED, "Read-only bind returned hr %#lx.\n", hr);
        ok(!swapchain1, "Got unexpected swapchain %p.\n", swapchain1);
        CloseHandle(readonly_surface);
    }

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, NULL, &surface);
    ok(hr == S_OK, "Surface creation failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(factory_media,
            (IUnknown *)device, surface, &desc, NULL, &swapchain1);
    if (hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED)
        win_skip("GPU external-memory sharing is unavailable, hr %#lx.\n", hr);
    else
        ok(hr == S_OK, "Composition swapchain creation failed, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        CloseHandle(surface);
        goto done;
    }

    {
        DXGI_SURFACE_DESC unpublished_desc;
        HANDLE unpublished_resource;
        UINT unpublished_front, unpublished_generation;

        hr = query_composition_surface(surface, &unpublished_resource, &unpublished_desc,
                &alpha_mode, &has_front, &unpublished_front, &unpublished_generation);
        ok(hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
                "Unpublished surface query returned hr %#lx.\n", hr);
        ok(!unpublished_resource, "Got unpublished resource %p.\n", unpublished_resource);
    }

    hr = clear_swapchain_buffer(device, context, swapchain1, first_color);
    ok(hr == S_OK, "Failed to clear the first composition buffer, hr %#lx.\n", hr);
    hr = read_swapchain_pixel(device, context, swapchain1, &pixel);
    ok(hr == S_OK && pixel == 0xff204080,
            "Got producer first pixel %#lx, hr %#lx.\n", pixel, hr);
    hr = IDXGISwapChain1_Present(swapchain1, 0, 0);
    ok(hr == S_OK, "Initial composition Present failed, hr %#lx.\n", hr);
    hr = import_composition_surface(dxgi_device, surface, &imported, &alpha_mode, &has_front,
            &first_front, &first_generation);
    ok(hr == S_OK, "Failed to import the first published buffer, hr %#lx.\n", hr);
    ok(alpha_mode == DXGI_ALPHA_MODE_PREMULTIPLIED,
            "Got alpha mode %u.\n", alpha_mode);
    ok(has_front, "Published surface has no front buffer.\n");
    if (SUCCEEDED(hr))
    {
        hr = read_surface_pixel(device, context, imported, &pixel);
        ok(hr == S_OK, "Failed to read the imported first buffer, hr %#lx.\n", hr);
        ok(pixel == 0xff204080, "Got first imported pixel %#lx.\n", pixel);
        IDXGISurface_Release(imported);
        imported = NULL;
    }

    {
        ID3D11DeviceContext *other_context = NULL;
        ID3D11Device *other_device = create_d3d11_device();
        IDXGIDevice *other_dxgi_device = NULL;

        if (other_device)
        {
            ID3D11Device_GetImmediateContext(other_device, &other_context);
            hr = ID3D11Device_QueryInterface(other_device, &IID_IDXGIDevice,
                    (void **)&other_dxgi_device);
            ok(hr == S_OK, "Second-device IDXGIDevice query failed, hr %#lx.\n", hr);
            if (SUCCEEDED(hr))
            {
                hr = import_composition_surface(other_dxgi_device, surface, &imported,
                        &alpha_mode, &has_front, &first_front, &first_generation);
                ok(hr == S_OK, "Cross-device surface import failed, hr %#lx.\n", hr);
                if (SUCCEEDED(hr))
                {
                    hr = read_surface_pixel(other_device, other_context, imported, &pixel);
                    ok(hr == S_OK && pixel == 0xff204080,
                            "Cross-device imported pixel %#lx, hr %#lx.\n", pixel, hr);
                    IDXGISurface_Release(imported);
                    imported = NULL;
                }
            }
            if (other_dxgi_device) IDXGIDevice_Release(other_dxgi_device);
            if (other_context) ID3D11DeviceContext_Release(other_context);
            ID3D11Device_Release(other_device);
        }
    }

    if (pDCompositionCreateDevice)
    {
        hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice,
                (void **)&composition_device);
        ok(hr == S_OK, "DCompositionCreateDevice for swapchain failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            WNDCLASSW child_wc = {0}, wc = {0};

            composition_brush = CreateSolidBrush(RGB(16, 32, 48));
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = GetModuleHandleW(NULL);
            wc.hbrBackground = composition_brush;
            wc.lpszClassName = L"WineDCompSwapchainPixelTest";
            composition_class = RegisterClassW(&wc);
            ok(!!composition_class, "Failed to register swapchain pixel class, error %lu.\n",
                    GetLastError());
            child_wc.lpfnWndProc = DefWindowProcW;
            child_wc.hInstance = wc.hInstance;
            child_wc.lpszClassName = L"WineDCompLayeredChildPixelTest";
            composition_child_class = RegisterClassW(&child_wc);
            ok(!!composition_child_class,
                    "Failed to register layered child pixel class, error %lu.\n", GetLastError());
            composition_parent = CreateWindowExW(0, wc.lpszClassName, L"dcomp parent",
                    WS_POPUP | WS_VISIBLE, 80, 80, 96, 64, NULL, NULL, wc.hInstance, NULL);
            ok(!!composition_parent, "Failed to create swapchain parent window.\n");
            composition_hwnd = CreateWindowExW(WS_EX_LAYERED, child_wc.lpszClassName,
                    L"dcomp layered child", WS_CHILD | WS_VISIBLE, 0, 0, 64, 32,
                    composition_parent, NULL, child_wc.hInstance, NULL);
            if (composition_hwnd) MoveWindow(composition_hwnd, 8, 8, 64, 32, TRUE);
            ok(!!composition_hwnd, "Failed to create layered child target window.\n");
            second_composition_parent = CreateWindowExW(0, wc.lpszClassName,
                    L"second dcomp parent", WS_POPUP | WS_VISIBLE, 240, 80, 96, 64,
                    NULL, NULL, wc.hInstance, NULL);
            ok(!!second_composition_parent, "Failed to create second composition parent.\n");
            second_composition_hwnd = CreateWindowExW(WS_EX_LAYERED, child_wc.lpszClassName,
                    L"second dcomp layered child", WS_CHILD | WS_VISIBLE, 8, 8, 64, 32,
                    second_composition_parent, NULL, child_wc.hInstance, NULL);
            ok(!!second_composition_hwnd, "Failed to create second layered child target.\n");
            if (composition_parent) UpdateWindow(composition_parent);
            if (composition_hwnd) UpdateWindow(composition_hwnd);
            if (second_composition_parent) UpdateWindow(second_composition_parent);
            if (second_composition_hwnd) UpdateWindow(second_composition_hwnd);
            if (composition_parent)
            {
                color = get_client_screen_pixel(composition_parent, 2, 2);
                screen_capture_supported = color_near(color, RGB(16, 32, 48), 1);
                if (!screen_capture_supported)
                    win_skip("Desktop DC cannot observe the Xwayland rootless window surface; "
                            "screen-composition assertions are unavailable (pixel %#lx).\n", color);
            }
            hr = IDCompositionDevice_CreateVisual(composition_device, &composition_visual);
            ok(hr == S_OK, "Create root visual failed, hr %#lx.\n", hr);
            hr = create_filled_composition_surface(composition_device, device, context, 64, 32,
                    base_color, &below_surface);
            ok(hr == S_OK, "Create CPU-below surface failed, hr %#lx.\n", hr);
            hr = create_filled_composition_surface(composition_device, device, context, 8, 8,
                    overlay_color, &overlay_surface);
            ok(hr == S_OK, "Create CPU-overlay surface failed, hr %#lx.\n", hr);
            hr = IDCompositionDevice_CreateVisual(composition_device, &below_visual);
            ok(hr == S_OK, "Create CPU-below visual failed, hr %#lx.\n", hr);
            if (below_visual && below_surface)
            {
                hr = IDCompositionVisual_SetContent(below_visual, (IUnknown *)below_surface);
                ok(hr == S_OK, "SetContent(CPU below) failed, hr %#lx.\n", hr);
            }
            hr = IDCompositionDevice_CreateVisual(composition_device, &swapchain_visual);
            ok(hr == S_OK, "Create swapchain visual failed, hr %#lx.\n", hr);
            if (swapchain_visual)
            {
                hr = IDCompositionVisual_SetContent(swapchain_visual, (IUnknown *)swapchain1);
                ok(hr == S_OK, "SetContent(composition swapchain) failed, hr %#lx.\n", hr);
            }
            hr = IDCompositionDevice_CreateVisual(composition_device, &overlay_visual);
            ok(hr == S_OK, "Create CPU-overlay visual failed, hr %#lx.\n", hr);
            if (overlay_visual && overlay_surface)
            {
                hr = IDCompositionVisual_SetContent(overlay_visual, (IUnknown *)overlay_surface);
                ok(hr == S_OK, "SetContent(CPU overlay) failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_SetOffsetX(overlay_visual, 40.0f);
                ok(hr == S_OK, "SetOffsetX(CPU overlay) failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_SetOffsetY(overlay_visual, 8.0f);
                ok(hr == S_OK, "SetOffsetY(CPU overlay) failed, hr %#lx.\n", hr);
            }
            hr = IDCompositionDevice_CreateEffectGroup(composition_device, &composition_effect);
            ok(hr == S_OK, "CreateEffectGroup for swapchain failed, hr %#lx.\n", hr);
            if (SUCCEEDED(hr) && swapchain_visual)
            {
                hr = IDCompositionEffectGroup_SetOpacity(composition_effect, 0.5f);
                ok(hr == S_OK, "SetOpacity for swapchain failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_SetEffect(swapchain_visual,
                        (IDCompositionEffect *)composition_effect);
                ok(hr == S_OK, "SetEffect for swapchain failed, hr %#lx.\n", hr);
            }
            hr = IDCompositionDevice_CreateEffectGroup(composition_device, &overlay_effect);
            ok(hr == S_OK, "CreateEffectGroup for CPU overlay failed, hr %#lx.\n", hr);
            if (SUCCEEDED(hr) && overlay_visual)
            {
                hr = IDCompositionEffectGroup_SetOpacity(overlay_effect, 0.5f);
                ok(hr == S_OK, "SetOpacity for CPU overlay failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_SetEffect(overlay_visual,
                        (IDCompositionEffect *)overlay_effect);
                ok(hr == S_OK, "SetEffect for CPU overlay failed, hr %#lx.\n", hr);
            }
            if (composition_visual && below_visual && swapchain_visual && overlay_visual)
            {
                hr = IDCompositionVisual_AddVisual(composition_visual, below_visual, TRUE, NULL);
                ok(hr == S_OK, "Add CPU-below visual failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_AddVisual(composition_visual, swapchain_visual,
                        TRUE, below_visual);
                ok(hr == S_OK, "Add swapchain visual failed, hr %#lx.\n", hr);
                hr = IDCompositionVisual_AddVisual(composition_visual, overlay_visual,
                        TRUE, swapchain_visual);
                ok(hr == S_OK, "Add CPU-overlay visual failed, hr %#lx.\n", hr);
            }
            if (composition_hwnd)
            {
                hr = IDCompositionDevice_CreateTargetForHwnd(composition_device,
                        composition_hwnd, FALSE, &composition_target);
                ok(hr == S_OK, "CreateTargetForHwnd for swapchain failed, hr %#lx.\n", hr);
                if (SUCCEEDED(hr))
                {
                    hr = IDCompositionTarget_SetRoot(composition_target, composition_visual);
                    ok(hr == S_OK, "SetRoot for swapchain failed, hr %#lx.\n", hr);
                    hr = IDCompositionDevice_Commit(composition_device);
                    ok(hr == S_OK, "Composition swapchain Commit failed, hr %#lx.\n", hr);
                    if (SUCCEEDED(hr))
                    {
                        RedrawWindow(composition_hwnd, NULL, NULL,
                                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                        flush_events();
                        first_composed_color = get_client_screen_pixel(composition_hwnd, 8, 8);
                        if (screen_capture_supported)
                            ok(color_near(first_composed_color, RGB(24, 48, 88), 2),
                                    "Got first composed desktop pixel %#lx.\n", first_composed_color);
                        color = get_window_pixel(composition_parent, 16, 16);
                        ok(color_near(color, RGB(16, 32, 48), 1),
                                "Composition contaminated the parent backing store, pixel %#lx.\n",
                                color);
                        color = get_client_screen_pixel(composition_hwnd, 44, 12);
                        if (screen_capture_supported) ok(color_near(color, RGB(12, 152, 44), 2),
                                "Mixed CPU/DXGI/CPU overlap desktop pixel is %#lx.\n", color);
                        color = get_window_pixel(composition_parent, 52, 20);
                        ok(color_near(color, RGB(16, 32, 48), 1),
                                "Mixed composition contaminated the parent backing store, "
                                "pixel %#lx.\n", color);
                        RedrawWindow(composition_hwnd, NULL, NULL,
                                RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
                        flush_events();
                        color = get_client_screen_pixel(composition_hwnd, 8, 8);
                        if (screen_capture_supported) ok(color == first_composed_color,
                                "Expose replay accumulated alpha, before %#lx after %#lx.\n",
                                first_composed_color, color);
                        color = get_window_pixel(composition_parent, 16, 16);
                        ok(color_near(color, RGB(16, 32, 48), 1),
                                "Child repaint contaminated the parent backing store, pixel %#lx.\n",
                                color);
                    }
                }
            }
            if (second_composition_hwnd)
            {
                hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice,
                        (void **)&second_composition_device);
                ok(hr == S_OK, "Create second DComp device failed, hr %#lx.\n", hr);
                if (SUCCEEDED(hr))
                {
                    hr = IDCompositionDevice_CreateVisual(second_composition_device,
                            &second_composition_visual);
                    ok(hr == S_OK, "Create second composition visual failed, hr %#lx.\n", hr);
                    if (SUCCEEDED(hr) && second_composition_visual)
                    {
                        hr = IDCompositionVisual_SetContent(second_composition_visual,
                                (IUnknown *)swapchain1);
                        ok(hr == S_OK, "Set second composition content failed, hr %#lx.\n", hr);
                    }
                    hr = IDCompositionDevice_CreateEffectGroup(second_composition_device,
                            &second_composition_effect);
                    ok(hr == S_OK, "Create second effect failed, hr %#lx.\n", hr);
                    if (SUCCEEDED(hr) && second_composition_effect
                            && second_composition_visual)
                    {
                        hr = IDCompositionEffectGroup_SetOpacity(second_composition_effect, 0.5f);
                        ok(hr == S_OK, "Set second opacity failed, hr %#lx.\n", hr);
                        hr = IDCompositionVisual_SetEffect(second_composition_visual,
                                (IDCompositionEffect *)second_composition_effect);
                        ok(hr == S_OK, "Set second effect failed, hr %#lx.\n", hr);
                    }
                    hr = IDCompositionDevice_CreateTargetForHwnd(second_composition_device,
                            second_composition_hwnd, FALSE, &second_composition_target);
                    ok(hr == S_OK, "Create second target failed, hr %#lx.\n", hr);
                    if (SUCCEEDED(hr) && second_composition_target
                            && second_composition_visual)
                    {
                        hr = IDCompositionTarget_SetRoot(second_composition_target,
                                second_composition_visual);
                        ok(hr == S_OK, "Set second target root failed, hr %#lx.\n", hr);
                        hr = IDCompositionDevice_Commit(second_composition_device);
                        ok(hr == S_OK, "Second-device same-generation Commit failed, hr %#lx.\n",
                                hr);
                        if (SUCCEEDED(hr))
                        {
                            flush_events();
                            color = get_client_screen_pixel(second_composition_hwnd, 8, 8);
                            if (screen_capture_supported)
                                ok(color_near(color, RGB(24, 48, 88), 2),
                                        "Second timeline consumer got pixel %#lx.\n", color);
                        }
                    }
                }
            }
            if (below_surface)
            {
                IDCompositionSurface_Release(below_surface);
                below_surface = NULL;
            }
            if (overlay_surface)
            {
                IDCompositionSurface_Release(overlay_surface);
                overlay_surface = NULL;
            }
        }
    }

    subscription_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!subscription_event, "Failed to create subscription event, error %lu.\n", GetLastError());
    if (subscription_event)
    {
        hr = subscribe_composition_surface(surface, first_generation, subscription_event,
                &subscription, &snapshot);
        ok(hr == S_OK, "Surface subscription failed, hr %#lx.\n", hr);
        ok(snapshot.has_front && snapshot.generation == first_generation,
                "Got snapshot front %u, generation %u.\n",
                snapshot.has_front, snapshot.generation);
        ok(snapshot.alpha_mode == DXGI_ALPHA_MODE_PREMULTIPLIED,
                "Got snapshot alpha mode %u.\n", snapshot.alpha_mode);
        if (snapshot.resource) CloseHandle(wine_server_ptr_handle(snapshot.resource));
        if (snapshot.sync_resource)
        {
            CloseHandle(wine_server_ptr_handle(snapshot.sync_resource));
            snapshot.sync_resource = 0;
        }
        ok(WaitForSingleObject(subscription_event, 0) == WAIT_TIMEOUT,
                "Subscription event was signaled before Present.\n");
    }

    hr = clear_swapchain_buffer(device, context, swapchain1, second_color);
    ok(hr == S_OK, "Failed to clear the second composition buffer, hr %#lx.\n", hr);
    hr = read_swapchain_pixel(device, context, swapchain1, &pixel);
    ok(hr == S_OK && pixel == 0xffbf8040,
            "Got producer second pixel %#lx, hr %#lx.\n", pixel, hr);
    hr = IDXGISwapChain1_Present(swapchain1, 0, 0);
    ok(hr == S_OK, "Second composition Present failed, hr %#lx.\n", hr);
    if (subscription_event)
        ok(WaitForSingleObject(subscription_event, 1000) == WAIT_OBJECT_0,
                "Present did not wake the retained surface subscription.\n");
    hr = import_composition_surface(dxgi_device, surface, &imported, &alpha_mode, &has_front,
            &second_front, &second_generation);
    ok(hr == S_OK, "Failed to import the second published buffer, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = read_surface_pixel(device, context, imported, &pixel);
        ok(hr == S_OK, "Failed to read the imported second buffer, hr %#lx.\n", hr);
        ok(pixel == 0xffbf8040, "Got second imported pixel %#lx.\n", pixel);
        IDXGISurface_Release(imported);
        imported = NULL;
    }
    ok(second_generation == first_generation + 1,
            "Generation did not advance from %u to %u.\n", first_generation, second_generation);
    ok(second_front != first_front, "Front buffer did not advance from %u.\n", first_front);
    if (composition_hwnd && composition_device)
    {
        RedrawWindow(composition_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        flush_events();
        color = get_client_screen_pixel(composition_hwnd, 8, 8);
        if (screen_capture_supported) ok(color_near(color, RGB(104, 80, 56), 2),
                "Present/SURFACES refresh produced pixel %#lx without another Commit.\n", color);
        color = get_window_pixel(composition_parent, 16, 16);
        ok(color_near(color, RGB(16, 32, 48), 1),
                "Present/SURFACES refresh contaminated the parent backing store, pixel %#lx.\n",
                color);
        color = get_client_screen_pixel(composition_hwnd, 44, 12);
        if (screen_capture_supported) ok(color_near(color, RGB(52, 168, 28), 2),
                "Present/SURFACES mixed overlap produced pixel %#lx.\n", color);
        if (second_composition_hwnd)
        {
            color = get_client_screen_pixel(second_composition_hwnd, 8, 8);
            if (screen_capture_supported) ok(color_near(color, RGB(104, 80, 56), 2),
                    "Second timeline consumer did not refresh, pixel %#lx.\n", color);
        }
        hr = IDCompositionVisual_SetOffsetX(swapchain_visual, 16.0f);
        ok(hr == S_OK, "SetOffsetX for second Commit failed, hr %#lx.\n", hr);
        hr = IDCompositionVisual_RemoveVisual(composition_visual, overlay_visual);
        ok(hr == S_OK, "Remove CPU overlay for second Commit failed, hr %#lx.\n", hr);
        hr = IDCompositionVisual_AddVisual(composition_visual, overlay_visual,
                FALSE, swapchain_visual);
        ok(hr == S_OK, "Reorder CPU overlay for second Commit failed, hr %#lx.\n", hr);
        hr = IDCompositionDevice_Commit(composition_device);
        ok(hr == S_OK, "Second graph Commit failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            RedrawWindow(composition_hwnd, NULL, NULL,
                    RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            flush_events();
            color = get_client_screen_pixel(composition_hwnd, 8, 8);
            if (screen_capture_supported) ok(color_near(color, RGB(16, 32, 48), 1),
                    "Second Commit left old transform pixel %#lx.\n", color);
            color = get_client_screen_pixel(composition_hwnd, 24, 8);
            if (screen_capture_supported) ok(color_near(color, RGB(104, 80, 56), 2),
                    "Second Commit transformed pixel is %#lx.\n", color);
            color = get_client_screen_pixel(composition_hwnd, 44, 12);
            if (screen_capture_supported) ok(color_near(color, RGB(100, 136, 44), 2),
                    "Second Commit reordered overlap pixel is %#lx.\n", color);
            color = get_window_pixel(composition_parent, 32, 16);
            ok(color_near(color, RGB(16, 32, 48), 1),
                    "Second Commit contaminated the parent backing store, pixel %#lx.\n", color);
            old_point.x = 16;
            old_point.y = 8;
            ok(ClientToScreen(composition_hwnd, &old_point),
                    "Failed to resolve layered child screen coordinates.\n");
            ok(MoveWindow(composition_hwnd, 16, 8, 64, 32, TRUE),
                    "Failed to move layered child, error %lu.\n", GetLastError());
            RedrawWindow(composition_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            flush_events();
            color = get_screen_pixel(old_point.x, old_point.y);
            if (screen_capture_supported) ok(color_near(color, RGB(16, 32, 48), 1),
                    "Old layered child location retained composed pixel %#lx.\n", color);
            color = get_client_screen_pixel(composition_hwnd, 16, 8);
            if (screen_capture_supported) ok(color_near(color, RGB(104, 80, 56), 2),
                    "Moved layered child location produced pixel %#lx.\n", color);
            color = get_window_pixel(composition_parent, 24, 16);
            ok(color_near(color, RGB(16, 32, 48), 1),
                    "Old parent-surface child location retained pixel %#lx.\n", color);
            color = get_window_pixel(composition_parent, 32, 16);
            ok(color_near(color, RGB(16, 32, 48), 1),
                    "Moved child contaminated the parent backing store, pixel %#lx.\n", color);
        }
    }

    memset(&second_snapshot, 0, sizeof(second_snapshot));
    second_subscription_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!second_subscription_event, "Failed to create second subscription event.\n");
    if (second_subscription_event)
    {
        hr = subscribe_composition_surface(surface, second_generation,
                second_subscription_event, &second_subscription, &second_snapshot);
        ok(hr == S_OK, "Second surface subscription failed, hr %#lx.\n", hr);
        if (second_snapshot.resource)
        {
            CloseHandle(wine_server_ptr_handle(second_snapshot.resource));
            second_snapshot.resource = 0;
        }
        if (second_snapshot.sync_resource)
        {
            CloseHandle(wine_server_ptr_handle(second_snapshot.sync_resource));
            second_snapshot.sync_resource = 0;
        }
    }
    hr = clear_swapchain_buffer(device, context, swapchain1, first_color);
    ok(hr == S_OK, "Failed to clear the leased-cycle buffer, hr %#lx.\n", hr);
    hr = IDXGISwapChain1_Present(swapchain1, 0, DXGI_PRESENT_DO_NOT_WAIT);
    ok(hr == DXGI_ERROR_WAS_STILL_DRAWING,
            "Present overwrote a leased composition buffer, hr %#lx.\n", hr);
    present_args.swapchain = swapchain1;
    present_args.hr = E_UNEXPECTED;
    thread = CreateThread(NULL, 0, present_thread, &present_args, 0, NULL);
    ok(!!thread, "Failed to create blocking Present thread, error %lu.\n", GetLastError());
    if (thread)
        ok(WaitForSingleObject(thread, 100) == WAIT_TIMEOUT,
                "Blocking Present completed while every reusable buffer was leased, hr %#lx.\n",
                present_args.hr);
    if (snapshot.lease)
    {
        CloseHandle(wine_server_ptr_handle(snapshot.lease));
        snapshot.lease = 0;
    }
    if (thread)
    {
        ok(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0,
                "Blocking Present was not woken after releasing a buffer lease.\n");
        ok(present_args.hr == S_OK,
                "Blocking Present did not resume after releasing a buffer lease, hr %#lx.\n",
                present_args.hr);
        CloseHandle(thread);
        thread = NULL;
    }
    if (second_snapshot.lease)
    {
        CloseHandle(wine_server_ptr_handle(second_snapshot.lease));
        second_snapshot.lease = 0;
    }

    hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain2,
            (void **)&swapchain2);
    ok(hr == S_OK, "IDXGISwapChain2 query failed, hr %#lx.\n", hr);
    hr = IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(factory_media,
            (IUnknown *)device, surface, &desc, NULL, &other);
    ok(hr == DXGI_ERROR_INVALID_CALL, "Duplicate surface bind returned hr %#lx.\n", hr);
    ok(!other, "Got unexpected second swapchain %p.\n", other);

    hr = IDXGISwapChain1_GetBuffer(swapchain1, 0, &IID_ID3D11Texture2D, (void **)&texture);
    ok(hr == S_OK, "GetBuffer failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ID3D11Texture2D_GetDesc(texture, &texture_desc);
        ok(texture_desc.Width == 64 && texture_desc.Height == 32,
                "Got texture size %ux%u.\n", texture_desc.Width, texture_desc.Height);
        hr = IDXGISwapChain1_ResizeBuffers(swapchain1, 0, 80, 40,
                DXGI_FORMAT_UNKNOWN, desc.Flags);
        ok(hr == DXGI_ERROR_INVALID_CALL, "Resize with referenced buffer returned hr %#lx.\n", hr);
        ID3D11Texture2D_Release(texture);
        texture = NULL;
    }

    hr = IDXGISwapChain1_ResizeBuffers(swapchain1, 0, 80, 40,
            DXGI_FORMAT_UNKNOWN, desc.Flags);
    ok(hr == S_OK, "ResizeBuffers failed, hr %#lx.\n", hr);
    hr = IDXGISwapChain1_GetDesc1(swapchain1, &desc);
    ok(hr == S_OK, "GetDesc1 failed, hr %#lx.\n", hr);
    ok(desc.Width == 80 && desc.Height == 40 && desc.BufferCount == 3,
            "Got desc %ux%u buffers %u.\n", desc.Width, desc.Height, desc.BufferCount);
    hr = clear_swapchain_buffer(device, context, swapchain1, first_color);
    ok(hr == S_OK, "Failed to clear resized composition buffer, hr %#lx.\n", hr);

    matrix._11 = 0.75f;
    matrix._22 = 0.75f;
    hr = IDXGISwapChain2_SetMatrixTransform(swapchain2, &matrix);
    ok(hr == S_OK, "SetMatrixTransform failed, hr %#lx.\n", hr);
    hr = IDXGISwapChain2_GetMatrixTransform(swapchain2, &result_matrix);
    ok(hr == S_OK && !memcmp(&matrix, &result_matrix, sizeof(matrix)),
            "GetMatrixTransform returned hr %#lx.\n", hr);
    hr = IDXGISwapChain2_SetMaximumFrameLatency(swapchain2, 1);
    ok(hr == S_OK, "SetMaximumFrameLatency failed, hr %#lx.\n", hr);
    latency = IDXGISwapChain2_GetFrameLatencyWaitableObject(swapchain2);
    ok(!!latency, "GetFrameLatencyWaitableObject returned NULL.\n");
    if (latency)
    {
        ok(WaitForSingleObject(latency, 0) == WAIT_OBJECT_0,
                "Initial frame latency wait did not signal.\n");
        present_parameters.DirtyRectsCount = 0;
        hr = IDXGISwapChain2_Present1(swapchain2, 1, 0, &present_parameters);
        ok(hr == S_OK, "Present1 failed, hr %#lx.\n", hr);
        ok(WaitForSingleObject(latency, 0) == WAIT_OBJECT_0,
                "Frame latency wait did not signal after Present1.\n");
        CloseHandle(latency);
    }
    hr = IDXGISwapChain1_GetLastPresentCount(swapchain1, &present_count);
    ok(hr == S_OK && present_count == 4, "Got present count %u, hr %#lx.\n",
            present_count, hr);

    IDXGISwapChain2_Release(swapchain2);
    swapchain2 = NULL;
    IDXGISwapChain1_Release(swapchain1);
    swapchain1 = NULL;
    if (composition_hwnd && composition_device)
    {
        RedrawWindow(composition_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        flush_events();
        color = get_client_screen_pixel(composition_hwnd, 24, 8);
        if (screen_capture_supported) ok(color_near(color, RGB(24, 48, 88), 2),
                "Retained visual lost producer content after caller release, pixel %#lx.\n", color);
        color = get_window_pixel(composition_parent, 40, 16);
        ok(color_near(color, RGB(16, 32, 48), 1),
                "Retained content contaminated the parent backing store, pixel %#lx.\n", color);
        old_point.x = 24;
        old_point.y = 8;
        ClientToScreen(composition_hwnd, &old_point);
        DestroyWindow(composition_hwnd);
        composition_hwnd = NULL;
        flush_events();
        color = get_screen_pixel(old_point.x, old_point.y);
        if (screen_capture_supported) ok(color_near(color, RGB(16, 32, 48), 1),
                "Destroyed layered child left composed pixel %#lx.\n", color);
        color = get_window_pixel(composition_parent, 40, 16);
        ok(color_near(color, RGB(16, 32, 48), 1),
                "Destroyed layered child left parent-surface composition %#lx.\n", color);
        hr = IDCompositionDevice_Commit(composition_device);
        ok(hr == E_INVALIDARG,
                "Commit after target HWND destruction returned hr %#lx.\n", hr);
    }
    if (composition_target) IDCompositionTarget_Release(composition_target);
    composition_target = NULL;
    if (below_visual) IDCompositionVisual_Release(below_visual);
    below_visual = NULL;
    if (swapchain_visual) IDCompositionVisual_Release(swapchain_visual);
    swapchain_visual = NULL;
    if (overlay_visual) IDCompositionVisual_Release(overlay_visual);
    overlay_visual = NULL;
    if (composition_visual) IDCompositionVisual_Release(composition_visual);
    composition_visual = NULL;
    if (overlay_effect) IDCompositionEffectGroup_Release(overlay_effect);
    overlay_effect = NULL;
    if (composition_effect) IDCompositionEffectGroup_Release(composition_effect);
    composition_effect = NULL;
    if (composition_device) IDCompositionDevice_Release(composition_device);
    composition_device = NULL;
    if (second_composition_target) IDCompositionTarget_Release(second_composition_target);
    second_composition_target = NULL;
    if (second_composition_visual) IDCompositionVisual_Release(second_composition_visual);
    second_composition_visual = NULL;
    if (second_composition_effect)
        IDCompositionEffectGroup_Release(second_composition_effect);
    second_composition_effect = NULL;
    if (second_composition_device) IDCompositionDevice_Release(second_composition_device);
    second_composition_device = NULL;
    if (second_composition_hwnd)
    {
        DestroyWindow(second_composition_hwnd);
        second_composition_hwnd = NULL;
    }
    if (second_composition_parent)
    {
        DestroyWindow(second_composition_parent);
        second_composition_parent = NULL;
    }
    if (composition_parent)
    {
        DestroyWindow(composition_parent);
        composition_parent = NULL;
    }
    if (composition_child_class)
    {
        UnregisterClassW(L"WineDCompLayeredChildPixelTest", GetModuleHandleW(NULL));
        composition_child_class = 0;
    }
    if (composition_class)
    {
        UnregisterClassW(L"WineDCompSwapchainPixelTest", GetModuleHandleW(NULL));
        composition_class = 0;
    }
    if (composition_brush)
    {
        DeleteObject(composition_brush);
        composition_brush = NULL;
    }

    hr = IDXGIFactoryMedia_CreateSwapChainForCompositionSurfaceHandle(factory_media,
            (IUnknown *)device, surface, &desc, NULL, &swapchain1);
    ok(hr == S_OK, "Surface did not detach after swapchain teardown, hr %#lx.\n", hr);
    CloseHandle(surface);
    if (SUCCEEDED(hr))
    {
        hr = IDXGISwapChain1_Present(swapchain1, 0, 0);
        ok(hr == S_OK, "Present after source handle close failed, hr %#lx.\n", hr);
    }

done:
    if (thread)
    {
        if (snapshot.lease)
        {
            CloseHandle(wine_server_ptr_handle(snapshot.lease));
            snapshot.lease = 0;
        }
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    if (composition_target) IDCompositionTarget_Release(composition_target);
    if (second_composition_target) IDCompositionTarget_Release(second_composition_target);
    if (below_visual) IDCompositionVisual_Release(below_visual);
    if (swapchain_visual) IDCompositionVisual_Release(swapchain_visual);
    if (overlay_visual) IDCompositionVisual_Release(overlay_visual);
    if (composition_visual) IDCompositionVisual_Release(composition_visual);
    if (second_composition_visual) IDCompositionVisual_Release(second_composition_visual);
    if (overlay_effect) IDCompositionEffectGroup_Release(overlay_effect);
    if (composition_effect) IDCompositionEffectGroup_Release(composition_effect);
    if (second_composition_effect)
        IDCompositionEffectGroup_Release(second_composition_effect);
    if (below_surface) IDCompositionSurface_Release(below_surface);
    if (overlay_surface) IDCompositionSurface_Release(overlay_surface);
    if (composition_device) IDCompositionDevice_Release(composition_device);
    if (second_composition_device) IDCompositionDevice_Release(second_composition_device);
    if (composition_hwnd) DestroyWindow(composition_hwnd);
    if (second_composition_hwnd) DestroyWindow(second_composition_hwnd);
    if (composition_parent) DestroyWindow(composition_parent);
    if (second_composition_parent) DestroyWindow(second_composition_parent);
    if (composition_child_class)
        UnregisterClassW(L"WineDCompLayeredChildPixelTest", GetModuleHandleW(NULL));
    if (composition_class)
        UnregisterClassW(L"WineDCompSwapchainPixelTest", GetModuleHandleW(NULL));
    if (composition_brush) DeleteObject(composition_brush);
    if (snapshot.lease) CloseHandle(wine_server_ptr_handle(snapshot.lease));
    if (second_snapshot.resource) CloseHandle(wine_server_ptr_handle(second_snapshot.resource));
    if (second_snapshot.sync_resource)
        CloseHandle(wine_server_ptr_handle(second_snapshot.sync_resource));
    if (second_snapshot.lease) CloseHandle(wine_server_ptr_handle(second_snapshot.lease));
    if (second_subscription) CloseHandle(second_subscription);
    if (second_subscription_event) CloseHandle(second_subscription_event);
    if (subscription) CloseHandle(subscription);
    if (subscription_event) CloseHandle(subscription_event);
    if (imported) IDXGISurface_Release(imported);
    if (texture) ID3D11Texture2D_Release(texture);
    if (swapchain2) IDXGISwapChain2_Release(swapchain2);
    if (swapchain1) IDXGISwapChain1_Release(swapchain1);
    if (other) IDXGISwapChain1_Release(other);
    if (factory_media) IDXGIFactoryMedia_Release(factory_media);
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (context) ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
}

/* IDXGIFactory2::CreateSwapChainForComposition() is the entry point a XAML
 * swap chain panel uses: it never names a composition surface handle.  The
 * buffer is only rendered into, never read back through DXGI, so Present() is
 * the one place that can hand the finished contents to the composition surface
 * pipeline. */
static void test_composition_swapchain_present(void)
{
    static const float color[4] = {32.0f / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f, 1.0f};
    DXGI_SWAP_CHAIN_DESC1 desc = {64, 32, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE,
            {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, DXGI_SCALING_NONE,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED, 0};
    IDCompositionDevice *composition_device = NULL;
    IDCompositionTarget *target = NULL;
    IDCompositionVisual *visual = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain1 *swapchain = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *device;
    HWND parent = NULL, child = NULL;
    HBRUSH brush = NULL;
    ATOM parent_class = 0, child_class = 0;
    WNDCLASSW parent_wc = {0}, child_wc = {0};
    COLORREF pixel;
    HRESULT hr;

    if (!pDCompositionCreateDevice)
    {
        win_skip("DCompositionCreateDevice is unavailable.\n");
        return;
    }
    if (!(device = create_d3d11_device())) return;
    ID3D11Device_GetImmediateContext(device, &context);

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "IDXGIDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    ok(hr == S_OK, "GetAdapter failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    ok(hr == S_OK, "IDXGIFactory2 query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    hr = IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)device, &desc,
            NULL, &swapchain);
    if (hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED)
    {
        win_skip("Composition swap chains are unavailable, hr %#lx.\n", hr);
        goto done;
    }
    ok(hr == S_OK, "CreateSwapChainForComposition failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    /* Deliberately no read back of the buffer before presenting: a panel only
     * renders into it, and a clear that nothing reads is all the contents the
     * publication has to carry. */
    hr = clear_swapchain_buffer(device, context, swapchain, color);
    ok(hr == S_OK, "Failed to clear the composition buffer, hr %#lx.\n", hr);
    hr = IDXGISwapChain1_Present(swapchain, 0, 0);
    ok(hr == S_OK, "Present failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice,
            (void **)&composition_device);
    ok(hr == S_OK, "DCompositionCreateDevice failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    brush = CreateSolidBrush(RGB(16, 32, 48));
    parent_wc.lpfnWndProc = DefWindowProcW;
    parent_wc.hInstance = GetModuleHandleW(NULL);
    parent_wc.hbrBackground = brush;
    parent_wc.lpszClassName = L"WineDCompPresentParent";
    parent_class = RegisterClassW(&parent_wc);
    ok(!!parent_class, "Failed to register the parent class, error %lu.\n", GetLastError());
    child_wc.lpfnWndProc = DefWindowProcW;
    child_wc.hInstance = parent_wc.hInstance;
    child_wc.lpszClassName = L"WineDCompPresentChild";
    child_class = RegisterClassW(&child_wc);
    ok(!!child_class, "Failed to register the child class, error %lu.\n", GetLastError());
    parent = CreateWindowExW(0, parent_wc.lpszClassName, L"dcomp present parent",
            WS_POPUP | WS_VISIBLE, 400, 240, 96, 64, NULL, NULL, parent_wc.hInstance, NULL);
    ok(!!parent, "Failed to create the parent window.\n");
    child = CreateWindowExW(WS_EX_LAYERED, child_wc.lpszClassName, L"dcomp present child",
            WS_CHILD | WS_VISIBLE, 8, 8, 64, 32, parent, NULL, child_wc.hInstance, NULL);
    ok(!!child, "Failed to create the target window.\n");
    if (!parent || !child) goto done;
    UpdateWindow(parent);
    UpdateWindow(child);

    hr = IDCompositionDevice_CreateTargetForHwnd(composition_device, child, FALSE, &target);
    ok(hr == S_OK, "CreateTargetForHwnd failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionDevice_CreateVisual(composition_device, &visual);
    ok(hr == S_OK, "CreateVisual failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK, "SetContent(composition swapchain) failed, hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "SetRoot failed, hr %#lx.\n", hr);
    hr = IDCompositionDevice_Commit(composition_device);
    ok(hr == S_OK, "Commit failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    RedrawWindow(child, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    flush_events();
    pixel = get_client_screen_pixel(parent, 2, 2);
    if (!color_near(pixel, RGB(16, 32, 48), 1))
    {
        win_skip("The desktop DC cannot observe the window surface, pixel %#lx.\n", pixel);
        goto done;
    }
    pixel = get_client_screen_pixel(child, 8, 8);
    ok(color_near(pixel, RGB(32, 64, 128), 2),
            "Got composed presented pixel %#lx.\n", pixel);

done:
    if (visual) IDCompositionVisual_Release(visual);
    if (target) IDCompositionTarget_Release(target);
    if (composition_device) IDCompositionDevice_Release(composition_device);
    if (child) DestroyWindow(child);
    if (parent) DestroyWindow(parent);
    if (child_class) UnregisterClassW(child_wc.lpszClassName, child_wc.hInstance);
    if (parent_class) UnregisterClassW(parent_wc.lpszClassName, parent_wc.hInstance);
    if (brush) DeleteObject(brush);
    if (swapchain) IDXGISwapChain1_Release(swapchain);
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (context) ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
}

/* Windows Terminal's AtlasEngine asks the swap chain for buffer 0 once, keeps
 * the render target view it builds from it for the swap chain's whole life, and
 * draws every later frame through that one view.  That is what the flip model
 * asks of a producer: buffer 0 names a stable resource whose allocation the
 * runtime renames, so the view a producer cached before the first Present still
 * addresses whatever the next Present publishes.  A swap chain that hands the
 * application a different buffer after each Present publishes a buffer the
 * application never drew into, and the composed window goes black from the
 * second frame on. */
static void test_composition_swapchain_cached_backbuffer(void)
{
    static const float first[4] = {32.0f / 255.0f, 64.0f / 255.0f, 128.0f / 255.0f, 1.0f};
    static const float second[4] = {128.0f / 255.0f, 32.0f / 255.0f, 64.0f / 255.0f, 1.0f};
    DXGI_SWAP_CHAIN_DESC1 desc = {64, 32, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE,
            {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3, DXGI_SCALING_NONE,
            DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, DXGI_ALPHA_MODE_PREMULTIPLIED, 0};
    IDCompositionDevice *composition_device = NULL;
    ID3D11RenderTargetView *view = NULL;
    IDCompositionTarget *target = NULL;
    IDCompositionVisual *visual = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain1 *swapchain = NULL;
    ID3D11Texture2D *texture = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *device;
    HWND parent = NULL, child = NULL;
    HBRUSH brush = NULL;
    ATOM parent_class = 0, child_class = 0;
    WNDCLASSW parent_wc = {0}, child_wc = {0};
    COLORREF pixel;
    HRESULT hr;

    if (!pDCompositionCreateDevice)
    {
        win_skip("DCompositionCreateDevice is unavailable.\n");
        return;
    }
    if (!(device = create_d3d11_device())) return;
    ID3D11Device_GetImmediateContext(device, &context);

    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "IDXGIDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIDevice_GetAdapter(dxgi_device, &adapter);
    ok(hr == S_OK, "GetAdapter failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory);
    ok(hr == S_OK, "IDXGIFactory2 query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    hr = IDXGIFactory2_CreateSwapChainForComposition(factory, (IUnknown *)device, &desc,
            NULL, &swapchain);
    if (hr == E_NOTIMPL || hr == DXGI_ERROR_UNSUPPORTED)
    {
        win_skip("Composition swap chains are unavailable, hr %#lx.\n", hr);
        goto done;
    }
    ok(hr == S_OK, "CreateSwapChainForComposition failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    /* The one and only GetBuffer of this test: everything below draws through
     * the view built from it, exactly as the producer does. */
    hr = IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void **)&texture);
    ok(hr == S_OK, "GetBuffer failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture, NULL, &view);
    ok(hr == S_OK, "CreateRenderTargetView failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    ID3D11DeviceContext_ClearRenderTargetView(context, view, first);
    ID3D11DeviceContext_Flush(context);
    hr = IDXGISwapChain1_Present(swapchain, 0, 0);
    ok(hr == S_OK, "Present failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice,
            (void **)&composition_device);
    ok(hr == S_OK, "DCompositionCreateDevice failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    brush = CreateSolidBrush(RGB(16, 32, 48));
    parent_wc.lpfnWndProc = DefWindowProcW;
    parent_wc.hInstance = GetModuleHandleW(NULL);
    parent_wc.hbrBackground = brush;
    parent_wc.lpszClassName = L"WineDCompCachedParent";
    parent_class = RegisterClassW(&parent_wc);
    ok(!!parent_class, "Failed to register the parent class, error %lu.\n", GetLastError());
    child_wc.lpfnWndProc = DefWindowProcW;
    child_wc.hInstance = parent_wc.hInstance;
    child_wc.lpszClassName = L"WineDCompCachedChild";
    child_class = RegisterClassW(&child_wc);
    ok(!!child_class, "Failed to register the child class, error %lu.\n", GetLastError());
    parent = CreateWindowExW(0, parent_wc.lpszClassName, L"dcomp cached parent",
            WS_POPUP | WS_VISIBLE, 400, 240, 96, 64, NULL, NULL, parent_wc.hInstance, NULL);
    ok(!!parent, "Failed to create the parent window.\n");
    child = CreateWindowExW(WS_EX_LAYERED, child_wc.lpszClassName, L"dcomp cached child",
            WS_CHILD | WS_VISIBLE, 8, 8, 64, 32, parent, NULL, child_wc.hInstance, NULL);
    ok(!!child, "Failed to create the target window.\n");
    if (!parent || !child) goto done;
    UpdateWindow(parent);
    UpdateWindow(child);

    hr = IDCompositionDevice_CreateTargetForHwnd(composition_device, child, FALSE, &target);
    ok(hr == S_OK, "CreateTargetForHwnd failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionDevice_CreateVisual(composition_device, &visual);
    ok(hr == S_OK, "CreateVisual failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionVisual_SetContent(visual, (IUnknown *)swapchain);
    ok(hr == S_OK, "SetContent(composition swapchain) failed, hr %#lx.\n", hr);
    hr = IDCompositionTarget_SetRoot(target, visual);
    ok(hr == S_OK, "SetRoot failed, hr %#lx.\n", hr);
    hr = IDCompositionDevice_Commit(composition_device);
    ok(hr == S_OK, "Commit failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    RedrawWindow(child, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    flush_events();
    pixel = get_client_screen_pixel(parent, 2, 2);
    if (!color_near(pixel, RGB(16, 32, 48), 1))
    {
        win_skip("The desktop DC cannot observe the window surface, pixel %#lx.\n", pixel);
        goto done;
    }
    pixel = get_client_screen_pixel(child, 8, 8);
    if (!color_near(pixel, RGB(32, 64, 128), 2))
    {
        win_skip("The first presented frame is not composed, pixel %#lx.\n", pixel);
        goto done;
    }

    /* Second frame, drawn through the same cached view and published by a
     * second Present. */
    ID3D11DeviceContext_ClearRenderTargetView(context, view, second);
    ID3D11DeviceContext_Flush(context);
    hr = IDXGISwapChain1_Present(swapchain, 0, 0);
    ok(hr == S_OK, "Second Present failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionDevice_Commit(composition_device);
    ok(hr == S_OK, "Second Commit failed, hr %#lx.\n", hr);

    RedrawWindow(child, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    flush_events();
    pixel = get_client_screen_pixel(child, 8, 8);
    ok(color_near(pixel, RGB(128, 32, 64), 2),
            "Got composed pixel %#lx after the second present through a cached view.\n", pixel);

done:
    if (visual) IDCompositionVisual_Release(visual);
    if (target) IDCompositionTarget_Release(target);
    if (composition_device) IDCompositionDevice_Release(composition_device);
    if (child) DestroyWindow(child);
    if (parent) DestroyWindow(parent);
    if (child_class) UnregisterClassW(child_wc.lpszClassName, child_wc.hInstance);
    if (parent_class) UnregisterClassW(parent_wc.lpszClassName, parent_wc.hInstance);
    if (brush) DeleteObject(brush);
    if (view) ID3D11RenderTargetView_Release(view);
    if (texture) ID3D11Texture2D_Release(texture);
    if (swapchain) IDXGISwapChain1_Release(swapchain);
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    if (context) ID3D11DeviceContext_Release(context);
    ID3D11Device_Release(device);
}

static void test_surface_updates(void)
{
    const float clear_color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    ID3D11RenderTargetView *render_target = NULL;
    ID3D11DeviceContext *context = NULL;
    ID3D11Texture2D *texture = NULL, *staging = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_TEXTURE2D_DESC desc;
    IDCompositionSurface *surface = NULL;
    IDCompositionEffectGroup *effect = NULL;
    IDCompositionTarget *target = NULL;
    IDCompositionVisual *visual = NULL;
    IDCompositionDevice *composition_device = NULL;
    IDXGISurface *draw_surface = NULL;
    IDXGIDevice *dxgi_device = NULL;
    ID3D11Device *device;
    RECT invalid = {-1, 0, 1, 1};
    POINT offset;
    const BYTE *pixel;
    HRESULT hr;
    HWND hwnd = NULL;

    if (!pDCompositionCreateDevice)
    {
        win_skip("DCompositionCreateDevice is unavailable.\n");
        return;
    }
    if (!(device = create_d3d11_device())) return;
    ID3D11Device_GetImmediateContext(device, &context);
    ok(!!context, "Failed to get the immediate context.\n");
    if (!context) goto done;
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void **)&dxgi_device);
    ok(hr == S_OK, "IDXGIDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = pDCompositionCreateDevice(dxgi_device, &IID_IDCompositionDevice,
            (void **)&composition_device);
    ok(hr == S_OK, "DCompositionCreateDevice failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;

    hr = IDCompositionDevice_CreateSurface(composition_device, 8, 4,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
    ok(hr == S_OK, "CreateSurface failed, hr %#lx.\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED,
            "EndDraw without BeginDraw returned hr %#lx.\n", hr);
    hr = IDCompositionSurface_BeginDraw(surface, &invalid, &IID_IDXGISurface,
            (void **)&draw_surface, &offset);
    ok(hr == E_INVALIDARG, "Invalid update rectangle returned hr %#lx.\n", hr);
    ok(!draw_surface, "Got unexpected draw surface %p.\n", draw_surface);

    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
            (void **)&draw_surface, &offset);
    ok(hr == S_OK, "BeginDraw failed, hr %#lx.\n", hr);
    ok(offset.x == 0 && offset.y == 0, "Got update offset %ld,%ld.\n", offset.x, offset.y);
    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_IDXGISurface,
            (void **)&staging, &offset);
    ok(hr == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED,
            "Nested BeginDraw returned hr %#lx.\n", hr);
    ok(!staging, "Got unexpected nested draw object %p.\n", staging);
    hr = IDCompositionSurface_SuspendDraw(surface);
    ok(hr == S_OK, "SuspendDraw failed, hr %#lx.\n", hr);
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == DCOMPOSITION_ERROR_SURFACE_NOT_BEING_RENDERED,
            "EndDraw while suspended returned hr %#lx.\n", hr);
    hr = IDCompositionSurface_ResumeDraw(surface);
    ok(hr == S_OK, "ResumeDraw failed, hr %#lx.\n", hr);

    hr = IDXGISurface_QueryInterface(draw_surface, &IID_ID3D11Texture2D, (void **)&texture);
    ok(hr == S_OK, "Texture query failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)texture,
                NULL, &render_target);
        ok(hr == S_OK, "CreateRenderTargetView failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            ID3D11DeviceContext_ClearRenderTargetView(context, render_target, clear_color);
            ID3D11DeviceContext_Flush(context);
            ID3D11RenderTargetView_Release(render_target);
            render_target = NULL;
        }
        ID3D11Texture2D_Release(texture);
        texture = NULL;
    }
    IDXGISurface_Release(draw_surface);
    draw_surface = NULL;
    hr = IDCompositionSurface_EndDraw(surface);
    ok(hr == S_OK, "EndDraw failed, hr %#lx.\n", hr);

    hwnd = CreateWindowExW(0, L"static", L"dcomp local surface", WS_OVERLAPPED,
            0, 0, 64, 64, NULL, NULL, NULL, NULL);
    ok(!!hwnd, "Failed to create local-surface target window.\n");
    hr = IDCompositionDevice_CreateVisual(composition_device, &visual);
    ok(hr == S_OK, "CreateVisual for local surface failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionVisual_SetContent(visual, (IUnknown *)surface);
        ok(hr == S_OK, "SetContent(local surface) failed, hr %#lx.\n", hr);
    }
    hr = IDCompositionDevice_CreateEffectGroup(composition_device, &effect);
    ok(hr == S_OK, "CreateEffectGroup failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionEffectGroup_SetOpacity(effect, 0.5f);
        ok(hr == S_OK, "SetOpacity failed, hr %#lx.\n", hr);
        hr = IDCompositionVisual_SetEffect(visual, (IDCompositionEffect *)effect);
        ok(hr == S_OK, "SetEffect failed, hr %#lx.\n", hr);
    }
    if (hwnd)
    {
        hr = IDCompositionDevice_CreateTargetForHwnd(composition_device, hwnd, FALSE, &target);
        ok(hr == S_OK, "CreateTargetForHwnd failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDCompositionTarget_SetRoot(target, visual);
            ok(hr == S_OK, "SetRoot(local surface) failed, hr %#lx.\n", hr);
        }
    }

    hr = IDCompositionSurface_BeginDraw(surface, NULL, &IID_ID3D11Texture2D,
            (void **)&texture, &offset);
    ok(hr == S_OK, "Second BeginDraw failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionDevice_Commit(composition_device);
        ok(hr == DCOMPOSITION_ERROR_SURFACE_BEING_RENDERED,
                "Commit while local surface was drawing returned hr %#lx.\n", hr);
        ID3D11Texture2D_GetDesc(texture, &desc);
        ok(desc.Width == 8 && desc.Height == 4, "Got texture size %ux%u.\n",
                desc.Width, desc.Height);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &staging);
        ok(hr == S_OK, "Create staging texture failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging,
                    (ID3D11Resource *)texture);
            hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0,
                    D3D11_MAP_READ, 0, &mapped);
            ok(hr == S_OK, "Map failed, hr %#lx.\n", hr);
            if (SUCCEEDED(hr))
            {
                pixel = mapped.pData;
                ok(pixel[0] >= 190 && pixel[0] <= 192 && pixel[1] >= 127 && pixel[1] <= 129
                        && pixel[2] >= 63 && pixel[2] <= 65 && pixel[3] == 255,
                        "Got BGRA pixel %#x %#x %#x %#x.\n",
                        pixel[0], pixel[1], pixel[2], pixel[3]);
                ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
            }
        }
        ID3D11Texture2D_Release(texture);
        texture = NULL;
        hr = IDCompositionSurface_EndDraw(surface);
        ok(hr == S_OK, "Second EndDraw failed, hr %#lx.\n", hr);
    }
    if (target)
    {
        hr = IDCompositionDevice_Commit(composition_device);
        ok(hr == S_OK, "Local surface Commit failed, hr %#lx.\n", hr);
    }

done:
    if (render_target) ID3D11RenderTargetView_Release(render_target);
    if (staging) ID3D11Texture2D_Release(staging);
    if (texture) ID3D11Texture2D_Release(texture);
    if (draw_surface) IDXGISurface_Release(draw_surface);
    if (target) IDCompositionTarget_Release(target);
    if (visual) IDCompositionVisual_Release(visual);
    if (effect) IDCompositionEffectGroup_Release(effect);
    if (hwnd) DestroyWindow(hwnd);
    if (surface) IDCompositionSurface_Release(surface);
    if (composition_device) IDCompositionDevice_Release(composition_device);
    if (context) ID3D11DeviceContext_Release(context);
    if (dxgi_device) IDXGIDevice_Release(dxgi_device);
    ID3D11Device_Release(device);
}

static void test_create_surface_handle(void)
{
    char buffer[1024];
    OBJECT_TYPE_INFORMATION *type = (OBJECT_TYPE_INFORMATION *)buffer;
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    OBJECT_BASIC_INFORMATION basic;
    HANDLE handle, duplicate;
    ULONG handle_flags;
    NTSTATUS status;
    HRESULT hr;
    BOOL ret;

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, NULL, &handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (FAILED(hr))
        return;

    ok(handle != NULL && handle != INVALID_HANDLE_VALUE, "Got invalid handle %p.\n", handle);
    status = NtQueryObject(handle, ObjectTypeInformation, buffer, sizeof(buffer), NULL);
    ok(status == STATUS_SUCCESS, "NtQueryObject failed, status %#lx.\n", status);
    if (!status)
        ok(type->TypeName.Length == 11 * sizeof(WCHAR)
                && !wcsncmp(type->TypeName.Buffer, L"Composition", 11),
                "Got unexpected object type %s.\n", wine_dbgstr_w(type->TypeName.Buffer));

    status = NtQueryObject(handle, ObjectBasicInformation, &basic, sizeof(basic), NULL);
    ok(status == STATUS_SUCCESS, "NtQueryObject failed, status %#lx.\n", status);
    if (!status)
        ok((basic.GrantedAccess & COMPOSITIONOBJECT_ALL_ACCESS) == COMPOSITIONOBJECT_ALL_ACCESS,
                "Got access %#lx.\n", basic.GrantedAccess);

    ret = DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate,
            0, FALSE, DUPLICATE_SAME_ACCESS);
    ok(ret, "DuplicateHandle failed, error %lu.\n", GetLastError());
    if (ret)
    {
        status = NtQueryObject(duplicate, ObjectTypeInformation, buffer, sizeof(buffer), NULL);
        ok(status == STATUS_SUCCESS, "NtQueryObject failed, status %#lx.\n", status);
        if (!status)
            ok(type->TypeName.Length == 11 * sizeof(WCHAR)
                    && !wcsncmp(type->TypeName.Buffer, L"Composition", 11),
                    "Got unexpected duplicate object type %s.\n", wine_dbgstr_w(type->TypeName.Buffer));
        CloseHandle(duplicate);
    }
    CloseHandle(handle);

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, &attributes, &handle);
    ok(hr == S_OK, "Got unexpected hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        ret = GetHandleInformation(handle, &handle_flags);
        ok(ret, "GetHandleInformation failed, error %lu.\n", GetLastError());
        if (ret)
            ok(handle_flags & HANDLE_FLAG_INHERIT, "Got handle flags %#lx.\n", handle_flags);
        CloseHandle(handle);
    }

    hr = pDCompositionCreateSurfaceHandle(0, NULL, &handle);
    ok(hr == S_OK, "Zero-access create returned hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
        CloseHandle(handle);
}

static void test_surface_handle_import(void)
{
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice2 *device2;
    IDCompositionDevice *device;
    IDCompositionTarget *target = NULL;
    IDCompositionVisual *visual = NULL;
    IUnknown *surface, *identity, *retained_surface = NULL;
    HANDLE handle, event;
    HWND hwnd = NULL;
    HRESULT hr;
    ULONG ref;

    hr = pDCompositionCreateDevice2(NULL, &IID_IDCompositionDevice, (void **)&device);
    ok(hr == S_OK, "DCompositionCreateDevice2 failed, hr %#lx.\n", hr);
    if (FAILED(hr)) return;

    hr = IDCompositionDevice_QueryInterface(device, &IID_IDCompositionDevice2, (void **)&device2);
    ok(hr == S_OK, "IDCompositionDevice2 query failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionDevice2_Release(device2);

    hr = IDCompositionDevice_QueryInterface(device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "IDCompositionDesktopDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDCompositionDevice_Release(device);
        return;
    }

    surface = (IUnknown *)0xdeadbeef;
    hr = IDCompositionDesktopDevice_CreateSurfaceFromHandle(desktop_device,
            INVALID_HANDLE_VALUE, &surface);
    ok(FAILED(hr), "Invalid handle unexpectedly succeeded.\n");
    ok(!surface, "Got unexpected surface %p.\n", surface);

    event = CreateEventW(NULL, FALSE, FALSE, NULL);
    ok(!!event, "CreateEventW failed, error %lu.\n", GetLastError());
    if (event)
    {
        surface = (IUnknown *)0xdeadbeef;
        hr = IDCompositionDesktopDevice_CreateSurfaceFromHandle(desktop_device, event, &surface);
        ok(FAILED(hr), "Non-composition handle unexpectedly succeeded.\n");
        ok(!surface, "Got unexpected surface %p.\n", surface);
        CloseHandle(event);
    }

    hr = pDCompositionCreateSurfaceHandle(0, NULL, &handle);
    ok(hr == S_OK, "Zero-access handle creation failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        surface = (IUnknown *)0xdeadbeef;
        hr = IDCompositionDesktopDevice_CreateSurfaceFromHandle(desktop_device, handle, &surface);
        ok(hr == E_ACCESSDENIED, "Zero-access import returned hr %#lx.\n", hr);
        ok(!surface, "Got unexpected surface %p.\n", surface);
        CloseHandle(handle);
    }

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_WRITE, NULL, &handle);
    ok(hr == S_OK, "Write-access handle creation failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        surface = (IUnknown *)0xdeadbeef;
        hr = IDCompositionDesktopDevice_CreateSurfaceFromHandle(desktop_device, handle, &surface);
        ok(hr == E_ACCESSDENIED, "Write-only import returned hr %#lx.\n", hr);
        ok(!surface, "Got unexpected surface %p.\n", surface);
        CloseHandle(handle);
    }

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_READ, NULL, &handle);
    ok(hr == S_OK, "Read-access handle creation failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        surface = NULL;
        hr = IDCompositionDesktopDevice_CreateSurfaceFromHandle(desktop_device, handle, &surface);
        ok(hr == S_OK, "Read-access import failed, hr %#lx.\n", hr);
        CloseHandle(handle);
        if (SUCCEEDED(hr))
        {
            hr = IUnknown_QueryInterface(surface, &IID_IUnknown, (void **)&identity);
            ok(hr == S_OK, "Imported surface identity query failed, hr %#lx.\n", hr);
            ok(identity == surface, "Got different identity %p, expected %p.\n", identity, surface);
            if (SUCCEEDED(hr)) IUnknown_Release(identity);
            retained_surface = surface;
        }
    }

    hwnd = CreateWindowExW(0, L"static", L"unbound dcomp surface",
            WS_POPUP, 0, 0, 32, 32, NULL, NULL, NULL, NULL);
    ok(!!hwnd, "Failed to create unbound-surface target window, error %lu.\n",
            GetLastError());
    if (hwnd && retained_surface)
    {
        hr = IDCompositionDevice_CreateTargetForHwnd(device, hwnd, FALSE, &target);
        ok(hr == S_OK, "CreateTargetForHwnd failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IDCompositionDevice_CreateVisual(device, &visual);
            ok(hr == S_OK, "CreateVisual failed, hr %#lx.\n", hr);
        }
        if (visual)
        {
            hr = IDCompositionVisual_SetContent(visual, retained_surface);
            ok(hr == S_OK, "SetContent for unbound surface failed, hr %#lx.\n", hr);
        }
        if (target && visual)
        {
            hr = IDCompositionTarget_SetRoot(target, visual);
            ok(hr == S_OK, "SetRoot for unbound surface failed, hr %#lx.\n", hr);
        }
    }

    hr = IDCompositionDevice_Commit(device);
    ok(hr == S_OK, "Commit with unbound imported surface returned hr %#lx.\n", hr);

    if (target) IDCompositionTarget_SetRoot(target, NULL);
    if (visual) IDCompositionVisual_Release(visual);
    if (target) IDCompositionTarget_Release(target);
    if (hwnd) DestroyWindow(hwnd);

    ref = IDCompositionDesktopDevice_Release(desktop_device);
    ok(ref == 1, "Device has unexpected refcount %lu.\n", ref);
    ref = IDCompositionDevice_Release(device);
    ok(!ref, "Device has %lu references left.\n", ref);
    if (retained_surface)
    {
        hr = IUnknown_QueryInterface(retained_surface, &IID_IUnknown, (void **)&identity);
        ok(hr == S_OK, "Surface did not outlive its handle and device, hr %#lx.\n", hr);
        if (SUCCEEDED(hr)) IUnknown_Release(identity);
        ref = IUnknown_Release(retained_surface);
        ok(!ref, "Imported surface has %lu references left.\n", ref);
    }
}

static void test_retained_visual_graph(void)
{
    IDCompositionVisual *parent, *child, *reference, *other_parent;
    IDCompositionVisual2 *parent2, *desktop_visual;
    IDCompositionTarget *target = NULL, *duplicate = NULL, *topmost = NULL;
    IDCompositionDesktopDevice *desktop_device;
    IDCompositionDevice *device;
    D2D_MATRIX_3X2_F matrix;
    D2D_RECT_F clip = {2.0f, 3.0f, 102.0f, 203.0f};
    IUnknown *surface;
    HANDLE handle;
    HRESULT hr;
    ULONG ref;
    HWND hwnd;

    matrix._11 = 1.0f;
    matrix._12 = 0.0f;
    matrix._21 = 0.0f;
    matrix._22 = 1.0f;
    matrix._31 = 12.0f;
    matrix._32 = 18.0f;

    hr = pDCompositionCreateDevice2(NULL, &IID_IDCompositionDevice, (void **)&device);
    ok(hr == S_OK, "DCompositionCreateDevice2 failed, hr %#lx.\n", hr);
    if (FAILED(hr)) return;
    hr = IDCompositionDevice_QueryInterface(device, &IID_IDCompositionDesktopDevice,
            (void **)&desktop_device);
    ok(hr == S_OK, "IDCompositionDesktopDevice query failed, hr %#lx.\n", hr);
    if (FAILED(hr))
    {
        IDCompositionDevice_Release(device);
        return;
    }

    hr = IDCompositionDevice_CreateVisual(device, &parent);
    ok(hr == S_OK, "CreateVisual failed, hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(device, NULL);
    ok(hr == E_INVALIDARG, "CreateVisual with NULL output returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_QueryInterface(parent, &IID_IDCompositionVisual2, (void **)&parent2);
    ok(hr == S_OK, "IDCompositionVisual2 query failed, hr %#lx.\n", hr);
    hr = IDCompositionDesktopDevice_CreateVisual(desktop_device, &desktop_visual);
    ok(hr == S_OK, "Desktop CreateVisual failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr)) IDCompositionVisual2_Release(desktop_visual);

    hr = IDCompositionDevice_CreateVisual(device, &child);
    ok(hr == S_OK, "Child CreateVisual failed, hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(device, &reference);
    ok(hr == S_OK, "Reference CreateVisual failed, hr %#lx.\n", hr);
    hr = IDCompositionDevice_CreateVisual(device, &other_parent);
    ok(hr == S_OK, "Second parent CreateVisual failed, hr %#lx.\n", hr);

    hr = IDCompositionVisual_SetOffsetX(parent, 7.5f);
    ok(hr == S_OK, "SetOffsetX failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetOffsetY(parent, -4.0f);
    ok(hr == S_OK, "SetOffsetY failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetTransform(parent, &matrix);
    ok(hr == S_OK, "SetTransform failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetClip(parent, &clip);
    ok(hr == S_OK, "SetClip failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetBitmapInterpolationMode(parent,
            DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    ok(hr == S_OK, "SetBitmapInterpolationMode failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetBorderMode(parent, DCOMPOSITION_BORDER_MODE_HARD);
    ok(hr == S_OK, "SetBorderMode failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetCompositeMode(parent, DCOMPOSITION_COMPOSITE_MODE_SOURCE_OVER);
    ok(hr == S_OK, "SetCompositeMode failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetOpacityMode(parent2, DCOMPOSITION_OPACITY_MODE_MULTIPLY);
    ok(hr == S_OK, "SetOpacityMode failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual2_SetBackFaceVisibility(parent2,
            DCOMPOSITION_BACKFACE_VISIBILITY_HIDDEN);
    ok(hr == S_OK, "SetBackFaceVisibility failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetBorderMode(parent, 7);
    ok(hr == E_INVALIDARG, "Invalid border mode returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_SetTransform(parent, NULL);
    ok(hr == E_INVALIDARG, "NULL matrix returned hr %#lx.\n", hr);

    hr = pDCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_READ, NULL, &handle);
    ok(hr == S_OK, "Surface handle creation failed, hr %#lx.\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IDCompositionDevice_CreateSurfaceFromHandle(device, handle, &surface);
        ok(hr == S_OK, "Surface import failed, hr %#lx.\n", hr);
        CloseHandle(handle);
        if (SUCCEEDED(hr))
        {
            hr = IDCompositionVisual_SetContent(parent, surface);
            ok(hr == S_OK, "SetContent failed, hr %#lx.\n", hr);
            ref = IUnknown_Release(surface);
            ok(ref == 1, "Visual did not retain content, refcount %lu.\n", ref);
        }
    }

    hr = IDCompositionVisual_AddVisual(parent, NULL, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Adding NULL returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent, child, FALSE, reference);
    ok(hr == E_INVALIDARG, "Unattached reference returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(other_parent, reference, FALSE, NULL);
    ok(hr == S_OK, "Attaching reference to second parent failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent, child, FALSE, reference);
    ok(hr == E_INVALIDARG, "Foreign reference returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveAllVisuals(other_parent);
    ok(hr == S_OK, "Clearing second parent failed, hr %#lx.\n", hr);

    hr = IDCompositionVisual_AddVisual(parent, reference, FALSE, NULL);
    ok(hr == S_OK, "Adding reference failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent, child, TRUE, reference);
    ok(hr == S_OK, "Relative child insertion failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(parent, child, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Duplicate child insertion returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_AddVisual(child, parent, FALSE, NULL);
    ok(hr == E_INVALIDARG, "Cycle insertion returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveVisual(parent, child);
    ok(hr == S_OK, "RemoveVisual failed, hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveVisual(parent, child);
    ok(hr == E_INVALIDARG, "Removing detached child returned hr %#lx.\n", hr);
    hr = IDCompositionVisual_RemoveAllVisuals(parent);
    ok(hr == S_OK, "RemoveAllVisuals failed, hr %#lx.\n", hr);

    hwnd = CreateWindowExW(0, L"static", L"dcomp retained target", WS_OVERLAPPED,
            0, 0, 64, 64, NULL, NULL, NULL, NULL);
    ok(!!hwnd, "CreateWindowExW failed, error %lu.\n", GetLastError());
    if (hwnd)
    {
        hr = IDCompositionDevice_CreateTargetForHwnd(device, NULL, FALSE, &duplicate);
        ok(hr == E_INVALIDARG, "NULL HWND returned hr %#lx.\n", hr);
        hr = IDCompositionDevice_CreateTargetForHwnd(device, hwnd, FALSE, NULL);
        ok(hr == E_INVALIDARG, "NULL target output returned hr %#lx.\n", hr);
        hr = IDCompositionDevice_CreateTargetForHwnd(device, hwnd, FALSE, &target);
        ok(hr == S_OK, "CreateTargetForHwnd failed, hr %#lx.\n", hr);
        hr = IDCompositionDevice_CreateTargetForHwnd(device, hwnd, FALSE, &duplicate);
        ok(hr == DCOMPOSITION_ERROR_WINDOW_ALREADY_COMPOSED,
                "Duplicate target returned hr %#lx.\n", hr);
        hr = IDCompositionDevice_CreateTargetForHwnd(device, hwnd, TRUE, &topmost);
        ok(hr == S_OK, "Topmost target creation failed, hr %#lx.\n", hr);
        if (SUCCEEDED(hr)) IDCompositionTarget_Release(topmost);
        if (target)
        {
            hr = IDCompositionTarget_SetRoot(target, parent);
            ok(hr == S_OK, "SetRoot failed, hr %#lx.\n", hr);
            hr = IDCompositionVisual_AddVisual(other_parent, parent, FALSE, NULL);
            ok(hr == E_INVALIDARG, "Adding root visual returned hr %#lx.\n", hr);

            IDCompositionVisual2_Release(parent2);
            ref = IDCompositionVisual_Release(parent);
            ok(!ref, "Root visual has visible refcount %lu.\n", ref);
            hr = IDCompositionTarget_SetRoot(target, NULL);
            ok(hr == S_OK, "Clearing root failed, hr %#lx.\n", hr);
            IDCompositionTarget_Release(target);
        }
        DestroyWindow(hwnd);
    }

    IDCompositionVisual_Release(other_parent);
    IDCompositionVisual_Release(reference);
    IDCompositionVisual_Release(child);
    ref = IDCompositionDesktopDevice_Release(desktop_device);
    ok(ref == 1, "Device has unexpected refcount %lu.\n", ref);
    ref = IDCompositionDevice_Release(device);
    ok(!ref, "Device has %lu visible references left.\n", ref);
}

START_TEST(dcomp)
{
    HMODULE module = LoadLibraryA("dcomp.dll");

    if (!module)
    {
        win_skip("dcomp.dll is unavailable.\n");
        return;
    }
    pDCompositionCreateSurfaceHandle = (void *)GetProcAddress(module,
            "DCompositionCreateSurfaceHandle");
    pDCompositionCreateDevice = (void *)GetProcAddress(module, "DCompositionCreateDevice");
    pDCompositionCreateDevice2 = (void *)GetProcAddress(module, "DCompositionCreateDevice2");
    if (!pDCompositionCreateSurfaceHandle)
    {
        win_skip("DCompositionCreateSurfaceHandle is unavailable.\n");
        FreeLibrary(module);
        return;
    }

    test_create_surface_handle();
    if (pDCompositionCreateDevice2)
    {
        test_surface_handle_import();
        test_retained_visual_graph();
    }
    else
        win_skip("DCompositionCreateDevice2 is unavailable.\n");
    test_composition_swapchain();
    test_composition_swapchain_present();
    test_composition_swapchain_cached_backbuffer();
    test_surface_updates();
    FreeLibrary(module);
}
