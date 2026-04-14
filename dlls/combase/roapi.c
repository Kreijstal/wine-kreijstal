/*
 * Copyright 2014 Martin Storsjo
 * Copyright 2016 Michael Müller
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
#include "objbase.h"
#include "ctxtcall.h"
#include "comsvcs.h"
#include "initguid.h"
#include "roapi.h"
#include "roparameterizediid.h"
#include "roerrorapi.h"
#include "winstring.h"
#include "errhandlingapi.h"

#include "combase_private.h"

#include "wine/debug.h"

HRESULT create_restricted_error_info(HRESULT hr, const WCHAR *description,
        const WCHAR *restricted_description, const WCHAR *capability_sid,
        const WCHAR *reference, IRestrictedErrorInfo **ret);

HRESULT capture_restricted_error_context(IRestrictedErrorInfo *iface, HRESULT hr);

HRESULT copy_restricted_error_context(IRestrictedErrorInfo *dst_iface, IRestrictedErrorInfo *src_iface);

HRESULT WINAPI RoGetMatchingRestrictedErrorInfo(HRESULT error, IRestrictedErrorInfo **info);

WINE_DEFAULT_DEBUG_CHANNEL(combase);

static UINT32 ro_error_reporting_flags = RO_ERROR_REPORTING_USESETERRORINFO;

#define RO_ERROR_REPORTING_VALID_MASK \
    (RO_ERROR_REPORTING_SUPPRESSEXCEPTIONS | \
    RO_ERROR_REPORTING_FORCEEXCEPTIONS | \
    RO_ERROR_REPORTING_USESETERRORINFO | \
    RO_ERROR_REPORTING_SUPPRESSSETERRORINFO)

struct activatable_class_data
{
    ULONG size;
    DWORD unk;
    DWORD module_len;
    DWORD module_offset;
    DWORD threading_model;
};

static WCHAR *duplicate_bounded_message(const WCHAR *message, UINT cchMax)
{
    UINT len = 0, limit = 511;
    WCHAR *copy;

    if (!message)
        return NULL;

    if (!cchMax)
    {
        while (len < limit && message[len])
            len++;
    }
    else
    {
        while (len < cchMax && len < limit && message[len])
            len++;
    }

    if (!len)
        return NULL;

    copy = malloc((len + 1) * sizeof(*copy));
    if (!copy)
        return NULL;

    memcpy(copy, message, len * sizeof(*copy));
    copy[len] = 0;
    return copy;
}

static BOOL is_current_thread_com_initialized(void)
{
    struct tlsdata *tlsdata = NtCurrentTeb()->ReservedForOle;

    return tlsdata && (tlsdata->inits || tlsdata->ole_inits);
}

static BOOL should_use_seterrorinfo(void)
{
    if (ro_error_reporting_flags & RO_ERROR_REPORTING_SUPPRESSSETERRORINFO)
        return FALSE;
    if (!(ro_error_reporting_flags & RO_ERROR_REPORTING_USESETERRORINFO))
        return FALSE;
    if (!is_current_thread_com_initialized())
        return FALSE;

    return TRUE;
}

static WCHAR *get_generic_error_message(HRESULT error)
{
    WCHAR *system_message = NULL, *copy;
    DWORD len, flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS;

    len = FormatMessageW(flags, NULL, error, 0, (WCHAR *)&system_message, 0, NULL);
    if (!len)
        len = FormatMessageW(flags, NULL, E_FAIL, 0, (WCHAR *)&system_message, 0, NULL);

    if (!len || !system_message)
        return wcsdup(L"Unspecified error");

    while (len && (system_message[len - 1] == '\r' || system_message[len - 1] == '\n'))
        system_message[--len] = 0;

    if (len > 511)
        len = 511;

    copy = malloc((len + 1) * sizeof(*copy));
    if (copy)
    {
        memcpy(copy, system_message, len * sizeof(*copy));
        copy[len] = 0;
    }

    LocalFree(system_message);
    return copy;
}

static HRESULT get_library_for_classid(const WCHAR *classid, WCHAR **out)
{
    ACTCTX_SECTION_KEYED_DATA data;
    HKEY hkey_root, hkey_class;
    DWORD type, size;
    HRESULT hr;
    WCHAR *buf = NULL;

    *out = NULL;

    /* search activation context first */
    data.cbSize = sizeof(data);
    if (FindActCtxSectionStringW(FIND_ACTCTX_SECTION_KEY_RETURN_HACTCTX, NULL,
            ACTIVATION_CONTEXT_SECTION_WINRT_ACTIVATABLE_CLASSES, classid, &data))
    {
        struct activatable_class_data *activatable_class = (struct activatable_class_data *)data.lpData;
        void *ptr = (BYTE *)data.lpSectionBase + activatable_class->module_offset;
        *out = wcsdup(ptr);
        return S_OK;
    }

    /* load class registry key */
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\WindowsRuntime\\ActivatableClassId",
                      0, KEY_READ, &hkey_root))
        return REGDB_E_READREGDB;
    if (RegOpenKeyExW(hkey_root, classid, 0, KEY_READ, &hkey_class))
    {
        WARN("Class %s not found in registry\n", debugstr_w(classid));
        RegCloseKey(hkey_root);
        return REGDB_E_CLASSNOTREG;
    }
    RegCloseKey(hkey_root);

    /* load (and expand) DllPath registry value */
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, &type, NULL, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ)
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (!(buf = malloc(size)))
    {
        hr = E_OUTOFMEMORY;
        goto done;
    }
    if (RegQueryValueExW(hkey_class, L"DllPath", NULL, NULL, (BYTE *)buf, &size))
    {
        hr = REGDB_E_READREGDB;
        goto done;
    }
    if (type == REG_EXPAND_SZ)
    {
        WCHAR *expanded;
        DWORD len = ExpandEnvironmentStringsW(buf, NULL, 0);
        if (!(expanded = malloc(len * sizeof(WCHAR))))
        {
            hr = E_OUTOFMEMORY;
            goto done;
        }
        ExpandEnvironmentStringsW(buf, expanded, len);
        free(buf);
        buf = expanded;
    }

    *out = buf;
    return S_OK;

