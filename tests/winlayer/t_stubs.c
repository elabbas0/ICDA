/* Functions icda-win does not have fail the way their callers check:
 * each returns its own kind of failure (E_NOTIMPL, FALSE with an error,
 * INVALID_HANDLE_VALUE, a Win32 or security error code ...) rather than
 * something that reads as success.  And a function asked for by its export
 * number is found under its name.
 * Prints one line a check; a line starting FAIL says what was wrong. */
#define COBJMACROS
#define INITGUID
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <dsound.h>
#include <dinput.h>
#include <xinput.h>
#include <shlwapi.h>
#define SECURITY_WIN32
#include <security.h>
#include <stdio.h>

static int fails;
static void check(const char *what, unsigned long long got, unsigned long long want) {
    printf("%s %s: 0x%llx\n", got == want ? "ok" : "FAIL", what, got);
    if (got != want) fails++;
}

int main(void) {
    void *p = (void *)1;
    HANDLE h;
    XINPUT_STATE st;
    ULONG size = 0;
    CtxtHandle ctx = { 0, 0 };
    char buf[64];
    HMODULE ws2;

    check("DirectSoundCreate8 E_NOTIMPL", (unsigned long)DirectSoundCreate8(NULL, (LPDIRECTSOUND8 *)&p, NULL), 0x80004001);
    check("DirectInput8Create E_NOTIMPL",
          (unsigned long)DirectInput8Create(GetModuleHandleW(0), DIRECTINPUT_VERSION, &IID_IDirectInput8W, &p, NULL),
          0x80004001);
    check("XInputGetState not connected", XInputGetState(0, &st), ERROR_DEVICE_NOT_CONNECTED);
    SetLastError(0);
    h = CreateFileTransactedW(L"x", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL, NULL, NULL, NULL);
    check("CreateFileTransactedW INVALID_HANDLE_VALUE", (unsigned long long)(ULONG_PTR)h,
          (unsigned long long)(ULONG_PTR)INVALID_HANDLE_VALUE);
    check("  and its last error", GetLastError(), ERROR_CALL_NOT_IMPLEMENTED);
    check("RegSaveKeyW error code", (unsigned long)RegSaveKeyW(HKEY_CURRENT_USER, L"x", NULL), ERROR_CALL_NOT_IMPLEMENTED);
    check("PathIsNetworkPathW FALSE", PathIsNetworkPathW(L"C:\\"), FALSE);
    check("CoGetCallContext E_NOTIMPL", (unsigned long)CoGetCallContext(&IID_IUnknown, &p), 0x80004001);
    check("GetAdaptersAddresses not supported", GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size),
          ERROR_NOT_SUPPORTED);
    check("QueryContextAttributesW unsupported", (unsigned long)QueryContextAttributesW(&ctx, 0, buf),
          0x80090302);
    ws2 = LoadLibraryW(L"ws2_32.dll");
    check("ws2_32 #111 is WSAGetLastError",
          (unsigned long long)(ULONG_PTR)GetProcAddress(ws2, (LPCSTR)111) ==
              (unsigned long long)(ULONG_PTR)GetProcAddress(ws2, "WSAGetLastError") &&
              GetProcAddress(ws2, "WSAGetLastError") != NULL,
          1);
    {
        /* shell32's IsUserAnAdmin is export number 680 */
        HMODULE sh = LoadLibraryW(L"shell32.dll");
        check("shell32 #680 is IsUserAnAdmin",
              GetProcAddress(sh, (LPCSTR)680) != NULL && GetProcAddress(sh, (LPCSTR)680) == GetProcAddress(sh, "IsUserAnAdmin"),
              1);
    }
    {
        /* GetProcAddress: what Windows' DLL has is there (and fails as it
         * would); what it does not have is not */
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        HANDLE (WINAPI *pCreateFileTransactedW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE,
                                                HANDLE, PUSHORT, PVOID) =
            (void *)GetProcAddress(k32, "CreateFileTransactedW");
        check("GetProcAddress of a missing Windows function", pCreateFileTransactedW != NULL, 1);
        if (pCreateFileTransactedW)
            check("  which fails as it would",
                  (unsigned long long)(ULONG_PTR)pCreateFileTransactedW(L"x", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0,
                                                                        NULL, NULL, NULL, NULL),
                  (unsigned long long)(ULONG_PTR)INVALID_HANDLE_VALUE);
        check("  the same each time", (void *)GetProcAddress(k32, "CreateFileTransactedW") == (void *)pCreateFileTransactedW, 1);
        check("GetProcAddress of no such function", GetProcAddress(k32, "NoSuchFunctionAtAll") == NULL, 1);
        check("GetProcAddress of a function Windows lacks",
              GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") == NULL, 1);
    }
    printf("done, %d failed\n", fails);
    return fails;
}
