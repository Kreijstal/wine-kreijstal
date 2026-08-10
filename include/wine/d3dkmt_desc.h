/*
 * D3DKMT shared resource runtime descriptors
 *
 * Copyright 2026 Kreijstal
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

#ifndef __WINE_D3DKMT_DESC_H
#define __WINE_D3DKMT_DESC_H

#include <d3d10.h>
#include <d3d11.h>
#include <ddk/d3dkmthk.h>

/* The private runtime data attached to a D3DKMT shared resource by the D3D
 * runtimes.  The layout matches the native runtimes, which allows a resource
 * shared by one runtime to be opened by another. */

struct d3dkmt_dxgi_desc
{
    UINT                        size;
    UINT                        version;
    UINT                        width;
    UINT                        height;
    DXGI_FORMAT                 format;
    UINT                        unknown_0;
    UINT                        unknown_1;
    UINT                        keyed_mutex;
    D3DKMT_HANDLE               mutex_handle;
    D3DKMT_HANDLE               sync_handle;
    UINT                        nt_shared;
    UINT                        unknown_2;
    UINT                        unknown_3;
    UINT                        unknown_4;
};

struct d3dkmt_d3d11_desc
{
    struct d3dkmt_dxgi_desc     dxgi;
    D3D11_RESOURCE_DIMENSION    dimension;
    union
    {
        D3D10_BUFFER_DESC       d3d10_buf;
        D3D10_TEXTURE1D_DESC    d3d10_1d;
        D3D10_TEXTURE2D_DESC    d3d10_2d;
        D3D10_TEXTURE3D_DESC    d3d10_3d;
        D3D11_BUFFER_DESC       d3d11_buf;
        D3D11_TEXTURE1D_DESC    d3d11_1d;
        D3D11_TEXTURE2D_DESC    d3d11_2d;
        D3D11_TEXTURE3D_DESC    d3d11_3d;
    };
};

#endif /* __WINE_D3DKMT_DESC_H */
