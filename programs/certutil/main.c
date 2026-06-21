/*
 * Copyright (C) 2022 Mohamad Al-Jaf
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

#include "windows.h"
#include "wincrypt.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(certutil);

static HRESULT add_store(const WCHAR *store_name, const WCHAR *cert_file, DWORD system_store)
{
    PCCERT_CONTEXT cert_context = NULL;
    HCERTSTORE source_store = NULL, dest_store = NULL;
    HRESULT hres = S_OK;
    BOOL ret;

    ret = CryptQueryObject(CERT_QUERY_OBJECT_FILE, cert_file, CERT_QUERY_CONTENT_FLAG_CERT,
                           CERT_QUERY_FORMAT_FLAG_ALL, 0, NULL, NULL, NULL,
                           &source_store, NULL, (const void **)&cert_context);
    if (!ret || !cert_context)
    {
        hres = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    dest_store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, system_store, store_name);
    if (!dest_store)
    {
        hres = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }

    if (!CertAddCertificateContextToStore(dest_store, cert_context, CERT_STORE_ADD_REPLACE_EXISTING, NULL))
        hres = HRESULT_FROM_WIN32(GetLastError());

done:
    if (dest_store) CertCloseStore(dest_store, 0);
    if (cert_context) CertFreeCertificateContext(cert_context);
    if (source_store) CertCloseStore(source_store, 0);
    return hres;
}

static HRESULT decode_hex(const WCHAR *from, const WCHAR *into)
{
    HRESULT hres = S_OK;
    HANDLE in = NULL, out = NULL;
    char in_buffer[1024];
    BYTE out_buffer[ARRAY_SIZE(in_buffer) / 2];
    ULONG64 total_written = 0;
    LARGE_INTEGER li;

    if ((in = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE ||
        !GetFileSizeEx(in, &li) ||
        (out = CreateFileW(into, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE)
        hres = HRESULT_FROM_WIN32(GetLastError());

    if (hres == S_OK)
        printf("Input Length = %I64u\n", li.QuadPart);

    while (hres == S_OK)
    {
        DWORD in_dw, out_dw;

        if (ReadFile(in, in_buffer, ARRAY_SIZE(in_buffer), &in_dw, NULL))
        {
            char *ptr = memchr(in_buffer, '\n', in_dw);
            char *ptr2 = memchr(in_buffer, '\r', in_dw);
            DWORD dw;

            if (!in_dw) break; /* EOF */
            if (ptr2 && (!ptr || ptr2 < ptr)) ptr = ptr2;
            if (ptr)
            {
                /* rewind just after the eol character */
                li.QuadPart = ptr + 1 - (in_buffer + in_dw);
                if (!SetFilePointerEx(in, li, NULL, FILE_CURRENT))
                {
                    hres = HRESULT_FROM_WIN32(GetLastError());
                    break;
                }
                in_dw = ptr - in_buffer;
            }
            out_dw = ARRAY_SIZE(out_buffer);
            if (CryptStringToBinaryA(in_buffer, in_dw, CRYPT_STRING_HEX,
                                     out_buffer, &out_dw, NULL, NULL) &&
                WriteFile(out, out_buffer, out_dw, &dw, NULL))
            {
                if (dw == out_dw)
                    total_written += out_dw;
                else
                    hres = HRESULT_FROM_WIN32(ERROR_CANTWRITE);
                continue;
            }
        }
        hres = HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(in);
    CloseHandle(out);

    if (hres == S_OK)
        printf("Output Length = %I64u\n", total_written);
    return hres;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    HRESULT hres = -1;
    DWORD system_store = CERT_SYSTEM_STORE_CURRENT_USER;
    const WCHAR *store_name, *cert_file;
    int i;

    if (argc == 4 && !wcscmp(argv[1], L"-decodehex"))
        hres = decode_hex(argv[2], argv[3]);
    else if (argc >= 4 && !wcsicmp(argv[1], L"-addstore"))
    {
        i = 2;
        while (i < argc && argv[i][0] == '-')
        {
            if (!wcsicmp(argv[i], L"-user"))
                system_store = CERT_SYSTEM_STORE_CURRENT_USER;
            else if (!wcsicmp(argv[i], L"-f"))
                ;
            else
                break;
            i++;
        }
        if (argc - i != 2)
            hres = E_INVALIDARG;
        else
        {
            store_name = argv[i];
            cert_file = argv[i + 1];
            hres = add_store(store_name, cert_file, system_store);
        }
    }
    else /* not a recognized command */
    {
        WINE_FIXME("stub:");
        for (i = 0; i < argc; i++)
            WINE_FIXME(" %s", wine_dbgstr_w(argv[i]));
        WINE_FIXME("\n");

        return 0;
    }
    if (hres == S_OK)
        printf("CertUtil: %ls command completed successfully.\n", argv[1]);
    else
        printf("CertUtil: %ls command FAILED: %08lx\n", argv[1], hres);
    return hres;
}
