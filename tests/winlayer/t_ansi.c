/* The ANSI ("A") functions: the ANSI code page is UTF-8, so A strings are
 * UTF-8 and A counts are bytes, and each function keeps Windows' rule for a
 * buffer that is too small.  The strings have a character past ASCII (é, two
 * bytes in UTF-8) so a count of UTF-16 units would show.
 * Prints one line a check; a line starting FAIL says what was wrong. */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *what, unsigned long long got, unsigned long long want) {
    printf("%s %s: 0x%llx\n", got == want ? "ok" : "FAIL", what, got);
    if (got != want) fails++;
}
static void check_str(const char *what, const char *got, const char *want) {
    int ok = got && !strcmp(got, want);
    printf("%s %s: \"%s\"\n", ok ? "ok" : "FAIL", what, got ? got : "(null)");
    if (!ok) fails++;
}

static char locale_seen[32];
static BOOL CALLBACK on_locale(LPSTR s) {
    if (!locale_seen[0]) snprintf(locale_seen, sizeof(locale_seen), "%s", s);
    return TRUE;
}

static void files(void) {
    char tmp[MAX_PATH], name[MAX_PATH], copy[MAX_PATH], moved[MAX_PATH], buf[MAX_PATH], dir[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD n;

    GetTempPathA(sizeof(tmp), tmp);
    check("GetTempFileNameA", GetTempFileNameA(tmp, "ans", 0, name) != 0, 1);
    check("  the file is there", GetFileAttributesA(name) != INVALID_FILE_ATTRIBUTES, 1);
    snprintf(copy, sizeof(copy), "%scopie \xc3\xa9t\xc3\xa9.txt", tmp);
    snprintf(moved, sizeof(moved), "%sd\xc3\xa9plac\xc3\xa9.txt", tmp);
    DeleteFileA(moved);
    check("CopyFileA to a UTF-8 name", CopyFileA(name, copy, FALSE), TRUE);
    check("MoveFileExA", MoveFileExA(copy, moved, MOVEFILE_REPLACE_EXISTING), TRUE);
    check("  the old name is gone", GetFileAttributesA(copy), INVALID_FILE_ATTRIBUTES);
    check("SetFileAttributesA", SetFileAttributesA(moved, FILE_ATTRIBUTE_NORMAL), TRUE);
    h = FindFirstFileExA(moved, FindExInfoStandard, &fd, FindExSearchNameMatch, NULL, 0);
    check("FindFirstFileExA", h != INVALID_HANDLE_VALUE, 1);
    if (h != INVALID_HANDLE_VALUE) {
        check_str("  its UTF-8 name", fd.cFileName, "d\xc3\xa9plac\xc3\xa9.txt");
        FindClose(h);
    }

    /* the sizes are bytes: "déplacé.txt" is 11 characters, 13 bytes */
    n = (DWORD)strlen(moved);
    check("GetLongPathNameA, too small: the size with the NUL", GetLongPathNameA(moved, buf, 5), n + 1);
    check("GetLongPathNameA: the length", GetLongPathNameA(moved, buf, sizeof(buf)), n);
    check_str("  the path", buf, moved);
    check("GetShortPathNameA", GetShortPathNameA(moved, buf, sizeof(buf)), n);

    h = CreateFileA(moved, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    check("CreateFileA", h != INVALID_HANDLE_VALUE, 1);
    n = GetFinalPathNameByHandleA(h, buf, sizeof(buf), 0);
    check("GetFinalPathNameByHandleA", n == strlen(buf) && !strncmp(buf, "\\\\?\\", 4) &&
                                           strstr(buf, "d\xc3\xa9plac\xc3\xa9.txt") != NULL, 1);
    check("GetFinalPathNameByHandleA, too small", GetFinalPathNameByHandleA(h, buf, 4, 0), n + 1);
    CloseHandle(h);
    DeleteFileA(moved);
    DeleteFileA(name);

    snprintf(dir, sizeof(dir), "%sdossier \xc3\xa9\\sous", tmp);
    check("SHCreateDirectoryExA", SHCreateDirectoryExA(NULL, dir, NULL), ERROR_SUCCESS);
    check("  a directory", (GetFileAttributesA(dir) & FILE_ATTRIBUTE_DIRECTORY) != 0, 1);
    RemoveDirectoryA(dir);
    snprintf(dir, sizeof(dir), "%sdossier \xc3\xa9", tmp);
    RemoveDirectoryA(dir);
}

static void system_names(void) {
    char buf[300], fs[32];
    DWORD serial = 0, maxlen = 0, flags = 0, n;
    ULARGE_INTEGER avail, total, freeb;
    CPINFOEXA cpa;
    struct { CPINFOEXW cp; unsigned char guard[16]; } cpw;

    check("GetSystemWindowsDirectoryA, too small", GetSystemWindowsDirectoryA(buf, 3), 11);
    check("GetSystemWindowsDirectoryA", GetSystemWindowsDirectoryA(buf, sizeof(buf)), 10);
    check_str("  the directory", buf, "C:\\Windows");
    check("GetVolumeInformationA", GetVolumeInformationA("C:\\", buf, sizeof(buf), &serial, &maxlen, &flags, fs,
                                                         sizeof(fs)), TRUE);
    check_str("  its file system", fs, "NTFS");
    check("  name length limit", maxlen, 255);
    check("GetDiskFreeSpaceExA", GetDiskFreeSpaceExA("C:\\", &avail, &total, &freeb) && total.QuadPart > 0, 1);
    check("GetVolumePathNameA", GetVolumePathNameA("C:\\Windows", buf, sizeof(buf)), TRUE);
    check_str("  its root", buf, "C:\\");
    check("GetLogicalDriveStringsA, no buffer: the size", GetLogicalDriveStringsA(0, NULL), 9);
    check("GetLogicalDriveStringsA", GetLogicalDriveStringsA(sizeof(buf), buf), 8);
    check("  C:\\ then Z:\\", !memcmp(buf, "C:\\\0Z:\\\0\0", 9), 1);

    n = 1;
    SetLastError(0);
    check("GetComputerNameExA, too small", GetComputerNameExA(ComputerNameNetBIOS, buf, &n), FALSE);
    check("  ERROR_MORE_DATA", GetLastError(), ERROR_MORE_DATA);
    check("  the size needed", n > 1, 1);
    n = sizeof(buf);
    check("GetComputerNameExA", GetComputerNameExA(ComputerNameNetBIOS, buf, &n) && n == strlen(buf) && n > 0, 1);

    check("GetCPInfoExA(CP_UTF8)", GetCPInfoExA(CP_UTF8, 0, &cpa), TRUE);
    check("  CodePage", cpa.CodePage, 65001);
    check("  MaxCharSize", cpa.MaxCharSize, 4);
    check_str("  CodePageName", cpa.CodePageName, "65001 (UTF-8)");
    memset(&cpw, 0x5A, sizeof(cpw));
    check("GetCPInfoExW(CP_ACP)", GetCPInfoExW(CP_ACP, 0, &cpw.cp), TRUE);
    check("  writes within its struct", cpw.guard[0] == 0x5A && cpw.guard[15] == 0x5A, 1);
    check("  the ANSI code page is UTF-8", cpw.cp.CodePage, 65001);

    n = K32GetModuleBaseNameA(GetCurrentProcess(), NULL, buf, sizeof(buf));
    check_str("K32GetModuleBaseNameA", buf, "t_ansi.exe");
    check("  its length", n, 10);
    check("K32GetModuleBaseNameA, too small: cut", K32GetModuleBaseNameA(GetCurrentProcess(), NULL, buf, 4), 3);
    check_str("  the cut name", buf, "t_a");
    check("K32GetModuleFileNameExA", K32GetModuleFileNameExA(GetCurrentProcess(), NULL, buf, sizeof(buf)) > 10 &&
                                         strstr(buf, "t_ansi.exe") != NULL, 1);

    check("EnumSystemLocalesA", EnumSystemLocalesA(on_locale, LCID_INSTALLED), TRUE);
    check_str("  its first locale", locale_seen, "00000409");
}

static void sync_objects(void) {
    HANDLE e = CreateEventA(NULL, TRUE, FALSE, "icda-ansi-event"), o;
    HANDLE m = CreateMutexExA(NULL, "icda-ansi-mutex", 0, MUTEX_ALL_ACCESS), s, t;
    o = OpenEventA(EVENT_ALL_ACCESS, FALSE, "icda-ansi-event");
    check("OpenEventA finds the named event", o != NULL, 1);
    if (o) {
        SetEvent(o);
        check("  it is the same event", WaitForSingleObject(e, 0), WAIT_OBJECT_0);
        CloseHandle(o);
    }
    check("CreateMutexExA", m != NULL, 1);
    o = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, "icda-ansi-mutex");
    check("OpenMutexA finds the named mutex", o != NULL, 1);
    if (o) CloseHandle(o);
    s = CreateSemaphoreExA(NULL, 1, 2, "icda-ansi-sem", 0, SEMAPHORE_ALL_ACCESS);
    check("CreateSemaphoreExA", s != NULL && WaitForSingleObject(s, 0) == WAIT_OBJECT_0, 1);
    t = CreateWaitableTimerA(NULL, TRUE, "icda-ansi-timer");
    check("CreateWaitableTimerA", t != NULL, 1);
    o = OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, "icda-ansi-sem");
    check("OpenSemaphoreA", o != NULL, 1);
    if (o) CloseHandle(o);

    /* a name in use: the same object again, and ERROR_ALREADY_EXISTS (how a
     * program finds it is running already) */
    SetLastError(0);
    o = CreateMutexA(NULL, TRUE, "icda-ansi-mutex");
    check("CreateMutexA of a name in use", o != NULL && GetLastError() == ERROR_ALREADY_EXISTS, 1);
    if (o) CloseHandle(o);
    SetLastError(0);
    o = CreateEventA(NULL, FALSE, FALSE, "icda-ansi-mutex");
    check("CreateEventA of a mutex's name", o == NULL && GetLastError() == ERROR_INVALID_HANDLE, 1);
    SetLastError(0);
    check("OpenEventA of a name nobody has", OpenEventA(EVENT_ALL_ACCESS, FALSE, "icda-ansi-none") == NULL &&
                                                 GetLastError() == ERROR_FILE_NOT_FOUND, 1);
    check("OpenEventA of a timer's name", OpenEventA(EVENT_ALL_ACCESS, FALSE, "icda-ansi-timer") != NULL, 0);
    CloseHandle(e), CloseHandle(m), CloseHandle(s), CloseHandle(t);
    /* the name goes with the last handle */
    check("OpenMutexA after the last handle closed", OpenMutexA(MUTEX_ALL_ACCESS, FALSE, "icda-ansi-mutex") != NULL, 0);
    SetLastError(0xDEAD);
    m = CreateMutexA(NULL, FALSE, "Local\\icda-ansi-mutex");
    check("CreateMutexA makes it anew", m != NULL && GetLastError() == 0, 1);
    o = OpenMutexA(MUTEX_ALL_ACCESS, FALSE, "icda-ansi-mutex");
    check("  Local\\ names the same place", o != NULL, 1);
    if (o) CloseHandle(o);
    CloseHandle(m);
}

