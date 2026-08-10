/* Windows.Web.Http content implementations.
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

struct http_content
{
    IHttpContent IHttpContent_iface;
    IClosable IClosable_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    BYTE *bytes;
    SIZE_T size;
    UnicodeEncoding encoding;
    HSTRING media_type;
    struct http_headers *headers;
    BOOL closed;
};

static inline struct http_content *content_from_iface( IHttpContent *iface )
{ return CONTAINING_RECORD( iface, struct http_content, IHttpContent_iface ); }
static inline struct http_content *content_from_closable( IClosable *iface )
{ return CONTAINING_RECORD( iface, struct http_content, IClosable_iface ); }

static HRESULT WINAPI content_QueryInterface( IHttpContent *iface, REFIID iid, void **out )
{
    struct http_content *content = content_from_iface( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IHttpContent ))
        *out = &content->IHttpContent_iface;
    else if (IsEqualGUID( iid, &IID_IClosable ))
        *out = &content->IClosable_iface;
    else return E_NOINTERFACE;
    IHttpContent_AddRef( &content->IHttpContent_iface );
    return S_OK;
}
static ULONG WINAPI content_AddRef( IHttpContent *iface )
{ return InterlockedIncrement( &content_from_iface( iface )->ref ); }
static ULONG WINAPI content_Release( IHttpContent *iface )
{
    struct http_content *content = content_from_iface( iface );
    ULONG ref = InterlockedDecrement( &content->ref );
    if (!ref)
    {
        free( content->bytes );
        WindowsDeleteString( content->media_type );
        http_headers_release( content->headers );
        content->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &content->cs );
        free( content );
    }
    return ref;
}
static HRESULT WINAPI content_GetIids( IHttpContent *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( 2 * sizeof(**iids) ))) return E_OUTOFMEMORY;
    (*iids)[0] = IID_IHttpContent;
    (*iids)[1] = IID_IClosable;
    *count = 2;
    return S_OK;
}
static HRESULT WINAPI content_GetRuntimeClassName( IHttpContent *iface, HSTRING *name )
{
    if (!name) return E_POINTER;
    return WindowsCreateString( L"Windows.Web.Http.HttpStringContent",
            ARRAY_SIZE(L"Windows.Web.Http.HttpStringContent") - 1, name );
}
static HRESULT WINAPI content_GetTrustLevel( IHttpContent *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}
static HRESULT WINAPI content_get_Headers( IHttpContent *iface, IHttpContentHeaderCollection **value )
{
    struct http_content *content = content_from_iface( iface );
    if (!value) return E_POINTER;
    EnterCriticalSection( &content->cs );
    if (content->closed)
    {
        LeaveCriticalSection( &content->cs );
        *value = NULL;
        return RO_E_CLOSED;
    }
    *value = http_headers_content_iface( content->headers );
    http_headers_addref( content->headers );
    LeaveCriticalSection( &content->cs );
    return S_OK;
}
static HRESULT WINAPI content_BufferAllAsync( IHttpContent *iface,
        IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static HRESULT WINAPI content_ReadAsBufferAsync( IHttpContent *iface, IInspectable **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static HRESULT WINAPI content_ReadAsInputStreamAsync( IHttpContent *iface, IInspectable **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }

static HRESULT bytes_to_hstring( const BYTE *bytes, SIZE_T size, UnicodeEncoding encoding, HSTRING *value )
{
    WCHAR *wide = NULL;
    int count;
    HRESULT hr;
    if (!value) return E_POINTER;
    *value = NULL;
    if (!size) return WindowsCreateString( L"", 0, value );
    if (encoding == UnicodeEncoding_Utf16LE || encoding == UnicodeEncoding_Utf16BE)
    {
        SIZE_T chars, i;
        if (size % 2 || size / 2 > ~(UINT32)0) return E_INVALIDARG;
        chars = size / 2;
        if (!(wide = malloc( chars * sizeof(*wide) ))) return E_OUTOFMEMORY;
        for (i = 0; i < chars; ++i)
        {
            UINT16 v = encoding == UnicodeEncoding_Utf16LE ? bytes[i * 2] | bytes[i * 2 + 1] << 8
                                                           : bytes[i * 2] << 8 | bytes[i * 2 + 1];
            wide[i] = v;
        }
        hr = WindowsCreateString( wide, chars, value );
        free( wide );
        return hr;
    }
    if (size > INT_MAX) return E_OUTOFMEMORY;
    if (!(count = MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, size, NULL, 0 )))
        return HRESULT_FROM_WIN32( GetLastError() );
    if (!(wide = malloc( count * sizeof(*wide) ))) return E_OUTOFMEMORY;
    MultiByteToWideChar( CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)bytes, size, wide, count );
    hr = WindowsCreateString( wide, count, value );
    free( wide );
    return hr;
}

static HRESULT WINAPI content_ReadAsStringAsync( IHttpContent *iface,
        IAsyncOperationWithProgress_HSTRING_UINT64 **operation )
{
    struct http_content *content = content_from_iface( iface );
    HSTRING value = NULL;
    HRESULT hr;
    if (!operation) return E_POINTER;
    EnterCriticalSection( &content->cs );
    if (content->closed) hr = RO_E_CLOSED;
    else hr = bytes_to_hstring( content->bytes, content->size, content->encoding, &value );
    LeaveCriticalSection( &content->cs );
    if (FAILED(hr)) return hr;
    hr = http_async_string_create( value, S_OK, operation );
    WindowsDeleteString( value );
    return hr;
}
static HRESULT WINAPI content_TryComputeLength( IHttpContent *iface, UINT64 *length, boolean *succeeded )
{
    struct http_content *content = content_from_iface( iface );
    if (!length || !succeeded) return E_POINTER;
    EnterCriticalSection( &content->cs );
    if (content->closed)
    {
        LeaveCriticalSection( &content->cs );
        return RO_E_CLOSED;
    }
    *length = content->size;
    *succeeded = TRUE;
    LeaveCriticalSection( &content->cs );
    return S_OK;
}
static HRESULT WINAPI content_WriteToStreamAsync( IHttpContent *iface, IOutputStream *output,
        IAsyncOperationWithProgress_UINT64_UINT64 **operation )
{ if (operation) *operation = NULL; return E_NOTIMPL; }
static const IHttpContentVtbl content_vtbl =
{
    content_QueryInterface, content_AddRef, content_Release, content_GetIids,
    content_GetRuntimeClassName, content_GetTrustLevel, content_get_Headers,
    content_BufferAllAsync, content_ReadAsBufferAsync, content_ReadAsInputStreamAsync,
    content_ReadAsStringAsync, content_TryComputeLength, content_WriteToStreamAsync,
};

static HRESULT WINAPI content_closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{ return content_QueryInterface( &content_from_closable( iface )->IHttpContent_iface, iid, out ); }
static ULONG WINAPI content_closable_AddRef( IClosable *iface )
{ return content_AddRef( &content_from_closable( iface )->IHttpContent_iface ); }
static ULONG WINAPI content_closable_Release( IClosable *iface )
{ return content_Release( &content_from_closable( iface )->IHttpContent_iface ); }
static HRESULT WINAPI content_closable_GetIids( IClosable *iface, ULONG *count, IID **iids )
{ return content_GetIids( &content_from_closable( iface )->IHttpContent_iface, count, iids ); }
static HRESULT WINAPI content_closable_GetRuntimeClassName( IClosable *iface, HSTRING *name )
{ return content_GetRuntimeClassName( &content_from_closable( iface )->IHttpContent_iface, name ); }
static HRESULT WINAPI content_closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{ return content_GetTrustLevel( &content_from_closable( iface )->IHttpContent_iface, level ); }
static HRESULT WINAPI content_closable_Close( IClosable *iface )
{
    struct http_content *content = content_from_closable( iface );
    EnterCriticalSection( &content->cs );
    content->closed = TRUE;
    LeaveCriticalSection( &content->cs );
    return S_OK;
}
static const IClosableVtbl content_closable_vtbl =
{
    content_closable_QueryInterface, content_closable_AddRef, content_closable_Release,
    content_closable_GetIids, content_closable_GetRuntimeClassName,
    content_closable_GetTrustLevel, content_closable_Close,
};

static HRESULT content_create_encoding( const BYTE *bytes, SIZE_T size, UnicodeEncoding encoding,
                                        HSTRING media_type, IHttpContent **out )
{
    struct http_content *content;
    HSTRING name = NULL;
    HRESULT hr;
    if (!out || (size && !bytes)) return E_POINTER;
    *out = NULL;
    if (!(content = calloc( 1, sizeof(*content) ))) return E_OUTOFMEMORY;
    content->IHttpContent_iface.lpVtbl = &content_vtbl;
    content->IClosable_iface.lpVtbl = &content_closable_vtbl;
    content->ref = 1;
    content->encoding = encoding;
    if (size)
    {
        if (!(content->bytes = malloc( size ))) { free( content ); return E_OUTOFMEMORY; }
        memcpy( content->bytes, bytes, size );
    }
    content->size = size;
    InitializeCriticalSectionEx( &content->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    content->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": http_content.cs");
    if (FAILED(hr = WindowsDuplicateString( media_type, &content->media_type )) ||
        FAILED(hr = http_headers_create( HTTP_HEADERS_CONTENT, &content->headers ))) goto failed;
    if (media_type && WindowsGetStringLen( media_type ))
    {
        if (FAILED(hr = WindowsCreateString( L"Content-Type", ARRAY_SIZE(L"Content-Type") - 1, &name )) ||
            FAILED(hr = http_headers_append( content->headers, name, media_type ))) goto failed;
    }
    WindowsDeleteString( name );
    *out = &content->IHttpContent_iface;
    return S_OK;

failed:
    WindowsDeleteString( name );
    if (content->headers) http_headers_release( content->headers );
    WindowsDeleteString( content->media_type );
    content->cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &content->cs );
    free( content->bytes );
    free( content );
    return hr;
}

HRESULT http_content_create( const BYTE *bytes, SIZE_T size, HSTRING media_type, IHttpContent **out )
{ return content_create_encoding( bytes, size, UnicodeEncoding_Utf8, media_type, out ); }

HRESULT http_content_get_bytes( IHttpContent *iface, const BYTE **bytes, SIZE_T *size, HSTRING *media_type )
{
    struct http_content *content;
    if (!iface || !bytes || !size || !media_type) return E_POINTER;
    if (iface->lpVtbl != &content_vtbl) return E_NOINTERFACE;
    content = content_from_iface( iface );
    EnterCriticalSection( &content->cs );
    if (content->closed)
    {
        LeaveCriticalSection( &content->cs );
        return RO_E_CLOSED;
    }
    *bytes = content->bytes;
    *size = content->size;
    WindowsDuplicateString( content->media_type, media_type );
    LeaveCriticalSection( &content->cs );
    return S_OK;
}

static HRESULT hstring_to_bytes( HSTRING string, UnicodeEncoding encoding, BYTE **bytes, SIZE_T *size )
{
    const WCHAR *value;
    UINT32 length;
    int count;
    SIZE_T i;
    value = WindowsGetStringRawBuffer( string, &length );
    if (!value) return E_INVALIDARG;
    *bytes = NULL;
    *size = 0;
    if (!length) return S_OK;
    if (encoding == UnicodeEncoding_Utf8)
    {
        if (!(count = WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, value, length, NULL, 0, NULL, NULL )))
            return HRESULT_FROM_WIN32( GetLastError() );
        if (!(*bytes = malloc( count ))) return E_OUTOFMEMORY;
        WideCharToMultiByte( CP_UTF8, WC_ERR_INVALID_CHARS, value, length, (char *)*bytes, count, NULL, NULL );
        *size = count;
        return S_OK;
    }
    if (!(*bytes = malloc( (SIZE_T)length * 2 ))) return E_OUTOFMEMORY;
    for (i = 0; i < length; ++i)
    {
        if (encoding == UnicodeEncoding_Utf16LE)
        {
            (*bytes)[i * 2] = value[i] & 0xff;
            (*bytes)[i * 2 + 1] = value[i] >> 8;
        }
        else
        {
            (*bytes)[i * 2] = value[i] >> 8;
            (*bytes)[i * 2 + 1] = value[i] & 0xff;
        }
    }
    *size = length * 2;
    return S_OK;
}

static HRESULT create_string_content( HSTRING string, UnicodeEncoding encoding, HSTRING media_type,
                                      IHttpContent **out )
{
    BYTE *bytes;
    SIZE_T size;
    HRESULT hr;
    if (encoding != UnicodeEncoding_Utf8 && encoding != UnicodeEncoding_Utf16LE &&
        encoding != UnicodeEncoding_Utf16BE) return E_INVALIDARG;
    if (FAILED(hr = hstring_to_bytes( string, encoding, &bytes, &size ))) return hr;
    hr = content_create_encoding( bytes, size, encoding, media_type, out );
    free( bytes );
    return hr;
}

static BOOL form_safe( BYTE c )
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '*' || c == '-' || c == '.' || c == '_'; }

static HRESULT form_append( BYTE **buffer, SIZE_T *length, SIZE_T *capacity,
                            const BYTE *value, SIZE_T value_length )
{
    static const char hex[] = "0123456789ABCDEF";
    SIZE_T i, required;
    BYTE *new_buffer;
    if (value_length > (SIZE_MAX - *length) / 3) return E_OUTOFMEMORY;
    required = *length + value_length * 3;
    if (required > *capacity)
    {
        SIZE_T new_capacity = *capacity ? *capacity : 128;
        while (new_capacity < required)
        {
            if (new_capacity > SIZE_MAX / 2) return E_OUTOFMEMORY;
            new_capacity *= 2;
        }
        if (!(new_buffer = realloc( *buffer, new_capacity ))) return E_OUTOFMEMORY;
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    for (i = 0; i < value_length; ++i)
    {
        BYTE c = value[i];
        if (form_safe( c )) (*buffer)[(*length)++] = c;
        else if (c == ' ') (*buffer)[(*length)++] = '+';
        else
        {
            (*buffer)[(*length)++] = '%';
            (*buffer)[(*length)++] = hex[c >> 4];
            (*buffer)[(*length)++] = hex[c & 0xf];
        }
    }
    return S_OK;
}

static HRESULT form_append_hstring( BYTE **buffer, SIZE_T *length, SIZE_T *capacity, HSTRING value )
{
    BYTE *bytes;
    SIZE_T size;
    HRESULT hr;
    if (FAILED(hr = hstring_to_bytes( value, UnicodeEncoding_Utf8, &bytes, &size ))) return hr;
    hr = form_append( buffer, length, capacity, bytes, size );
    free( bytes );
    return hr;
}

struct content_factory
{
    IActivationFactory IActivationFactory_iface;
    IHttpStringContentFactory IHttpStringContentFactory_iface;
    IHttpFormUrlEncodedContentFactory IHttpFormUrlEncodedContentFactory_iface;
    BOOL string_factory;
};
static struct content_factory string_factory, form_factory;

static HRESULT WINAPI content_factory_qi( IActivationFactory *iface, REFIID iid, void **out )
{
    struct content_factory *factory = CONTAINING_RECORD( iface, struct content_factory, IActivationFactory_iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IActivationFactory ))
        *out = &factory->IActivationFactory_iface;
    else if (factory->string_factory && IsEqualGUID( iid, &IID_IHttpStringContentFactory ))
        *out = &factory->IHttpStringContentFactory_iface;
    else if (!factory->string_factory && IsEqualGUID( iid, &IID_IHttpFormUrlEncodedContentFactory ))
        *out = &factory->IHttpFormUrlEncodedContentFactory_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}
static ULONG WINAPI content_factory_addref( IActivationFactory *iface ) { return 2; }
static ULONG WINAPI content_factory_release( IActivationFactory *iface ) { return 1; }
static HRESULT WINAPI content_factory_iids( IActivationFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI content_factory_name( IActivationFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI content_factory_trust( IActivationFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI content_factory_activate( IActivationFactory *iface, IInspectable **out )
{ if (out) *out = NULL; return E_NOTIMPL; }
static const IActivationFactoryVtbl content_factory_vtbl =
{
    content_factory_qi, content_factory_addref, content_factory_release,
    content_factory_iids, content_factory_name, content_factory_trust, content_factory_activate,
};

static inline struct content_factory *factory_from_string( IHttpStringContentFactory *iface )
{ return CONTAINING_RECORD( iface, struct content_factory, IHttpStringContentFactory_iface ); }
static HRESULT WINAPI string_factory_qi( IHttpStringContentFactory *iface, REFIID iid, void **out )
{ return content_factory_qi( &factory_from_string( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI string_factory_addref( IHttpStringContentFactory *iface ) { return 2; }
static ULONG WINAPI string_factory_release( IHttpStringContentFactory *iface ) { return 1; }
static HRESULT WINAPI string_factory_iids( IHttpStringContentFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI string_factory_name( IHttpStringContentFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI string_factory_trust( IHttpStringContentFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI string_factory_CreateFromString( IHttpStringContentFactory *iface, HSTRING string,
        IHttpContent **out )
{
    HSTRING media;
    HRESULT hr;
    if (FAILED(hr = WindowsCreateString( L"text/plain; charset=UTF-8",
            ARRAY_SIZE(L"text/plain; charset=UTF-8") - 1, &media ))) return hr;
    hr = create_string_content( string, UnicodeEncoding_Utf8, media, out );
    WindowsDeleteString( media );
    return hr;
}
static HRESULT WINAPI string_factory_CreateFromStringWithEncoding( IHttpStringContentFactory *iface,
        HSTRING string, UnicodeEncoding encoding, IHttpContent **out )
{
    static const WCHAR utf8_type[] = L"text/plain; charset=UTF-8";
    static const WCHAR utf16le_type[] = L"text/plain; charset=UTF-16LE";
    static const WCHAR utf16be_type[] = L"text/plain; charset=UTF-16BE";
    const WCHAR *type;
    HSTRING media;
    HRESULT hr;

    if (encoding == UnicodeEncoding_Utf8) type = utf8_type;
    else if (encoding == UnicodeEncoding_Utf16LE) type = utf16le_type;
    else if (encoding == UnicodeEncoding_Utf16BE) type = utf16be_type;
    else return E_INVALIDARG;
    if (FAILED(hr = WindowsCreateString( type, wcslen( type ), &media ))) return hr;
    hr = create_string_content( string, encoding, media, out );
    WindowsDeleteString( media );
    return hr;
}
static HRESULT WINAPI string_factory_CreateFromStringWithEncodingAndMediaType( IHttpStringContentFactory *iface,
        HSTRING string, UnicodeEncoding encoding, HSTRING media_type, IHttpContent **out )
{ return create_string_content( string, encoding, media_type, out ); }
static const IHttpStringContentFactoryVtbl string_factory_vtbl =
{
    string_factory_qi, string_factory_addref, string_factory_release,
    string_factory_iids, string_factory_name, string_factory_trust,
    string_factory_CreateFromString, string_factory_CreateFromStringWithEncoding,
    string_factory_CreateFromStringWithEncodingAndMediaType,
};

static inline struct content_factory *factory_from_form( IHttpFormUrlEncodedContentFactory *iface )
{ return CONTAINING_RECORD( iface, struct content_factory, IHttpFormUrlEncodedContentFactory_iface ); }
static HRESULT WINAPI form_factory_qi( IHttpFormUrlEncodedContentFactory *iface, REFIID iid, void **out )
{ return content_factory_qi( &factory_from_form( iface )->IActivationFactory_iface, iid, out ); }
static ULONG WINAPI form_factory_addref( IHttpFormUrlEncodedContentFactory *iface ) { return 2; }
static ULONG WINAPI form_factory_release( IHttpFormUrlEncodedContentFactory *iface ) { return 1; }
static HRESULT WINAPI form_factory_iids( IHttpFormUrlEncodedContentFactory *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT WINAPI form_factory_name( IHttpFormUrlEncodedContentFactory *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT WINAPI form_factory_trust( IHttpFormUrlEncodedContentFactory *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI form_factory_Create( IHttpFormUrlEncodedContentFactory *iface,
        IIterable_IKeyValuePair_HSTRING_HSTRING *values, IHttpContent **out )
{
    IIterator_IKeyValuePair_HSTRING_HSTRING *iterator = NULL;
    BYTE *buffer = NULL;
    SIZE_T length = 0, capacity = 0;
    boolean has_current;
    HSTRING media = NULL;
    HRESULT hr;
    BOOL first = TRUE;

    if (!values || !out) return E_POINTER;
    *out = NULL;
    if (FAILED(hr = IIterable_IKeyValuePair_HSTRING_HSTRING_First( values, &iterator ))) return hr;
    while (SUCCEEDED(hr = IIterator_IKeyValuePair_HSTRING_HSTRING_get_HasCurrent( iterator, &has_current )) &&
           has_current)
    {
        IKeyValuePair_HSTRING_HSTRING *pair = NULL;
        HSTRING key = NULL, value = NULL;
        boolean moved;
        if (FAILED(hr = IIterator_IKeyValuePair_HSTRING_HSTRING_get_Current( iterator, &pair )) ||
            FAILED(hr = IKeyValuePair_HSTRING_HSTRING_get_Key( pair, &key )) ||
            FAILED(hr = IKeyValuePair_HSTRING_HSTRING_get_Value( pair, &value )))
        {
            if (pair) IKeyValuePair_HSTRING_HSTRING_Release( pair );
            WindowsDeleteString( key );
            WindowsDeleteString( value );
            break;
        }
        if (!first)
        {
            if (length == capacity)
            {
                BYTE *new_buffer;
                SIZE_T new_capacity = capacity ? capacity * 2 : 128;
                if (!(new_buffer = realloc( buffer, new_capacity ))) hr = E_OUTOFMEMORY;
                else { buffer = new_buffer; capacity = new_capacity; }
            }
            if (SUCCEEDED(hr)) buffer[length++] = '&';
        }
        if (SUCCEEDED(hr)) hr = form_append_hstring( &buffer, &length, &capacity, key );
        if (SUCCEEDED(hr))
        {
            if (length == capacity)
            {
                BYTE *new_buffer;
                SIZE_T new_capacity = capacity ? capacity * 2 : 128;
                if (!(new_buffer = realloc( buffer, new_capacity ))) hr = E_OUTOFMEMORY;
                else { buffer = new_buffer; capacity = new_capacity; }
            }
            if (SUCCEEDED(hr)) buffer[length++] = '=';
        }
        if (SUCCEEDED(hr)) hr = form_append_hstring( &buffer, &length, &capacity, value );
        IKeyValuePair_HSTRING_HSTRING_Release( pair );
        WindowsDeleteString( key );
        WindowsDeleteString( value );
        if (FAILED(hr)) break;
        first = FALSE;
        if (FAILED(hr = IIterator_IKeyValuePair_HSTRING_HSTRING_MoveNext( iterator, &moved ))) break;
    }
    IIterator_IKeyValuePair_HSTRING_HSTRING_Release( iterator );
    if (FAILED(hr)) { free( buffer ); return hr; }
    if (FAILED(hr = WindowsCreateString( L"application/x-www-form-urlencoded",
            ARRAY_SIZE(L"application/x-www-form-urlencoded") - 1, &media )))
    { free( buffer ); return hr; }
    hr = content_create_encoding( buffer, length, UnicodeEncoding_Utf8, media, out );
    WindowsDeleteString( media );
    free( buffer );
    return hr;
}
static const IHttpFormUrlEncodedContentFactoryVtbl form_factory_vtbl =
{
    form_factory_qi, form_factory_addref, form_factory_release,
    form_factory_iids, form_factory_name, form_factory_trust, form_factory_Create,
};

static struct content_factory string_factory =
{
    {&content_factory_vtbl}, {&string_factory_vtbl}, {NULL}, TRUE,
};
static struct content_factory form_factory =
{
    {&content_factory_vtbl}, {NULL}, {&form_factory_vtbl}, FALSE,
};
IActivationFactory *http_string_content_factory = &string_factory.IActivationFactory_iface;
IActivationFactory *http_form_content_factory = &form_factory.IActivationFactory_iface;
