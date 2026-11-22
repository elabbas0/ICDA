/* t_keys: shows the input a Windows program gets, in its window and on stdout.
 *   L: lock the pointer as a game does (raw mouse input, hidden, clipped)
 *   U: unlock it          Q: quit (also after 300 seconds) */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define LINES 22
static char lines[LINES][96];
static int nlines;
static HWND win;
static int locked;
static DWORD t0;

static void add(const char *fmt, ...) {
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%6lu %s\n", GetTickCount() - t0, buf);
    fflush(stdout);
    if (nlines == LINES) {
        memmove(lines[0], lines[1], sizeof(lines[0]) * (LINES - 1));
        nlines--;
    }
    snprintf(lines[nlines++], sizeof(lines[0]), "%6lu %s", GetTickCount() - t0, buf);
    if (win) InvalidateRect(win, 0, TRUE);
}

static void lock(int on) {
    RAWINPUTDEVICE d = { 1, 2, on ? 0 : RIDEV_REMOVE, on ? win : 0 };
    RECT r;
    RegisterRawInputDevices(&d, 1, sizeof(d));
    if (on) {
        GetWindowRect(win, &r);
        ClipCursor(&r);
        ShowCursor(FALSE);
    } else {
        ClipCursor(0);
        ShowCursor(TRUE);
    }
    locked = on;
    add("pointer %s", on ? "locked" : "free");
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        add("%s vk %02X sc %02X%s%s", m == WM_KEYDOWN ? "KEYDOWN" : "SYSKEYDOWN", (unsigned)wp, (unsigned)((lp >> 16) & 0xFF),
            (lp & (1 << 24)) ? " ext" : "", (lp & (1 << 30)) ? " repeat" : "");
        if (wp == 'Q') PostQuitMessage(0);
        if (wp == 'L' && !locked) lock(1);
        if (wp == 'U' && locked) lock(0);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        add("%s vk %02X sc %02X%s", m == WM_KEYUP ? "KEYUP" : "SYSKEYUP", (unsigned)wp, (unsigned)((lp >> 16) & 0xFF),
            (lp & (1 << 24)) ? " ext" : "");
        return 0;
    case WM_CHAR:
        add("CHAR %u '%c'", (unsigned)wp, wp >= 32 && wp < 127 ? (int)wp : '.');
        return 0;
    case WM_SETFOCUS: add("SETFOCUS"); return 0;
    case WM_KILLFOCUS: add("KILLFOCUS"); return 0;
    case WM_ACTIVATE: add("ACTIVATE %u", (unsigned)LOWORD(wp)); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        add("BUTTONDOWN %s at %d,%d", m == WM_LBUTTONDOWN ? "left" : m == WM_RBUTTONDOWN ? "right" : "middle",
            (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        add("BUTTONUP %s", m == WM_LBUTTONUP ? "left" : m == WM_RBUTTONUP ? "right" : "middle");
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.lLastX || ri.data.mouse.lLastY || ri.data.mouse.usButtonFlags))
            add("INPUT mouse %ld,%ld buttons %04X", ri.data.mouse.lLastX, ri.data.mouse.lLastY, ri.data.mouse.usButtonFlags);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetTextColor(dc, RGB(120, 255, 120));
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < nlines; i++) TextOutA(dc, 6, 4 + i * 16, lines[i], (int)strlen(lines[i]));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

int main(void) {
    WNDCLASSA wc = { 0 };
    MSG msg;
    t0 = GetTickCount();
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "t_keys";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    win = CreateWindowA("t_keys", "Input test (L lock, U unlock, Q quit)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        CW_USEDEFAULT, CW_USEDEFAULT, 560, 400, 0, 0, wc.hInstance, 0);
    add("window up");
    SetTimer(win, 1, 300000, 0);
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        if (msg.message == WM_TIMER) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    add("done");
    return 0;
}
