/* display.c - run the game in a scalable 32-bit window (#included by orlyfix.c).
 *
 *  - the game is told the screen is 640x480 with 256 colours (GetDeviceCaps,
 *    GetSystemMetrics, system palette queries, EnumDisplaySettings); its mode changes,
 *    system palette and colour changes and broadcasts stay inside the game;
 *  - its window is a borderless full-screen popup or a resizable window, but the
 *    game still sees a 640x480 window at (0,0);
 *  - mouse positions are mapped between the real window and 640x480;
 *  - repaint requests are kept in a game-sized update region, so the engine's
 *    repaint logic works in game coordinates.
 * Only the game's own window and display DCs are affected.
 */

static HRGN vUpd;           /* pending game repaint area, in 640x480 game coordinates */
static UINT fakePalUse = SYSPAL_STATIC;   /* system palette use, as the game set it */

static BOOL is_game_wnd(HWND w) { return w && w == game_hwnd(); }
static BOOL is_display_dc(HDC dc) { return dc && GetDeviceCaps(dc, TECHNOLOGY) == DT_RASDISPLAY; }

static void real_to_game(LONG *x, LONG *y)
{
    int vw = view.right - view.left, vh = view.bottom - view.top;
    if (vw <= 0 || vh <= 0) return;
    LONG gx = (*x - view.left) * GAME_W / vw, gy = (*y - view.top) * GAME_H / vh;
    *x = gx < 0 ? 0 : gx >= GAME_W ? GAME_W - 1 : gx;
    *y = gy < 0 ? 0 : gy >= GAME_H ? GAME_H - 1 : gy;
}

static void game_to_real(LONG *x, LONG *y)
{
    int vw = view.right - view.left, vh = view.bottom - view.top;
    if (vw <= 0 || vh <= 0) return;
    *x = view.left + (2 * *x + 1) * vw / (2 * GAME_W);   /* centre of the game pixel */
    *y = view.top  + (2 * *y + 1) * vh / (2 * GAME_H);
}

/* ---- a 640x480 256-colour display ---- */

int WINAPI My_GetDeviceCaps(HDC dc, int i)
{
    if (is_display_dc(dc)) {
        switch (i) {
        case BITSPIXEL:   return 8;
        case PLANES:      return 1;
        case SIZEPALETTE: return 256;
        case NUMRESERVED: return 20;
        case HORZRES:     return GAME_W;
        case VERTRES:     return GAME_H;
        case LOGPIXELSX:  return 96;          /* a 1997 640x480 screen; the real value would */
        case LOGPIXELSY:  return 96;          /* shrink fonts and printouts on high-DPI screens */
        case RASTERCAPS:  return GetDeviceCaps(dc, RASTERCAPS) | RC_PALETTE;
        }
    }
    return GetDeviceCaps(dc, i);
}

int WINAPI My_GetSystemMetrics(int i)
{
    if (i == SM_CXSCREEN) return GAME_W;      /* the game asks for these and the border sizes only */
    if (i == SM_CYSCREEN) return GAME_H;
    return GetSystemMetrics(i);
}

/* Entries 0-9 and 246-255 are Windows' 20 fixed colours; the rest the game's palette. */
UINT WINAPI My_GetSystemPaletteEntries(HDC dc, UINT start, UINT n, LPPALETTEENTRY out)
{
    if (!is_display_dc(dc)) return GetSystemPaletteEntries(dc, start, n, out);
    if (!out) return 256;
    PALETTEENTRY def[20], all[256];
    ZeroMemory(all, sizeof all);
    if (screenPal) GetPaletteEntries(screenPal, 0, 256, all);
    if (GetPaletteEntries((HPALETTE)GetStockObject(DEFAULT_PALETTE), 0, 20, def) == 20)
        for (int i = 0; i < 10; i++) { all[i] = def[i]; all[246 + i] = def[10 + i]; }
    UINT c = 0;
    for (UINT i = start; i < 256 && c < n; i++, c++) { out[c] = all[i]; out[c].peFlags = 0; }
    return c;
}

UINT WINAPI My_GetSystemPaletteUse(HDC dc)
{
    if (is_display_dc(dc)) return fakePalUse;
    return GetSystemPaletteUse(dc);
}

