/* present.c - show the 640x480 screen in the window (#included by orlyfix.c).
 *
 * The shadow is shown at the largest 4:3 size that fits, with black bars, by
 * the GPU (OpenGL 1.1 + GLSL 1.10, loaded at run time), using Filter (F1-F4):
 *   0 = sharp pixels   1 = bilinear   2 = bicubic (Catmull-Rom)   3 = sharp bilinear
 * If OpenGL is unavailable, GDI's StretchBlt is used instead.
 * The screen is presented when a frame is complete (see WinMain), when the
 * palette changes, and - for drawing outside the normal frame rhythm, e.g. the
 * engine's own loops - after 40 ms (100 ms while inside a frame).
 */
#include <GL/gl.h>

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER   0x8B31
#define GL_COMPILE_STATUS  0x8B81
#define GL_LINK_STATUS     0x8B82
typedef char GLchar;
typedef GLuint (APIENTRY *PFNCREATESHADER)(GLenum);
typedef void   (APIENTRY *PFNSHADERSOURCE)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void   (APIENTRY *PFNCOMPILESHADER)(GLuint);
typedef void   (APIENTRY *PFNGETSHADERIV)(GLuint, GLenum, GLint *);
typedef GLuint (APIENTRY *PFNCREATEPROGRAM)(void);
typedef void   (APIENTRY *PFNATTACHSHADER)(GLuint, GLuint);
typedef void   (APIENTRY *PFNLINKPROGRAM)(GLuint);
typedef void   (APIENTRY *PFNGETPROGRAMIV)(GLuint, GLenum, GLint *);
typedef void   (APIENTRY *PFNUSEPROGRAM)(GLuint);
typedef GLint  (APIENTRY *PFNGETUNIFORMLOCATION)(GLuint, const GLchar *);
typedef void   (APIENTRY *PFNUNIFORM1I)(GLint, GLint);
typedef void   (APIENTRY *PFNUNIFORM2F)(GLint, GLfloat, GLfloat);
typedef BOOL   (WINAPI  *PFNSWAPINTERVAL)(int);

static RECT view;                /* where the image sits in the window's client area */
static BOOL shDirty;
static BOOL palPending;     /* a palette change during this frame, not yet shown */
static DWORD lastPresent;

static void compute_view(void)
{
    RECT c;
    if (!GetClientRect(game_hwnd(), &c)) return;
    int cw = c.right, ch = c.bottom, w = cw, h = cw * GAME_H / GAME_W;
    if (h > ch) { h = ch; w = ch * GAME_W / GAME_H; }
    view.left = (cw - w) / 2; view.top = (ch - h) / 2;
    view.right = view.left + w; view.bottom = view.top + h;
}

/* ---- GPU presenter ---- */

typedef struct { GLuint id; GLint uTex, uTexSize, uOutSize; } GLPROG;
static struct {
    HDC dc; HGLRC rc; GLuint tex; BOOL ready, failed;
    GLPROG bicubic, sharp;              /* id 0 if shaders are unavailable */
    GLint texFilter;                    /* current GL_TEXTURE_MIN/MAG_FILTER */
    PFNUSEPROGRAM UseProgram; PFNUNIFORM1I Uniform1i; PFNUNIFORM2F Uniform2f;
} gl;
static DWORD glRGBA[GAME_W * GAME_H];

static const char *glVertexSrc =
    "#version 110\n"
    "void main() { gl_Position = ftransform(); gl_TexCoord[0] = gl_MultiTexCoord0; }";

static const char *glBicubicSrc =          /* Catmull-Rom, texel centres sampled with GL_NEAREST */
    "#version 110\n"
    "uniform sampler2D tex; uniform vec2 texSize;\n"
    "float cr(float x) { x = abs(x);\n"
    "  if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;\n"
    "  if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;\n"
    "  return 0.0; }\n"
    "void main() {\n"
    "  vec2 p = gl_TexCoord[0].xy * texSize - 0.5;\n"
    "  vec2 i = floor(p), f = p - i;\n"
    "  vec4 sum = vec4(0.0); float wsum = 0.0;\n"
    "  for (int y = -1; y <= 2; y++)\n"
    "    for (int x = -1; x <= 2; x++) {\n"
    "      float w = cr(float(x) - f.x) * cr(float(y) - f.y);\n"
    "      sum += texture2D(tex, (i + vec2(float(x), float(y)) + 0.5) / texSize) * w;\n"
    "      wsum += w; }\n"
    "  gl_FragColor = clamp(sum / wsum, 0.0, 1.0);\n"
    "}\n";

