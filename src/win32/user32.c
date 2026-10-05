/* USER32, GDI32, ADVAPI32, OLE32: one SDL window and a minimal message loop. */
#include "../runtime/rt.h"
#include <SDL2/SDL.h>

#define HWND_MAIN 0x00010001u
#define WM_QUIT 0x12

SDL_Window *rt_window;
static uint32_t wndproc;
static int quit_posted;

void rt_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) { rt_log("window closed"); quit_posted = 1; }
}

WINAPI(LoadCursorA, "LoadCursorA", 2) { return 1; }
WINAPI(RegisterClassExA, "RegisterClassExA", 1) {
    wndproc = MEM32(ARG(0) + 8);  /* WNDCLASSEXA.lpfnWndProc */
    return 0xC001;
}
WINAPI(CreateWindowExA, "CreateWindowExA", 12) {
    if (rt_trace) rt_log("  CreateWindowExA(%s, %dx%d)", ARG(2) ? GSTR(ARG(2)) : "", ARG(6), ARG(7));
    if (!rt_window) {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");  /* let SIGTERM/SIGINT stop the port */
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER))
            rt_fatal("SDL_Init: %s", SDL_GetError());
        /* A scripted run (SH2_KEYS) stays off screen: it neither reads nor steals the real keyboard. */
        rt_window = SDL_CreateWindow("Silent Hill 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
                                         (getenv("SH2_KEYS") ? SDL_WINDOW_HIDDEN : 0));
        if (!rt_window) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    }
    return HWND_MAIN;
}
WINAPI(ShowWindow, "ShowWindow", 2) { return 1; }
WINAPI(UpdateWindow, "UpdateWindow", 1) { return 1; }
WINAPI(DefWindowProcA, "DefWindowProcA", 4) { return 0; }

/* MSG: hwnd, message, wParam, lParam, time, pt. Input goes through DirectInput, so the only
 * message the loop sees is WM_QUIT. */
WINAPI(PeekMessageA, "PeekMessageA", 5) {
    rt_pump_events();
    if (!quit_posted) return 0;
    uint32_t m = ARG(0);
    memset(GPTR(m), 0, 28);
    MEM32(m) = HWND_MAIN;
    MEM32(m + 4) = WM_QUIT;
    return 1;
}
WINAPI(TranslateMessage, "TranslateMessage", 1) { return 0; }
static uint32_t call_wndproc(uint32_t hwnd, uint32_t msg, uint32_t w, uint32_t l) {
    uint32_t a[4] = {hwnd, msg, w, l};
    return wndproc ? guest_call(wndproc, 4, a, 1) : 0;
}
WINAPI(DispatchMessageA, "DispatchMessageA", 1) {
    uint32_t m = ARG(0);
    return call_wndproc(MEM32(m), MEM32(m + 4), MEM32(m + 8), MEM32(m + 12));
}
WINAPI(SendMessageA, "SendMessageA", 4) { return call_wndproc(ARG(0), ARG(1), ARG(2), ARG(3)); }
WINAPI(PostQuitMessage, "PostQuitMessage", 1) { rt_log("PostQuitMessage(%d)", ARG(0)); quit_posted = 1; return 0; }
WINAPI(BeginPaint, "BeginPaint", 2) { memset(GPTR(ARG(1)), 0, 64); return 1; }
WINAPI(EndPaint, "EndPaint", 2) { return 1; }
WINAPI(MessageBoxA, "MessageBoxA", 4) {
    rt_log("MessageBox [%s]: %s", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    return 1;  /* IDOK */
}
CDECL(wsprintfA, "wsprintfA") {
    char tmp[1025];
    int k = rt_format(tmp, sizeof tmp, GSTR(ARG(1)), g_esp + 12);
    strcpy(GPTR(ARG(0)), tmp);
    return k;
}

WINAPI(DeleteObject, "DeleteObject", 1) { return 1; }

/* No registry: every key is missing, so the game uses its defaults. */
WINAPI(RegOpenKeyA, "RegOpenKeyA", 3) {
    if (rt_trace) rt_log("  RegOpenKeyA(%08X, %s)", ARG(0), ARG(1) ? GSTR(ARG(1)) : "");
    return 2;  /* ERROR_FILE_NOT_FOUND */
}
WINAPI(RegQueryValueExA, "RegQueryValueExA", 6) { return 2; }
WINAPI(RegCloseKey, "RegCloseKey", 1) { return 0; }

WINAPI(CoInitialize, "CoInitialize", 1) { return 0; }