/* The game looks these up by name with GetProcAddress (its wrappers CODE_CHANGE_DISPLAY and */
/* CODE_ENUM_DISPLAY and its CODE_DIRECTDRAW_SETUP then run unchanged).                       */

/* The only display mode is 640x480 with 256 colours. The game's DEVMODE buffer is the */
/* 148-byte Windows 95 one, so only fields inside that are touched.                     */
BOOL WINAPI My_EnumDisplaySettingsA(LPCSTR dev, DWORD n, DEVMODEA *dm)
{
    (void)dev;
    if (!dm || (n != 0 && n != ENUM_CURRENT_SETTINGS && n != ENUM_REGISTRY_SETTINGS)) return FALSE;
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    dm->dmBitsPerPel = 8;
    dm->dmPelsWidth = GAME_W;
    dm->dmPelsHeight = GAME_H;
    dm->dmDisplayFrequency = 60;
    return TRUE;
}

/* Mode changes are accepted; the real display is never changed. */
LONG WINAPI My_ChangeDisplaySettingsA(DEVMODEA *dm, DWORD flags)
{
    (void)dm; (void)flags;
    return DISP_CHANGE_SUCCESSFUL;
}

#define DDERR_NODIRECTDRAWHW ((HRESULT)0x88760208)   /* from ddraw.h, not included */

/* No DirectDraw: the engine falls back to GDI, which the 256-colour emulation needs. */
HRESULT WINAPI My_DirectDrawCreate(GUID *guid, void **dd, IUnknown *outer)
{
    (void)guid; (void)outer;
    if (dd) *dd = NULL;
    return DDERR_NODIRECTDRAWHW;
}

FARPROC WINAPI My_GetProcAddress(HMODULE m, LPCSTR name)
{
    if (HIWORD((DWORD)name)) {
        if (!lstrcmpA(name, "EnumDisplaySettingsA"))   return (FARPROC)My_EnumDisplaySettingsA;
        if (!lstrcmpA(name, "ChangeDisplaySettingsA")) return (FARPROC)My_ChangeDisplaySettingsA;
        if (!lstrcmpA(name, "DirectDrawCreate"))       return (FARPROC)My_DirectDrawCreate;
    }
    return GetProcAddress(m, name);
}

/* The game's palette-mode code (CODE_PALETTE_MODE) sets the system palette use and, for "no static */
/* colours", rewrites the system colours for every program. Here the palette use is just   */
/* remembered (and reported back), and system colours are never changed.                   */
UINT WINAPI My_SetSystemPaletteUse(HDC dc, UINT use)
{
    if (!is_display_dc(dc)) return SetSystemPaletteUse(dc, use);
    UINT old = fakePalUse;
    fakePalUse = use;
    return old;
}

BOOL WINAPI My_SetSysColors(int n, const INT *elements, const COLORREF *colours)
{
    (void)n; (void)elements; (void)colours;
    return TRUE;
}

/* Messages the game broadcasts to every window (WM_PALETTECHANGED, WM_SYSCOLORCHANGE,  */
/* WM_FONTCHANGE) go to its own windows only: a broadcast SendMessage waits for every   */
/* program on the system, and the changes they announce are the game's own.             */
typedef struct { UINT m; WPARAM wp; LPARAM lp; BOOL post; } OWN_MSG;

static BOOL CALLBACK to_own_window(HWND w, LPARAM p)
{
    OWN_MSG *q = (OWN_MSG *)p;
    if (q->post) PostMessageA(w, q->m, q->wp, q->lp); else SendMessageA(w, q->m, q->wp, q->lp);
    return TRUE;
}

LRESULT WINAPI My_SendMessageA(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (w != HWND_BROADCAST) return SendMessageA(w, m, wp, lp);
    OWN_MSG q = { m, wp, lp, FALSE };
    EnumThreadWindows(GetCurrentThreadId(), to_own_window, (LPARAM)&q);
    return 0;
}

BOOL WINAPI My_PostMessageA(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (w != HWND_BROADCAST) return PostMessageA(w, m, wp, lp);
    OWN_MSG q = { m, wp, lp, TRUE };
    EnumThreadWindows(GetCurrentThreadId(), to_own_window, (LPARAM)&q);
    return TRUE;
}