done:
    free(buf);
    RegCloseKey(hkey_class);
    return hr;
}


/***********************************************************************
 *      RoInitialize (combase.@)
 */
HRESULT WINAPI RoInitialize(RO_INIT_TYPE type)
{
    switch (type) {
    case RO_INIT_SINGLETHREADED:
        return CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    default:
        FIXME("type %d\n", type);
    case RO_INIT_MULTITHREADED:
        return CoInitializeEx(NULL, COINIT_MULTITHREADED);
    }
}

/***********************************************************************
 *      RoUninitialize (combase.@)
 */
void WINAPI RoUninitialize(void)
{
    CoUninitialize();
}

/***********************************************************************
 *      RoGetActivationFactory (combase.@)
 */
HRESULT WINAPI DECLSPEC_HOTPATCH RoGetActivationFactory(HSTRING classid, REFIID iid, void **class_factory)
{
    PFNGETACTIVATIONFACTORY pDllGetActivationFactory;
    IActivationFactory *factory;
    WCHAR *library;
    HMODULE module;
    HRESULT hr;

    FIXME("(%s, %s, %p): semi-stub\n", debugstr_hstring(classid), debugstr_guid(iid), class_factory);

    if (!iid || !class_factory)
        return E_INVALIDARG;

    if (FAILED(hr = ensure_mta()))
        return hr;

    hr = get_library_for_classid(WindowsGetStringRawBuffer(classid, NULL), &library);
    if (FAILED(hr))
    {
        ERR("Failed to find library for %s\n", debugstr_hstring(classid));
        return hr;
    }

    if (!(module = LoadLibraryW(library)))
    {
        ERR("Failed to load module %s\n", debugstr_w(library));
        hr = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    if (!(pDllGetActivationFactory = (void *)GetProcAddress(module, "DllGetActivationFactory")))
    {
        ERR("Module %s does not implement DllGetActivationFactory\n", debugstr_w(library));
        hr = E_FAIL;
        goto done;
    }

    TRACE("Found library %s for class %s\n", debugstr_w(library), debugstr_hstring(classid));

    hr = pDllGetActivationFactory(classid, &factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_QueryInterface(factory, iid, class_factory);
        if (SUCCEEDED(hr))
        {
            TRACE("Created interface %p\n", *class_factory);
            module = NULL;
        }
        IActivationFactory_Release(factory);
    }
    else
    {
        ERR("Class %s not found in %s, hr %#lx.\n", wine_dbgstr_hstring(classid), debugstr_w(library), hr);
    }

done:
    free(library);
    if (module) FreeLibrary(module);
    return hr;
}

/***********************************************************************
 *      RoGetParameterizedTypeInstanceIID (combase.@)
 */
HRESULT WINAPI RoGetParameterizedTypeInstanceIID(UINT32 name_element_count, const WCHAR **name_elements,
                                                 const IRoMetaDataLocator *meta_data_locator, GUID *iid,
                                                 ROPARAMIIDHANDLE *hiid)
{
    FIXME("stub: %d %p %p %p %p\n", name_element_count, name_elements, meta_data_locator, iid, hiid);
    if (iid) *iid = GUID_NULL;
    if (hiid) *hiid = INVALID_HANDLE_VALUE;
    return E_NOTIMPL;
}

/***********************************************************************
 *      RoActivateInstance (combase.@)
 */
HRESULT WINAPI RoActivateInstance(HSTRING classid, IInspectable **instance)
{
    IActivationFactory *factory;
    HRESULT hr;

    FIXME("(%p, %p): semi-stub\n", classid, instance);

    hr = RoGetActivationFactory(classid, &IID_IActivationFactory, (void **)&factory);
    if (SUCCEEDED(hr))
    {
        hr = IActivationFactory_ActivateInstance(factory, instance);
        IActivationFactory_Release(factory);
    }

    return hr;
}

struct agile_reference
{
    IAgileReference IAgileReference_iface;
    enum AgileReferenceOptions option;
    IStream *marshal_stream;
    CRITICAL_SECTION cs;
    IUnknown *obj;
    BOOLEAN is_agile;
    IUnknown *ctx;
    LONG ref;
};

static HRESULT marshal_object_in_agile_reference(struct agile_reference *ref, REFIID riid, IUnknown *obj)
{
    HRESULT hr;

    hr = CreateStreamOnHGlobal(0, TRUE, &ref->marshal_stream);
    if (FAILED(hr))
        return hr;

    hr = CoMarshalInterface(ref->marshal_stream, riid, obj, MSHCTX_INPROC, NULL, MSHLFLAGS_TABLESTRONG);
    if (FAILED(hr))
    {
        IStream_Release(ref->marshal_stream);
        ref->marshal_stream = NULL;
    }
    return hr;
}

static inline struct agile_reference *impl_from_IAgileReference(IAgileReference *iface)
{
    return CONTAINING_RECORD(iface, struct agile_reference, IAgileReference_iface);
}

static HRESULT WINAPI agile_ref_QueryInterface(IAgileReference *iface, REFIID riid, void **obj)
{
    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (!riid || !obj) return E_INVALIDARG;

    if (IsEqualGUID(riid, &IID_IUnknown)
        || IsEqualGUID(riid, &IID_IAgileObject)
        || IsEqualGUID(riid, &IID_IAgileReference))
    {
        IUnknown_AddRef(iface);
        *obj = iface;
        return S_OK;
    }

    *obj = NULL;
    FIXME("interface %s is not implemented\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI agile_ref_AddRef(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    return InterlockedIncrement(&impl->ref);
}

static ULONG WINAPI agile_ref_Release(IAgileReference *iface)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LONG ref = InterlockedDecrement(&impl->ref);

    if (!ref)
    {
        TRACE("destroying %p\n", iface);

        if (impl->obj)
            IUnknown_Release(impl->obj);

        if (impl->marshal_stream)
        {
            LARGE_INTEGER zero = {0};

            IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL);
            CoReleaseMarshalData(impl->marshal_stream);
            IStream_Release(impl->marshal_stream);
        }
        DeleteCriticalSection(&impl->cs);
        free(impl);
    }

    return ref;
}

