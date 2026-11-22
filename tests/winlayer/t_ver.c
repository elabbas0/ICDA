#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
/* t_ver [file]: a file's version resource (by default its own, from t_ver.rc) */
int main(int argc, char **argv) {
    WCHAR path[512]; DWORD h; MultiByteToWideChar(CP_UTF8, 0, argc > 1 ? argv[1] : argv[0], -1, path, 512);
    DWORD n = GetFileVersionInfoSizeW(path, &h);
    printf("size %lu\n", n);
    if (!n) return 1;
    void *b = malloc(n);
    if (!GetFileVersionInfoW(path, 0, n, b)) { printf("GetFileVersionInfo failed\n"); return 1; }
    VS_FIXEDFILEINFO *fi; UINT len;
    if (VerQueryValueW(b, L"\\", (void **)&fi, &len))
        printf("file version %lu.%lu.%lu.%lu\n", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    struct { WORD lang, cp; } *tr;
    if (VerQueryValueW(b, L"\\VarFileInfo\\Translation", (void **)&tr, &len)) {
        WCHAR q[128]; WCHAR *pv; swprintf(q, 128, L"\\StringFileInfo\\%04x%04x\\ProductVersion", tr->lang, tr->cp);
        if (VerQueryValueW(b, q, (void **)&pv, &len)) printf("product version %ls\n", pv);
    }
    return 0;
}
