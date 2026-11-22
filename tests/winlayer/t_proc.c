/* t_proc: the registry, and a child process that reads what its parent
 * wrote there and returns a 32-bit exit code */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    HKEY k; DWORD type, size; char buf[64];
    if (argc > 1 && !strcmp(argv[1], "child")) {
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ICDA\\Test", 0, KEY_READ, &k)) return 1;
        size = sizeof(buf);
        if (RegQueryValueExA(k, "greeting", 0, &type, (BYTE *)buf, &size) || type != REG_SZ) return 2;
        RegCloseKey(k);
        printf("child read [%s], args [%s] [%s]\n", buf, argv[2], argc > 3 ? argv[3] : "");
        return strcmp(buf, "hello from parent") ? 3 : 70000;
    }
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\ICDA\\Test", 0, 0, 0, KEY_ALL_ACCESS, 0, &k, 0)) { printf("create failed\n"); return 1; }
    RegSetValueExA(k, "greeting", 0, REG_SZ, (const BYTE *)"hello from parent", 18);
    DWORD v = 42; RegSetValueExA(k, "answer", 0, REG_DWORD, (const BYTE *)&v, 4);
    DWORD got = 0; size = 4; RegQueryValueExA(k, "answer", 0, &type, (BYTE *)&got, &size);
    char name[64]; DWORD nn = sizeof(name), i = 0, count = 0;
    while (nn = sizeof(name), RegEnumValueA(k, i++, name, &nn, 0, 0, 0, 0) == 0) count++;
    RegCloseKey(k);
    printf("dword %lu, %lu values\n", got, count);
    char cmd[512]; GetModuleFileNameA(0, cmd + 1, 400); cmd[0] = '"';
    strcat(cmd, "\" child \"two words\" last");
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(0, cmd, 0, 0, FALSE, 0, 0, 0, &si, &pi)) { printf("CreateProcess failed %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    printf("child exit code %lu\n", code);
    RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\ICDA\\Test");
    printf("after delete open=%ld\n", RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ICDA\\Test", 0, KEY_READ, &k));
    return 0;
}