static const char *glSharpSrc =            /* nearest inside texels, linear across their borders */
    "#version 110\n"
    "uniform sampler2D tex; uniform vec2 texSize; uniform vec2 outSize;\n"
    "void main() {\n"
    "  vec2 texel = gl_TexCoord[0].xy * texSize;\n"
    "  vec2 scale = max(outSize / texSize, vec2(1.0));\n"
    "  vec2 base = floor(texel), s = texel - base;\n"
    "  vec2 range = 0.5 - 0.5 / scale;\n"
    "  vec2 d = s - 0.5;\n"
    "  vec2 f = (d - clamp(d, -range, range)) * scale + 0.5;\n"
    "  gl_FragColor = texture2D(tex, (base + f) / texSize);\n"
    "}\n";

static GLPROG gl_shader_program(const char *fragSrc)
{
    GLPROG none = { 0, -1, -1, -1 };
    PFNCREATESHADER cs = (PFNCREATESHADER)wglGetProcAddress("glCreateShader");
    PFNSHADERSOURCE ss = (PFNSHADERSOURCE)wglGetProcAddress("glShaderSource");
    PFNCOMPILESHADER co = (PFNCOMPILESHADER)wglGetProcAddress("glCompileShader");
    PFNGETSHADERIV gi = (PFNGETSHADERIV)wglGetProcAddress("glGetShaderiv");
    PFNCREATEPROGRAM cp = (PFNCREATEPROGRAM)wglGetProcAddress("glCreateProgram");
    PFNATTACHSHADER as = (PFNATTACHSHADER)wglGetProcAddress("glAttachShader");
    PFNLINKPROGRAM lp = (PFNLINKPROGRAM)wglGetProcAddress("glLinkProgram");
    PFNGETPROGRAMIV gp = (PFNGETPROGRAMIV)wglGetProcAddress("glGetProgramiv");
    PFNGETUNIFORMLOCATION gu = (PFNGETUNIFORMLOCATION)wglGetProcAddress("glGetUniformLocation");
    gl.UseProgram = (PFNUSEPROGRAM)wglGetProcAddress("glUseProgram");
    gl.Uniform1i = (PFNUNIFORM1I)wglGetProcAddress("glUniform1i");
    gl.Uniform2f = (PFNUNIFORM2F)wglGetProcAddress("glUniform2f");
    if (!cs || !ss || !co || !gi || !cp || !as || !lp || !gp || !gu || !gl.UseProgram || !gl.Uniform1i || !gl.Uniform2f)
        return none;
    const char *src[2] = { glVertexSrc, fragSrc };
    GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint prog = cp();
    for (int i = 0; i < 2; i++) {
        GLuint sh_ = cs(type[i]);
        GLint ok = 0;
        ss(sh_, 1, &src[i], NULL);
        co(sh_);
        gi(sh_, GL_COMPILE_STATUS, &ok);
        if (!ok) return none;
        as(prog, sh_);
    }
    GLint ok = 0;
    lp(prog);
    gp(prog, GL_LINK_STATUS, &ok);
    if (!ok) return none;
    GLPROG r = { prog, gu(prog, "tex"), gu(prog, "texSize"), gu(prog, "outSize") };
    return r;
}