struct marshal_context_params
{
    struct agile_reference *impl;
    REFIID iid;
};

static HRESULT WINAPI marshal_object_in_context(ComCallData *arg)
{
    struct marshal_context_params *params = (struct marshal_context_params *)arg;
    HRESULT hr;

    hr = marshal_object_in_agile_reference(params->impl, params->iid, params->impl->obj);
    IUnknown_Release(params->impl->obj);
    params->impl->obj = NULL;
    return hr;
}

static HRESULT WINAPI agile_ref_Resolve(IAgileReference *iface, REFIID riid, void **obj)
{
    struct agile_reference *impl = impl_from_IAgileReference(iface);
    LARGE_INTEGER zero = {0};
    void *cur_ctx;
    HRESULT hr;

    TRACE("(%p, %s, %p)\n", iface, debugstr_guid(riid), obj);

    if (impl->is_agile)
        return IUnknown_QueryInterface(impl->obj, riid, obj);

    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&cur_ctx)))
        return hr;

    EnterCriticalSection(&impl->cs);
    if (impl->option == AGILEREFERENCE_DELAYEDMARSHAL && impl->marshal_stream == NULL)
    {
        struct marshal_context_params params = { impl, riid };
        IContextCallback *ctx;

        if (FAILED(hr = IUnknown_QueryInterface(impl->ctx, &IID_IContextCallback, (void **)&ctx)))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }

        hr = IContextCallback_ContextCallback(ctx, marshal_object_in_context, (ComCallData *)&params,
                                              &IID_IContextCallback, 5, NULL);
        IContextCallback_Release(ctx);
        if (FAILED(hr))
        {
            LeaveCriticalSection(&impl->cs);
            return hr;
        }
    }

    if (SUCCEEDED(hr = IStream_Seek(impl->marshal_stream, zero, STREAM_SEEK_SET, NULL)))
        hr = CoUnmarshalInterface(impl->marshal_stream, riid, obj);

    LeaveCriticalSection(&impl->cs);
    return hr;
}

