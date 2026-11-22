/* t_gl: a window with an OpenGL context through WGL, as games make one */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

typedef HGLRC (WINAPI *PFNCREATECTX)(HDC, HGLRC, const int *);
static int closed;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) { closed = 1; return 0; }
    if (m == WM_SIZE) printf("WM_SIZE %dx%d\n", LOWORD(lp), HIWORD(lp));
    return DefWindowProcW(h, m, wp, lp);
}

int main(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"tgl";
    wc.style = CS_OWNDC;
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }
    HWND w = CreateWindowExW(0, L"tgl", L"GL test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 320, 240, 0, 0, wc.hInstance, 0);
    if (!w) { printf("CreateWindow failed\n"); return 1; }
    ShowWindow(w, SW_SHOW);
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!SetPixelFormat(dc, pf, &pfd)) { printf("SetPixelFormat failed\n"); return 1; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { printf("legacy context failed\n"); return 1; }
    PFNCREATECTX cca = (PFNCREATECTX)wglGetProcAddress("wglCreateContextAttribsARB");
    printf("wglCreateContextAttribsARB %s\n", cca ? "found" : "MISSING");
    printf("version %.3s renderer %.8s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    void *gen = (void *)wglGetProcAddress("glGenBuffers");
    printf("glGenBuffers %s\n", gen ? "found" : "MISSING");
    RECT r; GetClientRect(w, &r);
    printf("client %ldx%ld\n", r.right, r.bottom);
    for (int frame = 0; frame < 5 && !closed; frame++) {
        MSG msg;
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        glViewport(0, 0, r.right, r.bottom);
        glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBegin(GL_TRIANGLES);
        glColor3f(1, 0, 0); glVertex2f(-0.8f, -0.8f);
        glColor3f(1, 0, 0); glVertex2f(0.8f, -0.8f);
        glColor3f(1, 0, 0); glVertex2f(0.0f, 0.8f);
        glEnd();
        SwapBuffers(dc);
    }
    printf("error %u\n", glGetError());
    wglMakeCurrent(0, 0);
    wglDeleteContext(rc);
    DestroyWindow(w);
    printf("done\n");
    return 0;
}
