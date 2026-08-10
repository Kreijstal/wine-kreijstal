/* Windows.Web.Http asynchronous operations.
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

#define ASYNC_CLOSED ((AsyncStatus)4)

static LONG next_async_id;

struct async_response
{
    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress operation_iface;
    IAsyncInfo info_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    AsyncStatus status;
    HRESULT error;
    UINT32 id;
    BOOL completed_assigned;
    IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress *progress;
    IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress *completed;
    IHttpResponseMessage *result;
    void *context;
    http_response_work work;
    http_async_context_destroy destroy;
};

static inline struct async_response *response_from_operation(
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface )
{ return CONTAINING_RECORD( iface, struct async_response, operation_iface ); }
static inline struct async_response *response_from_info( IAsyncInfo *iface )
{ return CONTAINING_RECORD( iface, struct async_response, info_iface ); }

static HRESULT WINAPI response_QueryInterface( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        REFIID iid, void **out )
{
    struct async_response *async = response_from_operation( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress ))
        *out = &async->operation_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo )) *out = &async->info_iface;
    else return E_NOINTERFACE;
    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_AddRef( &async->operation_iface );
    return S_OK;
}
static ULONG WINAPI response_AddRef( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface )
{ return InterlockedIncrement( &response_from_operation( iface )->ref ); }
static ULONG WINAPI response_Release( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface )
{
    struct async_response *async = response_from_operation( iface );
    ULONG ref = InterlockedDecrement( &async->ref );
    if (!ref)
    {
        if (async->progress) IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress_Release( async->progress );
        if (async->completed) IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Release( async->completed );
        if (async->result) IHttpResponseMessage_Release( async->result );
        if (async->destroy) async->destroy( async->context );
        async->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &async->cs );
        free( async );
    }
    return ref;
}
static HRESULT WINAPI response_GetIids( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(**iids) ))) return E_OUTOFMEMORY;
    **iids = IID_IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress;
    *count = 1;
    return S_OK;
}
static HRESULT WINAPI response_GetRuntimeClassName( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        HSTRING *name )
{
    const WCHAR value[] = L"Windows.Foundation.IAsyncOperationWithProgress`2<Windows.Web.Http.HttpResponseMessage,Windows.Web.Http.HttpProgress>";
    if (!name) return E_POINTER;
    return WindowsCreateString( value, ARRAY_SIZE(value) - 1, name );
}
static HRESULT WINAPI response_GetTrustLevel( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI response_put_Progress( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress *handler )
{
    struct async_response *async = response_from_operation( iface );
    IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress *old;
    if (handler) IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress_AddRef( handler );
    EnterCriticalSection( &async->cs );
    if (async->status == ASYNC_CLOSED)
    {
        LeaveCriticalSection( &async->cs );
        if (handler) IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress_Release( handler );
        return E_ILLEGAL_METHOD_CALL;
    }
    old = async->progress;
    async->progress = handler;
    LeaveCriticalSection( &async->cs );
    if (old) IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress_Release( old );
    return S_OK;
}
static HRESULT WINAPI response_get_Progress( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress **handler )
{
    struct async_response *async = response_from_operation( iface );
    if (!handler) return E_POINTER;
    EnterCriticalSection( &async->cs );
    if ((*handler = async->progress)) IAsyncOperationProgressHandler_HttpResponseMessage_HttpProgress_AddRef( *handler );
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI response_put_Completed( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress *handler )
{
    struct async_response *async = response_from_operation( iface );
    AsyncStatus status;
    if (!handler) return E_POINTER;
    IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_AddRef( handler );
    EnterCriticalSection( &async->cs );
    if (async->status == ASYNC_CLOSED || async->completed_assigned)
    {
        LeaveCriticalSection( &async->cs );
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Release( handler );
        return async->status == ASYNC_CLOSED ? E_ILLEGAL_METHOD_CALL : E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    async->completed_assigned = TRUE;
    async->completed = handler;
    status = async->status;
    if (status != Started)
    {
        async->completed = NULL;
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_AddRef( iface );
    }
    LeaveCriticalSection( &async->cs );
    if (status != Started)
    {
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Invoke( handler, iface, status );
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Release( handler );
        IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_Release( iface );
    }
    return S_OK;
}
static HRESULT WINAPI response_get_Completed( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress **handler )
{
    struct async_response *async = response_from_operation( iface );
    if (!handler) return E_POINTER;
    EnterCriticalSection( &async->cs );
    if ((*handler = async->completed)) IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_AddRef( *handler );
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI response_GetResults( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *iface,
        IHttpResponseMessage **result )
{
    struct async_response *async = response_from_operation( iface );
    HRESULT hr;
    if (!result) return E_POINTER;
    *result = NULL;
    EnterCriticalSection( &async->cs );
    if (async->status == Completed)
    {
        *result = async->result;
        if (*result) IHttpResponseMessage_AddRef( *result );
        hr = S_OK;
    }
    else if (async->status == Error || async->status == Canceled) hr = async->error;
    else hr = E_ILLEGAL_METHOD_CALL;
    LeaveCriticalSection( &async->cs );
    return hr;
}
static const IAsyncOperationWithProgress_HttpResponseMessage_HttpProgressVtbl response_vtbl =
{
    response_QueryInterface, response_AddRef, response_Release, response_GetIids,
    response_GetRuntimeClassName, response_GetTrustLevel, response_put_Progress,
    response_get_Progress, response_put_Completed, response_get_Completed, response_GetResults,
};

static HRESULT WINAPI response_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{ return response_QueryInterface( &response_from_info( iface )->operation_iface, iid, out ); }
static ULONG WINAPI response_info_AddRef( IAsyncInfo *iface )
{ return response_AddRef( &response_from_info( iface )->operation_iface ); }
static ULONG WINAPI response_info_Release( IAsyncInfo *iface )
{ return response_Release( &response_from_info( iface )->operation_iface ); }
static HRESULT WINAPI response_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )
{ return response_GetIids( &response_from_info( iface )->operation_iface, count, iids ); }
static HRESULT WINAPI response_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{ return response_GetRuntimeClassName( &response_from_info( iface )->operation_iface, name ); }
static HRESULT WINAPI response_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{ return response_GetTrustLevel( &response_from_info( iface )->operation_iface, level ); }
static HRESULT WINAPI response_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{ if (!id) return E_POINTER; *id = response_from_info( iface )->id; return S_OK; }
static HRESULT WINAPI response_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_response *async = response_from_info( iface );
    if (!status) return E_POINTER;
    EnterCriticalSection( &async->cs );
    if (async->status == ASYNC_CLOSED) { LeaveCriticalSection( &async->cs ); return E_ILLEGAL_METHOD_CALL; }
    *status = async->status;
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI response_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{
    struct async_response *async = response_from_info( iface );
    if (!error) return E_POINTER;
    EnterCriticalSection( &async->cs );
    *error = async->error;
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI response_info_Cancel( IAsyncInfo *iface )
{
    struct async_response *async = response_from_info( iface );
    EnterCriticalSection( &async->cs );
    if (async->status == Started) { async->status = Canceled; async->error = E_ABORT; }
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI response_info_Close( IAsyncInfo *iface )
{
    struct async_response *async = response_from_info( iface );
    EnterCriticalSection( &async->cs );
    if (async->status == Started) { LeaveCriticalSection( &async->cs ); return E_ILLEGAL_STATE_CHANGE; }
    async->status = ASYNC_CLOSED;
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static const IAsyncInfoVtbl response_info_vtbl =
{
    response_info_QueryInterface, response_info_AddRef, response_info_Release,
    response_info_GetIids, response_info_GetRuntimeClassName, response_info_GetTrustLevel,
    response_info_get_Id, response_info_get_Status, response_info_get_ErrorCode,
    response_info_Cancel, response_info_Close,
};

static DWORD WINAPI response_worker( void *parameter )
{
    struct async_response *async = parameter;
    IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress *handler = NULL;
    IHttpResponseMessage *result = NULL;
    AsyncStatus status;
    HRESULT hr = async->work( async->context, &result );

    EnterCriticalSection( &async->cs );
    if (async->status == Started)
    {
        async->error = hr;
        if (SUCCEEDED(hr))
        {
            async->result = result;
            result = NULL;
            async->status = Completed;
        }
        else async->status = Error;
    }
    status = async->status;
    if ((handler = async->completed))
    {
        async->completed = NULL;
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_AddRef( handler );
    }
    LeaveCriticalSection( &async->cs );
    if (result) IHttpResponseMessage_Release( result );
    if (handler)
    {
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Invoke(
                handler, &async->operation_iface, status );
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Release( handler );
        IAsyncOperationWithProgressCompletedHandler_HttpResponseMessage_HttpProgress_Release( handler );
    }
    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_Release( &async->operation_iface );
    return 0;
}

HRESULT http_async_response_create( void *context, http_response_work work,
                                    http_async_context_destroy destroy,
                                    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress **out )
{
    struct async_response *async;
    HANDLE thread;
    if (!out || !work) return E_POINTER;
    *out = NULL;
    if (!(async = calloc( 1, sizeof(*async) ))) return E_OUTOFMEMORY;
    async->operation_iface.lpVtbl = &response_vtbl;
    async->info_iface.lpVtbl = &response_info_vtbl;
    async->ref = 2; /* caller plus worker */
    async->status = Started;
    async->error = S_OK;
    do async->id = InterlockedIncrement( &next_async_id ); while (!async->id);
    async->context = context;
    async->work = work;
    async->destroy = destroy;
    InitializeCriticalSectionEx( &async->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    async->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": async_response.cs");
    if (!(thread = CreateThread( NULL, 0, response_worker, async, 0, NULL )))
    {
        HRESULT hr = HRESULT_FROM_WIN32( GetLastError() );
        async->ref = 1;
        response_Release( &async->operation_iface );
        return hr;
    }
    CloseHandle( thread );
    *out = &async->operation_iface;
    return S_OK;
}

