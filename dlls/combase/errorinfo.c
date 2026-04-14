/*
 * ErrorInfo API
 *
 * Copyright 2000 Patrik Stridvall, Juergen Schmied
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

#define COBJMACROS
#define WINOLEAUTAPI

#include "oleauto.h"

#include "combase_private.h"

#include "wine/debug.h"

#include "roerrorapi.h"


WINE_DEFAULT_DEBUG_CHANNEL(ole);

struct error_info
{
    IErrorInfo IErrorInfo_iface;
    ICreateErrorInfo ICreateErrorInfo_iface;
    ISupportErrorInfo ISupportErrorInfo_iface;
    LONG refcount;

    GUID guid;
    WCHAR *source;
    WCHAR *description;
    WCHAR *help_file;
    DWORD help_context;
};

static struct error_info *impl_from_IErrorInfo(IErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct error_info, IErrorInfo_iface);
}

static struct error_info *impl_from_ICreateErrorInfo(ICreateErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct error_info, ICreateErrorInfo_iface);
}

static struct error_info *impl_from_ISupportErrorInfo(ISupportErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct error_info, ISupportErrorInfo_iface);
}

static HRESULT WINAPI errorinfo_QueryInterface(IErrorInfo *iface, REFIID riid, void **obj)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    *obj = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IErrorInfo))
    {
        *obj = &error_info->IErrorInfo_iface;
    }
    else if (IsEqualIID(riid, &IID_ICreateErrorInfo))
    {
        *obj = &error_info->ICreateErrorInfo_iface;
    }
    else if (IsEqualIID(riid, &IID_ISupportErrorInfo))
    {
        *obj = &error_info->ISupportErrorInfo_iface;
    }

    if (*obj)
    {
        IUnknown_AddRef((IUnknown *)*obj);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI errorinfo_AddRef(IErrorInfo *iface)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);
    ULONG refcount = InterlockedIncrement(&error_info->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG WINAPI errorinfo_Release(IErrorInfo *iface)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);
    ULONG refcount = InterlockedDecrement(&error_info->refcount);

    TRACE("%p, refcount %lu.\n", iface, refcount);

    if (!refcount)
    {
        free(error_info->source);
        free(error_info->description);
        free(error_info->help_file);
        free(error_info);
    }

    return refcount;
}

static HRESULT WINAPI errorinfo_GetGUID(IErrorInfo *iface, GUID *guid)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, guid);

    if (!guid) return E_INVALIDARG;
    *guid = error_info->guid;
    return S_OK;
}

static HRESULT WINAPI errorinfo_GetSource(IErrorInfo* iface, BSTR *source)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, source);

    if (!source)
        return E_INVALIDARG;
    *source = SysAllocString(error_info->source);
    return S_OK;
}

static HRESULT WINAPI errorinfo_GetDescription(IErrorInfo *iface, BSTR *description)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, description);

    if (!description)
        return E_INVALIDARG;
    *description = SysAllocString(error_info->description);
    return S_OK;
}

static HRESULT WINAPI errorinfo_GetHelpFile(IErrorInfo *iface, BSTR *helpfile)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, helpfile);

    if (!helpfile)
        return E_INVALIDARG;
    *helpfile = SysAllocString(error_info->help_file);
    return S_OK;
}

static HRESULT WINAPI errorinfo_GetHelpContext(IErrorInfo *iface, DWORD *help_context)
{
    struct error_info *error_info = impl_from_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, help_context);

    if (!help_context)
        return E_INVALIDARG;
    *help_context = error_info->help_context;

    return S_OK;
}

static const IErrorInfoVtbl errorinfo_vtbl =
{
    errorinfo_QueryInterface,
    errorinfo_AddRef,
    errorinfo_Release,
    errorinfo_GetGUID,
    errorinfo_GetSource,
    errorinfo_GetDescription,
    errorinfo_GetHelpFile,
    errorinfo_GetHelpContext
};

static HRESULT WINAPI create_errorinfo_QueryInterface(ICreateErrorInfo *iface, REFIID riid, void **obj)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);
    return IErrorInfo_QueryInterface(&error_info->IErrorInfo_iface, riid, obj);
}

static ULONG WINAPI create_errorinfo_AddRef(ICreateErrorInfo *iface)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);
    return IErrorInfo_AddRef(&error_info->IErrorInfo_iface);
}

static ULONG WINAPI create_errorinfo_Release(ICreateErrorInfo *iface)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);
    return IErrorInfo_Release(&error_info->IErrorInfo_iface);
}

static HRESULT WINAPI create_errorinfo_SetGUID(ICreateErrorInfo *iface, REFGUID guid)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);

    TRACE("%p, %s.\n", iface, debugstr_guid(guid));

    error_info->guid = *guid;

    return S_OK;
}

static HRESULT WINAPI create_errorinfo_SetSource(ICreateErrorInfo *iface, LPOLESTR source)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);

    TRACE("%p, %s.\n", iface, debugstr_w(source));

    free(error_info->source);
    error_info->source = wcsdup(source);

    return S_OK;
}

static HRESULT WINAPI create_errorinfo_SetDescription(ICreateErrorInfo *iface, LPOLESTR description)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);

    TRACE("%p, %s.\n", iface, debugstr_w(description));

    free(error_info->description);
    error_info->description = wcsdup(description);

    return S_OK;
}

static HRESULT WINAPI create_errorinfo_SetHelpFile(ICreateErrorInfo *iface, LPOLESTR helpfile)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);

    TRACE("%p, %s.\n", iface, debugstr_w(helpfile));

    free(error_info->help_file);
    error_info->help_file = wcsdup(helpfile);

    return S_OK;
}

static HRESULT WINAPI create_errorinfo_SetHelpContext(ICreateErrorInfo *iface, DWORD help_context)
{
    struct error_info *error_info = impl_from_ICreateErrorInfo(iface);

    TRACE("%p, %#lx.\n", iface, help_context);

    error_info->help_context = help_context;

    return S_OK;
}

static const ICreateErrorInfoVtbl create_errorinfo_vtbl =
{
    create_errorinfo_QueryInterface,
    create_errorinfo_AddRef,
    create_errorinfo_Release,
    create_errorinfo_SetGUID,
    create_errorinfo_SetSource,
    create_errorinfo_SetDescription,
    create_errorinfo_SetHelpFile,
    create_errorinfo_SetHelpContext
};

static HRESULT WINAPI support_errorinfo_QueryInterface(ISupportErrorInfo *iface, REFIID riid, void **obj)
{
    struct error_info *error_info = impl_from_ISupportErrorInfo(iface);
    return IErrorInfo_QueryInterface(&error_info->IErrorInfo_iface, riid, obj);
}

static ULONG WINAPI support_errorinfo_AddRef(ISupportErrorInfo *iface)
{
    struct error_info *error_info = impl_from_ISupportErrorInfo(iface);
    return IErrorInfo_AddRef(&error_info->IErrorInfo_iface);
}

static ULONG WINAPI support_errorinfo_Release(ISupportErrorInfo *iface)
{
    struct error_info *error_info = impl_from_ISupportErrorInfo(iface);
    return IErrorInfo_Release(&error_info->IErrorInfo_iface);
}

static HRESULT WINAPI support_errorinfo_InterfaceSupportsErrorInfo(ISupportErrorInfo *iface, REFIID riid)
{
    struct error_info *error_info = impl_from_ISupportErrorInfo(iface);

    TRACE("%p, %s.\n", iface, debugstr_guid(riid));

    return IsEqualIID(riid, &error_info->guid) ? S_OK : S_FALSE;
}

static const ISupportErrorInfoVtbl support_errorinfo_vtbl =
{
    support_errorinfo_QueryInterface,
    support_errorinfo_AddRef,
    support_errorinfo_Release,
    support_errorinfo_InterfaceSupportsErrorInfo
};

/***********************************************************************
 *                CreateErrorInfo (combase.@)
 */
