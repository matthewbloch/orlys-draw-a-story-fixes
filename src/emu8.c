/* emu8.c - the game's 256-colour screen (#included by orlyfix.c).
 *
 * The game draws for 8-bit hardware: the screen holds palette indices, so a
 * palette change recolours everything already drawn, and some sprites are
 * drawn straight to the screen and kept nowhere else. Here the "screen" is a
 * 640x480 8-bit DIB (the shadow). Every drawing call aimed at the game window
 * is done on the shadow instead; blits from 8-bit bitmaps copy the raw indices
 * (including the bitwise ROPs used for sprite masks) so GDI's colour matching
 * can never alter them. present.c shows the shadow, scaled, through the
 * current palette.
 */

static struct {
    HDC dc; HBITMAP bmp;
    BYTE *bits; int w, h, stride;
    RGBQUAD pal[256]; BOOL palKnown;
} sh;
static HPALETTE screenPal;          /* the palette the game realizes into its window */
static DWORD mainTid;

static BOOL emu_target(HDC dc)
{
    HWND w = game_hwnd();
    return w && dc && GetCurrentThreadId() == mainTid && WindowFromDC(dc) == w;
}

static BOOL read_game_palette(RGBQUAD out[256])
{
    PALETTEENTRY pe[256];
    if (!screenPal || GetPaletteEntries(screenPal, 0, 256, pe) != 256) return FALSE;
    for (int i = 0; i < 256; i++) {
        out[i].rgbRed = pe[i].peRed; out[i].rgbGreen = pe[i].peGreen;
        out[i].rgbBlue = pe[i].peBlue; out[i].rgbReserved = 0;
    }
    return TRUE;
}

static BOOL ensure_shadow(void)
{
    if (sh.dc) return TRUE;
    struct { BITMAPINFOHEADER h; RGBQUAD c[256]; } bi;
    ZeroMemory(&bi, sizeof bi);
    bi.h.biSize = sizeof bi.h; bi.h.biWidth = GAME_W; bi.h.biHeight = -GAME_H;   /* top-down */
    bi.h.biPlanes = 1; bi.h.biBitCount = 8; bi.h.biCompression = BI_RGB; bi.h.biClrUsed = 256;
    sh.palKnown = read_game_palette(bi.c);
    memcpy(sh.pal, bi.c, sizeof sh.pal);
    void *bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = dc ? CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp) { if (dc) DeleteDC(dc); return FALSE; }
    SelectObject(dc, bmp);
    sh.dc = dc; sh.bmp = bmp; sh.bits = bits;
    sh.w = GAME_W; sh.h = GAME_H; sh.stride = (GAME_W + 3) & ~3;
    return TRUE;
}

/* Clip rectangles of a DC in device coordinates (whole screen if no clip region). */
static char rgnbuf[sizeof(RGNDATAHEADER) + 1024 * sizeof(RECT)];
static int clip_rects(HDC d, RECT **out)
{
    static RECT whole;
    SetRect(&whole, 0, 0, sh.w, sh.h);
    *out = &whole;
    int n = 1;
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    if (GetClipRgn(d, r) == 1) {
        DWORD need = GetRegionData(r, 0, NULL);
        if (need && need <= sizeof rgnbuf && GetRegionData(r, need, (RGNDATA *)rgnbuf)) {
            RGNDATA *rd = (RGNDATA *)rgnbuf;
            *out = (RECT *)rd->Buffer; n = (int)rd->rdh.nCount;
        }
    }
    DeleteObject(r);
    return n;
}

static void to_device(HDC d, int x, int y, int w, int h, int *ox, int *oy, int *ow, int *oh)
{
    POINT p[2] = { { x, y }, { x + w, y + h } };
    LPtoDP(d, p, 2);
    *ox = p[0].x; *oy = p[0].y; *ow = p[1].x - p[0].x; *oh = p[1].y - p[0].y;
}

