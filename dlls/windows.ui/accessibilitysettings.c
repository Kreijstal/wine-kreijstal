/* WinRT Windows.UI.ViewManagement.AccessibilitySettings Implementation
 *
 * Copyright 2025 Zhiyi Zhang for CodeWeavers
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

#include "private.h"
#include "weakref.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(ui);

struct accessibilitysettings
{
    IAccessibilitySettings IAccessibilitySettings_iface;
    struct weak_reference_source weak_reference_source;
    CRITICAL_SECTION handlers_cs;
    struct list high_contrast_changed_handlers;
};

struct high_contrast_changed_handler
{
    struct list entry;
    EventRegistrationToken token;
    ITypedEventHandler_AccessibilitySettings_IInspectable *handler;
};

static LONG64 next_high_contrast_changed_token;

static inline struct accessibilitysettings *impl_from_IAccessibilitySettings(IAccessibilitySettings *iface)
{
    return CONTAINING_RECORD(iface, struct accessibilitysettings, IAccessibilitySettings_iface);
}

static HRESULT WINAPI accessibilitysettings_QueryInterface(IAccessibilitySettings *iface,
                                                           REFIID iid, void **out)
{
    struct accessibilitysettings *impl = impl_from_IAccessibilitySettings(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (!out) return E_POINTER;
    *out = NULL;

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IAgileObject)
        || IsEqualGUID(iid, &IID_IAccessibilitySettings))
    {
        *out = &impl->IAccessibilitySettings_iface;
    }
    else if (IsEqualGUID(iid, &IID_IWeakReferenceSource))
        *out = &impl->weak_reference_source.IWeakReferenceSource_iface;

    if (*out)
    {
        IUnknown_AddRef((IUnknown *)*out);
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI accessibilitysettings_AddRef(IAccessibilitySettings *iface)
{
    struct accessibilitysettings *impl = impl_from_IAccessibilitySettings(iface);
    ULONG ref = weak_reference_strong_add_ref(&impl->weak_reference_source);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI accessibilitysettings_Release(IAccessibilitySettings *iface)
{
    struct accessibilitysettings *impl = impl_from_IAccessibilitySettings(iface);
    struct high_contrast_changed_handler *handler, *next;
    struct list handlers = LIST_INIT(handlers);
    ULONG ref = weak_reference_strong_release(&impl->weak_reference_source);

    TRACE("iface %p, ref %lu.\n", iface, ref);

    if (!ref)
    {
        EnterCriticalSection(&impl->handlers_cs);
        list_move_tail(&handlers, &impl->high_contrast_changed_handlers);
        LeaveCriticalSection(&impl->handlers_cs);

        LIST_FOR_EACH_ENTRY_SAFE(handler, next, &handlers,
                struct high_contrast_changed_handler, entry)
        {
            list_remove(&handler->entry);
            ITypedEventHandler_AccessibilitySettings_IInspectable_Release(handler->handler);
            free(handler);
        }
        DeleteCriticalSection(&impl->handlers_cs);
        free(impl);
    }
    return ref;
}

static HRESULT WINAPI accessibilitysettings_GetIids(IAccessibilitySettings *iface, ULONG *iid_count,
                                                    IID **iids)
{
    IID *values;

    TRACE("iface %p, iid_count %p, iids %p.\n", iface, iid_count, iids);
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    if (!(values = CoTaskMemAlloc(sizeof(*values)))) return E_OUTOFMEMORY;
    values[0] = IID_IAccessibilitySettings;
    *iid_count = 1;
    *iids = values;
    return S_OK;
}

static HRESULT WINAPI accessibilitysettings_GetRuntimeClassName(IAccessibilitySettings *iface,
                                                                HSTRING *class_name)
{
    TRACE("iface %p, class_name %p.\n", iface, class_name);
    if (!class_name) return E_POINTER;
    return WindowsCreateString(RuntimeClass_Windows_UI_ViewManagement_AccessibilitySettings,
            ARRAY_SIZE(RuntimeClass_Windows_UI_ViewManagement_AccessibilitySettings) - 1,
            class_name);
}

static HRESULT WINAPI accessibilitysettings_GetTrustLevel(IAccessibilitySettings *iface,
                                                          TrustLevel *trust_level)
{
    TRACE("iface %p, trust_level %p.\n", iface, trust_level);
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI accessibilitysettings_get_HighContrast(IAccessibilitySettings *iface,
                                                             boolean *value)
{
    HIGHCONTRASTW high_contrast = {.cbSize = sizeof(high_contrast)};

    TRACE("iface %p, value %p.\n", iface, value);

    if (!value) return E_POINTER;

    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0))
        return E_FAIL;

    *value = !!(high_contrast.dwFlags & HCF_HIGHCONTRASTON);
    return S_OK;
}

static HRESULT WINAPI accessibilitysettings_get_HighContrastScheme(IAccessibilitySettings *iface,
                                                                   HSTRING *value)
{
    HIGHCONTRASTW high_contrast = {.cbSize = sizeof(high_contrast)};
    const WCHAR *scheme;

    TRACE("iface %p, value %p.\n", iface, value);
    if (!value) return E_POINTER;
    *value = NULL;
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(high_contrast), &high_contrast, 0))
        return E_FAIL;
    scheme = high_contrast.lpszDefaultScheme ? high_contrast.lpszDefaultScheme : L"";
    return WindowsCreateString(scheme, wcslen(scheme), value);
}

static HRESULT WINAPI accessibilitysettings_add_HighContrastChanged(IAccessibilitySettings *iface,
                                                                    ITypedEventHandler_AccessibilitySettings_IInspectable *handler,
                                                                    EventRegistrationToken *cookie)
{
    struct accessibilitysettings *impl = impl_from_IAccessibilitySettings(iface);
    struct high_contrast_changed_handler *entry;

    TRACE("iface %p, handler %p, cookie %p.\n", iface, handler, cookie);
    if (!handler) return E_INVALIDARG;
    if (!cookie) return E_POINTER;
    cookie->value = 0;
    if (!(entry = calloc(1, sizeof(*entry)))) return E_OUTOFMEMORY;
    ITypedEventHandler_AccessibilitySettings_IInspectable_AddRef(handler);
    entry->handler = handler;
    entry->token.value = InterlockedIncrement64(&next_high_contrast_changed_token);

    EnterCriticalSection(&impl->handlers_cs);
    list_add_tail(&impl->high_contrast_changed_handlers, &entry->entry);
    LeaveCriticalSection(&impl->handlers_cs);
    *cookie = entry->token;
    return S_OK;
}

static HRESULT WINAPI accessibilitysettings_remove_HighContrastChanged(IAccessibilitySettings *iface,
                                                                       EventRegistrationToken cookie)
{
    struct accessibilitysettings *impl = impl_from_IAccessibilitySettings(iface);
    struct high_contrast_changed_handler *entry;
    BOOL found = FALSE;

    TRACE("iface %p, cookie %I64x.\n", iface, cookie.value);
    EnterCriticalSection(&impl->handlers_cs);
    LIST_FOR_EACH_ENTRY(entry, &impl->high_contrast_changed_handlers,
            struct high_contrast_changed_handler, entry)
    {
        if (entry->token.value != cookie.value) continue;
        list_remove(&entry->entry);
        found = TRUE;
        break;
    }
    LeaveCriticalSection(&impl->handlers_cs);
    if (found)
    {
        ITypedEventHandler_AccessibilitySettings_IInspectable_Release(entry->handler);
        free(entry);
    }
    return S_OK;
}

static const struct IAccessibilitySettingsVtbl accessibilitysettings_vtbl =
{
    accessibilitysettings_QueryInterface,
    accessibilitysettings_AddRef,
    accessibilitysettings_Release,
    /* IInspectable methods */
    accessibilitysettings_GetIids,
    accessibilitysettings_GetRuntimeClassName,
    accessibilitysettings_GetTrustLevel,
    /* IAccessibilitySettings methods */
    accessibilitysettings_get_HighContrast,
    accessibilitysettings_get_HighContrastScheme,
    accessibilitysettings_add_HighContrastChanged,
    accessibilitysettings_remove_HighContrastChanged,
};

