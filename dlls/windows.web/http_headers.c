/* Windows.Web.Http header collections.
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

struct header_pair
{
    HSTRING name;
    HSTRING value;
};

struct http_headers
{
    IHttpRequestHeaderCollection IHttpRequestHeaderCollection_iface;
    IHttpContentHeaderCollection IHttpContentHeaderCollection_iface;
    IHttpResponseHeaderCollection IHttpResponseHeaderCollection_iface;
    IHttpMediaTypeWithQualityHeaderValueCollection IHttpMediaTypeWithQualityHeaderValueCollection_iface;
    IHttpProductInfoHeaderValueCollection IHttpProductInfoHeaderValueCollection_iface;
    LONG ref;
    enum http_headers_kind kind;
    CRITICAL_SECTION cs;
    struct header_pair *pairs;
    SIZE_T count;
    SIZE_T capacity;
    IHttpCredentialsHeaderValue *authorization;
    IUriRuntimeClass *referer;
};

static inline struct http_headers *headers_from_request( IHttpRequestHeaderCollection *iface )
{
    return CONTAINING_RECORD( iface, struct http_headers, IHttpRequestHeaderCollection_iface );
}
static inline struct http_headers *headers_from_content( IHttpContentHeaderCollection *iface )
{
    return CONTAINING_RECORD( iface, struct http_headers, IHttpContentHeaderCollection_iface );
}
static inline struct http_headers *headers_from_response( IHttpResponseHeaderCollection *iface )
{
    return CONTAINING_RECORD( iface, struct http_headers, IHttpResponseHeaderCollection_iface );
}
static inline struct http_headers *headers_from_accept( IHttpMediaTypeWithQualityHeaderValueCollection *iface )
{
    return CONTAINING_RECORD( iface, struct http_headers, IHttpMediaTypeWithQualityHeaderValueCollection_iface );
}
static inline struct http_headers *headers_from_user_agent( IHttpProductInfoHeaderValueCollection *iface )
{
    return CONTAINING_RECORD( iface, struct http_headers, IHttpProductInfoHeaderValueCollection_iface );
}

void http_headers_addref( struct http_headers *headers )
{
    InterlockedIncrement( &headers->ref );
}

void http_headers_release( struct http_headers *headers )
{
    SIZE_T i;
    if (InterlockedDecrement( &headers->ref )) return;
    if (headers->authorization) IHttpCredentialsHeaderValue_Release( headers->authorization );
    if (headers->referer) IUriRuntimeClass_Release( headers->referer );
    for (i = 0; i < headers->count; ++i)
    {
        WindowsDeleteString( headers->pairs[i].name );
        WindowsDeleteString( headers->pairs[i].value );
    }
    free( headers->pairs );
    headers->cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &headers->cs );
    free( headers );
}

static HRESULT headers_query_interface( struct http_headers *headers, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ))
    {
        if (headers->kind == HTTP_HEADERS_REQUEST) *out = &headers->IHttpRequestHeaderCollection_iface;
        else if (headers->kind == HTTP_HEADERS_CONTENT) *out = &headers->IHttpContentHeaderCollection_iface;
        else *out = &headers->IHttpResponseHeaderCollection_iface;
    }
    else if (headers->kind == HTTP_HEADERS_REQUEST && IsEqualGUID( iid, &IID_IHttpRequestHeaderCollection ))
        *out = &headers->IHttpRequestHeaderCollection_iface;
    else if (headers->kind == HTTP_HEADERS_CONTENT && IsEqualGUID( iid, &IID_IHttpContentHeaderCollection ))
        *out = &headers->IHttpContentHeaderCollection_iface;
    else if (headers->kind == HTTP_HEADERS_RESPONSE && IsEqualGUID( iid, &IID_IHttpResponseHeaderCollection ))
        *out = &headers->IHttpResponseHeaderCollection_iface;
    else if (headers->kind == HTTP_HEADERS_REQUEST &&
             IsEqualGUID( iid, &IID_IHttpMediaTypeWithQualityHeaderValueCollection ))
        *out = &headers->IHttpMediaTypeWithQualityHeaderValueCollection_iface;
    else if (headers->kind == HTTP_HEADERS_REQUEST &&
             IsEqualGUID( iid, &IID_IHttpProductInfoHeaderValueCollection ))
        *out = &headers->IHttpProductInfoHeaderValueCollection_iface;
    else return E_NOINTERFACE;
    http_headers_addref( headers );
    return S_OK;
}

static ULONG headers_addref_iface( struct http_headers *headers )
{
    return InterlockedIncrement( &headers->ref );
}

static ULONG headers_release_iface( struct http_headers *headers )
{
    ULONG ref = InterlockedDecrement( &headers->ref );
    if (!ref)
    {
        InterlockedIncrement( &headers->ref );
        http_headers_release( headers );
    }
    return ref;
}

static HRESULT headers_get_iids( ULONG *count, IID **iids, const IID *iid )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(**iids) ))) return E_OUTOFMEMORY;
    **iids = *iid;
    *count = 1;
    return S_OK;
}

static HRESULT headers_get_name( enum http_headers_kind kind, HSTRING *name )
{
    const WCHAR *value;
    if (!name) return E_POINTER;
    if (kind == HTTP_HEADERS_REQUEST) value = L"Windows.Web.Http.Headers.HttpRequestHeaderCollection";
    else if (kind == HTTP_HEADERS_CONTENT) value = L"Windows.Web.Http.Headers.HttpContentHeaderCollection";
    else value = L"Windows.Web.Http.Headers.HttpResponseHeaderCollection";
    return WindowsCreateString( value, wcslen( value ), name );
}

static HRESULT headers_get_trust( TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static BOOL valid_header_string( HSTRING value )
{
    UINT32 length, i;
    const WCHAR *buffer = WindowsGetStringRawBuffer( value, &length );
    if (!buffer) return FALSE;
    for (i = 0; i < length; ++i) if (buffer[i] == '\r' || buffer[i] == '\n') return FALSE;
    return TRUE;
}

HRESULT http_headers_append( struct http_headers *headers, HSTRING name, HSTRING value )
{
    struct header_pair *pairs;
    HSTRING name_copy = NULL, value_copy = NULL;
    HRESULT hr;

    if (!headers || !valid_header_string( name ) || !valid_header_string( value ) ||
        !WindowsGetStringLen( name )) return E_INVALIDARG;
    if (FAILED(hr = WindowsDuplicateString( name, &name_copy ))) return hr;
    if (FAILED(hr = WindowsDuplicateString( value, &value_copy )))
    {
        WindowsDeleteString( name_copy );
        return hr;
    }

    EnterCriticalSection( &headers->cs );
    if (headers->count == headers->capacity)
    {
        SIZE_T capacity = headers->capacity ? headers->capacity * 2 : 8;
        if (!(pairs = realloc( headers->pairs, capacity * sizeof(*pairs) )))
        {
            LeaveCriticalSection( &headers->cs );
            WindowsDeleteString( name_copy );
            WindowsDeleteString( value_copy );
            return E_OUTOFMEMORY;
        }
        headers->pairs = pairs;
        headers->capacity = capacity;
    }
    headers->pairs[headers->count].name = name_copy;
    headers->pairs[headers->count].value = value_copy;
    ++headers->count;
    LeaveCriticalSection( &headers->cs );
    return S_OK;
}

static HRESULT append_literal( struct http_headers *headers, const WCHAR *name, HSTRING value )
{
    HSTRING name_string;
    HRESULT hr;
    if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &name_string ))) return hr;
    hr = http_headers_append( headers, name_string, value );
    WindowsDeleteString( name_string );
    return hr;
}

static HRESULT append_buffer( WCHAR **buffer, SIZE_T *length, SIZE_T *capacity,
                              const WCHAR *value, SIZE_T value_length )
{
    WCHAR *new_buffer;
    SIZE_T required;
    if (value_length > (SIZE_MAX / sizeof(WCHAR)) - *length - 1) return E_OUTOFMEMORY;
    required = *length + value_length + 1;
    if (required > *capacity)
    {
        SIZE_T new_capacity = *capacity ? *capacity : 128;
        while (new_capacity < required)
        {
            if (new_capacity > SIZE_MAX / 2) return E_OUTOFMEMORY;
            new_capacity *= 2;
        }
        if (!(new_buffer = realloc( *buffer, new_capacity * sizeof(*new_buffer) ))) return E_OUTOFMEMORY;
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    memcpy( *buffer + *length, value, value_length * sizeof(WCHAR) );
    *length += value_length;
    (*buffer)[*length] = 0;
    return S_OK;
}

HRESULT http_headers_build( struct http_headers *headers, WCHAR **value )
{
    WCHAR *buffer = NULL;
    SIZE_T length = 0, capacity = 0, i;
    HRESULT hr = S_OK;

    if (!headers || !value) return E_POINTER;
    *value = NULL;
    EnterCriticalSection( &headers->cs );
    for (i = 0; i < headers->count && SUCCEEDED(hr); ++i)
    {
        UINT32 name_length, value_length;
        const WCHAR *name = WindowsGetStringRawBuffer( headers->pairs[i].name, &name_length );
        const WCHAR *pair_value = WindowsGetStringRawBuffer( headers->pairs[i].value, &value_length );
        if (FAILED(hr = append_buffer( &buffer, &length, &capacity, name, name_length )) ||
            FAILED(hr = append_buffer( &buffer, &length, &capacity, L": ", 2 )) ||
            FAILED(hr = append_buffer( &buffer, &length, &capacity, pair_value, value_length )) ||
            FAILED(hr = append_buffer( &buffer, &length, &capacity, L"\r\n", 2 ))) break;
    }
    if (SUCCEEDED(hr) && headers->authorization)
    {
        HSTRING scheme = NULL, token = NULL;
        UINT32 scheme_length, token_length;
        const WCHAR *scheme_value, *token_value;
        if (SUCCEEDED(hr = IHttpCredentialsHeaderValue_get_Scheme( headers->authorization, &scheme )) &&
            SUCCEEDED(hr = IHttpCredentialsHeaderValue_get_Token( headers->authorization, &token )))
        {
            scheme_value = WindowsGetStringRawBuffer( scheme, &scheme_length );
            token_value = WindowsGetStringRawBuffer( token, &token_length );
            if (FAILED(hr = append_buffer( &buffer, &length, &capacity, L"Authorization: ", 15 )) ||
                FAILED(hr = append_buffer( &buffer, &length, &capacity, scheme_value, scheme_length )) ||
                (token_length && FAILED(hr = append_buffer( &buffer, &length, &capacity, L" ", 1 ))) ||
                (token_length && FAILED(hr = append_buffer( &buffer, &length, &capacity, token_value, token_length ))) ||
                FAILED(hr = append_buffer( &buffer, &length, &capacity, L"\r\n", 2 ))) {}
        }
        WindowsDeleteString( scheme );
        WindowsDeleteString( token );
    }
    if (SUCCEEDED(hr) && headers->referer)
    {
        HSTRING uri = NULL;
        UINT32 uri_length;
        const WCHAR *uri_value;
        if (SUCCEEDED(hr = IUriRuntimeClass_get_AbsoluteUri( headers->referer, &uri )))
        {
            uri_value = WindowsGetStringRawBuffer( uri, &uri_length );
            if (FAILED(hr = append_buffer( &buffer, &length, &capacity, L"Referer: ", 9 )) ||
                FAILED(hr = append_buffer( &buffer, &length, &capacity, uri_value, uri_length )) ||
                FAILED(hr = append_buffer( &buffer, &length, &capacity, L"\r\n", 2 ))) {}
        }
        WindowsDeleteString( uri );
    }
    LeaveCriticalSection( &headers->cs );
    if (FAILED(hr))
    {
        free( buffer );
        return hr;
    }
    if (!buffer && !(buffer = calloc( 1, sizeof(*buffer) ))) return E_OUTOFMEMORY;
    *value = buffer;
    return S_OK;
}

IHttpRequestHeaderCollection *http_headers_request_iface( struct http_headers *headers )
{
    return &headers->IHttpRequestHeaderCollection_iface;
}
IHttpContentHeaderCollection *http_headers_content_iface( struct http_headers *headers )
{
    return &headers->IHttpContentHeaderCollection_iface;
}
IHttpResponseHeaderCollection *http_headers_response_iface( struct http_headers *headers )
{
    return &headers->IHttpResponseHeaderCollection_iface;
}

#define DEFINE_REQUEST_BASE(name) \
static HRESULT WINAPI request_##name##_QueryInterface( IHttpRequestHeaderCollection *iface, REFIID iid, void **out ) \
{ return headers_query_interface( headers_from_request( iface ), iid, out ); } \
static ULONG WINAPI request_##name##_AddRef( IHttpRequestHeaderCollection *iface ) \
{ return headers_addref_iface( headers_from_request( iface ) ); } \
static ULONG WINAPI request_##name##_Release( IHttpRequestHeaderCollection *iface ) \
{ return headers_release_iface( headers_from_request( iface ) ); }
DEFINE_REQUEST_BASE(headers)

static HRESULT WINAPI request_headers_GetIids( IHttpRequestHeaderCollection *iface, ULONG *count, IID **iids )
{ return headers_get_iids( count, iids, &IID_IHttpRequestHeaderCollection ); }
static HRESULT WINAPI request_headers_GetRuntimeClassName( IHttpRequestHeaderCollection *iface, HSTRING *name )
{ return headers_get_name( HTTP_HEADERS_REQUEST, name ); }
static HRESULT WINAPI request_headers_GetTrustLevel( IHttpRequestHeaderCollection *iface, TrustLevel *level )
{ return headers_get_trust( level ); }
static HRESULT WINAPI request_headers_get_Accept( IHttpRequestHeaderCollection *iface,
        IHttpMediaTypeWithQualityHeaderValueCollection **value )
{
    struct http_headers *headers = headers_from_request( iface );
    if (!value) return E_POINTER;
    *value = &headers->IHttpMediaTypeWithQualityHeaderValueCollection_iface;
    http_headers_addref( headers );
    return S_OK;
}
#define REQUEST_GET_UNSUPPORTED(name) \
static HRESULT WINAPI request_headers_get_##name( IHttpRequestHeaderCollection *iface, IInspectable **value ) \
{ if (!value) return E_POINTER; *value = NULL; return E_NOTIMPL; }
REQUEST_GET_UNSUPPORTED(AcceptEncoding)
REQUEST_GET_UNSUPPORTED(AcceptLanguage)
static HRESULT WINAPI request_headers_get_Authorization( IHttpRequestHeaderCollection *iface,
        IHttpCredentialsHeaderValue **value )
{
    struct http_headers *headers = headers_from_request( iface );
    if (!value) return E_POINTER;
    EnterCriticalSection( &headers->cs );
    if ((*value = headers->authorization)) IHttpCredentialsHeaderValue_AddRef( *value );
    LeaveCriticalSection( &headers->cs );
    return S_OK;
}
static HRESULT WINAPI request_headers_put_Authorization( IHttpRequestHeaderCollection *iface,
        IHttpCredentialsHeaderValue *value )
{
    struct http_headers *headers = headers_from_request( iface );
    IHttpCredentialsHeaderValue *old;
    if (value) IHttpCredentialsHeaderValue_AddRef( value );
    EnterCriticalSection( &headers->cs );
    old = headers->authorization;
    headers->authorization = value;
    LeaveCriticalSection( &headers->cs );
    if (old) IHttpCredentialsHeaderValue_Release( old );
    return S_OK;
}
REQUEST_GET_UNSUPPORTED(CacheControl)
REQUEST_GET_UNSUPPORTED(Connection)
REQUEST_GET_UNSUPPORTED(Cookie)
static HRESULT WINAPI request_headers_get_Date( IHttpRequestHeaderCollection *iface, IReference_DateTime **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_Date( IHttpRequestHeaderCollection *iface, IReference_DateTime *value )
{ return E_NOTIMPL; }
REQUEST_GET_UNSUPPORTED(Expect)
static HRESULT WINAPI request_headers_get_From( IHttpRequestHeaderCollection *iface, HSTRING *value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_From( IHttpRequestHeaderCollection *iface, HSTRING value )
{ return append_literal( headers_from_request( iface ), L"From", value ); }
REQUEST_GET_UNSUPPORTED(Host)
static HRESULT WINAPI request_headers_put_Host( IHttpRequestHeaderCollection *iface, IInspectable *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI request_headers_get_IfModifiedSince( IHttpRequestHeaderCollection *iface, IReference_DateTime **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_IfModifiedSince( IHttpRequestHeaderCollection *iface, IReference_DateTime *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI request_headers_get_IfUnmodifiedSince( IHttpRequestHeaderCollection *iface, IReference_DateTime **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_IfUnmodifiedSince( IHttpRequestHeaderCollection *iface, IReference_DateTime *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI request_headers_get_MaxForwards( IHttpRequestHeaderCollection *iface, IReference_UINT32 **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_MaxForwards( IHttpRequestHeaderCollection *iface, IReference_UINT32 *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI request_headers_get_ProxyAuthorization( IHttpRequestHeaderCollection *iface,
        IHttpCredentialsHeaderValue **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI request_headers_put_ProxyAuthorization( IHttpRequestHeaderCollection *iface,
        IHttpCredentialsHeaderValue *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI request_headers_get_Referer( IHttpRequestHeaderCollection *iface, IUriRuntimeClass **value )
{
    struct http_headers *headers = headers_from_request( iface );
    if (!value) return E_POINTER;
    EnterCriticalSection( &headers->cs );
    if ((*value = headers->referer)) IUriRuntimeClass_AddRef( *value );
    LeaveCriticalSection( &headers->cs );
    return S_OK;
}
static HRESULT WINAPI request_headers_put_Referer( IHttpRequestHeaderCollection *iface, IUriRuntimeClass *value )
{
    struct http_headers *headers = headers_from_request( iface );
    IUriRuntimeClass *old;
    if (value) IUriRuntimeClass_AddRef( value );
    EnterCriticalSection( &headers->cs );
    old = headers->referer;
    headers->referer = value;
    LeaveCriticalSection( &headers->cs );
    if (old) IUriRuntimeClass_Release( old );
    return S_OK;
}
REQUEST_GET_UNSUPPORTED(TransferEncoding)
static HRESULT WINAPI request_headers_get_UserAgent( IHttpRequestHeaderCollection *iface,
        IHttpProductInfoHeaderValueCollection **value )
{
    struct http_headers *headers = headers_from_request( iface );
    if (!value) return E_POINTER;
    *value = &headers->IHttpProductInfoHeaderValueCollection_iface;
    http_headers_addref( headers );
    return S_OK;
}
static HRESULT WINAPI request_headers_Append( IHttpRequestHeaderCollection *iface, HSTRING name, HSTRING value )
{ return http_headers_append( headers_from_request( iface ), name, value ); }
static HRESULT WINAPI request_headers_TryAppendWithoutValidation( IHttpRequestHeaderCollection *iface,
        HSTRING name, HSTRING value, boolean *result )
{
    HRESULT hr;
    if (!result) return E_POINTER;
    hr = http_headers_append( headers_from_request( iface ), name, value );
    *result = SUCCEEDED(hr);
    return SUCCEEDED(hr) || hr == E_INVALIDARG ? S_OK : hr;
}

static const IHttpRequestHeaderCollectionVtbl request_headers_vtbl =
{
    request_headers_QueryInterface, request_headers_AddRef, request_headers_Release,
    request_headers_GetIids, request_headers_GetRuntimeClassName, request_headers_GetTrustLevel,
    request_headers_get_Accept,
    (void *)request_headers_get_AcceptEncoding, (void *)request_headers_get_AcceptLanguage,
    request_headers_get_Authorization, request_headers_put_Authorization,
    (void *)request_headers_get_CacheControl, (void *)request_headers_get_Connection,
    (void *)request_headers_get_Cookie,
    request_headers_get_Date, request_headers_put_Date,
    (void *)request_headers_get_Expect,
    request_headers_get_From, request_headers_put_From,
    (void *)request_headers_get_Host, (void *)request_headers_put_Host,
    request_headers_get_IfModifiedSince, request_headers_put_IfModifiedSince,
    request_headers_get_IfUnmodifiedSince, request_headers_put_IfUnmodifiedSince,
    request_headers_get_MaxForwards, request_headers_put_MaxForwards,
    request_headers_get_ProxyAuthorization, request_headers_put_ProxyAuthorization,
    request_headers_get_Referer, request_headers_put_Referer,
    (void *)request_headers_get_TransferEncoding,
    request_headers_get_UserAgent,
    request_headers_Append, request_headers_TryAppendWithoutValidation,
};

#define DEFINE_PARSE_COLLECTION(prefix, iface_type, from, header_name, iid_value, class_name) \
static HRESULT WINAPI prefix##_QueryInterface( iface_type *iface, REFIID iid, void **out ) \
{ return headers_query_interface( from( iface ), iid, out ); } \
static ULONG WINAPI prefix##_AddRef( iface_type *iface ) { return headers_addref_iface( from( iface ) ); } \
static ULONG WINAPI prefix##_Release( iface_type *iface ) { return headers_release_iface( from( iface ) ); } \
static HRESULT WINAPI prefix##_GetIids( iface_type *iface, ULONG *count, IID **iids ) \
{ return headers_get_iids( count, iids, iid_value ); } \
static HRESULT WINAPI prefix##_GetRuntimeClassName( iface_type *iface, HSTRING *name ) \
{ return WindowsCreateString( class_name, wcslen( class_name ), name ); } \
static HRESULT WINAPI prefix##_GetTrustLevel( iface_type *iface, TrustLevel *level ) \
{ return headers_get_trust( level ); } \
static HRESULT WINAPI prefix##_ParseAdd( iface_type *iface, HSTRING input ) \
{ return append_literal( from( iface ), header_name, input ); } \
static HRESULT WINAPI prefix##_TryParseAdd( iface_type *iface, HSTRING input, boolean *result ) \
{ HRESULT hr; if (!result) return E_POINTER; hr = prefix##_ParseAdd( iface, input ); *result = SUCCEEDED(hr); \
  return SUCCEEDED(hr) || hr == E_INVALIDARG ? S_OK : hr; }

DEFINE_PARSE_COLLECTION(accept, IHttpMediaTypeWithQualityHeaderValueCollection, headers_from_accept,
                        L"Accept", &IID_IHttpMediaTypeWithQualityHeaderValueCollection,
                        L"Windows.Web.Http.Headers.HttpMediaTypeWithQualityHeaderValueCollection")
static const IHttpMediaTypeWithQualityHeaderValueCollectionVtbl accept_vtbl =
{
    accept_QueryInterface, accept_AddRef, accept_Release, accept_GetIids,
    accept_GetRuntimeClassName, accept_GetTrustLevel, accept_ParseAdd, accept_TryParseAdd,
};

DEFINE_PARSE_COLLECTION(user_agent, IHttpProductInfoHeaderValueCollection, headers_from_user_agent,
                        L"User-Agent", &IID_IHttpProductInfoHeaderValueCollection,
                        L"Windows.Web.Http.Headers.HttpProductInfoHeaderValueCollection")
static const IHttpProductInfoHeaderValueCollectionVtbl user_agent_vtbl =
{
    user_agent_QueryInterface, user_agent_AddRef, user_agent_Release, user_agent_GetIids,
    user_agent_GetRuntimeClassName, user_agent_GetTrustLevel,
    user_agent_ParseAdd, user_agent_TryParseAdd,
};

#define DEFINE_SIMPLE_HEADER_BASE(prefix, iface_type, from, iid_value, kind_value) \
static HRESULT WINAPI prefix##_QueryInterface( iface_type *iface, REFIID iid, void **out ) \
{ return headers_query_interface( from( iface ), iid, out ); } \
static ULONG WINAPI prefix##_AddRef( iface_type *iface ) { return headers_addref_iface( from( iface ) ); } \
static ULONG WINAPI prefix##_Release( iface_type *iface ) { return headers_release_iface( from( iface ) ); } \
static HRESULT WINAPI prefix##_GetIids( iface_type *iface, ULONG *count, IID **iids ) \
{ return headers_get_iids( count, iids, iid_value ); } \
static HRESULT WINAPI prefix##_GetRuntimeClassName( iface_type *iface, HSTRING *name ) \
{ return headers_get_name( kind_value, name ); } \
static HRESULT WINAPI prefix##_GetTrustLevel( iface_type *iface, TrustLevel *level ) \
{ return headers_get_trust( level ); }

DEFINE_SIMPLE_HEADER_BASE(content_headers, IHttpContentHeaderCollection, headers_from_content,
                          &IID_IHttpContentHeaderCollection, HTTP_HEADERS_CONTENT)
static HRESULT WINAPI content_get_object( IHttpContentHeaderCollection *iface, IInspectable **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI content_put_object( IHttpContentHeaderCollection *iface, IInspectable *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI content_get_u64( IHttpContentHeaderCollection *iface, IReference_UINT64 **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI content_put_u64( IHttpContentHeaderCollection *iface, IReference_UINT64 *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI content_get_uri( IHttpContentHeaderCollection *iface, IUriRuntimeClass **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI content_put_uri( IHttpContentHeaderCollection *iface, IUriRuntimeClass *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI content_get_buffer( IHttpContentHeaderCollection *iface, IBuffer **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI content_put_buffer( IHttpContentHeaderCollection *iface, IBuffer *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI content_get_date( IHttpContentHeaderCollection *iface, IReference_DateTime **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI content_put_date( IHttpContentHeaderCollection *iface, IReference_DateTime *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI content_headers_Append( IHttpContentHeaderCollection *iface, HSTRING name, HSTRING value )
{ return http_headers_append( headers_from_content( iface ), name, value ); }
static HRESULT WINAPI content_headers_TryAppendWithoutValidation( IHttpContentHeaderCollection *iface,
        HSTRING name, HSTRING value, boolean *result )
{
    HRESULT hr;
    if (!result) return E_POINTER;
    hr = content_headers_Append( iface, name, value );
    *result = SUCCEEDED(hr);
    return SUCCEEDED(hr) || hr == E_INVALIDARG ? S_OK : hr;
}
static const IHttpContentHeaderCollectionVtbl content_headers_vtbl =
{
    content_headers_QueryInterface, content_headers_AddRef, content_headers_Release,
    content_headers_GetIids, content_headers_GetRuntimeClassName, content_headers_GetTrustLevel,
    (void *)content_get_object, (void *)content_put_object,
    (void *)content_get_object, (void *)content_get_object,
    content_get_u64, content_put_u64,
    content_get_uri, content_put_uri,
    content_get_buffer, content_put_buffer,
    (void *)content_get_object, (void *)content_put_object,
    (void *)content_get_object, (void *)content_put_object,
    content_get_date, content_put_date,
    content_get_date, content_put_date,
    content_headers_Append, content_headers_TryAppendWithoutValidation,
};

DEFINE_SIMPLE_HEADER_BASE(response_headers, IHttpResponseHeaderCollection, headers_from_response,
                          &IID_IHttpResponseHeaderCollection, HTTP_HEADERS_RESPONSE)
static HRESULT WINAPI response_get_object( IHttpResponseHeaderCollection *iface, IInspectable **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI response_put_object( IHttpResponseHeaderCollection *iface, IInspectable *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI response_get_date( IHttpResponseHeaderCollection *iface, IReference_DateTime **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI response_put_date( IHttpResponseHeaderCollection *iface, IReference_DateTime *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI response_get_time( IHttpResponseHeaderCollection *iface, IReference_TimeSpan **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI response_put_time( IHttpResponseHeaderCollection *iface, IReference_TimeSpan *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI response_get_uri( IHttpResponseHeaderCollection *iface, IUriRuntimeClass **value )
{ if (!value) return E_POINTER; *value = NULL; return S_OK; }
static HRESULT WINAPI response_put_uri( IHttpResponseHeaderCollection *iface, IUriRuntimeClass *value )
{ return E_NOTIMPL; }
static HRESULT WINAPI response_headers_Append( IHttpResponseHeaderCollection *iface, HSTRING name, HSTRING value )
{ return http_headers_append( headers_from_response( iface ), name, value ); }
static HRESULT WINAPI response_headers_TryAppendWithoutValidation( IHttpResponseHeaderCollection *iface,
        HSTRING name, HSTRING value, boolean *result )
{
    HRESULT hr;
    if (!result) return E_POINTER;
    hr = response_headers_Append( iface, name, value );
    *result = SUCCEEDED(hr);
    return SUCCEEDED(hr) || hr == E_INVALIDARG ? S_OK : hr;
}
static const IHttpResponseHeaderCollectionVtbl response_headers_vtbl =
{
    response_headers_QueryInterface, response_headers_AddRef, response_headers_Release,
    response_headers_GetIids, response_headers_GetRuntimeClassName, response_headers_GetTrustLevel,
    response_get_time, response_put_time,
    (void *)response_get_object, (void *)response_get_object, (void *)response_get_object,
    response_get_date, response_put_date,
    response_get_uri, response_put_uri,
    (void *)response_get_object, (void *)response_get_object, (void *)response_put_object,
    (void *)response_get_object, (void *)response_get_object,
    response_headers_Append, response_headers_TryAppendWithoutValidation,
};

HRESULT http_headers_create( enum http_headers_kind kind, struct http_headers **out )
{
    struct http_headers *headers;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(headers = calloc( 1, sizeof(*headers) ))) return E_OUTOFMEMORY;
    headers->IHttpRequestHeaderCollection_iface.lpVtbl = &request_headers_vtbl;
    headers->IHttpContentHeaderCollection_iface.lpVtbl = &content_headers_vtbl;
    headers->IHttpResponseHeaderCollection_iface.lpVtbl = &response_headers_vtbl;
    headers->IHttpMediaTypeWithQualityHeaderValueCollection_iface.lpVtbl = &accept_vtbl;
    headers->IHttpProductInfoHeaderValueCollection_iface.lpVtbl = &user_agent_vtbl;
    headers->ref = 1;
    headers->kind = kind;
    InitializeCriticalSectionEx( &headers->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    headers->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": http_headers.cs");
    *out = headers;
    return S_OK;
}

struct credentials_value
{
    IHttpCredentialsHeaderValue IHttpCredentialsHeaderValue_iface;
    LONG ref;
    HSTRING scheme;
    HSTRING token;
};

static inline struct credentials_value *credentials_from_iface( IHttpCredentialsHeaderValue *iface )
{ return CONTAINING_RECORD( iface, struct credentials_value, IHttpCredentialsHeaderValue_iface ); }
static HRESULT WINAPI credentials_QueryInterface( IHttpCredentialsHeaderValue *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (!IsEqualGUID( iid, &IID_IUnknown ) && !IsEqualGUID( iid, &IID_IInspectable ) &&
        !IsEqualGUID( iid, &IID_IAgileObject ) && !IsEqualGUID( iid, &IID_IHttpCredentialsHeaderValue ))
        return E_NOINTERFACE;
    *out = iface;
    IHttpCredentialsHeaderValue_AddRef( iface );
    return S_OK;
}
static ULONG WINAPI credentials_AddRef( IHttpCredentialsHeaderValue *iface )
{ return InterlockedIncrement( &credentials_from_iface( iface )->ref ); }
static ULONG WINAPI credentials_Release( IHttpCredentialsHeaderValue *iface )
{
    struct credentials_value *value = credentials_from_iface( iface );
    ULONG ref = InterlockedDecrement( &value->ref );
    if (!ref)
    {
        WindowsDeleteString( value->scheme );
        WindowsDeleteString( value->token );
        free( value );
    }
    return ref;
}
static HRESULT WINAPI credentials_GetIids( IHttpCredentialsHeaderValue *iface, ULONG *count, IID **iids )
{ return headers_get_iids( count, iids, &IID_IHttpCredentialsHeaderValue ); }
static HRESULT WINAPI credentials_GetRuntimeClassName( IHttpCredentialsHeaderValue *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Web_Http_Headers_HttpCredentialsHeaderValue,
            wcslen( RuntimeClass_Windows_Web_Http_Headers_HttpCredentialsHeaderValue ), name );
}
static HRESULT WINAPI credentials_GetTrustLevel( IHttpCredentialsHeaderValue *iface, TrustLevel *level )
{ return headers_get_trust( level ); }
static HRESULT WINAPI credentials_get_Parameters( IHttpCredentialsHeaderValue *iface, IInspectable **value )
{ if (!value) return E_POINTER; *value = NULL; return E_NOTIMPL; }
static HRESULT WINAPI credentials_get_Scheme( IHttpCredentialsHeaderValue *iface, HSTRING *value )
{ if (!value) return E_POINTER; return WindowsDuplicateString( credentials_from_iface( iface )->scheme, value ); }
static HRESULT WINAPI credentials_get_Token( IHttpCredentialsHeaderValue *iface, HSTRING *value )
{ if (!value) return E_POINTER; return WindowsDuplicateString( credentials_from_iface( iface )->token, value ); }
static const IHttpCredentialsHeaderValueVtbl credentials_vtbl =
{
    credentials_QueryInterface, credentials_AddRef, credentials_Release, credentials_GetIids,
    credentials_GetRuntimeClassName, credentials_GetTrustLevel, credentials_get_Parameters,
    credentials_get_Scheme, credentials_get_Token,
};

static HRESULT credentials_create( HSTRING scheme, HSTRING token, IHttpCredentialsHeaderValue **out )
{
    struct credentials_value *value;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!WindowsGetStringLen( scheme ) || !valid_header_string( scheme ) || !valid_header_string( token ))
        return E_INVALIDARG;
    if (!(value = calloc( 1, sizeof(*value) ))) return E_OUTOFMEMORY;
    value->IHttpCredentialsHeaderValue_iface.lpVtbl = &credentials_vtbl;
    value->ref = 1;
    if (FAILED(hr = WindowsDuplicateString( scheme, &value->scheme )) ||
        FAILED(hr = WindowsDuplicateString( token, &value->token )))
    {
        WindowsDeleteString( value->scheme );
        free( value );
        return hr;
    }
    *out = &value->IHttpCredentialsHeaderValue_iface;
    return S_OK;
}

struct credentials_factory
{
    IActivationFactory IActivationFactory_iface;
    IHttpCredentialsHeaderValueFactory IHttpCredentialsHeaderValueFactory_iface;
};
static struct credentials_factory credentials_factory;
static HRESULT WINAPI credentials_factory_qi( IActivationFactory *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &credentials_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IHttpCredentialsHeaderValueFactory ))
        *out = &credentials_factory.IHttpCredentialsHeaderValueFactory_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}
static ULONG WINAPI credentials_factory_addref( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI credentials_factory_release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI credentials_factory_iids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI credentials_factory_name( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI credentials_factory_trust( IActivationFactory *iface, TrustLevel *level )
{ return headers_get_trust( level ); }
static HRESULT WINAPI credentials_factory_activate( IActivationFactory *iface, IInspectable **out )
{ if (out) *out = NULL; return E_NOTIMPL; }
static const IActivationFactoryVtbl credentials_activation_vtbl =
{
    credentials_factory_qi, credentials_factory_addref, credentials_factory_release,
    credentials_factory_iids, credentials_factory_name, credentials_factory_trust,
    credentials_factory_activate,
};
static inline struct credentials_factory *credentials_factory_from_iface( IHttpCredentialsHeaderValueFactory *iface )
{ return CONTAINING_RECORD( iface, struct credentials_factory, IHttpCredentialsHeaderValueFactory_iface ); }
static HRESULT WINAPI credentials_value_factory_qi( IHttpCredentialsHeaderValueFactory *iface, REFIID iid, void **out )
{ return credentials_factory_qi( &credentials_factory_from_iface( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI credentials_value_factory_addref( IHttpCredentialsHeaderValueFactory *iface ) { return 2; }
static ULONG WINAPI credentials_value_factory_release( IHttpCredentialsHeaderValueFactory *iface ) { return 1; }
static HRESULT WINAPI credentials_value_factory_iids( IHttpCredentialsHeaderValueFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI credentials_value_factory_name( IHttpCredentialsHeaderValueFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI credentials_value_factory_trust( IHttpCredentialsHeaderValueFactory *iface, TrustLevel *level )
{ return headers_get_trust( level ); }
static HRESULT WINAPI credentials_CreateFromScheme( IHttpCredentialsHeaderValueFactory *iface,
        HSTRING scheme, IHttpCredentialsHeaderValue **out )
{ return credentials_create( scheme, NULL, out ); }
static HRESULT WINAPI credentials_CreateFromSchemeWithToken( IHttpCredentialsHeaderValueFactory *iface,
        HSTRING scheme, HSTRING token, IHttpCredentialsHeaderValue **out )
{ return credentials_create( scheme, token, out ); }
static const IHttpCredentialsHeaderValueFactoryVtbl credentials_value_factory_vtbl =
{
    credentials_value_factory_qi, credentials_value_factory_addref, credentials_value_factory_release,
    credentials_value_factory_iids, credentials_value_factory_name, credentials_value_factory_trust,
    credentials_CreateFromScheme, credentials_CreateFromSchemeWithToken,
};
static struct credentials_factory credentials_factory =
{
    {&credentials_activation_vtbl},
    {&credentials_value_factory_vtbl},
};
IActivationFactory *http_credentials_factory = &credentials_factory.IActivationFactory_iface;