/* Raster operations that only involve source and destination, applied to indices. */
static int rop_op(DWORD rop)
{
    switch (rop) {
    case SRCCOPY:     return 0;
    case SRCPAINT:    return 1;
    case SRCAND:      return 2;
    case SRCINVERT:   return 3;
    case SRCERASE:    return 4;
    case NOTSRCCOPY:  return 5;
    case NOTSRCERASE: return 6;
    case MERGEPAINT:  return 7;
    }
    return -1;
}

static inline BYTE rop_apply(int op, BYTE d, BYTE s)
{
    switch (op) {
    case 0: return s;
    case 1: return d | s;
    case 2: return d & s;
    case 3: return d ^ s;
    case 4: return s & (BYTE)~d;
    case 5: return (BYTE)~s;
    case 6: return (BYTE)~(s | d);
    default: return (BYTE)(~s | d);
    }
}

/* Copy (and stretch) 8-bit source pixels into the shadow at device rect (dx,dy,dw,dh).
 * Source column = sx + (x-dx)*sw/dw; source memory row = rowBase + rowStep*((y-dy)*sh/dh). */
static void put_indices(HDC d, int dx, int dy, int dw, int dh,
                        const BYTE *src, int sstride, int srcW, int srcRows,
                        int sx, int rowBase, int rowStep, int sw, int shh, int op, const BYTE *xlat)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || shh <= 0) return;
    RECT *cr; int nc = clip_rects(d, &cr);
    GdiFlush();
    for (int k = 0; k < nc; k++) {
        int x0 = dx, x1 = dx + dw, y0 = dy, y1 = dy + dh;
        if (x0 < cr[k].left) x0 = cr[k].left;
        if (y0 < cr[k].top) y0 = cr[k].top;
        if (x1 > cr[k].right) x1 = cr[k].right;
        if (y1 > cr[k].bottom) y1 = cr[k].bottom;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > sh.w) x1 = sh.w;
        if (y1 > sh.h) y1 = sh.h;
        for (int y = y0; y < y1; y++) {
            int r = rowBase + rowStep * (int)((long long)(y - dy) * shh / dh);
            if (r < 0 || r >= srcRows) continue;
            const BYTE *srow = src + (size_t)r * sstride;
            BYTE *drow = sh.bits + (size_t)y * sh.stride;
            for (int x = x0; x < x1; x++) {
                int c = sx + (int)((long long)(x - dx) * sw / dw);
                if (c < 0 || c >= srcW) continue;
                BYTE s = srow[c];
                if (xlat) s = xlat[s];
                drow[x] = rop_apply(op, drow[x], s);
            }
        }
    }
}

/* For everything else: make the shadow DC look like the game's DC and let GDI draw. */
static int gdi_begin(HDC d)
{
    int saved = SaveDC(sh.dc);
    int mm = GetMapMode(d);
    POINT pt; SIZE sz;
    SetMapMode(sh.dc, mm);
    if (mm != MM_TEXT) {
        GetWindowExtEx(d, &sz);   SetWindowExtEx(sh.dc, sz.cx, sz.cy, NULL);
        GetViewportExtEx(d, &sz); SetViewportExtEx(sh.dc, sz.cx, sz.cy, NULL);
    }
    GetWindowOrgEx(d, &pt);   SetWindowOrgEx(sh.dc, pt.x, pt.y, NULL);
    GetViewportOrgEx(d, &pt); SetViewportOrgEx(sh.dc, pt.x, pt.y, NULL);
    HRGN r = CreateRectRgn(0, 0, 0, 0);
    if (GetClipRgn(d, r) == 1) SelectClipRgn(sh.dc, r); else SelectClipRgn(sh.dc, NULL);
    DeleteObject(r);
    SelectObject(sh.dc, GetCurrentObject(d, OBJ_BRUSH));
    SelectObject(sh.dc, GetCurrentObject(d, OBJ_PEN));
    SelectPalette(sh.dc, (HPALETTE)GetCurrentObject(d, OBJ_PAL), TRUE);
    GetCurrentPositionEx(d, &pt); MoveToEx(sh.dc, pt.x, pt.y, NULL);
    SetROP2(sh.dc, GetROP2(d));
    SetPolyFillMode(sh.dc, GetPolyFillMode(d));
    SetBkMode(sh.dc, GetBkMode(d));
    SetBkColor(sh.dc, GetBkColor(d));
    SetTextColor(sh.dc, GetTextColor(d));
    return saved;
}

