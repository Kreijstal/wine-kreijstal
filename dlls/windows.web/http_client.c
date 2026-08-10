/* Windows.Web.Http.HttpClient implementation using WinHTTP.
 *
 * Copyright 2026 OpenTerminal contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "private.h"

#include "winhttp.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(web);

struct http_client
{
    IHttpClient IHttpClient_iface;
    IClosable IClosable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    struct http_headers *headers;
    BOOL closed;
};

static inline struct http_client *client_from_iface( IHttpClient *iface )
{ return CONTAINING_RECORD( iface, struct http_client, IHttpClient_iface ); }
static inline struct http_client *client_from_closable( IClosable *iface )
{ return CONTAINING_RECORD( iface, struct http_client, IClosable_iface ); }

static HRESULT WINAPI client_QueryInterface( IHttpClient *iface, REFIID iid, void **out )
{
    struct http_client *client = client_from_iface( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHttpClient ))
        *out = &client->IHttpClient_iface;
    else if (IsEqualGUID( iid, &IID_IClosable )) *out = &client->IClosable_iface;
    else return E_NOINTERFACE;
    IHttpClient_AddRef( &client->IHttpClient_iface );
    return S_OK;
}
static ULONG WINAPI client_AddRef( IHttpClient *iface )
{ return InterlockedIncrement( &client_from_iface( iface )->ref ); }
static ULONG WINAPI client_Release( IHttpClient *iface )
{
    struct http_client *client = client_from_iface( iface );
    ULONG ref = InterlockedDecrement( &client->ref );
    if (!ref)
    {
        http_headers_release( client->headers );
        client->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &client->cs );
        free( client );
    }
    return ref;
}
static HRESULT WINAPI client_GetIids( IHttpClient *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(**iids) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IHttpClient; (*iids)[1] = IID_IClosable; *count = 2; return S_OK;
}
static HRESULT WINAPI client_GetRuntimeClassName( IHttpClient *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( RuntimeClass_Windows_Web_Http_HttpClient,
            wcslen( RuntimeClass_Windows_Web_Http_HttpClient ), name );
}
static HRESULT WINAPI client_GetTrustLevel( IHttpClient *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }

struct request_context
{
    IHttpRequestMessage *request;
    struct http_headers *default_headers;
};

static void request_context_destroy( void *context )
{
    struct request_context *request = context;
    IHttpRequestMessage_Release( request->request );
    http_headers_release( request->default_headers );
    free( request );
}

static HRESULT append_wide( WCHAR **buffer, SIZE_T *length, SIZE_T *capacity,
                            const WCHAR *value, SIZE_T value_length )
{
    WCHAR *new_buffer;
    SIZE_T required;
    if (value_length > SIZE_MAX - *length - 1) return E_OUTOFMEMORY;
    required = *length + value_length + 1;
    if (required > *capacity)
    {
        SIZE_T new_capacity = *capacity ? *capacity : 256;
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

static HRESULT combine_headers( WCHAR *first, WCHAR *second, HSTRING media_type, WCHAR **result )
{
    WCHAR *buffer = NULL;
    SIZE_T length = 0, capacity = 0;
    HRESULT hr;
    UINT32 media_length;
    const WCHAR *media;
    if (!result) return E_POINTER;
    *result = NULL;
    if (FAILED(hr = append_wide( &buffer, &length, &capacity, first, wcslen( first ) )) ||
        FAILED(hr = append_wide( &buffer, &length, &capacity, second, wcslen( second ) ))) goto failed;
    media = WindowsGetStringRawBuffer( media_type, &media_length );
    if (media && media_length)
    {
        if (FAILED(hr = append_wide( &buffer, &length, &capacity, L"Content-Type: ", 14 )) ||
            FAILED(hr = append_wide( &buffer, &length, &capacity, media, media_length )) ||
            FAILED(hr = append_wide( &buffer, &length, &capacity, L"\r\n", 2 ))) goto failed;
    }
    if (!buffer && !(buffer = calloc( 1, sizeof(*buffer) ))) return E_OUTOFMEMORY;
    *result = buffer;
    return S_OK;
failed:
    free( buffer );
    return hr;
}

static HRESULT winhttp_error(void)
{ return HRESULT_FROM_WIN32( GetLastError() ); }

static HRESULT request_work( void *parameter, IHttpResponseMessage **result )
{
    struct request_context *context = parameter;
    IHttpMethod *method = NULL;
    IUriRuntimeClass *uri = NULL;
    IHttpContent *content = NULL;
    struct http_headers *request_headers = NULL;
    HSTRING method_string = NULL, uri_string = NULL, media_type = NULL;
    const WCHAR *method_value, *url_value;
    const BYTE *body = NULL;
    SIZE_T body_size = 0;
    URL_COMPONENTS components = {sizeof(components)};
    WCHAR *host = NULL, *path = NULL, *default_raw = NULL, *request_raw = NULL, *headers = NULL;
    HINTERNET session = NULL, connection = NULL, winhttp_request = NULL;
    BYTE *response = NULL;
    SIZE_T response_size = 0, response_capacity = 0;
    DWORD status, status_size = sizeof(status), available, read;
    HRESULT hr;

    *result = NULL;
    if (FAILED(hr = http_request_get_transport( context->request, &method, &uri, &content, &request_headers ))) goto done;
    if (!uri) { hr = E_INVALIDARG; goto done; }
    if (FAILED(hr = IHttpMethod_get_Method( method, &method_string )) ||
        FAILED(hr = IUriRuntimeClass_get_AbsoluteUri( uri, &uri_string ))) goto done;
    method_value = WindowsGetStringRawBuffer( method_string, NULL );
    url_value = WindowsGetStringRawBuffer( uri_string, NULL );
    components.dwSchemeLength = components.dwHostNameLength = components.dwUrlPathLength =
            components.dwExtraInfoLength = ~(DWORD)0;
    if (!WinHttpCrackUrl( url_value, 0, 0, &components )) { hr = winhttp_error(); goto done; }
    if (!(host = malloc( (components.dwHostNameLength + 1) * sizeof(*host) )) ||
        !(path = malloc( (components.dwUrlPathLength + components.dwExtraInfoLength + 2) * sizeof(*path) )))
    { hr = E_OUTOFMEMORY; goto done; }
    memcpy( host, components.lpszHostName, components.dwHostNameLength * sizeof(*host) );
    host[components.dwHostNameLength] = 0;
    memcpy( path, components.lpszUrlPath, components.dwUrlPathLength * sizeof(*path) );
    memcpy( path + components.dwUrlPathLength, components.lpszExtraInfo,
            components.dwExtraInfoLength * sizeof(*path) );
    path[components.dwUrlPathLength + components.dwExtraInfoLength] = 0;
    if (!*path) wcscpy( path, L"/" );

    if (content && FAILED(hr = http_content_get_bytes( content, &body, &body_size, &media_type ))) goto done;
    if (body_size > ~(DWORD)0) { hr = E_OUTOFMEMORY; goto done; }
    if (FAILED(hr = http_headers_build( context->default_headers, &default_raw )) ||
        FAILED(hr = http_headers_build( request_headers, &request_raw )) ||
        FAILED(hr = combine_headers( default_raw, request_raw, media_type, &headers ))) goto done;

    if (!(session = WinHttpOpen( L"Wine Windows.Web.Http/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 )) ||
        !(connection = WinHttpConnect( session, host, components.nPort, 0 )) ||
        !(winhttp_request = WinHttpOpenRequest( connection, method_value, path, NULL,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0 )))
    { hr = winhttp_error(); goto done; }
    if (!WinHttpSendRequest( winhttp_request, headers, -1, (void *)body, (DWORD)body_size, (DWORD)body_size, 0 ) ||
        !WinHttpReceiveResponse( winhttp_request, NULL ))
    { hr = winhttp_error(); goto done; }
    if (!WinHttpQueryHeaders( winhttp_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX ))
    { hr = winhttp_error(); goto done; }
    for (;;)
    {
        if (!WinHttpQueryDataAvailable( winhttp_request, &available )) { hr = winhttp_error(); goto done; }
        if (!available) break;
        if (response_size > SIZE_MAX - available) { hr = E_OUTOFMEMORY; goto done; }
        if (response_size + available > response_capacity)
        {
            BYTE *new_response;
            SIZE_T capacity = response_capacity ? response_capacity : 4096;
            while (capacity < response_size + available)
            {
                if (capacity > SIZE_MAX / 2) { hr = E_OUTOFMEMORY; goto done; }
                capacity *= 2;
            }
            if (!(new_response = realloc( response, capacity ))) { hr = E_OUTOFMEMORY; goto done; }
            response = new_response;
            response_capacity = capacity;
        }
        if (!WinHttpReadData( winhttp_request, response + response_size, available, &read ))
        { hr = winhttp_error(); goto done; }
        response_size += read;
        if (!read) break;
    }
    hr = http_response_create( status, response, response_size, context->request, result );

done:
    free( response );
    if (winhttp_request) WinHttpCloseHandle( winhttp_request );
    if (connection) WinHttpCloseHandle( connection );
    if (session) WinHttpCloseHandle( session );
    free( headers ); free( request_raw ); free( default_raw ); free( path ); free( host );
    WindowsDeleteString( media_type ); WindowsDeleteString( uri_string ); WindowsDeleteString( method_string );
    if (request_headers) http_headers_release( request_headers );
    if (content) IHttpContent_Release( content );
    if (uri) IUriRuntimeClass_Release( uri );
    if (method) IHttpMethod_Release( method );
    return hr;
}

static HRESULT send_request( struct http_client *client, IHttpRequestMessage *request,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{
    struct request_context *context;
    HRESULT hr;
    if (!request || !operation) return E_POINTER;
    *operation = NULL;
    EnterCriticalSection( &client->cs );
    if (client->closed) { LeaveCriticalSection( &client->cs ); return RO_E_CLOSED; }
    if (!(context = calloc( 1, sizeof(*context) ))) { LeaveCriticalSection( &client->cs ); return E_OUTOFMEMORY; }
    context->request = request;
    IHttpRequestMessage_AddRef( request );
    context->default_headers = client->headers;
    http_headers_addref( context->default_headers );
    LeaveCriticalSection( &client->cs );
    if (FAILED(hr = http_async_response_create( context, request_work, request_context_destroy, operation )))
        request_context_destroy( context );
    return hr;
}

static HRESULT request_from_uri( struct http_client *client, const WCHAR *method_name, IUriRuntimeClass *uri,
        IHttpContent *content, IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{
    IHttpMethod *method = NULL;
    IHttpRequestMessage *request = NULL;
    HRESULT hr;
    if (!uri || !operation) return E_POINTER;
    if (FAILED(hr = http_method_create_literal( method_name, &method )) ||
        FAILED(hr = http_request_create( method, uri, &request ))) goto done;
    if (content && FAILED(hr = IHttpRequestMessage_put_Content( request, content ))) goto done;
    hr = send_request( client, request, operation );
done:
    if (request) IHttpRequestMessage_Release( request );
    if (method) IHttpMethod_Release( method );
    return hr;
}

static HRESULT WINAPI client_DeleteAsync( IHttpClient *iface, IUriRuntimeClass *uri,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return request_from_uri( client_from_iface( iface ), L"DELETE", uri, NULL, operation ); }
static HRESULT WINAPI client_GetAsync( IHttpClient *iface, IUriRuntimeClass *uri,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return request_from_uri( client_from_iface( iface ), L"GET", uri, NULL, operation ); }
static HRESULT WINAPI client_GetWithOptionAsync( IHttpClient *iface, IUriRuntimeClass *uri,
        HttpCompletionOption option, IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return client_GetAsync( iface, uri, operation ); }
static HRESULT WINAPI client_GetBufferAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static HRESULT WINAPI client_GetInputStreamAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static HRESULT WINAPI client_GetStringAsync( IHttpClient *iface, IUriRuntimeClass *uri, IInspectable **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static HRESULT WINAPI client_PostAsync( IHttpClient *iface, IUriRuntimeClass *uri, IHttpContent *content,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return request_from_uri( client_from_iface( iface ), L"POST", uri, content, operation ); }
static HRESULT WINAPI client_PutAsync( IHttpClient *iface, IUriRuntimeClass *uri, IHttpContent *content,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return request_from_uri( client_from_iface( iface ), L"PUT", uri, content, operation ); }
static HRESULT WINAPI client_SendRequestAsync( IHttpClient *iface, IHttpRequestMessage *request,
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return send_request( client_from_iface( iface ), request, operation ); }
static HRESULT WINAPI client_SendRequestWithOptionAsync( IHttpClient *iface, IHttpRequestMessage *request,
        HttpCompletionOption option, IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **operation )
{ return client_SendRequestAsync( iface, request, operation ); }
static HRESULT WINAPI client_get_DefaultRequestHeaders( IHttpClient *iface, IHttpRequestHeaderCollection **value )
{
    struct http_client *client = client_from_iface( iface );
    if (!value) return E_POINTER;
    *value = http_headers_request_iface( client->headers );
    http_headers_addref( client->headers );
    return S_OK;
}
static const IHttpClientVtbl client_vtbl =
{
    client_QueryInterface, client_AddRef, client_Release, client_GetIids,
    client_GetRuntimeClassName, client_GetTrustLevel, client_DeleteAsync, client_GetAsync,
    client_GetWithOptionAsync, client_GetBufferAsync, client_GetInputStreamAsync,
    client_GetStringAsync, client_PostAsync, client_PutAsync, client_SendRequestAsync,
    client_SendRequestWithOptionAsync, client_get_DefaultRequestHeaders,
};

static HRESULT WINAPI client_closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{ return client_QueryInterface( &client_from_closable( iface )->IHttpClient_iface, iid, out ); }
static ULONG WINAPI client_closable_AddRef( IClosable *iface )
{ return client_AddRef( &client_from_closable( iface )->IHttpClient_iface ); }
static ULONG WINAPI client_closable_Release( IClosable *iface )
{ return client_Release( &client_from_closable( iface )->IHttpClient_iface ); }
static HRESULT WINAPI client_closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{ return client_GetIids( &client_from_closable( iface )->IHttpClient_iface, count, iids ); }
static HRESULT WINAPI client_closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{ return client_GetRuntimeClassName( &client_from_closable( iface )->IHttpClient_iface, name ); }
static HRESULT WINAPI client_closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{ return client_GetTrustLevel( &client_from_closable( iface )->IHttpClient_iface, level ); }
static HRESULT WINAPI client_closable_Close( IClosable *iface )
{
    struct http_client *client = client_from_closable( iface );
    EnterCriticalSection( &client->cs ); client->closed = TRUE; LeaveCriticalSection( &client->cs );
    return S_OK;
}
static const IClosableVtbl client_closable_vtbl =
{
    client_closable_QueryInterface, client_closable_AddRef, client_closable_Release,
    client_closable_GetIids, client_closable_GetRuntimeClassName,
    client_closable_GetTrustLevel, client_closable_Close,
};

static HRESULT client_create( IHttpClient **out )
{
    struct http_client *client;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(client = calloc( 1, sizeof(*client) ))) return E_OUTOFMEMORY;
    client->IHttpClient_iface.lpVtbl = &client_vtbl;
    client->IClosable_iface.lpVtbl = &client_closable_vtbl;
    client->ref = 1;
    InitializeCriticalSectionEx( &client->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    client->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": http_client.cs");
    if (FAILED(hr = http_headers_create( HTTP_HEADERS_REQUEST, &client->headers )))
    { client_Release( &client->IHttpClient_iface ); return hr; }
    *out = &client->IHttpClient_iface;
    return S_OK;
}

struct client_factory
{
    IActivationFactory IActivationFactory_iface;
    IHttpClientFactory IHttpClientFactory_iface;
};
static struct client_factory client_factory;
static HRESULT WINAPI client_factory_qi( IActivationFactory *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER; *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &client_factory.IActivationFactory_iface;
    else if (IsEqualGUID( iid, &IID_IHttpClientFactory )) *out = &client_factory.IHttpClientFactory_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out ); return S_OK;
}
static ULONG WINAPI client_factory_addref( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI client_factory_release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI client_factory_iids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI client_factory_name( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI client_factory_trust( IActivationFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI client_factory_activate( IActivationFactory *iface, IInspectable **out )
{
    IHttpClient *client;
    HRESULT hr;
    if (!out) return E_POINTER; *out = NULL;
    if (SUCCEEDED(hr = client_create( &client ))) *out = (IInspectable *)client;
    return hr;
}
static const IActivationFactoryVtbl client_activation_vtbl =
{
    client_factory_qi, client_factory_addref, client_factory_release,
    client_factory_iids, client_factory_name, client_factory_trust, client_factory_activate,
};
static inline struct client_factory *factory_from_client( IHttpClientFactory *iface )
{ return CONTAINING_RECORD( iface, struct client_factory, IHttpClientFactory_iface ); }
static HRESULT WINAPI client_value_factory_qi( IHttpClientFactory *iface, REFIID iid, void **out )
{ return client_factory_qi( &factory_from_client( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI client_value_factory_addref( IHttpClientFactory *iface ) { return 2; }
static ULONG WINAPI client_value_factory_release( IHttpClientFactory *iface ) { return 1; }
static HRESULT WINAPI client_value_factory_iids( IHttpClientFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI client_value_factory_name( IHttpClientFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI client_value_factory_trust( IHttpClientFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI client_value_factory_Create( IHttpClientFactory *iface, IInspectable *filter, IHttpClient **out )
{ if (filter) return E_NOTIMPL; return client_create( out ); }
static const IHttpClientFactoryVtbl client_value_factory_vtbl =
{
    client_value_factory_qi, client_value_factory_addref, client_value_factory_release,
    client_value_factory_iids, client_value_factory_name, client_value_factory_trust,
    client_value_factory_Create,
};
static struct client_factory client_factory =
{ {&client_activation_vtbl}, {&client_value_factory_vtbl} };
IActivationFactory *http_client_factory = &client_factory.IActivationFactory_iface;
