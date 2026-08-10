/* Windows.Web.Http.HttpMethod implementation.
 *
 * Copyright 2026 OpenTerminal contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(web);

struct http_method
{
    IHttpMethod IHttpMethod_iface;
    LONG ref;
    HSTRING value;
};

static inline struct http_method *impl_from_IHttpMethod( IHttpMethod *iface )
{
    return CONTAINING_RECORD( iface, struct http_method, IHttpMethod_iface );
}

static HRESULT WINAPI method_QueryInterface( IHttpMethod *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHttpMethod ))
    {
        *out = iface;
        IHttpMethod_AddRef( iface );
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI method_AddRef( IHttpMethod *iface )
{
    return InterlockedIncrement( &impl_from_IHttpMethod( iface )->ref );
}

static ULONG WINAPI method_Release( IHttpMethod *iface )
{
    struct http_method *impl = impl_from_IHttpMethod( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        WindowsDeleteString( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI method_GetIids( IHttpMethod *iface, ULONG *count, IID **iids )
{
    IID *values;
    if (!count || !iids) return E_POINTER;
    if (!(values = CoTaskMemAlloc( sizeof(*values) ))) return E_OUTOFMEMORY;
    values[0] = IID_IHttpMethod;
    *count = 1;
    *iids = values;
    return S_OK;
}

static HRESULT WINAPI method_GetRuntimeClassName( IHttpMethod *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Web_Http_HttpMethod,
                                wcslen( RuntimeClass_Windows_Web_Http_HttpMethod ), name );
}

static HRESULT WINAPI method_GetTrustLevel( IHttpMethod *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI method_get_Method( IHttpMethod *iface, HSTRING *value )
{
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl_from_IHttpMethod( iface )->value, value );
}

static const IHttpMethodVtbl method_vtbl =
{
    method_QueryInterface,
    method_AddRef,
    method_Release,
    method_GetIids,
    method_GetRuntimeClassName,
    method_GetTrustLevel,
    method_get_Method,
};

static HRESULT method_create( HSTRING value, IHttpMethod **out )
{
    struct http_method *impl;
    UINT32 length;
    const WCHAR *buffer;
    HRESULT hr;

    if (!out) return E_POINTER;
    *out = NULL;
    buffer = WindowsGetStringRawBuffer( value, &length );
    if (!buffer || !length) return E_INVALIDARG;
    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IHttpMethod_iface.lpVtbl = &method_vtbl;
    impl->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( value, &impl->value )))
    {
        free( impl );
        return hr;
    }
    *out = &impl->IHttpMethod_iface;
    return S_OK;
}

struct method_factory
{
    IActivationFactory IActivationFactory_iface;
    IHttpMethodFactory IHttpMethodFactory_iface;
    IHttpMethodStatics IHttpMethodStatics_iface;
};

static struct method_factory method_factory;

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &method_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IHttpMethodFactory ))
        *out = &method_factory.IHttpMethodFactory_iface;
    else if (IsEqualGUID( iid, &IID_IHttpMethodStatics ))
        *out = &method_factory.IHttpMethodStatics_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI factory_Release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    if (instance) *instance = NULL;
    return E_NOTIMPL;
}

static const IActivationFactoryVtbl activation_vtbl =
{
    factory_QueryInterface, factory_AddRef, factory_Release, factory_GetIids,
    factory_GetRuntimeClassName, factory_GetTrustLevel, factory_ActivateInstance,
};

static inline struct method_factory *factory_from_method( IHttpMethodFactory *iface )
{
    return CONTAINING_RECORD( iface, struct method_factory, IHttpMethodFactory_iface );
}

static HRESULT WINAPI method_factory_QueryInterface( IHttpMethodFactory *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &factory_from_method( iface )->IActivationFactory_iface, iid, out );
}
static ULONG WINAPI method_factory_AddRef( IHttpMethodFactory *iface ) { return 2; }
static ULONG WINAPI method_factory_Release( IHttpMethodFactory *iface ) { return 1; }
static HRESULT WINAPI method_factory_GetIids( IHttpMethodFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI method_factory_GetRuntimeClassName( IHttpMethodFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI method_factory_GetTrustLevel( IHttpMethodFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI method_factory_Create( IHttpMethodFactory *iface, HSTRING value, IHttpMethod **out )
{
    return method_create( value, out );
}
static const IHttpMethodFactoryVtbl method_factory_vtbl =
{
    method_factory_QueryInterface, method_factory_AddRef, method_factory_Release,
    method_factory_GetIids, method_factory_GetRuntimeClassName, method_factory_GetTrustLevel,
    method_factory_Create,
};

static inline struct method_factory *factory_from_statics( IHttpMethodStatics *iface )
{
    return CONTAINING_RECORD( iface, struct method_factory, IHttpMethodStatics_iface );
}
static HRESULT WINAPI method_statics_QueryInterface( IHttpMethodStatics *iface, REFIID iid, void **out )
{
    return factory_QueryInterface( &factory_from_statics( iface )->IActivationFactory_iface, iid, out );
}
static ULONG WINAPI method_statics_AddRef( IHttpMethodStatics *iface ) { return 2; }
static ULONG WINAPI method_statics_Release( IHttpMethodStatics *iface ) { return 1; }
static HRESULT WINAPI method_statics_GetIids( IHttpMethodStatics *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI method_statics_GetRuntimeClassName( IHttpMethodStatics *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI method_statics_GetTrustLevel( IHttpMethodStatics *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

HRESULT http_method_create_literal( const WCHAR *literal, IHttpMethod **out )
{
    HSTRING value;
    HRESULT hr;
    if (FAILED(hr = WindowsCreateString( literal, wcslen( literal ), &value ))) return hr;
    hr = method_create( value, out );
    WindowsDeleteString( value );
    return hr;
}

#define DEFINE_METHOD_GETTER(name, literal) \
static HRESULT WINAPI method_statics_get_##name( IHttpMethodStatics *iface, IHttpMethod **out ) \
{ return http_method_create_literal( L##literal, out ); }
DEFINE_METHOD_GETTER(Delete, "DELETE")
DEFINE_METHOD_GETTER(Get, "GET")
DEFINE_METHOD_GETTER(Head, "HEAD")
DEFINE_METHOD_GETTER(Options, "OPTIONS")
DEFINE_METHOD_GETTER(Patch, "PATCH")
DEFINE_METHOD_GETTER(Post, "POST")
DEFINE_METHOD_GETTER(Put, "PUT")

static const IHttpMethodStaticsVtbl method_statics_vtbl =
{
    method_statics_QueryInterface, method_statics_AddRef, method_statics_Release,
    method_statics_GetIids, method_statics_GetRuntimeClassName, method_statics_GetTrustLevel,
    method_statics_get_Delete, method_statics_get_Get, method_statics_get_Head,
    method_statics_get_Options, method_statics_get_Patch, method_statics_get_Post,
    method_statics_get_Put,
};

static struct method_factory method_factory =
{
    {&activation_vtbl},
    {&method_factory_vtbl},
    {&method_statics_vtbl},
};

IActivationFactory *http_method_factory = &method_factory.IActivationFactory_iface;