static void gdi_end(int saved)
{
    GdiFlush();
    RestoreDC(sh.dc, saved);
    emu_dirty();
}

static void mirror_blt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int shh, DWORD rop)
{
    int op = rop_op(rop);
    int dx, dy, dw, dh;
    to_device(d, x, y, w, h, &dx, &dy, &dw, &dh);
    if (op >= 0 && s && dw > 0 && dh > 0 && sw > 0 && shh > 0) {
        int sdx, sdy, sdw, sdh;
        to_device(s, sx, sy, sw, shh, &sdx, &sdy, &sdw, &sdh);
        if (sdw > 0 && sdh > 0) {
            if (emu_target(s)) {               /* screen -> screen: the source is the shadow */
                BYTE *tmp = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)sdw * sdh);
                if (tmp) {
                    GdiFlush();
                    for (int j = 0; j < sdh; j++)
                        for (int i = 0; i < sdw; i++) {
                            int yy = sdy + j, xx = sdx + i;
                            if (yy >= 0 && yy < sh.h && xx >= 0 && xx < sh.w)
                                tmp[(size_t)j * sdw + i] = sh.bits[(size_t)yy * sh.stride + xx];
                        }
                    put_indices(d, dx, dy, dw, dh, tmp, sdw, sdw, sdh, 0, 0, 1, sdw, sdh, op, NULL);
                    HeapFree(GetProcessHeap(), 0, tmp);
                    emu_dirty();
                    return;
                }
            } else {
                HGDIOBJ hb = GetCurrentObject(s, OBJ_BITMAP);
                DIBSECTION ds;
                if (hb && GetObject(hb, sizeof ds, &ds) == sizeof ds &&
                    ds.dsBm.bmBitsPixel == 8 && ds.dsBm.bmBits) {
                    int H = ds.dsBmih.biHeight < 0 ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;
                    BOOL bottomUp = ds.dsBmih.biHeight > 0;
                    put_indices(d, dx, dy, dw, dh, ds.dsBm.bmBits, ds.dsBm.bmWidthBytes, ds.dsBm.bmWidth, H,
                                sdx, bottomUp ? H - 1 - sdy : sdy, bottomUp ? -1 : 1, sdw, sdh, op, NULL);
                    emu_dirty();
                    return;
                }
            }
        }
    }
    int sv = gdi_begin(d);
    if (!s) PatBlt(sh.dc, x, y, w, h, rop);
    else StretchBlt(sh.dc, x, y, w, h, emu_target(s) ? sh.dc : s, sx, sy, sw, shh, rop);
    gdi_end(sv);
}

/* Reading from the screen into an 8-bit DIB section: copy indices from the shadow. */
static BOOL shadow_to_dib(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    int op = rop_op(rop);
    HGDIOBJ hb = GetCurrentObject(d, OBJ_BITMAP);
    DIBSECTION ds;
    if (op < 0 || !hb || GetObject(hb, sizeof ds, &ds) != sizeof ds || ds.dsBm.bmBitsPixel != 8 || !ds.dsBm.bmBits)
        return FALSE;
    int dx, dy, dw, dh, sdx, sdy, sdw, sdh;
    to_device(d, x, y, w, h, &dx, &dy, &dw, &dh);
    to_device(s, sx, sy, w, h, &sdx, &sdy, &sdw, &sdh);
    int H = ds.dsBmih.biHeight < 0 ? -ds.dsBmih.biHeight : ds.dsBmih.biHeight;
    BOOL bottomUp = ds.dsBmih.biHeight > 0;
    BYTE *bits = ds.dsBm.bmBits;
    GdiFlush();
    for (int j = 0; j < dh; j++) {
        int ty = dy + j, syy = sdy + j;
        if (ty < 0 || ty >= H || syy < 0 || syy >= sh.h) continue;
        BYTE *drow = bits + (size_t)(bottomUp ? H - 1 - ty : ty) * ds.dsBm.bmWidthBytes;
        for (int i = 0; i < dw; i++) {
            int tx = dx + i, sxx = sdx + i;
            if (tx < 0 || tx >= ds.dsBm.bmWidth || sxx < 0 || sxx >= sh.w) continue;
            drow[tx] = rop_apply(op, drow[tx], sh.bits[(size_t)syy * sh.stride + sxx]);
        }
    }
    return TRUE;
}