static const IAgileReferenceVtbl agile_ref_vtbl =
{
    agile_ref_QueryInterface,
    agile_ref_AddRef,
    agile_ref_Release,
    agile_ref_Resolve,
};

static BOOL object_has_interface(IUnknown *obj, REFIID iid)
{
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface(obj, iid, (void **)&unk);
    if (SUCCEEDED(hr))
        IUnknown_Release(unk);
    return SUCCEEDED(hr);
}

/***********************************************************************
 *      RoGetAgileReference (combase.@)
 */
HRESULT WINAPI RoGetAgileReference(enum AgileReferenceOptions option, REFIID riid, IUnknown *obj,
                                   IAgileReference **agile_reference)
{
    struct apartment *apt;
    struct agile_reference *impl;
    HRESULT hr;

    TRACE("(%d, %s, %p, %p).\n", option, debugstr_guid(riid), obj, agile_reference);

    if (option != AGILEREFERENCE_DEFAULT && option != AGILEREFERENCE_DELAYEDMARSHAL)
        return E_INVALIDARG;

    if (!(apt = apartment_get_current_or_mta()))
    {
        ERR("Apartment not initialized\n");
        return CO_E_NOTINITIALIZED;
    }
    rpc_start_remoting(apt);
    apartment_release(apt);

    if (!object_has_interface(obj, riid))
        return E_NOINTERFACE;
    if (object_has_interface(obj, &IID_INoMarshal))
        return CO_E_NOT_SUPPORTED;

    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return E_OUTOFMEMORY;

    impl->IAgileReference_iface.lpVtbl = &agile_ref_vtbl;
    impl->option = option;
    impl->is_agile = object_has_interface(obj, &IID_IAgileObject);
    impl->ref = 1;
    if (FAILED(hr = CoGetContextToken((ULONG_PTR *)&impl->ctx)))
    {
        free( impl );
        return hr;
    }

    if (option == AGILEREFERENCE_DELAYEDMARSHAL || impl->is_agile)
    {
        impl->obj = obj;
        IUnknown_AddRef(impl->obj);
    }
    else if (option == AGILEREFERENCE_DEFAULT)
    {
        if (FAILED(hr = marshal_object_in_agile_reference(impl, riid, obj)))
        {
            free(impl);
            return hr;
        }
    }

    InitializeCriticalSection(&impl->cs);

    *agile_reference = &impl->IAgileReference_iface;
    return S_OK;
}