HRESULT WINAPI CreateErrorInfo(ICreateErrorInfo **ret)
{
    struct error_info *error_info;

    TRACE("%p.\n", ret);

    if (!ret) return E_INVALIDARG;

    if (!(error_info = malloc(sizeof(*error_info))))
        return E_OUTOFMEMORY;

    error_info->IErrorInfo_iface.lpVtbl = &errorinfo_vtbl;
    error_info->ICreateErrorInfo_iface.lpVtbl = &create_errorinfo_vtbl;
    error_info->ISupportErrorInfo_iface.lpVtbl = &support_errorinfo_vtbl;
    error_info->refcount = 1;
    error_info->source = NULL;
    error_info->description = NULL;
    error_info->help_file = NULL;
    error_info->help_context = 0;

    *ret = &error_info->ICreateErrorInfo_iface;

    return S_OK;
}

/***********************************************************************
 *                GetErrorInfo    (combase.@)
 */
HRESULT WINAPI GetErrorInfo(ULONG reserved, IErrorInfo **error_info)
{
    struct tlsdata *tlsdata;
    HRESULT hr;

    TRACE("%lu, %p\n", reserved, error_info);

    if (reserved || !error_info)
        return E_INVALIDARG;

    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;

    if (!tlsdata->errorinfo)
    {
        *error_info = NULL;
        return S_FALSE;
    }

    *error_info = tlsdata->errorinfo;
    tlsdata->errorinfo = NULL;

    return S_OK;
}