/* ---- hooked drawing functions: game window -> shadow, everything else -> GDI ---- */

BOOL WINAPI My_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    if (s && emu_target(s) && !emu_target(d) && ensure_shadow()) {     /* reading the screen */
        if (shadow_to_dib(d, x, y, w, h, s, sx, sy, rop)) return TRUE;
        int sdx, sdy, sdw, sdh;
        to_device(s, sx, sy, w, h, &sdx, &sdy, &sdw, &sdh);
        GdiFlush();
        return BitBlt(d, x, y, w, h, sh.dc, sdx, sdy, rop);
    }
    if (emu_target(d) && ensure_shadow()) { mirror_blt(d, x, y, w, h, s, sx, sy, w, h, rop); return TRUE; }
    return BitBlt(d, x, y, w, h, s, sx, sy, rop);
}

BOOL WINAPI My_StretchBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int shh, DWORD rop)
{
    if (emu_target(d) && ensure_shadow()) { mirror_blt(d, x, y, w, h, s, sx, sy, sw, shh, rop); return TRUE; }
    return StretchBlt(d, x, y, w, h, s, sx, sy, sw, shh, rop);
}

int WINAPI My_StretchDIBits(HDC d, int xD, int yD, int wD, int hD, int xS, int yS, int wS, int hS,
                            const void *bits, const BITMAPINFO *bmi, UINT usage, DWORD rop)
{
    if (!emu_target(d) || !ensure_shadow())
        return StretchDIBits(d, xD, yD, wD, hD, xS, yS, wS, hS, bits, bmi, usage, rop);

    int op = rop_op(rop), dx, dy, dw, dh;
    to_device(d, xD, yD, wD, hD, &dx, &dy, &dw, &dh);
    const BITMAPINFOHEADER *bh = &bmi->bmiHeader;
    if (op >= 0 && bits && bh->biBitCount == 8 && bh->biCompression == BI_RGB &&
        dw > 0 && dh > 0 && wS > 0 && hS > 0) {
        BYTE xl[256], *xlat = NULL;
        if (usage == DIB_PAL_COLORS) {         /* colour table holds palette indices */
            const WORD *ix = (const WORD *)((const BYTE *)bmi + bh->biSize);
            int n = bh->biClrUsed ? (int)bh->biClrUsed : 256;
            for (int i = 0; i < 256; i++) xl[i] = (BYTE)(i < n ? ix[i] : i);
            xlat = xl;
        }
        int H = bh->biHeight < 0 ? -bh->biHeight : bh->biHeight;
        int stride = ((bh->biWidth * 8 + 31) / 32) * 4;
        BOOL bottomUp = bh->biHeight > 0;      /* ySrc counts from the bottom for bottom-up DIBs */
        put_indices(d, dx, dy, dw, dh, bits, stride, bh->biWidth, H,
                    xS, bottomUp ? yS + hS - 1 : yS, bottomUp ? -1 : 1, wS, hS, op, xlat);
        emu_dirty();
    } else {
        int sv = gdi_begin(d);
        StretchDIBits(sh.dc, xD, yD, wD, hD, xS, yS, wS, hS, bits, bmi, usage, rop);
        gdi_end(sv);
    }
    return hS;
}