/***********************************************************************
 *      RoFailFastWithErrorContextInternal2 (combase.@)
 */
void WINAPI RoFailFastWithErrorContextInternal2(HRESULT error, ULONG exception_count, /* PSTOWED_EXCEPTION_INFORMATION_V2 */void *information)
{
    FIXME("%#lx, %lu, %p stub.\n", error, exception_count, information);
    RaiseFailFastException(NULL, NULL, 0);
}

/***********************************************************************
 *      RoGetApartmentIdentifier (combase.@)
 */
HRESULT WINAPI RoGetApartmentIdentifier(UINT64 *identifier)
{
    FIXME("(%p): stub\n", identifier);

    if (!identifier)
        return E_INVALIDARG;

    *identifier = 0xdeadbeef;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterForApartmentShutdown (combase.@)
 */
HRESULT WINAPI RoRegisterForApartmentShutdown(IApartmentShutdown *callback,
        UINT64 *identifier, APARTMENT_SHUTDOWN_REGISTRATION_COOKIE *cookie)
{
    HRESULT hr;

    FIXME("(%p, %p, %p): stub\n", callback, identifier, cookie);

    hr = RoGetApartmentIdentifier(identifier);
    if (FAILED(hr))
        return hr;

    if (cookie)
        *cookie = (void *)0xcafecafe;
    return S_OK;
}

/***********************************************************************
 *      RoGetServerActivatableClasses (combase.@)
 */
HRESULT WINAPI RoGetServerActivatableClasses(HSTRING name, HSTRING **classes, DWORD *count)
{
    FIXME("(%p, %p, %p): stub\n", name, classes, count);

    if (count)
        *count = 0;
    return S_OK;
}

/***********************************************************************
 *      RoRegisterActivationFactories (combase.@)
 */
HRESULT WINAPI RoRegisterActivationFactories(HSTRING *classes, PFNGETACTIVATIONFACTORY *callbacks,
                                             UINT32 count, RO_REGISTRATION_COOKIE *cookie)
{
    FIXME("(%p, %p, %d, %p): stub\n", classes, callbacks, count, cookie);

    return S_OK;
}

/***********************************************************************
 *      GetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI GetRestrictedErrorInfo(IRestrictedErrorInfo **info)
{
    IErrorInfo *error_info = NULL;
    HRESULT hr;

    TRACE("(%p)\n", info);

    if (!info) return E_POINTER;
    *info = NULL;

    hr = GetErrorInfo(0, &error_info);
    if (hr == S_FALSE)
        return S_FALSE;
    if (FAILED(hr))
    {
        WARN("GetErrorInfo failed, hr %#lx.\n", hr);
        return hr;
    }

    if (!error_info)
        return S_FALSE;

    hr = IErrorInfo_QueryInterface(error_info, &IID_IRestrictedErrorInfo, (void **)info);
    IErrorInfo_Release(error_info);

    if (FAILED(hr))
    {
        TRACE("Current error object does not support IRestrictedErrorInfo.\n");
        return S_FALSE;
    }

    return S_OK;
}

/***********************************************************************
 *      SetRestrictedErrorInfo (combase.@)
 */
HRESULT WINAPI SetRestrictedErrorInfo(IRestrictedErrorInfo *info)
{
    IErrorInfo *error_info = NULL;
    HRESULT hr;

    TRACE("(%p)\n", info);

    if (!info)
        return SetErrorInfo(0, NULL);

    hr = IRestrictedErrorInfo_QueryInterface(info, &IID_IErrorInfo, (void **)&error_info);
    if (FAILED(hr))
    {
        WARN("Restricted error object does not expose IErrorInfo, hr %#lx.\n", hr);
        return hr;
    }

    hr = SetErrorInfo(0, error_info);
    IErrorInfo_Release(error_info);

    return hr;
}

/***********************************************************************
 * RoCaptureErrorContext (combase.@)
 *
 * Captures the current restricted error context for the specified HRESULT.
 *
 * Wine stores the captured context in the restricted error object,
 * including a stack backtrace. Native Windows Error Reporting (WER)
 * integration is not implemented yet.
 */
HRESULT WINAPI RoCaptureErrorContext(HRESULT error)
{
    IRestrictedErrorInfo *info = NULL;
    HRESULT hr, stored_error = S_OK;

    TRACE("%#lx.\n", error);

    if (!FAILED(error))
        return S_OK;

    hr = GetRestrictedErrorInfo(&info);
    if (FAILED(hr) && hr != S_FALSE)
    {
        WARN("GetRestrictedErrorInfo failed, hr %#lx.\n", hr);
        return hr;
    }

    if (info)
    {
        hr = IRestrictedErrorInfo_GetErrorDetails(info, NULL, &stored_error, NULL, NULL);
        if (FAILED(hr) || stored_error != error)
        {
            IRestrictedErrorInfo_Release(info);
            info = NULL;
        }
    }

    if (!info)
    {
        hr = create_restricted_error_info(error, NULL, NULL, NULL, NULL, &info);
        if (FAILED(hr))
        {
            WARN("create_restricted_error_info failed, hr %#lx.\n", hr);
            return hr;
        }
    }

    hr = capture_restricted_error_context(info, error);
    if (FAILED(hr))
    {
        WARN("capture_restricted_error_context failed, hr %#lx.\n", hr);
        IRestrictedErrorInfo_Release(info);
        return hr;
    }

    hr = SetRestrictedErrorInfo(info);
    IRestrictedErrorInfo_Release(info);

    return hr;
}

/***********************************************************************
 *      RoClearError (combase.@)
 *
 *  Clears the current restricted error info for the calling thread.
 */
void WINAPI RoClearError(void)
{
    struct tlsdata *tlsdata = NtCurrentTeb()->ReservedForOle;
    IErrorInfo *error_info;

    TRACE("()\n");

    if (!tlsdata)
        return;

    error_info = tlsdata->errorinfo;
    tlsdata->errorinfo = NULL;

    if (error_info)
        IErrorInfo_Release(error_info);
}

/***********************************************************************
 *      RoOriginateLanguageException (combase.@)
 */
BOOL WINAPI RoOriginateLanguageException(HRESULT error, HSTRING message, IUnknown *language_exception)
{
    FIXME("%#lx, %s, %p: stub\n", error, debugstr_hstring(message), language_exception);
    return FALSE;
}

/***********************************************************************
 *      RoOriginateError (combase.@)
 */
BOOL WINAPI RoOriginateError(HRESULT error, HSTRING message)
{
    const WCHAR *str = NULL;
    UINT32 len = 0;

    TRACE("%#lx, %s.\n", error, debugstr_hstring(message));

    if (message)
        str = WindowsGetStringRawBuffer(message, &len);

    return RoOriginateErrorW(error, len, str);
}

/***********************************************************************
 *      RoOriginateErrorW (combase.@)
 */
BOOL WINAPI RoOriginateErrorW(HRESULT error, UINT max_len, const WCHAR *message)
{
    IRestrictedErrorInfo *info = NULL;
    WCHAR *text = NULL;
    HRESULT hr;

    TRACE("%#lx, %u, %s.\n", error, max_len, debugstr_w(message));

    if (!FAILED(error))
        return FALSE;

    if (message)
        text = duplicate_bounded_message(message, max_len);
    else
        text = get_generic_error_message(error);

    if (message && !text)
        return FALSE;

    /* The call still succeeds even if we don't attach the error object
     * to the COM channel. */
    if (!should_use_seterrorinfo())
    {
        free(text);
        return TRUE;
    }

    hr = create_restricted_error_info(error, text, NULL, NULL, NULL, &info);
    free(text);
    if (FAILED(hr))
    {
        WARN("create_restricted_error_info failed, hr %#lx.\n", hr);
        return FALSE;
    }

    hr = SetRestrictedErrorInfo(info);
    IRestrictedErrorInfo_Release(info);

    return SUCCEEDED(hr);
}

/***********************************************************************
 * RoReportUnhandledError (combase.@)
 *
 * Reports an unhandled error by updating the COM error channel state
 * for the current thread and logging the error details.
 */
HRESULT WINAPI RoReportUnhandledError(IRestrictedErrorInfo *info)
{
    HRESULT hr;
    BSTR description = NULL, restricted = NULL, capability = NULL;
    HRESULT orig_error = S_OK;

    TRACE("(%p)\n", info);

    if (!info)
        return E_POINTER;

    hr = SetRestrictedErrorInfo(info);
    if (FAILED(hr))
    {
        WARN("SetRestrictedErrorInfo failed, hr %#lx.\n", hr);
        return hr;
    }

    if (SUCCEEDED(IRestrictedErrorInfo_GetErrorDetails(info, &description, &orig_error,
            &restricted, &capability)))
    {
        WARN("Unhandled WinRT error: hr %#lx, desc %s, restricted %s, capability %s\n",
             orig_error, debugstr_w(description), debugstr_w(restricted),
             debugstr_w(capability));

        SysFreeString(description);
        SysFreeString(restricted);
        SysFreeString(capability);
    }
    else
    {
        WARN("Unhandled WinRT error reported, but failed to get details.\n");
    }

    /* Native Windows triggers the Global Error Handler here.
     * Wine currently preserves the reported restricted error object
     * on the current thread for later observation.
     */
    return S_OK;
}

/***********************************************************************
 *      RoSetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoSetErrorReportingFlags(UINT32 flags)
{
    TRACE("(%08x)\n", flags);

    if (flags & ~RO_ERROR_REPORTING_VALID_MASK)
        return E_INVALIDARG;

    ro_error_reporting_flags = flags;
    return S_OK;
}

/***********************************************************************
 *      RoTransformErrorW (combase.@)
 *
 *  Transforms the current restricted error into a new one with a different
 *  HRESULT and (optionally) a new user-visible message.
 */

BOOL WINAPI RoTransformErrorW(HRESULT old_error, HRESULT new_error,
                              UINT max_len, const WCHAR *message)
{
    IRestrictedErrorInfo *old_info = NULL, *new_info = NULL;
    BSTR desc = NULL, restricted_desc = NULL, cap_sid = NULL, reference = NULL;
    HRESULT hr, stored_hr = S_OK;
    WCHAR *bounded_message = NULL;
    const WCHAR *new_desc = NULL, *new_restricted_desc = NULL;

    TRACE("%#lx, %#lx, %u, %s.\n", old_error, new_error, max_len, debugstr_w(message));

    if (old_error == new_error)
        return FALSE;
    if (!FAILED(old_error) && !FAILED(new_error))
        return FALSE;

    if (message)
    {
        bounded_message = duplicate_bounded_message(message, max_len);
        if (!bounded_message)
            return FALSE;
    }
    else
    {
        bounded_message = get_generic_error_message(new_error);
        if (!bounded_message)
            return FALSE;
    }

    /* As with RoOriginateErrorW(), the call can still succeed even if
     * we don't attach the transformed error object to the COM channel. */
    if (!should_use_seterrorinfo())
    {
        free(bounded_message);
        return TRUE;
    }

    hr = RoGetMatchingRestrictedErrorInfo(old_error, &old_info);
    if (FAILED(hr))
    {
        WARN("RoGetMatchingRestrictedErrorInfo failed, hr %#lx.\n", hr);
        free(bounded_message);
        return FALSE;
    }

    if (old_info)
    {
        hr = IRestrictedErrorInfo_GetErrorDetails(old_info, &desc, &stored_hr,
                                                  &restricted_desc, &cap_sid);
        if (FAILED(hr))
        {
            desc = NULL;
            restricted_desc = NULL;
            cap_sid = NULL;
        }

        hr = IRestrictedErrorInfo_GetReference(old_info, &reference);
        if (FAILED(hr))
            reference = NULL;
    }

    new_desc = bounded_message;
    new_restricted_desc = desc ? desc : restricted_desc;

    hr = create_restricted_error_info(new_error, new_desc, new_restricted_desc,
                                      cap_sid, reference, &new_info);
    if (FAILED(hr))
    {
        WARN("create_restricted_error_info failed, hr %#lx.\n", hr);
        goto done;
    }

    if (old_info)
    {
        hr = copy_restricted_error_context(new_info, old_info);
        if (FAILED(hr))
        {
            WARN("copy_restricted_error_context failed, hr %#lx.\n", hr);
            goto done;
        }
    }

    hr = SetRestrictedErrorInfo(new_info);
    if (FAILED(hr))
    {
        WARN("SetRestrictedErrorInfo failed, hr %#lx.\n", hr);
        goto done;
    }

done:
    if (new_info)
        IRestrictedErrorInfo_Release(new_info);
    if (old_info)
        IRestrictedErrorInfo_Release(old_info);
    if (desc)
        SysFreeString(desc);
    if (restricted_desc)
        SysFreeString(restricted_desc);
    if (cap_sid)
        SysFreeString(cap_sid);
    if (reference)
        SysFreeString(reference);
    free(bounded_message);

    return SUCCEEDED(hr);
}

/***********************************************************************
 *      RoTransformError (combase.@)
 */
BOOL WINAPI RoTransformError(HRESULT old_error, HRESULT new_error, HSTRING message)
{
    const WCHAR *msg = NULL;
    UINT32 len = 0;

    TRACE("%#lx, %#lx, %s.\n", old_error, new_error, debugstr_hstring(message));

    if (message)
        msg = WindowsGetStringRawBuffer(message, &len);

    return RoTransformErrorW(old_error, new_error, len, msg);
}

/***********************************************************************
 *      RoGetMatchingRestrictedErrorInfo (combase.@)
 *
 *  Ensures that there is a restricted error info whose HRESULT matches
 *  the given error code, and returns it.
 */
HRESULT WINAPI RoGetMatchingRestrictedErrorInfo(HRESULT error, IRestrictedErrorInfo **info)
{
    IRestrictedErrorInfo *current = NULL;
    HRESULT hr, stored_hr = S_OK;

    TRACE("%#lx, %p.\n", error, info);

    if (!info) return E_POINTER;
    *info = NULL;

    if (!FAILED(error))
        return S_FALSE;

    hr = GetRestrictedErrorInfo(&current);
    if (FAILED(hr) && hr != S_FALSE)
    {
        WARN("GetRestrictedErrorInfo failed, hr %#lx.\n", hr);
        return hr;
    }

    if (current)
    {
        hr = IRestrictedErrorInfo_GetErrorDetails(current, NULL, &stored_hr, NULL, NULL);
        if (SUCCEEDED(hr) && stored_hr == error)
        {
            hr = SetRestrictedErrorInfo(current);
            if (FAILED(hr))
            {
                WARN("SetRestrictedErrorInfo failed, hr %#lx.\n", hr);
                IRestrictedErrorInfo_Release(current);
                return hr;
            }

            IRestrictedErrorInfo_AddRef(current);
            *info = current;
            IRestrictedErrorInfo_Release(current);
            return S_OK;
        }

        IRestrictedErrorInfo_Release(current);
        current = NULL;
    }

    hr = create_restricted_error_info(error, NULL, NULL, NULL, NULL, &current);
    if (FAILED(hr))
    {
        WARN("create_restricted_error_info failed, hr %#lx.\n", hr);
        return hr;
    }

    hr = SetRestrictedErrorInfo(current);
    if (FAILED(hr))
    {
        WARN("SetRestrictedErrorInfo failed, hr %#lx.\n", hr);
        IRestrictedErrorInfo_Release(current);
        return hr;
    }

    *info = current;
    return S_OK;
}

/***********************************************************************
 *      RoGetErrorReportingFlags (combase.@)
 */
HRESULT WINAPI RoGetErrorReportingFlags(UINT32 *flags)
{
    TRACE("(%p)\n", flags);

    if (!flags)
        return E_POINTER;

    *flags = ro_error_reporting_flags;
    return S_OK;
}


/***********************************************************************
 *      CleanupTlsOleState (combase.@)
 */
void WINAPI CleanupTlsOleState(void *unknown)
{
    FIXME("(%p): stub\n", unknown);
}

/***********************************************************************
 *      DllGetActivationFactory (combase.@)
 */
HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    FIXME("(%s, %p): stub\n", debugstr_hstring(classid), factory);

    return REGDB_E_CLASSNOTREG;
}

/***********************************************************************
 *      RoFailFastWithErrorContext (combase.@)
 */
void WINAPI RoFailFastWithErrorContext(HRESULT hr)
{
    FIXME("(0x%08lx)\n", hr);
    RaiseFailFastException(NULL, NULL, 0);
}