struct async_string
{
    IAsyncOperationWithProgress_HSTRING_UINT64 operation_iface;
    IAsyncInfo info_iface;
    LONG ref;
    CRITICAL_SECTION cs;
    AsyncStatus status;
    HRESULT error;
    UINT32 id;
    BOOL completed_assigned;
    IAsyncOperationProgressHandler_HSTRING_UINT64 *progress;
    IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64 *completed;
    HSTRING value;
};
static inline struct async_string *string_from_operation( IAsyncOperationWithProgress_HSTRING_UINT64 *iface )
{ return CONTAINING_RECORD( iface, struct async_string, operation_iface ); }
static inline struct async_string *string_from_info( IAsyncInfo *iface )
{ return CONTAINING_RECORD( iface, struct async_string, info_iface ); }
static HRESULT WINAPI string_QueryInterface( IAsyncOperationWithProgress_HSTRING_UINT64 *iface, REFIID iid, void **out )
{
    struct async_string *async = string_from_operation( iface );
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IAsyncOperationWithProgress_HSTRING_UINT64 ))
        *out = &async->operation_iface;
    else if (IsEqualGUID( iid, &IID_IAsyncInfo )) *out = &async->info_iface;
    else return E_NOINTERFACE;
    IAsyncOperationWithProgress_HSTRING_UINT64_AddRef( &async->operation_iface );
    return S_OK;
}
static ULONG WINAPI string_AddRef( IAsyncOperationWithProgress_HSTRING_UINT64 *iface )
{ return InterlockedIncrement( &string_from_operation( iface )->ref ); }
static ULONG WINAPI string_Release( IAsyncOperationWithProgress_HSTRING_UINT64 *iface )
{
    struct async_string *async = string_from_operation( iface );
    ULONG ref = InterlockedDecrement( &async->ref );
    if (!ref)
    {
        if (async->progress) IAsyncOperationProgressHandler_HSTRING_UINT64_Release( async->progress );
        if (async->completed) IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64_Release( async->completed );
        WindowsDeleteString( async->value );
        async->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &async->cs );
        free( async );
    }
    return ref;
}
static HRESULT WINAPI string_GetIids( IAsyncOperationWithProgress_HSTRING_UINT64 *iface, ULONG *count, IID **iids )
{
    if (!count || !iids) return E_POINTER;
    if (!(*iids = CoTaskMemAlloc( sizeof(**iids) ))) return E_OUTOFMEMORY;
    **iids = IID_IAsyncOperationWithProgress_HSTRING_UINT64;
    *count = 1;
    return S_OK;
}
static HRESULT WINAPI string_GetRuntimeClassName( IAsyncOperationWithProgress_HSTRING_UINT64 *iface, HSTRING *name )
{
    const WCHAR value[] = L"Windows.Foundation.IAsyncOperationWithProgress`2<String,UInt64>";
    if (!name) return E_POINTER;
    return WindowsCreateString( value, ARRAY_SIZE(value) - 1, name );
}
static HRESULT WINAPI string_GetTrustLevel( IAsyncOperationWithProgress_HSTRING_UINT64 *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }
static HRESULT WINAPI string_put_Progress( IAsyncOperationWithProgress_HSTRING_UINT64 *iface,
        IAsyncOperationProgressHandler_HSTRING_UINT64 *handler )
{
    struct async_string *async = string_from_operation( iface );
    IAsyncOperationProgressHandler_HSTRING_UINT64 *old;
    if (handler) IAsyncOperationProgressHandler_HSTRING_UINT64_AddRef( handler );
    EnterCriticalSection( &async->cs ); old = async->progress; async->progress = handler; LeaveCriticalSection( &async->cs );
    if (old) IAsyncOperationProgressHandler_HSTRING_UINT64_Release( old );
    return S_OK;
}
static HRESULT WINAPI string_get_Progress( IAsyncOperationWithProgress_HSTRING_UINT64 *iface,
        IAsyncOperationProgressHandler_HSTRING_UINT64 **handler )
{
    struct async_string *async = string_from_operation( iface );
    if (!handler) return E_POINTER;
    EnterCriticalSection( &async->cs );
    if ((*handler = async->progress)) IAsyncOperationProgressHandler_HSTRING_UINT64_AddRef( *handler );
    LeaveCriticalSection( &async->cs );
    return S_OK;
}
static HRESULT WINAPI string_put_Completed( IAsyncOperationWithProgress_HSTRING_UINT64 *iface,
        IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64 *handler )
{
    struct async_string *async = string_from_operation( iface );
    if (!handler) return E_POINTER;
    IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64_AddRef( handler );
    EnterCriticalSection( &async->cs );
    if (async->completed_assigned)
    {
        LeaveCriticalSection( &async->cs );
        IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64_Release( handler );
        return E_ILLEGAL_DELEGATE_ASSIGNMENT;
    }
    async->completed_assigned = TRUE;
    LeaveCriticalSection( &async->cs );
    IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64_Invoke( handler, iface, async->status );
    IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64_Release( handler );
    return S_OK;
}
static HRESULT WINAPI string_get_Completed( IAsyncOperationWithProgress_HSTRING_UINT64 *iface,
        IAsyncOperationWithProgressCompletedHandler_HSTRING_UINT64 **handler )
{ if (!handler) return E_POINTER; *handler = NULL; return S_OK; }
static HRESULT WINAPI string_GetResults( IAsyncOperationWithProgress_HSTRING_UINT64 *iface, HSTRING *result )
{
    struct async_string *async = string_from_operation( iface );
    if (!result) return E_POINTER;
    *result = NULL;
    if (FAILED(async->error)) return async->error;
    return WindowsDuplicateString( async->value, result );
}
static const IAsyncOperationWithProgress_HSTRING_UINT64Vtbl string_vtbl =
{
    string_QueryInterface, string_AddRef, string_Release, string_GetIids,
    string_GetRuntimeClassName, string_GetTrustLevel, string_put_Progress,
    string_get_Progress, string_put_Completed, string_get_Completed, string_GetResults,
};
static HRESULT WINAPI string_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{ return string_QueryInterface( &string_from_info( iface )->operation_iface, iid, out ); }
static ULONG WINAPI string_info_AddRef( IAsyncInfo *iface )
{ return string_AddRef( &string_from_info( iface )->operation_iface ); }
static ULONG WINAPI string_info_Release( IAsyncInfo *iface )
{ return string_Release( &string_from_info( iface )->operation_iface ); }
static HRESULT WINAPI string_info_GetIids( IAsyncInfo *iface, ULONG *count, IID **iids )
{ return string_GetIids( &string_from_info( iface )->operation_iface, count, iids ); }
static HRESULT WINAPI string_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *name )
{ return string_GetRuntimeClassName( &string_from_info( iface )->operation_iface, name ); }
static HRESULT WINAPI string_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{ return string_GetTrustLevel( &string_from_info( iface )->operation_iface, level ); }
static HRESULT WINAPI string_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{ if (!id) return E_POINTER; *id = string_from_info( iface )->id; return S_OK; }
static HRESULT WINAPI string_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{ if (!status) return E_POINTER; *status = string_from_info( iface )->status; return S_OK; }
static HRESULT WINAPI string_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error )
{ if (!error) return E_POINTER; *error = string_from_info( iface )->error; return S_OK; }
static HRESULT WINAPI string_info_Cancel( IAsyncInfo *iface ) { return S_OK; }
static HRESULT WINAPI string_info_Close( IAsyncInfo *iface )
{ string_from_info( iface )->status = ASYNC_CLOSED; return S_OK; }
static const IAsyncInfoVtbl string_info_vtbl =
{
    string_info_QueryInterface, string_info_AddRef, string_info_Release,
    string_info_GetIids, string_info_GetRuntimeClassName, string_info_GetTrustLevel,
    string_info_get_Id, string_info_get_Status, string_info_get_ErrorCode,
    string_info_Cancel, string_info_Close,
};

HRESULT http_async_string_create( HSTRING value, HRESULT error,
                                  IAsyncOperationWithProgress_HSTRING_UINT64 **out )
{
    struct async_string *async;
    HRESULT hr;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!(async = calloc( 1, sizeof(*async) ))) return E_OUTOFMEMORY;
    async->operation_iface.lpVtbl = &string_vtbl;
    async->info_iface.lpVtbl = &string_info_vtbl;
    async->ref = 1;
    async->status = FAILED(error) ? Error : Completed;
    async->error = error;
    do async->id = InterlockedIncrement( &next_async_id ); while (!async->id);
    InitializeCriticalSectionEx( &async->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    async->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": async_string.cs");
    if (FAILED(hr = WindowsDuplicateString( value, &async->value )))
    {
        string_Release( &async->operation_iface );
        return hr;
    }
    *out = &async->operation_iface;
    return S_OK;
}