#define SHADOW_OR_GDI(call_on_shadow, real_call)                        \
    if (emu_target(d) && ensure_shadow()) {                             \
        int sv = gdi_begin(d); call_on_shadow; gdi_end(sv); return TRUE; \
    }                                                                   \
    return real_call;

BOOL WINAPI My_PatBlt(HDC d, int x, int y, int w, int h, DWORD rop)
{ SHADOW_OR_GDI(PatBlt(sh.dc, x, y, w, h, rop), PatBlt(d, x, y, w, h, rop)) }

int WINAPI My_FillRect(HDC d, const RECT *rc, HBRUSH b)
{ SHADOW_OR_GDI(FillRect(sh.dc, rc, b), FillRect(d, rc, b)) }

BOOL WINAPI My_FillRgn(HDC d, HRGN rg, HBRUSH b)
{ SHADOW_OR_GDI(FillRgn(sh.dc, rg, b), FillRgn(d, rg, b)) }

BOOL WINAPI My_Rectangle(HDC d, int l, int t, int r, int b)
{ SHADOW_OR_GDI(Rectangle(sh.dc, l, t, r, b), Rectangle(d, l, t, r, b)) }

BOOL WINAPI My_Ellipse(HDC d, int l, int t, int r, int b)
{ SHADOW_OR_GDI(Ellipse(sh.dc, l, t, r, b), Ellipse(d, l, t, r, b)) }

BOOL WINAPI My_Polygon(HDC d, const POINT *p, int n)
{ SHADOW_OR_GDI(Polygon(sh.dc, p, n), Polygon(d, p, n)) }

BOOL WINAPI My_LineTo(HDC d, int x, int y)
{ SHADOW_OR_GDI(LineTo(sh.dc, x, y), LineTo(d, x, y)) }

/* ScrollDC: move the shadow's pixels (overlap-safe) and report the uncovered area. */
BOOL WINAPI My_ScrollDC(HDC d, int ddx, int ddy, const RECT *scroll, const RECT *clip, HRGN upd, LPRECT updRc)
{
    if (!emu_target(d) || !ensure_shadow()) return ScrollDC(d, ddx, ddy, scroll, clip, upd, updRc);

    /* uncovered area, as ScrollDC reports it: (scroll & clip) minus its moved copy */
    RECT a = scroll ? *scroll : (RECT){ 0, 0, GAME_W, GAME_H }, b = a, cl = clip ? *clip : a;
    OffsetRect(&b, ddx, ddy);
    HRGN ra = CreateRectRgnIndirect(&a), rb = CreateRectRgnIndirect(&b), rc = CreateRectRgnIndirect(&cl);
    CombineRgn(ra, ra, rc, RGN_AND);
    CombineRgn(rb, rb, rc, RGN_AND);
    CombineRgn(ra, ra, rb, RGN_DIFF);
    if (upd) CombineRgn(upd, ra, NULL, RGN_COPY);
    if (updRc) GetRgnBox(ra, updRc);
    DeleteObject(ra); DeleteObject(rb); DeleteObject(rc);

    RECT s = { 0, 0, sh.w, sh.h }, c = s, t;
    if (scroll) { s = *scroll; LPtoDP(d, (POINT *)&s, 2); }
    if (clip)   { c = *clip;   LPtoDP(d, (POINT *)&c, 2); }
    POINT o[2] = { { 0, 0 }, { ddx, ddy } };
    LPtoDP(d, o, 2);
    int mx = o[1].x - o[0].x, my = o[1].y - o[0].y;
    /* destination: scrolled rect within the clip, the screen, and where the source is on screen */
    RECT dst = { s.left + mx, s.top + my, s.right + mx, s.bottom + my };
    RECT whole = { 0, 0, sh.w, sh.h }, srcOk = { mx, my, sh.w + mx, sh.h + my };
    if (!IntersectRect(&t, &dst, &c) || !IntersectRect(&dst, &t, &whole) || !IntersectRect(&t, &dst, &srcOk))
        return TRUE;
    dst = t;
    GdiFlush();
    int width = dst.right - dst.left;
    if (my > 0)   /* moving down: copy bottom rows first */
        for (int y = dst.bottom - 1; y >= dst.top; y--)
            memmove(sh.bits + (size_t)y * sh.stride + dst.left, sh.bits + (size_t)(y - my) * sh.stride + dst.left - mx, width);
    else
        for (int y = dst.top; y < dst.bottom; y++)
            memmove(sh.bits + (size_t)y * sh.stride + dst.left, sh.bits + (size_t)(y - my) * sh.stride + dst.left - mx, width);
    emu_dirty();
    return TRUE;
}

