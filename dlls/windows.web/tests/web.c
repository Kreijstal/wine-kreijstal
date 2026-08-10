/*
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
#define COBJMACROS
#include "initguid.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "winsock2.h"
#include "ws2tcpip.h"

#include "roapi.h"

#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Data_Json
#include "windows.data.json.h"
#define WIDL_using_Windows_Storage_Streams
#define WIDL_using_Windows_Web_Http
#define WIDL_using_Windows_Web_Http_Headers
#include "windows.web.http.h"

#include "wine/test.h"

#define check_interface( obj, iid ) check_interface_( __LINE__, obj, iid )
static void check_interface_( unsigned int line, void *obj, const IID *iid )
{
    IUnknown *iface = obj;
    IUnknown *unk;
    HRESULT hr;

    hr = IUnknown_QueryInterface( iface, iid, (void **)&unk );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    IUnknown_Release( unk );
}

static void test_JsonArrayStatics(void)
{
    static const WCHAR *json_value_statics_name = L"Windows.Data.Json.JsonValue";
    static const WCHAR *json_array_name = L"Windows.Data.Json.JsonArray";
    IJsonValueStatics *json_value_statics = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    IInspectable *inspectable = (void *)0xdeadbeef;
    IJsonObject *child_object = (void *)0xdeadbeef;
    IJsonArray *child_array = (void *)0xdeadbeef;
    IJsonArray *json_array = (void *)0xdeadbeef;
    IJsonValue *json_value = (void *)0xdeadbeef;
    BOOLEAN child_boolean;
    HSTRING child_string;
    DOUBLE child_number;
    HSTRING str = NULL;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( json_value_statics_name, wcslen( json_value_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( json_value_statics_name ) );
        return;
    }

    hr = IActivationFactory_QueryInterface( factory, &IID_IJsonValueStatics, (void **)&json_value_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ref = IActivationFactory_Release( factory );

    hr = WindowsCreateString( json_array_name, wcslen( json_array_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( json_array_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );

    hr = IActivationFactory_QueryInterface( factory, &IID_IJsonArray, (void **)&json_array );
    ok( hr == E_NOINTERFACE, "got hr %#lx.\n", hr );

    hr = WindowsCreateString( json_array_name, wcslen( json_array_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoActivateInstance( str, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    hr = IInspectable_QueryInterface( inspectable, &IID_IJsonArray, (void **)&json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( inspectable, &IID_IAgileObject );

    hr = IJsonArray_GetObjectAt( json_array, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, NULL );
    ok( hr == E_INVALIDARG, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == E_BOUNDS, "got hr %#lx.\n", hr );

    IJsonArray_Release( json_array );
    IInspectable_Release( inspectable );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );

    hr = WindowsCreateString( L"[{}]", wcslen( L"[{}]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IJsonValue_GetArray( json_value, &json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( json_value );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonObject_Release( child_object );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    IJsonArray_Release( json_array );

    hr = WindowsCreateString( L"[[]]", wcslen( L"[[]]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IJsonValue_GetArray( json_value, &json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( json_value );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonArray_Release( child_array );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    IJsonArray_Release( json_array );

    hr = WindowsCreateString( L"[\"Hello, World!\"]", wcslen( L"[\"Hello, World!\"]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IJsonValue_GetArray( json_value, &json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( json_value );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( child_string );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    IJsonArray_Release( json_array );

    hr = WindowsCreateString( L"[12.6]", wcslen( L"[12.6]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IJsonValue_GetArray( json_value, &json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( json_value );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    IJsonArray_Release( json_array );

    hr = WindowsCreateString( L"[true]", wcslen( L"[true]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = IJsonValue_GetArray( json_value, &json_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( json_value );
    hr = IJsonArray_GetObjectAt( json_array, 0, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetArrayAt( json_array, 0, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetStringAt( json_array, 0, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetNumberAt( json_array, 0, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonArray_GetBooleanAt( json_array, 0, &child_boolean );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonArray_Release( json_array );

    IJsonValueStatics_Release( json_value_statics );
}

static void test_JsonObjectStatics(void)
{
    static const WCHAR *json_value_statics_name = L"Windows.Data.Json.JsonValue";
    static const WCHAR *json_object_name = L"Windows.Data.Json.JsonObject";
    IJsonValueStatics *json_value_statics = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    IInspectable *inspectable = (void *)0xdeadbeef;
    IJsonObject *child_object = (void *)0xdeadbeef;
    IJsonObject *json_object = (void *)0xdeadbeef;
    IJsonArray *child_array = (void *)0xdeadbeef;
    IJsonValue *child_value = (void *)0xdeadbeef;
    BOOLEAN child_boolean;
    HSTRING child_string;
    DOUBLE child_number;
    HSTRING str = NULL;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( json_value_statics_name, wcslen( json_value_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( json_value_statics_name ) );
        return;
    }

    hr = IActivationFactory_QueryInterface( factory, &IID_IJsonValueStatics, (void **)&json_value_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ref = IActivationFactory_Release( factory );

    hr = WindowsCreateString( json_object_name, wcslen( json_object_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( json_object_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );

    hr = IActivationFactory_QueryInterface( factory, &IID_IJsonObject, (void **)&json_object );
    ok( hr == E_NOINTERFACE, "got hr %#lx.\n", hr );

    hr = WindowsCreateString( json_object_name, wcslen( json_object_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = RoActivateInstance( str, &inspectable );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    hr = IInspectable_QueryInterface( inspectable, &IID_IJsonObject, (void **)&json_object );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    check_interface( inspectable, &IID_IAgileObject );
    IInspectable_Release( inspectable );

    hr = WindowsCreateString( L"key", wcslen( L"key" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    /* key pair does not exist */

    hr = IJsonObject_GetNamedValue( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedValue( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedValue( json_object, NULL, &child_value );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    hr = IJsonObject_GetNamedObject( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedObject( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedObject( json_object, NULL, &child_object );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    hr = IJsonObject_GetNamedArray( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, NULL, &child_array );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    hr = IJsonObject_GetNamedString( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, NULL, &child_string );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    hr = IJsonObject_GetNamedNumber( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, NULL, &child_number );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    hr = IJsonObject_GetNamedBoolean( json_object, NULL, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, NULL, &child_boolean );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == WEB_E_JSON_VALUE_NOT_FOUND, "got hr %#lx.\n", hr );

    /* key pair exists */

    WindowsDeleteString( str );
    hr = WindowsCreateString( L"{}", wcslen( L"{}" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = WindowsCreateString( L"key", wcslen( L"key" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_SetNamedValue( json_object, str, child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonObject_Release( child_object );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

    WindowsDeleteString( str );
    hr = WindowsCreateString( L"[]", wcslen( L"[]" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    hr = WindowsCreateString( L"key", wcslen( L"key" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_SetNamedValue( json_object, str, child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonArray_Release( child_array );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

    hr = IJsonValueStatics_CreateStringValue( json_value_statics, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_SetNamedValue( json_object, str, child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( child_string );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

    hr = IJsonValueStatics_CreateNumberValue( json_value_statics, 10, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_SetNamedValue( json_object, str, child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

    hr = IJsonValueStatics_CreateBooleanValue( json_value_statics, FALSE, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonObject_SetNamedValue( json_object, str, child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedValue( json_object, str, &child_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    IJsonValue_Release( child_value );
    hr = IJsonObject_GetNamedObject( json_object, str, &child_object );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedArray( json_object, str, &child_array );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedString( json_object, str, &child_string );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedNumber( json_object, str, &child_number );
    ok( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
    hr = IJsonObject_GetNamedBoolean( json_object, str, &child_boolean );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    IJsonObject_Release( json_object );
    IJsonValueStatics_Release( json_value_statics );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

#define check_json( json_value_statics, json, expected_json_value_type, valid ) check_json_( __LINE__, json_value_statics, json, expected_json_value_type, valid )
static void check_json_( unsigned int line, IJsonValueStatics *json_value_statics, const WCHAR *json, JsonValueType expected_json_value_type, boolean valid )
{
    HSTRING str = NULL, parsed_str = NULL, empty_space = NULL;
    IJsonObject *json_object = (void *)0xdeadbeef;
    IJsonArray *json_array = (void *)0xdeadbeef;
    IJsonValue *json_value = (void *)0xdeadbeef;
    boolean parsed_boolean, expected_boolean;
    JsonValueType json_value_type;
    DOUBLE parsed_num;
    HRESULT hr;
    LONG ref;
    int res;

    hr = WindowsCreateString( json, wcslen( json ), &str );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    if (!valid)
    {
        if (expected_json_value_type == JsonValueType_Number)
            ok_(__FILE__, line)( hr == WEB_E_INVALID_JSON_NUMBER, "got hr %#lx.\n", hr );
        else
            ok_(__FILE__, line)( hr == WEB_E_INVALID_JSON_STRING, "got hr %#lx.\n", hr );

        WindowsDeleteString( str );
        return;
    }

    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    if (FAILED(hr)) return;
    hr = IJsonValue_get_ValueType( json_value, &json_value_type );
    ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
    ok_(__FILE__, line)( json_value_type == expected_json_value_type, "got json_value_type %d.\n", json_value_type );

    switch (expected_json_value_type)
    {
        case JsonValueType_Null:
            hr = IJsonValue_GetString( json_value, NULL );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetString( json_value, &parsed_str );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

            hr = IJsonValue_GetNumber( json_value, NULL );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetNumber( json_value, &parsed_num );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

            hr = IJsonValue_GetBoolean( json_value, NULL );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetBoolean( json_value, &parsed_boolean );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );

            hr = IJsonValue_GetArray( json_value, NULL );
            ok_(__FILE__, line)( hr == E_POINTER, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetArray( json_value, &json_array );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
            if (hr == S_OK) IJsonArray_Release( json_array );

            hr = IJsonValue_GetObject( json_value, NULL );
            ok_(__FILE__, line)( hr == E_POINTER, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetObject( json_value, &json_object );
            ok_(__FILE__, line)( hr == E_ILLEGAL_METHOD_CALL, "got hr %#lx.\n", hr );
            if (hr == S_OK) IJsonObject_Release( json_object );
            break;
        case JsonValueType_Boolean:
            hr = WindowsCreateString( L" ", wcslen( L" " ), &empty_space );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            hr = WindowsTrimStringStart( str, empty_space, &parsed_str );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            hr = WindowsTrimStringEnd( parsed_str, empty_space, &parsed_str );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            expected_boolean = !wcscmp( L"true", WindowsGetStringRawBuffer( parsed_str, NULL ) );

            hr = IJsonValue_GetBoolean( json_value, NULL );
            ok_(__FILE__, line)( hr == E_POINTER, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetBoolean( json_value, &parsed_boolean );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            ok_(__FILE__, line)( parsed_boolean == expected_boolean, "boolean mismatch, got %d, expected %d.\n", parsed_boolean, expected_boolean );
            break;
        case JsonValueType_Number:
            parsed_num = 0xdeadbeef;
            hr = IJsonValue_GetNumber( json_value, NULL );
            ok_(__FILE__, line)( hr == E_POINTER, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetNumber( json_value, &parsed_num );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            ok_(__FILE__, line)( parsed_num != 0xdeadbeef, "failed to get parsed_num\n" );
            break;
        case JsonValueType_String:
            hr = IJsonValue_GetString( json_value, NULL );
            ok_(__FILE__, line)( hr == E_POINTER, "got hr %#lx.\n", hr );
            hr = IJsonValue_GetString( json_value, &parsed_str );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            hr = WindowsCompareStringOrdinal( str, parsed_str, &res );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            ok_(__FILE__, line)( res != 0, "got same HSTRINGS str = %s, parsed_str = %s.\n", wine_dbgstr_hstring( str ), wine_dbgstr_hstring( parsed_str ) );
            break;
        case JsonValueType_Array:
            hr = IJsonValue_GetArray( json_value, &json_array );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            if (hr == S_OK) IJsonArray_Release( json_array );
            break;
        case JsonValueType_Object:
            hr = IJsonValue_GetObject( json_value, &json_object );
            ok_(__FILE__, line)( hr == S_OK, "got hr %#lx.\n", hr );
            if (hr == S_OK) IJsonObject_Release( json_object );
            break;
    }

    WindowsDeleteString( empty_space );
    WindowsDeleteString( parsed_str );
    WindowsDeleteString( str );
    ref = IJsonValue_Release( json_value );
    ok_(__FILE__, line)( ref == 0, "got ref %ld.\n", ref );
}

WCHAR *create_non_null_terminated( const WCHAR *str )
{
    UINT len = wcslen( str );
    WCHAR *buffer = malloc( (len + 1) * sizeof( WCHAR ) );
    if (buffer)
    {
        memcpy( buffer, str, len * sizeof( WCHAR ) );
        buffer[len] = 1;
        return buffer;
    }
    trace( "create_non_null_terminated failed to return a string\n" );
    return NULL;
}

#define check_non_null_terminated_json( json_value_statics, json, expected_json_value_type ) \
        check_non_null_terminated_json_( __LINE__, json_value_statics, json, expected_json_value_type )
static void check_non_null_terminated_json_( unsigned int line, IJsonValueStatics *json_value_statics, const WCHAR *json, JsonValueType expected_json_value_type )
{
    WCHAR *str = create_non_null_terminated( json );
    check_json_( line, json_value_statics, str, expected_json_value_type, FALSE );
    free( str );
}

static void test_JsonValueStatics(void)
{
    static const WCHAR *json_value_statics_name = L"Windows.Data.Json.JsonValue";
    IJsonValueStatics *json_value_statics = (void *)0xdeadbeef;
    IActivationFactory *factory = (void *)0xdeadbeef;
    IJsonValue *json_value = (void *)0xdeadbeef;
    JsonValueType json_value_type;
    HSTRING str = NULL;
    const WCHAR *json;
    HRESULT hr;
    LONG ref;

    hr = WindowsCreateString( json_value_statics_name, wcslen( json_value_statics_name ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = RoGetActivationFactory( str, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( str );
    ok( hr == S_OK || broken( hr == REGDB_E_CLASSNOTREG ), "got hr %#lx.\n", hr );
    if (hr == REGDB_E_CLASSNOTREG)
    {
        win_skip( "%s runtimeclass not registered, skipping tests.\n", wine_dbgstr_w( json_value_statics_name ) );
        return;
    }

    check_interface( factory, &IID_IUnknown );
    check_interface( factory, &IID_IInspectable );
    check_interface( factory, &IID_IAgileObject );

    hr = IActivationFactory_QueryInterface( factory, &IID_IJsonValueStatics, (void **)&json_value_statics );
    ok( hr == S_OK, "got hr %#lx.\n", hr );

    hr = IJsonValueStatics_CreateBooleanValue( json_value_statics, FALSE, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_CreateBooleanValue( json_value_statics, FALSE, &json_value );
    ok( hr == S_OK, "got hr %#lx,\n", hr );
    hr = IJsonValue_get_ValueType( json_value, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValue_get_ValueType( json_value, &json_value_type );
    ok( json_value_type == JsonValueType_Boolean, "got JsonValueType %d.\n", json_value_type );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ref = IJsonValue_Release( json_value );
    ok( ref == 0, "got ref %ld.\n", ref );

    hr = IJsonValueStatics_CreateNumberValue( json_value_statics, 0, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_CreateNumberValue( json_value_statics, 0, &json_value );
    ok( hr == S_OK, "got hr %#lx,\n", hr );
    hr = IJsonValue_get_ValueType( json_value, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValue_get_ValueType( json_value, &json_value_type );
    ok( json_value_type == JsonValueType_Number, "got JsonValueType %d.\n", json_value_type );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ref = IJsonValue_Release( json_value );
    ok( ref == 0, "got ref %ld.\n", ref );

    hr = IJsonValueStatics_CreateStringValue( json_value_statics, NULL, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValue_get_ValueType( json_value, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValue_get_ValueType( json_value, &json_value_type );
    ok( json_value_type == JsonValueType_String, "got JsonValueType %d.\n", json_value_type );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    ref = IJsonValue_Release( json_value );
    ok( ref == 0, "got ref %ld.\n", ref );
    hr = WindowsCreateString( L"Wine", wcslen( L"Wine" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_CreateStringValue( json_value_statics, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_CreateStringValue( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValue_get_ValueType( json_value, &json_value_type );
    ok( json_value_type == JsonValueType_String, "got JsonValueType %d.\n", json_value_type );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    ref = IJsonValue_Release( json_value );
    ok( ref == 0, "got ref %ld.\n", ref );

    hr = IJsonValueStatics_Parse( json_value_statics, NULL, &json_value );
    ok( hr == WEB_E_INVALID_JSON_STRING, "got hr %#lx.\n", hr );
    hr = WindowsCreateString( L"Wine", wcslen( L"Wine" ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, NULL );
    ok( hr == E_POINTER, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == WEB_E_INVALID_JSON_STRING, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    /* Valid JSON */

    json = L"\"Wine\\\"\"";
    hr = WindowsCreateString( json, wcslen( json ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    if (SUCCEEDED(hr))
    {
        HSTRING parsed_str = NULL;
        int res;

        json = L"Wine\"";
        hr = WindowsCreateString( json, wcslen( json ), &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = IJsonValue_GetString( json_value, &parsed_str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = WindowsCompareStringOrdinal( str, parsed_str, &res );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( res == 0, "got different HSTRINGS str = %s, parsed_str = %s.\n", wine_dbgstr_hstring( str ), wine_dbgstr_hstring( parsed_str ) );

        WindowsDeleteString( parsed_str );
        WindowsDeleteString( str );
        IJsonValue_Release( json_value );
    }

    json = L"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\u0057\\u0069\\u006e\\u0065\\udAbC\\uDcEf\"";
    hr = WindowsCreateString( json, wcslen( json ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );
    if (SUCCEEDED(hr))
    {
        HSTRING parsed_str = NULL;
        int res;

        json = L"\"\\/\b\f\n\r\t\0Wine\U000BF0EF";
        hr = WindowsCreateString( json, 15, &str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = IJsonValue_GetString( json_value, &parsed_str );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        hr = WindowsCompareStringOrdinal( str, parsed_str, &res );
        ok( hr == S_OK, "got hr %#lx.\n", hr );
        ok( res == 0, "got different HSTRINGS str = %s, parsed_str = %s.\n", wine_dbgstr_hstring( str ), wine_dbgstr_hstring( parsed_str ) );

        WindowsDeleteString( parsed_str );
        WindowsDeleteString( str );
        IJsonValue_Release( json_value );
    }

    json = L"null";
    check_json( json_value_statics, json, JsonValueType_Null, TRUE );
    json = L"false";
    check_json( json_value_statics, json, JsonValueType_Boolean, TRUE );
    json = L" true ";
    check_json( json_value_statics, json, JsonValueType_Boolean, TRUE );
    json = L"\"true\"";
    check_json( json_value_statics, json, JsonValueType_String, TRUE );
    json = L" 9.22 ";
    check_json( json_value_statics, json, JsonValueType_Number, TRUE );
    json = L" \"The Wine     Project\"";
    check_json( json_value_statics, json, JsonValueType_String, TRUE );
    json = L"\r\t\n \"The Wine     Project\"";
    check_json( json_value_statics, json, JsonValueType_String, TRUE );
    json = L"[\"Wine\", \"Linux\"]";
    check_json( json_value_statics, json, JsonValueType_Array, TRUE );
    json = L"{"
            "    \"Wine\": \"The Wine Project\","
            "    \"Linux\": [\"Arch\", \"BTW\"]"
            "}";
    check_json( json_value_statics, json, JsonValueType_Object, TRUE );

    /* Invalid JSON */

    json = L"null";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_Null );
    json = L"false";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_Boolean );
    json = L" true ";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_Boolean );
    json = L"\"true\"";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_String );
    json = L" 9.22 ";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_String );
    json = L" \"Wine\"";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_String );
    json = L"[\"Wine\", \"Linux\"]";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_Array );
    json = L"{"
            "    \"Wine\": \"The Wine Project\","
            "    \"Linux\": [\"Arch\", \"BTW\"]"
            "}";
    check_non_null_terminated_json( json_value_statics, json, JsonValueType_Object );

    json = L"\" \"\"";
    hr = WindowsCreateString( json, wcslen( json ), &str );
    ok( hr == S_OK, "got hr %#lx.\n", hr );
    hr = IJsonValueStatics_Parse( json_value_statics, str, &json_value );
    ok( hr == WEB_E_INVALID_JSON_STRING, "got hr %#lx.\n", hr );
    WindowsDeleteString( str );

    json = L"True";
    check_json( json_value_statics, json, JsonValueType_Boolean, FALSE );
    json = L"1.7976931348623158e+3080";
    check_json( json_value_statics, json, JsonValueType_Number, FALSE );
    json = L"2.2250738585072014e-3080";
    check_json( json_value_statics, json, JsonValueType_Number, FALSE );
    json = L" \"Wine\":";
    check_json( json_value_statics, json, JsonValueType_String, FALSE );
    json = L" \"The Wine \t Project\"";
    check_json( json_value_statics, json, JsonValueType_String, FALSE );
    json = L"\v \"The Wine     Project\"";
    check_json( json_value_statics, json, JsonValueType_String, FALSE );
    json = L"\"\\\"";
    check_json( json_value_statics, json, JsonValueType_String, FALSE );
    json = L"\"\\u123\"";
    check_json( json_value_statics, json, JsonValueType_String, FALSE );
    json = L"[\"Wine\" \"Linux\"]";
    check_json( json_value_statics, json, JsonValueType_Array, FALSE );
    json = L"[\"Wine\", \"Linux\",]";
    check_json( json_value_statics, json, JsonValueType_Array, FALSE );
    json = L"{"
            "    \"Wine\": \"The Wine Project\","
            "    \"Linux\": [\"Arch\", \"BTW\"]"
            "";
    check_json( json_value_statics, json, JsonValueType_Object, FALSE );
    json = L"{"
            "    \"Wine\": \"The Wine Project\","
            "    \"Linux\": [\"Arch\", \"BTW\"],"
            "}";
    check_json( json_value_statics, json, JsonValueType_Object, FALSE );

    ref = IJsonValueStatics_Release( json_value_statics );
    ok( ref == 2, "got ref %ld.\n", ref );
    ref = IActivationFactory_Release( factory );
    ok( ref == 1, "got ref %ld.\n", ref );
}

struct http_test_server
{
    SOCKET listener;
    char request[8192];
    unsigned int request_size;
};

static DWORD WINAPI http_test_server_proc( void *parameter )
{
    static const char response[] = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\n"
            "Content-Length: 17\r\nConnection: close\r\n\r\n{\"result\":\"wine\"}";
    struct http_test_server *server = parameter;
    SOCKET client;
    int received;

    if ((client = accept( server->listener, NULL, NULL )) == INVALID_SOCKET) return 1;
    while (server->request_size < sizeof(server->request) - 1)
    {
        unsigned int left = sizeof(server->request) - 1 - server->request_size;
        received = recv( client, server->request + server->request_size, left, 0 );
        if (received <= 0) break;
        server->request_size += received;
        server->request[server->request_size] = 0;
        if (strstr( server->request, "\r\n\r\n" ) && strstr( server->request, "{\"hello\":\"world\"}" )) break;
    }
    send( client, response, sizeof(response) - 1, 0 );
    shutdown( client, SD_BOTH );
    closesocket( client );
    return 0;
}

static HRESULT get_activation_interface( const WCHAR *name, const IID *iid, void **value )
{
    IActivationFactory *factory;
    HSTRING class_name;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &class_name ))) return hr;
    hr = RoGetActivationFactory( class_name, &IID_IActivationFactory, (void **)&factory );
    WindowsDeleteString( class_name );
    if (FAILED(hr)) return hr;
    hr = IActivationFactory_QueryInterface( factory, iid, value );
    IActivationFactory_Release( factory );
    return hr;
}

static HRESULT activate_instance_interface( const WCHAR *name, const IID *iid, void **value )
{
    IInspectable *instance;
    HSTRING class_name;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = WindowsCreateString( name, wcslen( name ), &class_name ))) return hr;
    hr = RoActivateInstance( class_name, &instance );
    WindowsDeleteString( class_name );
    if (FAILED(hr)) return hr;
    hr = IInspectable_QueryInterface( instance, iid, value );
    IInspectable_Release( instance );
    return hr;
}

static HRESULT create_uri( const WCHAR *text, IUriRuntimeClass **value )
{
    IUriRuntimeClassFactory *factory;
    HSTRING string;
    HRESULT hr;

    *value = NULL;
    if (FAILED(hr = get_activation_interface( RuntimeClass_Windows_Foundation_Uri,
            &IID_IUriRuntimeClassFactory, (void **)&factory ))) return hr;
    if (SUCCEEDED(hr = WindowsCreateString( text, wcslen( text ), &string )))
    {
        hr = IUriRuntimeClassFactory_CreateUri( factory, string, value );
        WindowsDeleteString( string );
    }
    IUriRuntimeClassFactory_Release( factory );
    return hr;
}

static HRESULT wait_response( IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *operation,
        IHttpResponseMessage **response )
{
    IAsyncInfo *info;
    AsyncStatus status = Started;
    HRESULT hr;
    unsigned int i;

    *response = NULL;
    if (FAILED(hr = IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_QueryInterface(
            operation, &IID_IAsyncInfo, (void **)&info ))) return hr;
    for (i = 0; i < 500 && status == Started; ++i)
    {
        Sleep( 10 );
        if (FAILED(hr = IAsyncInfo_get_Status( info, &status ))) break;
    }
    if (SUCCEEDED(hr) && status != Completed)
    {
        if (status == Error) IAsyncInfo_get_ErrorCode( info, &hr );
        else hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
    }
    IAsyncInfo_Release( info );
    if (SUCCEEDED(hr)) hr = IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_GetResults(
            operation, response );
    return hr;
}

static HRESULT wait_string( IAsyncOperationWithProgress_HSTRING_UINT64 *operation, HSTRING *value )
{
    IAsyncInfo *info;
    AsyncStatus status = Started;
    HRESULT hr;
    unsigned int i;

    *value = NULL;
    if (FAILED(hr = IAsyncOperationWithProgress_HSTRING_UINT64_QueryInterface(
            operation, &IID_IAsyncInfo, (void **)&info ))) return hr;
    for (i = 0; i < 500 && status == Started; ++i)
    {
        Sleep( 10 );
        if (FAILED(hr = IAsyncInfo_get_Status( info, &status ))) break;
    }
    if (SUCCEEDED(hr) && status != Completed) hr = HRESULT_FROM_WIN32( ERROR_TIMEOUT );
    IAsyncInfo_Release( info );
    if (SUCCEEDED(hr)) hr = IAsyncOperationWithProgress_HSTRING_UINT64_GetResults( operation, value );
    return hr;
}

struct form_test_values
{
    IIterable_IKeyValuePair_HSTRING_HSTRING iterable_iface;
    IIterator_IKeyValuePair_HSTRING_HSTRING iterator_iface;
    IKeyValuePair_HSTRING_HSTRING pair_iface;
    HSTRING key, value;
    unsigned int index;
};

static struct form_test_values form_values;

static HRESULT form_values_qi( void *iface, REFIID iid, void **out )
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualGUID( iid, &IID_IUnknown ) || IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) || IsEqualGUID( iid, &IID_IIterable_IKeyValuePair_HSTRING_HSTRING ))
        *out = &form_values.iterable_iface;
    else if (IsEqualGUID( iid, &IID_IIterator_IKeyValuePair_HSTRING_HSTRING ))
        *out = &form_values.iterator_iface;
    else if (IsEqualGUID( iid, &IID_IKeyValuePair_HSTRING_HSTRING ))
        *out = &form_values.pair_iface;
    else return E_NOINTERFACE;
    IUnknown_AddRef( (IUnknown *)*out );
    return S_OK;
}
static ULONG form_values_addref( void *iface ) { return 2; }
static ULONG form_values_release( void *iface ) { return 1; }
static HRESULT form_values_iids( void *iface, ULONG *count, IID **iids ) { return E_NOTIMPL; }
static HRESULT form_values_name( void *iface, HSTRING *name ) { return E_NOTIMPL; }
static HRESULT form_values_trust( void *iface, TrustLevel *level )
{ if (!level) return E_POINTER; *level = BaseTrust; return S_OK; }

static HRESULT WINAPI form_iterable_First( IIterable_IKeyValuePair_HSTRING_HSTRING *iface,
        IIterator_IKeyValuePair_HSTRING_HSTRING **value )
{
    if (!value) return E_POINTER;
    form_values.index = 0;
    *value = &form_values.iterator_iface;
    IIterator_IKeyValuePair_HSTRING_HSTRING_AddRef( *value );
    return S_OK;
}
static const IIterable_IKeyValuePair_HSTRING_HSTRINGVtbl form_iterable_vtbl =
{
    (void *)form_values_qi, (void *)form_values_addref, (void *)form_values_release,
    (void *)form_values_iids, (void *)form_values_name, (void *)form_values_trust,
    form_iterable_First,
};

static HRESULT WINAPI form_iterator_Current( IIterator_IKeyValuePair_HSTRING_HSTRING *iface,
        IKeyValuePair_HSTRING_HSTRING **value )
{
    if (!value) return E_POINTER;
    *value = NULL;
    if (form_values.index) return E_BOUNDS;
    *value = &form_values.pair_iface;
    IKeyValuePair_HSTRING_HSTRING_AddRef( *value );
    return S_OK;
}
static HRESULT WINAPI form_iterator_HasCurrent( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{ if (!value) return E_POINTER; *value = !form_values.index; return S_OK; }
static HRESULT WINAPI form_iterator_MoveNext( IIterator_IKeyValuePair_HSTRING_HSTRING *iface, boolean *value )
{ if (!value) return E_POINTER; form_values.index = 1; *value = FALSE; return S_OK; }
static HRESULT WINAPI form_iterator_GetMany( IIterator_IKeyValuePair_HSTRING_HSTRING *iface,
        UINT32 capacity, IKeyValuePair_HSTRING_HSTRING **value, UINT32 *actual )
{ if (!actual) return E_POINTER; *actual = 0; return S_OK; }
static const IIterator_IKeyValuePair_HSTRING_HSTRINGVtbl form_iterator_vtbl =
{
    (void *)form_values_qi, (void *)form_values_addref, (void *)form_values_release,
    (void *)form_values_iids, (void *)form_values_name, (void *)form_values_trust,
    form_iterator_Current, form_iterator_HasCurrent, form_iterator_MoveNext, form_iterator_GetMany,
};

static HRESULT WINAPI form_pair_get_Key( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *value )
{ if (!value) return E_POINTER; return WindowsDuplicateString( form_values.key, value ); }
static HRESULT WINAPI form_pair_get_Value( IKeyValuePair_HSTRING_HSTRING *iface, HSTRING *value )
{ if (!value) return E_POINTER; return WindowsDuplicateString( form_values.value, value ); }
static const IKeyValuePair_HSTRING_HSTRINGVtbl form_pair_vtbl =
{
    (void *)form_values_qi, (void *)form_values_addref, (void *)form_values_release,
    (void *)form_values_iids, (void *)form_values_name, (void *)form_values_trust,
    form_pair_get_Key, form_pair_get_Value,
};

static void test_HttpFormUrlEncodedContent(void)
{
    IHttpFormUrlEncodedContentFactory *factory = NULL;
    IAsyncOperationWithProgress_HSTRING_UINT64 *operation = NULL;
    IHttpContent *content = NULL;
    HSTRING result = NULL;
    HRESULT hr;

    form_values.iterable_iface.lpVtbl = &form_iterable_vtbl;
    form_values.iterator_iface.lpVtbl = &form_iterator_vtbl;
    form_values.pair_iface.lpVtbl = &form_pair_vtbl;
    WindowsCreateString( L"scope", 5, &form_values.key );
    WindowsCreateString( L"a b&c", 5, &form_values.value );
    hr = get_activation_interface( RuntimeClass_Windows_Web_Http_HttpFormUrlEncodedContent,
            &IID_IHttpFormUrlEncodedContentFactory, (void **)&factory );
    ok( hr == S_OK, "form factory failed %#lx.\n", hr );
    if (SUCCEEDED(hr)) hr = IHttpFormUrlEncodedContentFactory_Create(
            factory, &form_values.iterable_iface, &content );
    ok( hr == S_OK, "form Create failed %#lx.\n", hr );
    if (SUCCEEDED(hr)) hr = IHttpContent_ReadAsStringAsync( content, &operation );
    ok( hr == S_OK, "form ReadAsStringAsync failed %#lx.\n", hr );
    if (SUCCEEDED(hr)) hr = wait_string( operation, &result );
    ok( hr == S_OK, "form read failed %#lx.\n", hr );
    ok( result && !wcscmp( WindowsGetStringRawBuffer( result, NULL ), L"scope=a+b%26c" ),
            "unexpected encoded form %s.\n", debugstr_hstring( result ) );
    WindowsDeleteString( result );
    if (operation) IAsyncOperationWithProgress_HSTRING_UINT64_Release( operation );
    if (content) IHttpContent_Release( content );
    if (factory) IHttpFormUrlEncodedContentFactory_Release( factory );
    WindowsDeleteString( form_values.key );
    WindowsDeleteString( form_values.value );
    memset( &form_values, 0, sizeof(form_values) );
}

static void test_HttpClient_loopback(void)
{
    struct http_test_server server = {INVALID_SOCKET};
    IHttpMediaTypeWithQualityHeaderValueCollection *accept = NULL;
    IHttpProductInfoHeaderValueCollection *user_agent = NULL;
    IHttpCredentialsHeaderValueFactory *credentials_factory = NULL;
    IHttpCredentialsHeaderValue *credentials = NULL;
    IHttpRequestHeaderCollection *default_headers = NULL, *request_headers = NULL;
    IHttpRequestMessageFactory *request_factory = NULL;
    IHttpStringContentFactory *content_factory = NULL;
    IHttpMethodStatics *method_statics = NULL;
    IHttpClient *client = NULL;
    IHttpMethod *post = NULL;
    IHttpRequestMessage *request = NULL;
    IHttpResponseMessage *response = NULL;
    IHttpContent *content = NULL, *response_content = NULL;
    IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress *response_operation = NULL;
    IAsyncOperationWithProgress_HSTRING_UINT64 *string_operation = NULL;
    IUriRuntimeClass *uri = NULL;
    SOCKADDR_IN address = {0};
    WSADATA wsadata;
    HANDLE thread = NULL;
    HSTRING string = NULL, result = NULL;
    HttpStatusCode status;
    boolean parsed, success;
    WCHAR url[128];
    int address_size = sizeof(address);
    HRESULT hr;

    hr = WSAStartup( MAKEWORD(2, 2), &wsadata );
    ok( !hr, "WSAStartup failed %ld.\n", hr );
    if (hr) return;
    server.listener = socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );
    ok( server.listener != INVALID_SOCKET, "socket failed %d.\n", WSAGetLastError() );
    if (server.listener == INVALID_SOCKET) goto done;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
    hr = bind( server.listener, (SOCKADDR *)&address, sizeof(address) );
    ok( !hr, "bind failed %d.\n", WSAGetLastError() );
    hr = listen( server.listener, 1 );
    ok( !hr, "listen failed %d.\n", WSAGetLastError() );
    hr = getsockname( server.listener, (SOCKADDR *)&address, &address_size );
    ok( !hr, "getsockname failed %d.\n", WSAGetLastError() );
    swprintf( url, ARRAY_SIZE(url), L"http://127.0.0.1:%u/azure?api=1", ntohs( address.sin_port ) );
    thread = CreateThread( NULL, 0, http_test_server_proc, &server, 0, NULL );
    ok( !!thread, "CreateThread failed %lu.\n", GetLastError() );

    hr = activate_instance_interface( RuntimeClass_Windows_Web_Http_HttpClient,
            &IID_IHttpClient, (void **)&client );
    ok( hr == S_OK, "HttpClient activation failed %#lx.\n", hr );
    hr = get_activation_interface( RuntimeClass_Windows_Web_Http_HttpMethod,
            &IID_IHttpMethodStatics, (void **)&method_statics );
    ok( hr == S_OK, "HttpMethod statics failed %#lx.\n", hr );
    hr = get_activation_interface( RuntimeClass_Windows_Web_Http_HttpRequestMessage,
            &IID_IHttpRequestMessageFactory, (void **)&request_factory );
    ok( hr == S_OK, "HttpRequestMessage factory failed %#lx.\n", hr );
    hr = get_activation_interface( RuntimeClass_Windows_Web_Http_HttpStringContent,
            &IID_IHttpStringContentFactory, (void **)&content_factory );
    ok( hr == S_OK, "HttpStringContent factory failed %#lx.\n", hr );
    hr = get_activation_interface( RuntimeClass_Windows_Web_Http_Headers_HttpCredentialsHeaderValue,
            &IID_IHttpCredentialsHeaderValueFactory, (void **)&credentials_factory );
    ok( hr == S_OK, "credentials factory failed %#lx.\n", hr );
    if (FAILED(hr) || !client || !method_statics || !request_factory || !content_factory) goto done;

    hr = IHttpClient_get_DefaultRequestHeaders( client, &default_headers );
    ok( hr == S_OK, "get_DefaultRequestHeaders failed %#lx.\n", hr );
    hr = IHttpRequestHeaderCollection_get_UserAgent( default_headers, &user_agent );
    ok( hr == S_OK, "get_UserAgent failed %#lx.\n", hr );
    WindowsCreateString( L"OpenTerminal/1.0", 16, &string );
    hr = IHttpProductInfoHeaderValueCollection_TryParseAdd( user_agent, string, &parsed );
    ok( hr == S_OK && parsed, "UserAgent TryParseAdd failed %#lx, %u.\n", hr, parsed );
    WindowsDeleteString( string ); string = NULL;

    hr = IHttpMethodStatics_get_Post( method_statics, &post );
    ok( hr == S_OK, "get_Post failed %#lx.\n", hr );
    hr = create_uri( url, &uri );
    ok( hr == S_OK, "create_uri failed %#lx.\n", hr );
    hr = IHttpRequestMessageFactory_Create( request_factory, post, uri, &request );
    ok( hr == S_OK, "request Create failed %#lx.\n", hr );
    WindowsCreateString( L"{\"hello\":\"world\"}", 17, &string );
    hr = IHttpStringContentFactory_CreateFromString( content_factory, string, &content );
    ok( hr == S_OK, "string content Create failed %#lx.\n", hr );
    WindowsDeleteString( string ); string = NULL;
    hr = IHttpRequestMessage_put_Content( request, content );
    ok( hr == S_OK, "put_Content failed %#lx.\n", hr );
    hr = IHttpRequestMessage_get_Headers( request, &request_headers );
    ok( hr == S_OK, "get_Headers failed %#lx.\n", hr );
    hr = IHttpRequestHeaderCollection_get_Accept( request_headers, &accept );
    ok( hr == S_OK, "get_Accept failed %#lx.\n", hr );
    WindowsCreateString( L"application/json", 16, &string );
    hr = IHttpMediaTypeWithQualityHeaderValueCollection_TryParseAdd( accept, string, &parsed );
    ok( hr == S_OK && parsed, "Accept TryParseAdd failed %#lx, %u.\n", hr, parsed );
    WindowsDeleteString( string ); string = NULL;
    WindowsCreateString( L"Bearer", 6, &string );
    {
        HSTRING token;
        WindowsCreateString( L"token-123", 9, &token );
        hr = IHttpCredentialsHeaderValueFactory_CreateFromSchemeWithToken(
                credentials_factory, string, token, &credentials );
        WindowsDeleteString( token );
    }
    WindowsDeleteString( string ); string = NULL;
    ok( hr == S_OK, "credentials Create failed %#lx.\n", hr );
    hr = IHttpRequestHeaderCollection_put_Authorization( request_headers, credentials );
    ok( hr == S_OK, "put_Authorization failed %#lx.\n", hr );

    hr = IHttpClient_SendRequestAsync( client, request, &response_operation );
    ok( hr == S_OK, "SendRequestAsync failed %#lx.\n", hr );
    if (SUCCEEDED(hr)) hr = wait_response( response_operation, &response );
    ok( hr == S_OK, "async request failed %#lx.\n", hr );
    if (FAILED(hr)) goto done;
    hr = IHttpResponseMessage_get_StatusCode( response, &status );
    ok( hr == S_OK && status == HttpStatusCode_Created, "status %#x, hr %#lx.\n", status, hr );
    hr = IHttpResponseMessage_get_IsSuccessStatusCode( response, &success );
    ok( hr == S_OK && success, "success %u, hr %#lx.\n", success, hr );
    hr = IHttpResponseMessage_get_Content( response, &response_content );
    ok( hr == S_OK, "response get_Content failed %#lx.\n", hr );
    hr = IHttpContent_ReadAsStringAsync( response_content, &string_operation );
    ok( hr == S_OK, "ReadAsStringAsync failed %#lx.\n", hr );
    if (SUCCEEDED(hr)) hr = wait_string( string_operation, &result );
    ok( hr == S_OK, "read string failed %#lx.\n", hr );
    ok( result && !wcscmp( WindowsGetStringRawBuffer( result, NULL ), L"{\"result\":\"wine\"}" ),
            "unexpected body %s.\n", debugstr_hstring( result ) );

    ok( WaitForSingleObject( thread, 5000 ) == WAIT_OBJECT_0, "server did not finish.\n" );
    ok( strstr( server.request, "POST /azure?api=1 HTTP/1.1" ) != NULL,
            "unexpected request %s.\n", server.request );
    ok( strstr( server.request, "Authorization: Bearer token-123" ) != NULL,
            "missing authorization in %s.\n", server.request );
    ok( strstr( server.request, "Accept: application/json" ) != NULL,
            "missing accept in %s.\n", server.request );
    ok( strstr( server.request, "User-Agent: OpenTerminal/1.0" ) != NULL,
            "missing user agent in %s.\n", server.request );
    ok( strstr( server.request, "{\"hello\":\"world\"}" ) != NULL,
            "missing body in %s.\n", server.request );

done:
    WindowsDeleteString( result );
    WindowsDeleteString( string );
    if (string_operation) IAsyncOperationWithProgress_HSTRING_UINT64_Release( string_operation );
    if (response_operation) IAsyncOperationWithProgress_HttpResponseMessage_HttpProgress_Release( response_operation );
    if (response_content) IHttpContent_Release( response_content );
    if (response) IHttpResponseMessage_Release( response );
    if (accept) IHttpMediaTypeWithQualityHeaderValueCollection_Release( accept );
    if (request_headers) IHttpRequestHeaderCollection_Release( request_headers );
    if (credentials) IHttpCredentialsHeaderValue_Release( credentials );
    if (credentials_factory) IHttpCredentialsHeaderValueFactory_Release( credentials_factory );
    if (content) IHttpContent_Release( content );
    if (request) IHttpRequestMessage_Release( request );
    if (uri) IUriRuntimeClass_Release( uri );
    if (post) IHttpMethod_Release( post );
    if (content_factory) IHttpStringContentFactory_Release( content_factory );
    if (request_factory) IHttpRequestMessageFactory_Release( request_factory );
    if (method_statics) IHttpMethodStatics_Release( method_statics );
    if (user_agent) IHttpProductInfoHeaderValueCollection_Release( user_agent );
    if (default_headers) IHttpRequestHeaderCollection_Release( default_headers );
    if (client) IHttpClient_Release( client );
    if (thread) CloseHandle( thread );
    if (server.listener != INVALID_SOCKET) closesocket( server.listener );
    WSACleanup();
}

START_TEST(web)
{
    HRESULT hr;

    hr = RoInitialize( RO_INIT_MULTITHREADED );
    ok( hr == S_OK, "RoInitialize failed, hr %#lx\n", hr );

    test_JsonArrayStatics();
    test_JsonObjectStatics();
    test_JsonValueStatics();
    test_HttpFormUrlEncodedContent();
    test_HttpClient_loopback();

    RoUninitialize();
}