/* "Screen-compatible" bitmaps are 8-bit, as they were on a 256-colour screen. */
HBITMAP WINAPI My_CreateCompatibleBitmap(HDC dc, int w, int h)
{
    if (is_display_dc(dc) && w > 0 && h > 0) {
        struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
        ZeroMemory(&bi, sizeof bi);
        bi.h.biSize = sizeof bi.h; bi.h.biWidth = w; bi.h.biHeight = -h;
        bi.h.biPlanes = 1; bi.h.biBitCount = 8; bi.h.biCompression = BI_RGB; bi.h.biClrUsed = 256;
        if (sh.palKnown) memcpy(bi.c, sh.pal, sizeof bi.c); else read_game_palette(bi.c);
        void *bits;
        HBITMAP b = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (b) return b;
    }
    return CreateCompatibleBitmap(dc, w, h);
}

/* ---- the game window ---- */

#define WINDOW_STYLE  WS_OVERLAPPEDWINDOW
#define POPUP_STYLE   WS_POPUP

/* Starting window size: WindowScale x 640x480 (smaller if it would not fit), centred. */
static void window_rect_for(const MONITORINFO *mi, DWORD style, DWORD ex, RECT *out)
{
    int workW = mi->rcWork.right - mi->rcWork.left, workH = mi->rcWork.bottom - mi->rcWork.top;
    int scale = cfg.windowScale < 1 ? 1 : cfg.windowScale;
    RECT r;
    for (;;) {
        SetRect(&r, 0, 0, GAME_W * scale, GAME_H * scale);
        AdjustWindowRectEx(&r, style, FALSE, ex);
        if (scale == 1 || (r.right - r.left <= workW && r.bottom - r.top <= workH)) break;
        scale--;
    }
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi->rcWork.left + (workW - w) / 2, y = mi->rcWork.top + (workH - h) / 2;
    if (y < mi->rcWork.top) y = mi->rcWork.top;
    SetRect(out, x, y, x + w, y + h);
}

/* Alt+Enter: switch between the window and borderless full screen on the current monitor. */
static WINDOWPLACEMENT windowedPlacement;