COLORREF WINAPI My_GetPixel(HDC d, int x, int y)
{
    if (emu_target(d) && ensure_shadow()) {
        POINT p = { x, y };
        LPtoDP(d, &p, 1);
        if (p.x < 0 || p.y < 0 || p.x >= sh.w || p.y >= sh.h) return CLR_INVALID;
        GdiFlush();
        RGBQUAD q = sh.pal[sh.bits[(size_t)p.y * sh.stride + p.x]];
        return RGB(q.rgbRed, q.rgbGreen, q.rgbBlue);
    }
    return GetPixel(d, x, y);
}

/* ---- palette: a change recolours the whole screen, as on the hardware ---- */

static void emu_palette_check(void)
{
    if (!sh.dc || GetCurrentThreadId() != mainTid) return;
    RGBQUAD p[256];
    if (!read_game_palette(p)) return;
    if (sh.palKnown && memcmp(p, sh.pal, sizeof p) == 0) return;
    emu_palette_changing();
    memcpy(sh.pal, p, sizeof p);
    sh.palKnown = TRUE;
    SetDIBColorTable(sh.dc, 0, 256, sh.pal);
    emu_palette_changed();
}

UINT WINAPI My_RealizePalette(HDC d)
{
    UINT r = RealizePalette(d);
    if (emu_target(d)) {
        HPALETTE p = (HPALETTE)GetCurrentObject(d, OBJ_PAL);
        if (p && p != (HPALETTE)GetStockObject(DEFAULT_PALETTE)) screenPal = p;
        if (ensure_shadow()) emu_palette_check();
    }
    return r;
}

BOOL WINAPI My_AnimatePalette(HPALETTE pal, UINT start, UINT n, const PALETTEENTRY *pe)
{
    if (!screenPal && n == 256) screenPal = pal;
    BOOL r = AnimatePalette(pal, start, n, pe);
    emu_palette_check();
    return r;
}

UINT WINAPI My_SetPaletteEntries(HPALETTE pal, UINT start, UINT n, const PALETTEENTRY *pe)
{
    if (!screenPal && n == 256) screenPal = pal;
    UINT r = SetPaletteEntries(pal, start, n, pe);
    emu_palette_check();
    return r;
}

static void emu_install(void)
{
    mainTid = GetCurrentThreadId();
    HOOK_IMPORT(BitBlt,            My_BitBlt);
    HOOK_IMPORT(StretchBlt,        My_StretchBlt);
    HOOK_IMPORT(StretchDIBits,     My_StretchDIBits);
    HOOK_IMPORT(PatBlt,            My_PatBlt);
    HOOK_IMPORT(FillRect,          My_FillRect);
    HOOK_IMPORT(FillRgn,           My_FillRgn);
    HOOK_IMPORT(ScrollDC,          My_ScrollDC);
    HOOK_IMPORT(Rectangle,         My_Rectangle);
    HOOK_IMPORT(Ellipse,           My_Ellipse);
    HOOK_IMPORT(Polygon,           My_Polygon);
    HOOK_IMPORT(LineTo,            My_LineTo);
    HOOK_IMPORT(GetPixel,          My_GetPixel);
    HOOK_IMPORT(RealizePalette,    My_RealizePalette);
    HOOK_IMPORT(AnimatePalette,    My_AnimatePalette);
    HOOK_IMPORT(SetPaletteEntries, My_SetPaletteEntries);
}