struct factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct factory *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct factory, IActivationFactory_iface);
}

static HRESULT WINAPI factory_QueryInterface(IActivationFactory *iface, REFIID iid, void **out)
{
    struct factory *impl = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown)
        || IsEqualGUID(iid, &IID_IInspectable)
        || IsEqualGUID(iid, &IID_IActivationFactory))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef(*out);
        return S_OK;
    }

    *out = NULL;
    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IActivationFactory *iface)
{
    struct factory *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedIncrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static ULONG WINAPI factory_Release(IActivationFactory *iface)
{
    struct factory *impl = impl_from_IActivationFactory(iface);
    ULONG ref = InterlockedDecrement(&impl->ref);
    TRACE("iface %p, ref %lu.\n", iface, ref);
    return ref;
}

static HRESULT WINAPI factory_GetIids(IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName(IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel(IActivationFactory *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance(IActivationFactory *iface, IInspectable **instance)
{
    struct accessibilitysettings *impl;
    HRESULT hr;

    TRACE("iface %p, instance %p.\n", iface, instance);
    if (!instance) return E_POINTER;
    *instance = NULL;

    if (!(impl = calloc(1, sizeof(*impl))))
    {
        *instance = NULL;
        return E_OUTOFMEMORY;
    }

    impl->IAccessibilitySettings_iface.lpVtbl = &accessibilitysettings_vtbl;
    InitializeCriticalSection(&impl->handlers_cs);
    list_init(&impl->high_contrast_changed_handlers);

    if (FAILED(hr = weak_reference_source_init(&impl->weak_reference_source,
            (IUnknown *)&impl->IAccessibilitySettings_iface)))
    {
        DeleteCriticalSection(&impl->handlers_cs);
        free(impl);
        return hr;
    }

    *instance = (IInspectable *)&impl->IAccessibilitySettings_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

static struct factory factory =
{
    {&factory_vtbl},
    1,
};

IActivationFactory *accessibilitysettings_factory = &factory.IActivationFactory_iface;