static BOOL gl_init(HWND w)
{
    gl.dc = GetDC(w);                   /* kept for the life of the window */
    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(gl.dc, &pfd);
    if (!pf || !SetPixelFormat(gl.dc, pf, &pfd)) return FALSE;
    gl.rc = wglCreateContext(gl.dc);
    if (!gl.rc || !wglMakeCurrent(gl.dc, gl.rc)) return FALSE;

    gl.bicubic = gl_shader_program(glBicubicSrc);     /* both ready, so F1-F4 switch instantly */
    gl.sharp = gl_shader_program(glSharpSrc);

    glGenTextures(1, &gl.tex);
    glBindTexture(GL_TEXTURE_2D, gl.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GAME_W, GAME_H, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glEnable(GL_TEXTURE_2D);

    PFNSWAPINTERVAL si = (PFNSWAPINTERVAL)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(cfg.vsync ? 1 : 0);
    return TRUE;
}

static BOOL gl_present(HWND w)
{
    if (gl.failed) return FALSE;
    if (!gl.ready) {
        if (!gl_init(w)) {
            gl.failed = TRUE;
            if (gl.rc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(gl.rc); gl.rc = NULL; }
            return FALSE;
        }
        gl.ready = TRUE;
    }

    DWORD lut[256];                     /* 8-bit shadow -> 32-bit BGRA through the palette */
    for (int i = 0; i < 256; i++)
        lut[i] = 0xFF000000u | ((DWORD)sh.pal[i].rgbRed << 16) | ((DWORD)sh.pal[i].rgbGreen << 8) | sh.pal[i].rgbBlue;
    GdiFlush();
    for (int y = 0; y < GAME_H; y++) {
        const BYTE *s = sh.bits + (size_t)y * sh.stride;
        DWORD *d = glRGBA + (size_t)y * GAME_W;
        for (int x = 0; x < GAME_W; x++) d[x] = lut[s[x]];
    }

    RECT c; GetClientRect(w, &c);
    int vw = view.right - view.left, vh = view.bottom - view.top;
    glViewport(0, 0, c.right, c.bottom);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport(view.left, c.bottom - view.bottom, vw, vh);     /* GL counts y from the bottom */
    /* Filter: 0 nearest; 1 bilinear; 2 bicubic shader (reads texel centres: nearest);
       3 sharp-bilinear shader (needs linear). Without shaders 2/3 fall back to bilinear. */
    const GLPROG *prog = cfg.filter == 2 && gl.bicubic.id ? &gl.bicubic :
                         cfg.filter == 3 && gl.sharp.id ? &gl.sharp : NULL;
    GLint filt = (cfg.filter == 0 || prog == &gl.bicubic) ? GL_NEAREST : GL_LINEAR;
    glBindTexture(GL_TEXTURE_2D, gl.tex);
    if (filt != gl.texFilter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
        gl.texFilter = filt;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GAME_W, GAME_H, GL_BGRA, GL_UNSIGNED_BYTE, glRGBA);
    if (prog) {
        gl.UseProgram(prog->id);
        gl.Uniform1i(prog->uTex, 0);
        gl.Uniform2f(prog->uTexSize, (GLfloat)GAME_W, (GLfloat)GAME_H);
        if (prog->uOutSize >= 0) gl.Uniform2f(prog->uOutSize, (GLfloat)vw, (GLfloat)vh);
    }
    glBegin(GL_QUADS);                                          /* image row 0 is the top */
    glTexCoord2f(0, 1); glVertex2f(-1, -1);
    glTexCoord2f(1, 1); glVertex2f( 1, -1);
    glTexCoord2f(1, 0); glVertex2f( 1,  1);
    glTexCoord2f(0, 0); glVertex2f(-1,  1);
    glEnd();
    if (prog) gl.UseProgram(0);
    SwapBuffers(gl.dc);
    return TRUE;
}

/* ---- GDI fallback ---- */

static void gdi_present(HWND w)
{
    RECT c, b;
    GetClientRect(w, &c);
    HDC dc = GetDC(w);
    GdiFlush();
    SetStretchBltMode(dc, cfg.filter == 0 ? COLORONCOLOR : HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, view.left, view.top, view.right - view.left, view.bottom - view.top,
               sh.dc, 0, 0, sh.w, sh.h, SRCCOPY);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (view.left > 0) { SetRect(&b, 0, 0, view.left, c.bottom);         FillRect(dc, &b, black);
                         SetRect(&b, view.right, 0, c.right, c.bottom);  FillRect(dc, &b, black); }
    if (view.top > 0)  { SetRect(&b, 0, 0, c.right, view.top);           FillRect(dc, &b, black);
                         SetRect(&b, 0, view.bottom, c.right, c.bottom); FillRect(dc, &b, black); }
    ReleaseDC(w, dc);
}

static void emu_present(BOOL force)
{
    HWND w = game_hwnd();
    if (!sh.dc || !w || (!shDirty && !force) || IsIconic(w) || GetCurrentThreadId() != mainTid) return;
    compute_view();
    if (!gl_present(w)) gdi_present(w);
    shDirty = FALSE;
    palPending = FALSE;
    lastPresent = GetTickCount();
}

static void emu_dirty(void)
{
    shDirty = TRUE;
    if (GetTickCount() - lastPresent >= (DWORD)(inFrame ? 100 : 40)) emu_present(FALSE);
}

/* A palette change inside a frame waits for the end of the frame, like drawing does: to cut
 * to a new picture the game sets the new palette first and then draws, and showing the screen
 * in between would flash the old picture in the new colours (a real 256-colour screen never
 * showed that, as both happened within one refresh). Outside a frame it is shown at once. */
static void emu_palette_changing(void)
{
    if (inFrame && palPending) emu_present(TRUE);   /* a second change in one frame: a fade
                                                       is running - show the previous step */
}

static void emu_palette_changed(void)
{
    if (!inFrame) { emu_present(TRUE); return; }
    shDirty = TRUE;
    palPending = TRUE;
}