static void toggle_fullscreen(HWND w)
{
    DWORD style = GetWindowLongA(w, GWL_STYLE) & ~(WINDOW_STYLE | POPUP_STYLE);
    if (!cfg.fullscreen) {
        windowedPlacement.length = sizeof windowedPlacement;
        GetWindowPlacement(w, &windowedPlacement);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        cfg.fullscreen = 1;
        SetWindowLongA(w, GWL_STYLE, style | POPUP_STYLE);
        SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        cfg.fullscreen = 0;
        SetWindowLongA(w, GWL_STYLE, style | WINDOW_STYLE);
        if (windowedPlacement.length) {
            SetWindowPlacement(w, &windowedPlacement);
        } else {                              /* started full screen: use the default window */
            MONITORINFO mi = { sizeof mi };
            RECT r;
            GetMonitorInfoA(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
            window_rect_for(&mi, WINDOW_STYLE, GetWindowLongA(w, GWL_EXSTYLE), &r);
            SetWindowPos(w, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, 0);
        }
        SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    InvalidateRect(w, NULL, FALSE);
}

HWND WINAPI My_CreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style, int x, int y, int w, int h,
                               HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
{
    BOOL ours = cls && HIWORD((DWORD)cls) && lstrcmpA(cls, "Orly's Draw-A-Story") == 0;
    if (ours) {
        POINT origin = { 0, 0 };
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
        ex &= ~WS_EX_TOPMOST;
        if (cfg.fullscreen) {                      /* borderless full screen */
            style = POPUP_STYLE | (style & WS_VISIBLE);
            x = mi.rcMonitor.left; y = mi.rcMonitor.top;
            w = mi.rcMonitor.right - mi.rcMonitor.left; h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        } else {                                   /* resizable window */
            RECT r;
            style = WINDOW_STYLE | (style & WS_VISIBLE);
            window_rect_for(&mi, style, ex, &r);
            x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top;
        }
    }
    HWND hw = CreateWindowExA(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (ours && hw) {
        gameWnd = hw;
        origWndProc = (WNDPROC)SetWindowLongA(hw, GWL_WNDPROC, (LONG)GameWndProc);
    }
    return hw;
}

BOOL WINAPI My_ShowWindow(HWND w, int cmd)
{
    if (is_game_wnd(w)) {
        if (cmd == SW_MAXIMIZE) cmd = IsIconic(w) ? SW_RESTORE : SW_SHOW;   /* "full screen" = restore */
        if (cmd == SW_MINIMIZE && !cfg.fullscreen) return TRUE;           /* windowed: stay visible  */
    }
    return ShowWindow(w, cmd);
}

BOOL WINAPI My_MoveWindow(HWND w, int x, int y, int cw, int ch, BOOL repaint)
{
    return is_game_wnd(w) ? TRUE : MoveWindow(w, x, y, cw, ch, repaint);
}

BOOL WINAPI My_GetClientRect(HWND w, LPRECT r)
{
    if (is_game_wnd(w) && r) { SetRect(r, 0, 0, GAME_W, GAME_H); return TRUE; }
    return GetClientRect(w, r);
}

BOOL WINAPI My_GetWindowRect(HWND w, LPRECT r)
{
    if (is_game_wnd(w) && r) { SetRect(r, 0, 0, GAME_W, GAME_H); return TRUE; }
    return GetWindowRect(w, r);
}

BOOL WINAPI My_ClientToScreen(HWND w, LPPOINT p)
{
    return is_game_wnd(w) ? TRUE : ClientToScreen(w, p);   /* game window sits at (0,0) */
}

BOOL WINAPI My_GetCursorPos(LPPOINT p)
{
    BOOL r = GetCursorPos(p);
    HWND w = game_hwnd();
    if (r && w) { ScreenToClient(w, p); compute_view(); real_to_game(&p->x, &p->y); }
    return r;
}

BOOL WINAPI My_SetCursorPos(int x, int y)
{
    HWND w = game_hwnd();
    if (!w) return SetCursorPos(x, y);
    POINT p = { x, y };
    compute_view();
    game_to_real(&p.x, &p.y);
    ClientToScreen(w, &p);
    return SetCursorPos(p.x, p.y);
}

/* ---- repaint requests, in game coordinates ---- */

static void vupd_add(const RECT *rc)
{
    HRGN r = rc ? CreateRectRgnIndirect(rc) : CreateRectRgn(0, 0, GAME_W, GAME_H);
    CombineRgn(vUpd, vUpd, r, RGN_OR);
    DeleteObject(r);
}

BOOL WINAPI My_InvalidateRect(HWND w, const RECT *rc, BOOL erase)
{
    if (is_game_wnd(w)) { vupd_add(rc); return InvalidateRect(w, NULL, FALSE); }   /* -> WM_PAINT */
    return InvalidateRect(w, rc, erase);
}

BOOL WINAPI My_InvalidateRgn(HWND w, HRGN rg, BOOL erase)
{
    if (!is_game_wnd(w)) return InvalidateRgn(w, rg, erase);
    if (rg) CombineRgn(vUpd, vUpd, rg, RGN_OR); else vupd_add(NULL);
    return InvalidateRect(w, NULL, FALSE);
}

BOOL WINAPI My_ValidateRect(HWND w, const RECT *rc)
{
    if (!is_game_wnd(w)) return ValidateRect(w, rc);
    if (!rc) SetRectRgn(vUpd, 0, 0, 0, 0);
    else { HRGN r = CreateRectRgnIndirect(rc); CombineRgn(vUpd, vUpd, r, RGN_DIFF); DeleteObject(r); }
    return TRUE;
}

BOOL WINAPI My_ValidateRgn(HWND w, HRGN rg)
{
    if (!is_game_wnd(w)) return ValidateRgn(w, rg);
    if (!rg) SetRectRgn(vUpd, 0, 0, 0, 0); else CombineRgn(vUpd, vUpd, rg, RGN_DIFF);
    return TRUE;
}

int WINAPI My_GetUpdateRgn(HWND w, HRGN rg, BOOL erase)
{
    return is_game_wnd(w) ? CombineRgn(rg, vUpd, NULL, RGN_COPY) : GetUpdateRgn(w, rg, erase);
}

HDC WINAPI My_BeginPaint(HWND w, LPPAINTSTRUCT ps)
{
    if (!is_game_wnd(w)) return BeginPaint(w, ps);
    ZeroMemory(ps, sizeof *ps);
    ps->hdc = GetDC(w);
    GetRgnBox(vUpd, &ps->rcPaint);
    SetRectRgn(vUpd, 0, 0, 0, 0);
    return ps->hdc;
}

BOOL WINAPI My_EndPaint(HWND w, const PAINTSTRUCT *ps)
{
    if (!is_game_wnd(w)) return EndPaint(w, ps);
    ReleaseDC(w, ps->hdc);
    return TRUE;
}

/* Called first by GameWndProc for every message to the game window. */
static BOOL display_wndproc(HWND w, UINT m, LPARAM *lp, LRESULT *res)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;                       /* the real window: just show the screen */
        BeginPaint(w, &ps);
        EndPaint(w, &ps);
        emu_present(TRUE);
        RECT b;
        if (GetRgnBox(vUpd, &b) != NULLREGION) {    /* the game asked to repaint */
            CallWindowProcA(origWndProc, w, WM_PAINT, 0, 0);
            emu_present(FALSE);
        }
        *res = 0;
        return TRUE;
    }
    case WM_ERASEBKGND:
        *res = 1;
        return TRUE;
    case WM_SIZE:
        *lp = MAKELPARAM(GAME_W, GAME_H);     /* the game always sees 640x480 */
        InvalidateRect(w, NULL, FALSE);       /* show at the new size */
        return FALSE;
    case WM_GETMINMAXINFO: {                  /* window no smaller than a 320x240 picture */
        RECT r = { 0, 0, GAME_W / 2, GAME_H / 2 };
        AdjustWindowRectEx(&r, GetWindowLongA(w, GWL_STYLE), FALSE, GetWindowLongA(w, GWL_EXSTYLE));
        MINMAXINFO *mm = (MINMAXINFO *)*lp;
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        *res = 0;
        return TRUE;
    }
    }
    if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL) {
        LONG x = (short)LOWORD(*lp), y = (short)HIWORD(*lp);
        compute_view();
        real_to_game(&x, &y);
        *lp = MAKELPARAM(x, y);
    }
    return FALSE;
}

