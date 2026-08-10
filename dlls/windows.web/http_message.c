/* Windows.Web.Http request and response messages.
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

struct http_request
{
    IHttpRequestMessage IHttpRequestMessage_iface;
    IClosable IClosable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IHttpContent *content;
    IHttpMethod *method;
    IUriRuntimeClass *uri;
    struct http_headers *headers;
    BOOL closed;
};

static inline struct http_request *request_from_iface( IHttpRequestMessage *iface )
{ return CONTAINING_RECORD( iface, struct http_request, IHttpRequestMessage_iface ); }
static inline struct http_request *request_from_closable( IClosable *iface )
{ return CONTAINING_RECORD( iface, struct http_request, IClosable_iface ); }

static HRESULT WINAPI request_QueryInterface( IHttpRequestMessage *iface, REFIID iid, void **out )
{
    struct http_request *request = request_from_iface( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHttpRequestMessage ))
        *out = &request->IHttpRequestMessage_iface;
    else if (IsEqualGUID( iid, &IID_IClosable )) *out = &request->IClosable_iface;
    else return E_NOINTERFACE;
    IHttpRequestMessage_AddRef( &request->IHttpRequestMessage_iface );
    return S_OK;
}
static ULONG WINAPI request_AddRef( IHttpRequestMessage *iface )
{ return InterlockedIncrement( &request_from_iface( iface )->ref ); }
static ULONG WINAPI request_Release( IHttpRequestMessage *iface )
{
    struct http_request *request = request_from_iface( iface );
    ULONG ref = InterlockedDecrement( &request->ref );
    if (!ref)
    {
        if (request->content) IHttpContent_Release( request->content );
        if (request->method) IHttpMethod_Release( request->method );
        if (request->uri) IUriRuntimeClass_Release( request->uri );
        http_headers_release( request->headers );
        request->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &request->cs );
        free( request );
    }
    return ref;
}
static HRESULT WINAPI request_GetIids( IHttpRequestMessage *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(**iids) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IHttpRequestMessage;
    (*iids)[1] = IID_IClosable;
    *count = 2;
    return S_OK;
}
static HRESULT WINAPI request_GetRuntimeClassName( IHttpRequestMessage *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Web_Http_HttpRequestMessage,
            wcslen( RuntimeClass_Windows_Web_Http_HttpRequestMessage ), name );
}
static HRESULT WINAPI request_GetTrustLevel( IHttpRequestMessage *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }

#define REQUEST_GET_INTERFACE(name, type, field, addref) \
static HRESULT WINAPI request_get_##name( IHttpRequestMessage *iface, type **value ) \
{ struct http_request *request = request_from_iface( iface ); if (!value) return E_POINTER; \
  EnterCriticalSection( &request->cs ); if ((*value = request->field)) addref( *value ); \
  LeaveCriticalSection( &request->cs ); return S_OK; }
REQUEST_GET_INTERFACE(Content, IHttpContent, content, IHttpContent_AddRef)
REQUEST_GET_INTERFACE(Method, IHttpMethod, method, IHttpMethod_AddRef)
REQUEST_GET_INTERFACE(RequestUri, IUriRuntimeClass, uri, IUriRuntimeClass_AddRef)

#define REQUEST_PUT_INTERFACE(name, type, field, addref, release) \
static HRESULT WINAPI request_put_##name( IHttpRequestMessage *iface, type *value ) \
{ struct http_request *request = request_from_iface( iface ); type *old; if (value) addref( value ); \
  EnterCriticalSection( &request->cs ); if (request->closed) { LeaveCriticalSection( &request->cs ); \
  if (value) release( value ); return RO_E_CLOSED; } old = request->field; request->field = value; \
  LeaveCriticalSection( &request->cs ); if (old) release( old ); return S_OK; }
REQUEST_PUT_INTERFACE(Content, IHttpContent, content, IHttpContent_AddRef, IHttpContent_Release)
REQUEST_PUT_INTERFACE(Method, IHttpMethod, method, IHttpMethod_AddRef, IHttpMethod_Release)
REQUEST_PUT_INTERFACE(RequestUri, IUriRuntimeClass, uri, IUriRuntimeClass_AddRef, IUriRuntimeClass_Release)

static HRESULT WINAPI request_get_Headers( IHttpRequestMessage *iface, IHttpRequestHeaderCollection **value )
{
    struct http_request *request = request_from_iface( iface );
    if (!value) return E_POINTER;
    *value = http_headers_request_iface( request->headers );
    http_headers_addref( request->headers );
    return S_OK;
}
static HRESULT WINAPI request_get_Properties( IHttpRequestMessage *iface, IInspectable **value )
{ if (!value) return E_POINTER; *value = NULL; return E_NOTIMPL; }
static HRESULT WINAPI request_get_TransportInformation( IHttpRequestMessage *iface, IInspectable **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static const IHttpRequestMessageVtbl request_vtbl =
{
    request_QueryInterface, request_AddRef, request_Release, request_GetIids,
    request_GetRuntimeClassName, request_GetTrustLevel, request_get_Content,
    request_put_Content, request_get_Headers, request_get_Method, request_put_Method,
    request_get_Properties, request_get_RequestUri, request_put_RequestUri,
    request_get_TransportInformation,
};

static HRESULT WINAPI request_closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{ return request_QueryInterface( &request_from_closable( iface )->IHttpRequestMessage_iface, iid, out ); }
static ULONG WINAPI request_closable_AddRef( IClosable *iface )
{ return request_AddRef( &request_from_closable( iface )->IHttpRequestMessage_iface ); }
static ULONG WINAPI request_closable_Release( IClosable *iface )
{ return request_Release( &request_from_closable( iface )->IHttpRequestMessage_iface ); }
static HRESULT WINAPI request_closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{ return request_GetIids( &request_from_closable( iface )->IHttpRequestMessage_iface, count, iids ); }
static HRESULT WINAPI request_closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{ return request_GetRuntimeClassName( &request_from_closable( iface )->IHttpRequestMessage_iface, name ); }
static HRESULT WINAPI request_closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{ return request_GetTrustLevel( &request_from_closable( iface )->IHttpRequestMessage_iface, level ); }
static HRESULT WINAPI request_closable_Close( IClosable *iface )
{
    struct http_request *request = request_from_closable( iface );
    EnterCriticalSection( &request->cs ); request->closed = TRUE; LeaveCriticalSection( &request->cs );
    return S_OK;
}
static const IClosableVtbl request_closable_vtbl =
{
    request_closable_QueryInterface, request_closable_AddRef, request_closable_Release,
    request_closable_GetIids, request_closable_GetRuntimeClassName,
    request_closable_GetTrustLevel, request_closable_Close,
};

HRESULT http_request_create( IHttpMethod *method, IUriRuntimeClass *uri, IHttpRequestMessage **out )
{
    struct http_request *request;
    HRESULT hr;
    if (!method || !out) return E_POINTER;
    *out = NULL;
    if (!(request = calloc( 1, sizeof(*request) ))) return E_OUTOFMEMORY;
    request->IHttpRequestMessage_iface.lpVtbl = &request_vtbl;
    request->IClosable_iface.lpVtbl = &request_closable_vtbl;
    request->ref = 1;
    request->method = method;
    IHttpMethod_AddRef( method );
    if ((request->uri = uri)) IUriRuntimeClass_AddRef( uri );
    InitializeCriticalSectionEx( &request->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    request->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": http_request.cs");
    if (FAILED(hr = http_headers_create( HTTP_HEADERS_REQUEST, &request->headers )))
    {
        request_Release( &request->IHttpRequestMessage_iface );
        return hr;
    }
    *out = &request->IHttpRequestMessage_iface;
    return S_OK;
}

HRESULT http_request_get_transport( IHttpRequestMessage *iface, IHttpMethod **method, IUriRuntimeClass **uri,
                                    IHttpContent **content, struct http_headers **headers )
{
    struct http_request *request;
    if (!iface || !method || !uri || !content || !headers) return E_POINTER;
    if (iface->lpVtbl != &request_vtbl) return E_NOINTERFACE;
    request = request_from_iface( iface );
    EnterCriticalSection( &request->cs );
    if (request->closed)
    {
        LeaveCriticalSection( &request->cs );
        return RO_E_CLOSED;
    }
    *method = request->method;
    *uri = request->uri;
    *content = request->content;
    *headers = request->headers;
    if (*method) IHttpMethod_AddRef( *method );
    if (*uri) IUriRuntimeClass_AddRef( *uri );
    if (*content) IHttpContent_AddRef( *content );
    http_headers_addref( *headers );
    LeaveCriticalSection( &request->cs );
    return S_OK;
}

struct http_response
{
    IHttpResponseMessage IHttpResponseMessage_iface;
    IClosable IClosable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    IHttpContent *content;
    IHttpRequestMessage *request;
    struct http_headers *headers;
    HSTRING reason;
    HttpResponseMessageSource source;
    HttpStatusCode status;
    HttpVersion version;
    BOOL closed;
};
static inline struct http_response *response_from_iface( IHttpResponseMessage *iface )
{ return CONTAINING_RECORD( iface, struct http_response, IHttpResponseMessage_iface ); }
static inline struct http_response *response_from_closable( IClosable *iface )
{ return CONTAINING_RECORD( iface, struct http_response, IClosable_iface ); }
static HRESULT WINAPI http_response_QueryInterface( IHttpResponseMessage *iface, REFIID iid, void **out )
{
    struct http_response *response = response_from_iface( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHttpResponseMessage ))
        *out = &response->IHttpResponseMessage_iface;
    else if (IsEqualGUID( iid, &IID_IClosable )) *out = &response->IClosable_iface;
    else return E_NOINTERFACE;
    IHttpResponseMessage_AddRef( &response->IHttpResponseMessage_iface );
    return S_OK;
}
static ULONG WINAPI http_response_AddRef( IHttpResponseMessage *iface )
{ return InterlockedIncrement( &response_from_iface( iface )->ref ); }
static ULONG WINAPI http_response_Release( IHttpResponseMessage *iface )
{
    struct http_response *response = response_from_iface( iface );
    ULONG ref = InterlockedDecrement( &response->ref );
    if (!ref)
    {
        if (response->content) IHttpContent_Release( response->content );
        if (response->request) IHttpRequestMessage_Release( response->request );
        http_headers_release( response->headers );
        WindowsDeleteString( response->reason );
        response->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &response->cs );
        free( response );
    }
    return ref;
}
static HRESULT WINAPI http_response_GetIids( IHttpResponseMessage *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(**iids) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IHttpResponseMessage; (*iids)[1] = IID_IClosable; *count = 2; return S_OK;
}
static HRESULT WINAPI http_response_GetRuntimeClassName( IHttpResponseMessage *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Web_Http_HttpResponseMessage,
            wcslen( RuntimeClass_Windows_Web_Http_HttpResponseMessage ), name );
}
static HRESULT WINAPI http_response_GetTrustLevel( IHttpResponseMessage *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
#define RESPONSE_GET_INTERFACE(name, type, field, addref) \
static HRESULT WINAPI http_response_get_##name( IHttpResponseMessage *iface, type **value ) \
{ struct http_response *response = response_from_iface( iface ); if (!value) return E_POINTER; \
  EnterCriticalSection( &response->cs ); if ((*value = response->field)) addref( *value ); \
  LeaveCriticalSection( &response->cs ); return S_OK; }
RESPONSE_GET_INTERFACE(Content, IHttpContent, content, IHttpContent_AddRef)
RESPONSE_GET_INTERFACE(RequestMessage, IHttpRequestMessage, request, IHttpRequestMessage_AddRef)
#define RESPONSE_PUT_INTERFACE(name, type, field, addref, release) \
static HRESULT WINAPI http_response_put_##name( IHttpResponseMessage *iface, type *value ) \
{ struct http_response *response = response_from_iface( iface ); type *old; if (value) addref( value ); \
  EnterCriticalSection( &response->cs ); old = response->field; response->field = value; \
  LeaveCriticalSection( &response->cs ); if (old) release( old ); return S_OK; }
RESPONSE_PUT_INTERFACE(Content, IHttpContent, content, IHttpContent_AddRef, IHttpContent_Release)
RESPONSE_PUT_INTERFACE(RequestMessage, IHttpRequestMessage, request, IHttpRequestMessage_AddRef, IHttpRequestMessage_Release)
static HRESULT WINAPI http_response_get_Headers( IHttpResponseMessage *iface, IHttpResponseHeaderCollection **value )
{
    struct http_response *response = response_from_iface( iface );
    if (!value) return E_POINTER;
    *value = http_headers_response_iface( response->headers );
    http_headers_addref( response->headers );
    return S_OK;
}
static HRESULT WINAPI http_response_get_IsSuccessStatusCode( IHttpResponseMessage *iface, boolean *value )
{ if (!value) return E_POINTER; *value = response_from_iface( iface )->status >= 200 && response_from_iface( iface )->status <= 299; return S_OK; }
static HRESULT WINAPI http_response_get_ReasonPhrase( IHttpResponseMessage *iface, HSTRING *value )
{ if (!value) return E_POINTER; return WindowsDuplicateString( response_from_iface( iface )->reason, value ); }
static HRESULT WINAPI http_response_put_ReasonPhrase( IHttpResponseMessage *iface, HSTRING value )
{
    struct http_response *response = response_from_iface( iface ); HSTRING copy, old; HRESULT hr;
    if (FAILED(hr = WindowsDuplicateString( value, &copy ))) return hr;
    EnterCriticalSection( &response->cs ); old = response->reason; response->reason = copy; LeaveCriticalSection( &response->cs );
    WindowsDeleteString( old ); return S_OK;
}
static HRESULT WINAPI http_response_get_Source( IHttpResponseMessage *iface, HttpResponseMessageSource *value )
{ if (!value) return E_POINTER; *value = response_from_iface( iface )->source; return S_OK; }
static HRESULT WINAPI http_response_put_Source( IHttpResponseMessage *iface, HttpResponseMessageSource value )
{ response_from_iface( iface )->source = value; return S_OK; }
static HRESULT WINAPI http_response_get_StatusCode( IHttpResponseMessage *iface, HttpStatusCode *value )
{ if (!value) return E_POINTER; *value = response_from_iface( iface )->status; return S_OK; }
static HRESULT WINAPI http_response_put_StatusCode( IHttpResponseMessage *iface, HttpStatusCode value )
{ response_from_iface( iface )->status = value; return S_OK; }
static HRESULT WINAPI http_response_get_Version( IHttpResponseMessage *iface, HttpVersion *value )
{ if (!value) return E_POINTER; *value = response_from_iface( iface )->version; return S_OK; }
static HRESULT WINAPI http_response_put_Version( IHttpResponseMessage *iface, HttpVersion value )
{ response_from_iface( iface )->version = value; return S_OK; }
static HRESULT WINAPI http_response_EnsureSuccessStatusCode( IHttpResponseMessage *iface, IHttpResponseMessage **value )
{
    boolean success;
    if (!value) return E_POINTER;
    *value = NULL;
    http_response_get_IsSuccessStatusCode( iface, &success );
    if (!success) return E_FAIL;
    *value = iface; IHttpResponseMessage_AddRef( iface ); return S_OK;
}
static const IHttpResponseMessageVtbl response_vtbl =
{
    http_response_QueryInterface, http_response_AddRef, http_response_Release,
    http_response_GetIids, http_response_GetRuntimeClassName, http_response_GetTrustLevel,
    http_response_get_Content, http_response_put_Content, http_response_get_Headers,
    http_response_get_IsSuccessStatusCode, http_response_get_ReasonPhrase,
    http_response_put_ReasonPhrase, http_response_get_RequestMessage,
    http_response_put_RequestMessage, http_response_get_Source, http_response_put_Source,
    http_response_get_StatusCode, http_response_put_StatusCode,
    http_response_get_Version, http_response_put_Version,
    http_response_EnsureSuccessStatusCode,
};
static HRESULT WINAPI response_closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{ return http_response_QueryInterface( &response_from_closable( iface )->IHttpResponseMessage_iface, iid, out ); }
static ULONG WINAPI response_closable_AddRef( IClosable *iface )
{ return http_response_AddRef( &response_from_closable( iface )->IHttpResponseMessage_iface ); }
static ULONG WINAPI response_closable_Release( IClosable *iface )
{ return http_response_Release( &response_from_closable( iface )->IHttpResponseMessage_iface ); }
static HRESULT WINAPI response_closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{ return http_response_GetIids( &response_from_closable( iface )->IHttpResponseMessage_iface, count, iids ); }
static HRESULT WINAPI response_closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{ return http_response_GetRuntimeClassName( &response_from_closable( iface )->IHttpResponseMessage_iface, name ); }
static HRESULT WINAPI response_closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{ return http_response_GetTrustLevel( &response_from_closable( iface )->IHttpResponseMessage_iface, level ); }
static HRESULT WINAPI response_closable_Close( IClosable *iface )
{ response_from_closable( iface )->closed = TRUE; return S_OK; }
static const IClosableVtbl response_closable_vtbl =
{
    response_closable_QueryInterface, response_closable_AddRef, response_closable_Release,
    response_closable_GetIids, response_closable_GetRuntimeClassName,
    response_closable_GetTrustLevel, response_closable_Close,
};

static const WCHAR *status_reason( HttpStatusCode status )
{
    switch (status)
    {
    case HttpStatusCode_Ok: return L"OK";
    case HttpStatusCode_Created: return L"Created";
    case HttpStatusCode_Accepted: return L"Accepted";
    case HttpStatusCode_NoContent: return L"No Content";
    case HttpStatusCode_BadRequest: return L"Bad Request";
    case HttpStatusCode_Unauthorized: return L"Unauthorized";
    case HttpStatusCode_Forbidden: return L"Forbidden";
    case HttpStatusCode_NotFound: return L"Not Found";
    case HttpStatusCode_InternalServerError: return L"Internal Server Error";
    default: return L"";
    }
}

HRESULT http_response_create( HttpStatusCode status, const BYTE *body, SIZE_T body_size,
                              IHttpRequestMessage *request, IHttpResponseMessage **out )
{
    struct http_response *response;
    HSTRING media = NULL;
    const WCHAR *reason;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(response = calloc( 1, sizeof(*response) ))) return E_OUTOFMEMORY;
    response->IHttpResponseMessage_iface.lpVtbl = &response_vtbl;
    response->IClosable_iface.lpVtbl = &response_closable_vtbl;
    response->ref = 1;
    response->status = status;
    response->source = HttpResponseMessageSource_Network;
    response->version = HttpVersion_Http11;
    if ((response->request = request)) IHttpRequestMessage_AddRef( request );
    InitializeCriticalSectionEx( &response->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    response->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": http_response.cs");
    reason = status_reason( status );
    if (FAILED(hr = WindowsCreateString( reason, wcslen( reason ), &response->reason )) ||
        FAILED(hr = WindowsCreateString( L"application/octet-stream",
                ARRAY_SIZE(L"application/octet-stream") - 1, &media )) ||
        FAILED(hr = http_content_create( body, body_size, media, &response->content )) ||
        FAILED(hr = http_headers_create( HTTP_HEADERS_RESPONSE, &response->headers )))
    {
        WindowsDeleteString( media );
        http_response_Release( &response->IHttpResponseMessage_iface );
        return hr;
    }
    WindowsDeleteString( media );
    *out = &response->IHttpResponseMessage_iface;
    return S_OK;
}

struct message_factory
{
    IActivationFactory IActivationFactory_iface;
    IHttpRequestMessageFactory IHttpRequestMessageFactory_iface;
    IHttpResponseMessageFactory IHttpResponseMessageFactory_iface;
    BOOL request;
};
static struct message_factory request_factory, response_factory;
static HRESULT WINAPI message_factory_qi( IActivationFactory *iface, REFIID iid, void **out )
{
    struct message_factory *factory = CONTAINING_RECORD( iface, struct message_factory, IActivationFactory_iface );
    if (!out) return E_POINTER; *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory )) *out = iface;
    else if (factory->request && IsEqualGUID( iid, &IID_IHttpRequestMessageFactory ))
        *out = &factory->IHttpRequestMessageFactory_iface;
    else if (!factory->request && IsEqualGUID( iid, &IID_IHttpResponseMessageFactory ))
        *out = &factory->IHttpResponseMessageFactory_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out ); return S_OK;
}
static ULONG WINAPI message_factory_addref( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI message_factory_release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI message_factory_iids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI message_factory_name( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI message_factory_trust( IActivationFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI message_factory_activate( IActivationFactory *iface, IInspectable **out )
{
    struct message_factory *factory = CONTAINING_RECORD( iface, struct message_factory, IActivationFactory_iface );
    HRESULT hr;
    if (!out) return E_POINTER; *out = NULL;
    if (factory->request)
    {
        IHttpMethod *method; IHttpRequestMessage *request;
        if (FAILED(hr = http_method_create_literal( L"GET", &method ))) return hr;
        hr = http_request_create( method, NULL, &request ); IHttpMethod_Release( method );
        if (SUCCEEDED(hr)) *out = (IInspectable *)request;
        return hr;
    }
    else
    {
        IHttpResponseMessage *response;
        if (SUCCEEDED(hr = http_response_create( HttpStatusCode_Ok, NULL, 0, NULL, &response )))
            *out = (IInspectable *)response;
        return hr;
    }
}
static const IActivationFactoryVtbl message_activation_vtbl =
{
    message_factory_qi, message_factory_addref, message_factory_release,
    message_factory_iids, message_factory_name, message_factory_trust,
    message_factory_activate,
};
static inline struct message_factory *factory_from_request_iface( IHttpRequestMessageFactory *iface )
{ return CONTAINING_RECORD( iface, struct message_factory, IHttpRequestMessageFactory_iface ); }
static HRESULT WINAPI request_factory_qi( IHttpRequestMessageFactory *iface, REFIID iid, void **out )
{ return message_factory_qi( &factory_from_request_iface( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI request_factory_addref( IHttpRequestMessageFactory *iface ) { return 2; }
static ULONG WINAPI request_factory_release( IHttpRequestMessageFactory *iface ) { return 1; }
static HRESULT WINAPI request_factory_iids( IHttpRequestMessageFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI request_factory_name( IHttpRequestMessageFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI request_factory_trust( IHttpRequestMessageFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI request_factory_Create( IHttpRequestMessageFactory *iface, IHttpMethod *method,
        IUriRuntimeClass *uri, IHttpRequestMessage **out )
{ return http_request_create( method, uri, out ); }
static const IHttpRequestMessageFactoryVtbl request_factory_vtbl =
{
    request_factory_qi, request_factory_addref, request_factory_release,
    request_factory_iids, request_factory_name, request_factory_trust, request_factory_Create,
};
static inline struct message_factory *factory_from_response_iface( IHttpResponseMessageFactory *iface )
{ return CONTAINING_RECORD( iface, struct message_factory, IHttpResponseMessageFactory_iface ); }
static HRESULT WINAPI response_factory_qi( IHttpResponseMessageFactory *iface, REFIID iid, void **out )
{ return message_factory_qi( &factory_from_response_iface( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI response_factory_addref( IHttpResponseMessageFactory *iface ) { return 2; }
static ULONG WINAPI response_factory_release( IHttpResponseMessageFactory *iface ) { return 1; }
static HRESULT WINAPI response_factory_iids( IHttpResponseMessageFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI response_factory_name( IHttpResponseMessageFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI response_factory_trust( IHttpResponseMessageFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI response_factory_Create( IHttpResponseMessageFactory *iface, HttpStatusCode status,
        IHttpResponseMessage **out )
{ return http_response_create( status, NULL, 0, NULL, out ); }
static const IHttpResponseMessageFactoryVtbl response_factory_vtbl =
{
    response_factory_qi, response_factory_addref, response_factory_release,
    response_factory_iids, response_factory_name, response_factory_trust, response_factory_Create,
};
static struct message_factory request_factory =
{ {&message_activation_vtbl}, {&request_factory_vtbl}, {NULL}, TRUE };
static struct message_factory response_factory =
{ {&message_activation_vtbl}, {NULL}, {&response_factory_vtbl}, FALSE };
IActivationFactory *http_request_factory = &request_factory.IActivationFactory_iface;
IActivationFactory *http_response_factory = &response_factory.IActivationFactory_iface;