static void formats(void) {
    SYSTEMTIME st = { 2026, 11, 4, 26, 13, 45, 7, 0 };
    char buf[64];
    WORD types[8];
    check("GetDateFormatA, no buffer: the size", GetDateFormatA(0x409, 0, &st, "yyyy-MM-dd", NULL, 0), 11);
    check("GetDateFormatA", GetDateFormatA(0x409, 0, &st, "yyyy-MM-dd", buf, sizeof(buf)), 11);
    check_str("  the date", buf, "2026-11-26");
    SetLastError(0);
    check("GetDateFormatA, too small", GetDateFormatA(0x409, 0, &st, "yyyy-MM-dd", buf, 5), 0);
    check("  ERROR_INSUFFICIENT_BUFFER", GetLastError(), ERROR_INSUFFICIENT_BUFFER);
    check("GetTimeFormatA", GetTimeFormatA(0x409, 0, &st, "HH:mm", buf, sizeof(buf)), 6);
    check_str("  the time", buf, "13:45");
    GetDateFormatA(0x409, DATE_LONGDATE, &st, NULL, buf, sizeof(buf));
    check_str("GetDateFormatA, DATE_LONGDATE", buf, "Thursday, November 26, 2026");
    GetDateFormatA(0x409, 0, &st, "ddd d MMM yy", buf, sizeof(buf));
    check_str("GetDateFormatA, short names", buf, "Thu 26 Nov 26");
    GetDateFormatA(0x409, DATE_SHORTDATE, &st, NULL, buf, sizeof(buf));
    check_str("GetDateFormatA, DATE_SHORTDATE", buf, "11/26/2026");
    GetTimeFormatA(0x409, 0, &st, NULL, buf, sizeof(buf));
    check_str("GetTimeFormatA, the default", buf, "1:45:07 PM");
    GetTimeFormatA(0x409, TIME_NOSECONDS, &st, NULL, buf, sizeof(buf));
    check_str("GetTimeFormatA, TIME_NOSECONDS", buf, "1:45 PM");
    GetTimeFormatA(0x409, TIME_FORCE24HOURFORMAT, &st, NULL, buf, sizeof(buf));
    check_str("GetTimeFormatA, TIME_FORCE24HOURFORMAT", buf, "13:45:07");
    GetTimeFormatA(0x409, 0, &st, "'o''clock:' h tt", buf, sizeof(buf));
    check_str("GetTimeFormatA, quoted text", buf, "o'clock: 1 PM");
    st.wMonth = 13;
    SetLastError(0);
    check("GetDateFormatA, month 13", GetDateFormatA(0x409, 0, &st, NULL, buf, sizeof(buf)) == 0 &&
                                         GetLastError() == ERROR_INVALID_PARAMETER, 1);
    st.wMonth = 11;
    /* one word for each UTF-16 unit: a, é, 1 and the NUL */
    memset(types, 0xFF, sizeof(types));
    check("GetStringTypeExA", GetStringTypeExA(0x409, CT_CTYPE1, "a\xc3\xa9" "1", -1, types), TRUE);
    check("  a is a lower-case letter", (types[0] & (C1_ALPHA | C1_LOWER)) == (C1_ALPHA | C1_LOWER), 1);
    check("  so is \xc3\xa9", (types[1] & (C1_ALPHA | C1_LOWER)) == (C1_ALPHA | C1_LOWER), 1);
    check("  1 is a digit", (types[2] & C1_DIGIT) != 0, 1);
    check("  four words, no more", types[4], 0xFFFF);
}