static void display_install(void)
{
    SetProcessDPIAware();                     /* real pixels, so scaling is sharp */
    vUpd = CreateRectRgn(0, 0, 0, 0);
    HOOK_IMPORT(GetProcAddress,          My_GetProcAddress);
    HOOK_IMPORT(SetSystemPaletteUse,     My_SetSystemPaletteUse);
    HOOK_IMPORT(SetSysColors,            My_SetSysColors);
    HOOK_IMPORT(SendMessageA,            My_SendMessageA);
    HOOK_IMPORT(PostMessageA,            My_PostMessageA);
    HOOK_IMPORT(GetDeviceCaps,           My_GetDeviceCaps);
    HOOK_IMPORT(GetSystemMetrics,        My_GetSystemMetrics);
    HOOK_IMPORT(GetSystemPaletteEntries, My_GetSystemPaletteEntries);
    HOOK_IMPORT(GetSystemPaletteUse,     My_GetSystemPaletteUse);
    HOOK_IMPORT(CreateCompatibleBitmap,  My_CreateCompatibleBitmap);
    HOOK_IMPORT(CreateWindowExA,         My_CreateWindowExA);
    HOOK_IMPORT(ShowWindow,              My_ShowWindow);
    HOOK_IMPORT(MoveWindow,              My_MoveWindow);
    HOOK_IMPORT(GetClientRect,           My_GetClientRect);
    HOOK_IMPORT(GetWindowRect,           My_GetWindowRect);
    HOOK_IMPORT(ClientToScreen,          My_ClientToScreen);
    HOOK_IMPORT(GetCursorPos,            My_GetCursorPos);
    HOOK_IMPORT(SetCursorPos,            My_SetCursorPos);
    HOOK_IMPORT(InvalidateRect,          My_InvalidateRect);
    HOOK_IMPORT(InvalidateRgn,           My_InvalidateRgn);
    HOOK_IMPORT(ValidateRect,            My_ValidateRect);
    HOOK_IMPORT(ValidateRgn,             My_ValidateRgn);
    HOOK_IMPORT(GetUpdateRgn,            My_GetUpdateRgn);
    HOOK_IMPORT(BeginPaint,              My_BeginPaint);
    HOOK_IMPORT(EndPaint,                My_EndPaint);
}