/***********************************************************************
 *               SetErrorInfo    (combase.@)
 */
HRESULT WINAPI SetErrorInfo(ULONG reserved, IErrorInfo *error_info)
{
    struct tlsdata *tlsdata;
    HRESULT hr;

    TRACE("%lu, %p\n", reserved, error_info);

    if (reserved)
        return E_INVALIDARG;

    if (FAILED(hr = com_get_tlsdata(&tlsdata)))
        return hr;

    if (tlsdata->errorinfo)
        IErrorInfo_Release(tlsdata->errorinfo);

    tlsdata->errorinfo = error_info;
    if (error_info)
        IErrorInfo_AddRef(error_info);

    return S_OK;
}

/***********************************************************************
 *    IRestrictedErrorInfo implementation / helper (combase internal)
 */
struct restricted_error_info
{
    IRestrictedErrorInfo IRestrictedErrorInfo_iface;
    IErrorInfo IErrorInfo_iface;
    LONG refcount;

    GUID guid;
    WCHAR *source;
    WCHAR *help_file;
    DWORD help_context;

    HRESULT hr;
    WCHAR *description;
    WCHAR *restricted_description;
    WCHAR *capability_sid;
    WCHAR *reference;

    BOOL context_captured;
    USHORT stack_frame_count;
    void *stack_frames[32];
    ULONG stack_hash;
};

static struct restricted_error_info *impl_from_IRestrictedErrorInfo(IRestrictedErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct restricted_error_info, IRestrictedErrorInfo_iface);
}

static struct restricted_error_info *impl_from_restricted_IErrorInfo(IErrorInfo *iface)
{
    return CONTAINING_RECORD(iface, struct restricted_error_info, IErrorInfo_iface);
}

static ULONG restricted_errorinfo_addref(struct restricted_error_info *info)
{
    ULONG refcount = InterlockedIncrement(&info->refcount);

    TRACE("%p, refcount %lu.\n", info, refcount);
    return refcount;
}

static ULONG restricted_errorinfo_release(struct restricted_error_info *info)
{
    ULONG refcount = InterlockedDecrement(&info->refcount);

    TRACE("%p, refcount %lu.\n", info, refcount);

    if (!refcount)
    {
        free(info->source);
        free(info->help_file);
        free(info->description);
        free(info->restricted_description);
        free(info->capability_sid);
        free(info->reference);
        free(info);
    }

    return refcount;
}