static void registry(void) {
    HKEY k;
    char buf[64];
    DWORD n = sizeof(buf), type = 0;
    check("RegSetKeyValueA makes the key", RegSetKeyValueA(HKEY_CURRENT_USER, "Software\\ICDA-ansi\\sous-cl\xc3\xa9", "nom",
                                                           REG_SZ, "valeur \xc3\xa9", 10), ERROR_SUCCESS);
    check("RegGetValueA reads it back", RegGetValueA(HKEY_CURRENT_USER, "Software\\ICDA-ansi\\sous-cl\xc3\xa9", "nom",
                                                     RRF_RT_REG_SZ, &type, buf, &n), ERROR_SUCCESS);
    check_str("  the value", buf, "valeur \xc3\xa9");
    check("RegOpenKeyExA", RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ICDA-ansi", 0, KEY_READ, &k), ERROR_SUCCESS);
    check("RegEnumKeyA", RegEnumKeyA(k, 0, buf, sizeof(buf)), ERROR_SUCCESS);
    check_str("  the subkey", buf, "sous-cl\xc3\xa9");
    RegCloseKey(k);
    check("RegDeleteKeyValueA", RegDeleteKeyValueA(HKEY_CURRENT_USER, "Software\\ICDA-ansi\\sous-cl\xc3\xa9", "nom"),
          ERROR_SUCCESS);
    check("RegDeleteTreeA", RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\ICDA-ansi"), ERROR_SUCCESS);
    check("  the tree is gone", RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\ICDA-ansi", 0, KEY_READ, &k),
          ERROR_FILE_NOT_FOUND);
}

static void security(void) {
    EXPLICIT_ACCESS_A ea;
    TRUSTEE_A t;
    PACL acl = NULL;
    ACCESS_ALLOWED_ACE *ace = NULL;
    SID_IDENTIFIER_AUTHORITY world = { SECURITY_WORLD_SID_AUTHORITY };
    PSID everyone = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    char *sddl = NULL;
    ULONG len = 0;

    BuildTrusteeWithNameA(&t, "Everyone");
    check("BuildTrusteeWithNameA: a name", t.TrusteeForm, TRUSTEE_IS_NAME);
    check_str("  the name as given", t.ptstrName, "Everyone");
    memset(&ea, 0, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_READ;
    ea.grfAccessMode = SET_ACCESS;
    ea.Trustee = t;
    check("SetEntriesInAclA", SetEntriesInAclA(1, &ea, NULL, &acl), ERROR_SUCCESS);
    AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone);
    check("  its entry is for Everyone, read as UTF-8",
          acl && GetAce(acl, 0, (void **)&ace) && EqualSid((PSID)&ace->SidStart, everyone), 1);
    if (acl) LocalFree(acl);
    FreeSid(everyone);

    check("ConvertStringSecurityDescriptorToSecurityDescriptorA",
          ConvertStringSecurityDescriptorToSecurityDescriptorA("D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, NULL), TRUE);
    check("ConvertSecurityDescriptorToStringSecurityDescriptorA",
          ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION, &sddl,
                                                               &len), TRUE);
    check("  SDDL in UTF-8, its length with the NUL", sddl && !strncmp(sddl, "O:", 2) && len == strlen(sddl) + 1, 1);
    if (sddl) LocalFree(sddl);
    if (sd) LocalFree(sd);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, m, wp, lp); }

static void user(void) {
    static const char s[] = "a\xc3\xa9";
    char up[] = "ab\xc3\xa9!";
    char buf[16];
    WNDCLASSA wc = { 0 }, got;
    WNDCLASSW gotw;
    HDC dc;

    check("CharPrevA steps over a whole character", (unsigned long long)(CharPrevA(s, s + 3) - s), 1);
    check("CharPrevA at the start", (unsigned long long)(CharPrevA(s, s) - s), 0);
    check("CharUpperBuffA", CharUpperBuffA(up, 5), 5);
    check_str("  upper-cased past ASCII", up, "AB\xc3\x89!");
    check("IsCharAlphaA('a')", IsCharAlphaA('a') != 0, 1);
    check("IsCharAlphaA of a lone byte past ASCII", IsCharAlphaA((CHAR)0xC3), FALSE);
    check("IsCharAlphaNumericA('7')", IsCharAlphaNumericA('7') != 0, 1);
    check("VkKeyScanA('a')", (unsigned short)VkKeyScanA('a'), 'A');
    check("VkKeyScanA('A'): with Shift", (unsigned short)VkKeyScanA('A'), 0x100 | 'A');
    check("GetKeyboardLayoutNameA", GetKeyboardLayoutNameA(buf), TRUE);
    check_str("  US English", buf, "00000409");
    check("GetKeyNameTextA(A key)", GetKeyNameTextA(0x1E << 16, buf, sizeof(buf)), 1);
    check_str("  its name", buf, "A");

    wc.lpfnWndProc = wndproc;
    wc.cbWndExtra = 16;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "IcdaAnsiClass";
    check("RegisterClassA", RegisterClassA(&wc) != 0, 1);
    memset(&got, 0, sizeof(got));
    check("GetClassInfoA", GetClassInfoA(wc.hInstance, "IcdaAnsiClass", &got) != 0, 1);
    check("  its window procedure", got.lpfnWndProc == wndproc, 1);
    check("  its window bytes", got.cbWndExtra, 16);
    check_str("  its name", got.lpszClassName, "IcdaAnsiClass");
    memset(&gotw, 0, sizeof(gotw));
    check("GetClassInfoW: the fields where WNDCLASSW has them",
          GetClassInfoW(wc.hInstance, L"IcdaAnsiClass", &gotw) && gotw.lpfnWndProc == wndproc && gotw.cbWndExtra == 16, 1);

    {
        /* the A structs have the face names in bytes */
        NONCLIENTMETRICSA nca;
        NONCLIENTMETRICSW ncw;
        LOGFONTA lfa;
        BOOL on = FALSE;
        memset(&nca, 0x55, sizeof(nca));
        nca.cbSize = sizeof(nca);
        check("SystemParametersInfoA(SPI_GETNONCLIENTMETRICS)",
              SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(nca), &nca, 0), TRUE);
        check_str("  the message font", nca.lfMessageFont.lfFaceName, "Segoe UI");
        check("  9 points", nca.lfMessageFont.lfHeight, (unsigned long long)-12);
        check("  iPaddedBorderWidth", nca.iPaddedBorderWidth, 4);
        memset(&ncw, 0x55, sizeof(ncw));
        ncw.cbSize = sizeof(ncw);
        check("SystemParametersInfoW(SPI_GETNONCLIENTMETRICS)",
              SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncw), &ncw, 0) &&
                  !wcscmp(ncw.lfCaptionFont.lfFaceName, L"Segoe UI") && ncw.iScrollWidth == 17, 1);
        ncw.cbSize = 12;
        check("  a wrong cbSize fails", SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, 12, &ncw, 0), FALSE);
        check("SystemParametersInfoA(SPI_GETICONTITLELOGFONT)",
              SystemParametersInfoA(SPI_GETICONTITLELOGFONT, sizeof(lfa), &lfa, 0) && !strcmp(lfa.lfFaceName, "Segoe UI"), 1);
        check("SystemParametersInfoW(SPI_GETFONTSMOOTHING)", SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0, &on, 0) && on, 1);
    }

    dc = CreateCompatibleDC(NULL);
    if (dc) {
        int fit = -1, dx[3] = { 0, 0, 0 };
        SIZE sz;
        check("GetTextExtentExPointA", GetTextExtentExPointA(dc, s, 3, 10000, &fit, dx, &sz), TRUE);
        check("  all three bytes fit", fit, 3);
        check("  both bytes of \xc3\xa9 end where it ends", dx[1] == dx[2] && dx[1] > dx[0] && dx[2] == sz.cx, 1);
        check("  none fits in no room", GetTextExtentExPointA(dc, s, 3, 0, &fit, NULL, &sz) && fit == 0, 1);
        DeleteDC(dc);
    }
}

int main(void) {
    WSADATA wsa;
    SOCKET so;
    files();
    system_names();
    sync_objects();
    formats();
    registry();
    security();
    user();
    WSAStartup(MAKEWORD(2, 2), &wsa);
    so = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, 0);
    check("WSASocketA", so != INVALID_SOCKET, 1);
    if (so != INVALID_SOCKET) closesocket(so);
    check("DragQueryFileA, no drop: no files", DragQueryFileA(NULL, 0xFFFFFFFF, NULL, 0), 0);
    printf("done, %d failed\n", fails);
    return fails != 0;
}
