/* WinRT Windows.Web Implementation
 *
 * Copyright (C) 2024 Mohamad Al-Jaf
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

#ifndef __WINE_WINDOWS_WEB_PRIVATE_H
#define __WINE_WINDOWS_WEB_PRIVATE_H

#include <stdarg.h>
#include <errno.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"

#include "activation.h"
#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_Storage_Streams
#include "windows.foundation.h"
#define WIDL_using_Windows_Data_Json
#include "windows.data.json.h"
#define WIDL_using_Windows_Web_Http
#define WIDL_using_Windows_Web_Http_Headers
#include "windows.web.http.h"

extern IActivationFactory *json_array_factory;
extern IActivationFactory *json_object_factory;
extern IActivationFactory *json_value_factory;
extern IActivationFactory *http_client_factory;
extern IActivationFactory *http_form_content_factory;
extern IActivationFactory *http_method_factory;
extern IActivationFactory *http_request_factory;
extern IActivationFactory *http_response_factory;
extern IActivationFactory *http_string_content_factory;
extern IActivationFactory *http_credentials_factory;

enum http_headers_kind
{
    HTTP_HEADERS_REQUEST,
    HTTP_HEADERS_CONTENT,
    HTTP_HEADERS_RESPONSE,
};

struct http_headers;

HRESULT http_headers_create( enum http_headers_kind kind, struct http_headers **out );
void http_headers_addref( struct http_headers *headers );
void http_headers_release( struct http_headers *headers );
IHttpRequestHeaderCollection *http_headers_request_iface( struct http_headers *headers );
IHttpContentHeaderCollection *http_headers_content_iface( struct http_headers *headers );
IHttpResponseHeaderCollection *http_headers_response_iface( struct http_headers *headers );
HRESULT http_headers_append( struct http_headers *headers, HSTRING name, HSTRING value );
HRESULT http_headers_build( struct http_headers *headers, WCHAR **value );

HRESULT http_content_create( const BYTE *bytes, SIZE_T size, HSTRING media_type, IHttpContent **out );
HRESULT http_content_get_bytes( IHttpContent *content, const BYTE **bytes, SIZE_T *size, HSTRING *media_type );
HRESULT http_method_create_literal( const WCHAR *literal, IHttpMethod **out );

HRESULT http_request_create( IHttpMethod *method, IUriRuntimeClass *uri, IHttpRequestMessage **out );
HRESULT http_request_get_transport( IHttpRequestMessage *request, IHttpMethod **method, IUriRuntimeClass **uri,
                                    IHttpContent **content, struct http_headers **headers );
HRESULT http_response_create( HttpStatusCode status, const BYTE *body, SIZE_T body_size,
                              IHttpRequestMessage *request, IHttpResponseMessage **out );

typedef HRESULT (*http_response_work)( void *context, IHttpResponseMessage **result );
typedef void (*http_async_context_destroy)( void *context );
HRESULT http_async_response_create( void *context, http_response_work work,
                                    http_async_context_destroy destroy,
                                    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **out );
HRESULT http_async_string_create( HSTRING value, HRESULT error,
                                  IAsyncOperationWithProgress_HSTRING_UINT64 **out );

HRESULT json_array_push( IJsonArray *iface, IJsonValue *value );

#define DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from, iface_mem, expr )             \
    static inline impl_type *impl_from( iface_type *iface )                                        \
    {                                                                                              \
        return CONTAINING_RECORD( iface, impl_type, iface_mem );                                   \
    }                                                                                              \
    static HRESULT WINAPI pfx##_QueryInterface( iface_type *iface, REFIID iid, void **out )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_QueryInterface( (IInspectable *)(expr), iid, out );                    \
    }                                                                                              \
    static ULONG WINAPI pfx##_AddRef( iface_type *iface )                                          \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_AddRef( (IInspectable *)(expr) );                                      \
    }                                                                                              \
    static ULONG WINAPI pfx##_Release( iface_type *iface )                                         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_Release( (IInspectable *)(expr) );                                     \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetIids( iface_type *iface, ULONG *iid_count, IID **iids )         \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetIids( (IInspectable *)(expr), iid_count, iids );                    \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetRuntimeClassName( iface_type *iface, HSTRING *class_name )      \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetRuntimeClassName( (IInspectable *)(expr), class_name );             \
    }                                                                                              \
    static HRESULT WINAPI pfx##_GetTrustLevel( iface_type *iface, TrustLevel *trust_level )        \
    {                                                                                              \
        impl_type *impl = impl_from( iface );                                                      \
        return IInspectable_GetTrustLevel( (IInspectable *)(expr), trust_level );                  \
    }
#define DEFINE_IINSPECTABLE( pfx, iface_type, impl_type, base_iface )                              \
    DEFINE_IINSPECTABLE_( pfx, iface_type, impl_type, impl_from_##iface_type, iface_type##_iface, &impl->base_iface )

#endif