static HRESULT WINAPI restricted_errorinfo_QueryInterface(IRestrictedErrorInfo *iface, REFIID riid, void **obj)
{
    struct restricted_error_info *info = impl_from_IRestrictedErrorInfo(iface);

    TRACE("%p, %s, %p.\n", iface, debugstr_guid(riid), obj);

    if (!obj) return E_POINTER;
    *obj = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IRestrictedErrorInfo))
        *obj = &info->IRestrictedErrorInfo_iface;
    else if (IsEqualIID(riid, &IID_IErrorInfo))
        *obj = &info->IErrorInfo_iface;

    if (*obj)
    {
        restricted_errorinfo_addref(info);
        return S_OK;
    }

    WARN("Unsupported interface %s.\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI restricted_errorinfo_AddRef(IRestrictedErrorInfo *iface)
{
    struct restricted_error_info *info = impl_from_IRestrictedErrorInfo(iface);
    return restricted_errorinfo_addref(info);
}

static ULONG WINAPI restricted_errorinfo_Release(IRestrictedErrorInfo *iface)
{
    struct restricted_error_info *info = impl_from_IRestrictedErrorInfo(iface);
    return restricted_errorinfo_release(info);
}

static HRESULT WINAPI restricted_errorinfo_GetErrorDetails(IRestrictedErrorInfo *iface,
        BSTR *description, HRESULT *error, BSTR *restricted_description, BSTR *capability_sid)
{
    struct restricted_error_info *info = impl_from_IRestrictedErrorInfo(iface);

    TRACE("%p, %p, %p, %p, %p.\n", iface, description, error, restricted_description, capability_sid);

    if (description)
        *description = info->description ? SysAllocString(info->description) : NULL;
    if (error)
        *error = info->hr;
    if (restricted_description)
        *restricted_description = info->restricted_description ? SysAllocString(info->restricted_description) : NULL;
    if (capability_sid)
        *capability_sid = info->capability_sid ? SysAllocString(info->capability_sid) : NULL;

    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_GetReference(IRestrictedErrorInfo *iface, BSTR *reference)
{
    struct restricted_error_info *info = impl_from_IRestrictedErrorInfo(iface);

    TRACE("%p, %p.\n", iface, reference);

    if (!reference) return E_POINTER;
    *reference = info->reference ? SysAllocString(info->reference) : NULL;
    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_ierror_QueryInterface(IErrorInfo *iface, REFIID riid, void **obj)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);
    return IRestrictedErrorInfo_QueryInterface(&info->IRestrictedErrorInfo_iface, riid, obj);
}

static ULONG WINAPI restricted_errorinfo_ierror_AddRef(IErrorInfo *iface)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);
    return restricted_errorinfo_addref(info);
}

static ULONG WINAPI restricted_errorinfo_ierror_Release(IErrorInfo *iface)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);
    return restricted_errorinfo_release(info);
}

static HRESULT WINAPI restricted_errorinfo_ierror_GetGUID(IErrorInfo *iface, GUID *guid)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, guid);

    if (!guid) return E_INVALIDARG;
    *guid = info->guid;
    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_ierror_GetSource(IErrorInfo *iface, BSTR *source)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, source);

    if (!source) return E_INVALIDARG;
    *source = info->source ? SysAllocString(info->source) : NULL;
    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_ierror_GetDescription(IErrorInfo *iface, BSTR *description)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, description);

    if (!description) return E_INVALIDARG;
    *description = info->description ? SysAllocString(info->description) : NULL;
    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_ierror_GetHelpFile(IErrorInfo *iface, BSTR *helpfile)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, helpfile);

    if (!helpfile) return E_INVALIDARG;
    *helpfile = info->help_file ? SysAllocString(info->help_file) : NULL;
    return S_OK;
}

static HRESULT WINAPI restricted_errorinfo_ierror_GetHelpContext(IErrorInfo *iface, DWORD *help_context)
{
    struct restricted_error_info *info = impl_from_restricted_IErrorInfo(iface);

    TRACE("%p, %p.\n", iface, help_context);

    if (!help_context) return E_INVALIDARG;
    *help_context = info->help_context;
    return S_OK;
}

static const IErrorInfoVtbl restricted_errorinfo_ierror_vtbl =
{
    restricted_errorinfo_ierror_QueryInterface,
    restricted_errorinfo_ierror_AddRef,
    restricted_errorinfo_ierror_Release,
    restricted_errorinfo_ierror_GetGUID,
    restricted_errorinfo_ierror_GetSource,
    restricted_errorinfo_ierror_GetDescription,
    restricted_errorinfo_ierror_GetHelpFile,
    restricted_errorinfo_ierror_GetHelpContext
};

static const IRestrictedErrorInfoVtbl restricted_errorinfo_vtbl =
{
    restricted_errorinfo_QueryInterface,
    restricted_errorinfo_AddRef,
    restricted_errorinfo_Release,
    restricted_errorinfo_GetErrorDetails,
    restricted_errorinfo_GetReference
};

HRESULT create_restricted_error_info(HRESULT hr, const WCHAR *description,
        const WCHAR *restricted_description, const WCHAR *capability_sid,
        const WCHAR *reference, IRestrictedErrorInfo **ret)
{
    struct restricted_error_info *info;

    TRACE("%#lx, %s, %s, %s, %s, %p.\n", hr, debugstr_w(description),
          debugstr_w(restricted_description), debugstr_w(capability_sid),
          debugstr_w(reference), ret);

    if (!ret) return E_INVALIDARG;
    *ret = NULL;

    info = calloc(1, sizeof(*info));
    if (!info) return E_OUTOFMEMORY;

    info->IRestrictedErrorInfo_iface.lpVtbl = &restricted_errorinfo_vtbl;
    info->IErrorInfo_iface.lpVtbl = &restricted_errorinfo_ierror_vtbl;
    info->refcount = 1;

    info->guid = GUID_NULL;
    info->source = NULL;
    info->help_file = NULL;
    info->help_context = 0;

    info->hr = hr;
    info->description = description ? wcsdup(description) : NULL;
    info->restricted_description = restricted_description ? wcsdup(restricted_description) : NULL;
    info->capability_sid = capability_sid ? wcsdup(capability_sid) : NULL;
    info->reference = reference ? wcsdup(reference) : NULL;

    info->context_captured = FALSE;
    info->stack_frame_count = 0;
    info->stack_hash = 0;

    *ret = &info->IRestrictedErrorInfo_iface;
    return S_OK;
}

HRESULT capture_restricted_error_context(IRestrictedErrorInfo *iface, HRESULT hr)
{
    struct restricted_error_info *info;
    ULONG hash = 0;

    TRACE("%p, %#lx.\n", iface, hr);

    if (!iface)
        return E_INVALIDARG;

    info = impl_from_IRestrictedErrorInfo(iface);

    info->hr = hr;
    memset(info->stack_frames, 0, sizeof(info->stack_frames));
    info->stack_frame_count = RtlCaptureStackBackTrace(1, ARRAY_SIZE(info->stack_frames),
            info->stack_frames, &hash);
    info->stack_hash = hash;
    info->context_captured = TRUE;

    return S_OK;
}

HRESULT copy_restricted_error_context(IRestrictedErrorInfo *dst_iface, IRestrictedErrorInfo *src_iface)
{
    struct restricted_error_info *dst, *src;

    TRACE("%p, %p.\n", dst_iface, src_iface);

    if (!dst_iface || !src_iface)
        return E_INVALIDARG;

    dst = impl_from_IRestrictedErrorInfo(dst_iface);
    src = impl_from_IRestrictedErrorInfo(src_iface);

    dst->context_captured = src->context_captured;
    dst->stack_frame_count = src->stack_frame_count;
    memcpy(dst->stack_frames, src->stack_frames, sizeof(dst->stack_frames));
    dst->stack_hash = src->stack_hash;

    return S_OK;
}
