/* orlyfix.c - fixes for ORLY32.EXE (Orly's Draw-A-Story, Broderbund 1997), built as winspool.dll.
 *
 * ORLY32.EXE imports three printing functions from WINSPOOL.dll, a name that no longer
 * exists on 64-bit Windows (the real library is winspool.drv). Windows looks for imported
 * DLLs in the program's folder first, so this DLL, saved there as winspool.dll, is loaded
 * with the unmodified game: it passes those three functions on to the real winspool.drv,
 * and its DllMain - which runs before any game code - redirects game functions and
 * imports to the replacements here (and changes a few instructions, checked byte-for-byte):
 *   orlyfix.c  - one CPU core, main loop waits, engine task stacks, disk-space check,
 *                media keys, a crash when leaving the screen while a saved drawing loads,
 *                printer driver loading under Wine
 *   emu8.c     - the 256-colour screen, kept as an 8-bit image
 *   present.c  - showing that image scaled in the window (GPU, GDI fallback)
 *   display.c  - the window, a fake 640x480 256-colour display (no real mode changes,
 *                no DirectDraw, no system-wide colour or broadcast changes), mouse mapping
 *   userdata.c - the player's files kept in %APPDATA%\Orly's Draw-A-Story
 * Settings: orlyfix.ini - in the game folder, or (taking precedence) in the user folder.
 */
#include <windows.h>
#include <string.h>
#include "orly32.h"                         /* every game address and instruction byte */

#define APP_HWND (*(volatile HWND *)VAR_MAIN_HWND)

#define GAME_W 640
#define GAME_H 480

static struct { int fullscreen, windowScale, filter, vsync; } cfg;
static char gameIni[MAX_PATH], userIni[MAX_PATH];   /* settings: user copy overrides game folder */

static HWND gameWnd;                    /* the game's window, set when it is created */
static HWND game_hwnd(void) { return gameWnd ? gameWnd : APP_HWND; }
static WNDPROC origWndProc;             /* the game's own window procedure */
static volatile int inFrame;            /* the main loop is between passes (may be drawing a frame) */

static void emu_present(BOOL force);
static void emu_dirty(void);
static void emu_palette_changing(void);
static void emu_palette_changed(void);
static LRESULT CALLBACK GameWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp);

/* ---- patching helpers: every change is checked against the expected bytes ---- */

static char skipped[512];               /* names of patches that did not match */

static void note_skip(const char *name)
{
    if (lstrlenA(skipped) + lstrlenA(name) + 3 < (int)sizeof skipped) {
        lstrcatA(skipped, "\n  ");
        lstrcatA(skipped, name);
    }
}

/* Find the game's import address table slot for an imported function, by name. Windows fills
 * each slot with the function's address when it loads the game. ORLY32.EXE's linker wrote no
 * import name tables (OriginalFirstThunk is 0), so where a DLL has none the slot is found by
 * that address - GetProcAddress gives the same one the loader used, forwarded exports included. */
static void **import_slot(const char *name)
{
    BYTE *exe = (BYTE *)GetModuleHandleA(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return NULL;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(exe + d->FirstThunk);
        IMAGE_THUNK_DATA *names = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(exe + d->OriginalFirstThunk) : NULL;
        HMODULE dll = GetModuleHandleA((const char *)(exe + d->Name));
        FARPROC addr = dll ? GetProcAddress(dll, name) : NULL;
        if (!names && !addr) continue;                      /* not exported by this DLL */
        for (int i = 0; slots[i].u1.Function; i++) {
            if (names) {
                if (IMAGE_SNAP_BY_ORDINAL(names[i].u1.Ordinal)) continue;
                IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(exe + names[i].u1.AddressOfData);
                if (strcmp((const char *)n->Name, name) == 0) return (void **)&slots[i].u1.Function;
            } else if ((FARPROC)slots[i].u1.Function == addr) {
                return (void **)&slots[i].u1.Function;
            }
        }
    }
    return NULL;
}

/* Redirect one of the game's imports to fn. */
static void hook_import(const char *name, void *fn)
{
    void **slot = import_slot(name);
    DWORD old;
    if (!slot) { note_skip(name); return; }
    VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old);
    *slot = fn;
    VirtualProtect(slot, sizeof *slot, old, &old);
}
#define HOOK_IMPORT(name, fn) hook_import(#name, (fn))

static void patch_bytes(const char *name, DWORD addr, const void *expect, const void *repl, int n)
{
    BYTE *p = (BYTE *)addr; DWORD old;
    if (memcmp(p, expect, n) != 0) { note_skip(name); return; }
    VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, repl, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
}

static void warn_if_skipped(void)
{
    static BOOL shown;
    if (!skipped[0] || shown) return;
    shown = TRUE;
    char msg[700];
    wsprintfA(msg, "This ORLY32.EXE does not match the one orlyfix was made for, "
                   "so these fixes were not applied:%s", skipped);
    MessageBoxA(NULL, msg, "orlyfix", MB_OK | MB_ICONWARNING);
}

#include "emu8.c"
#include "present.c"
#include "display.c"
#include "userdata.c"

/* ---- the game window: our keys, then display handling, then the game ----
 * Alt+Enter toggles borderless full screen; F1-F4 pick the filter (saved to the ini).
 * Volume, mute, media, browser and app-launch keys (VK_BROWSER_BACK-VK_LAUNCH_APP2) did
 * not exist in 1997.
 * The game treats any key as "skip", so none of these reach it; Windows still acts
 * on the media keys as usual. */
#define IS_MEDIA_KEY(vk) ((vk) >= VK_BROWSER_BACK && (vk) <= VK_LAUNCH_APP2)
#define IS_FILTER_KEY(vk) ((vk) >= VK_F1 && (vk) <= VK_F4)

static void set_filter(int f)
{
    char v[4];
    cfg.filter = f;
    wsprintfA(v, "%d", f);
    WritePrivateProfileStringA("orlyfix", "Filter", v, userIni[0] ? userIni : gameIni);
    emu_present(TRUE);
}

static LRESULT CALLBACK GameWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    LRESULT res;
    BOOL key = m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP;
    if (key && IS_MEDIA_KEY(wp))
        return DefWindowProcA(w, m, wp, lp);
    if (key && IS_FILTER_KEY(wp)) {
        if (m == WM_KEYDOWN && !(lp & (1 << 30))) set_filter((int)(wp - VK_F1));
        return 0;
    }
    if (key && wp == VK_RETURN && (lp & (1 << 29))) {          /* Alt+Enter */
        if (m == WM_SYSKEYDOWN && !(lp & (1 << 30))) toggle_fullscreen(w);
        return 0;
    }
    if (m == WM_SYSCHAR && wp == '\r') return 0;               /* no beep for Alt+Enter */
    if (display_wndproc(w, m, &lp, &res)) return res;
    return CallWindowProcA(origWndProc, w, m, wp, lp);
}

BOOL WINAPI My_GetKeyboardState(PBYTE keys)
{
    BOOL r = GetKeyboardState(keys);
    if (r && keys) {
        for (int vk = VK_BROWSER_BACK; vk <= VK_LAUNCH_APP2; vk++) keys[vk] = 0;
        for (int vk = VK_F1; vk <= VK_F4; vk++) keys[vk] = 0;
        if (keys[VK_MENU] & 0x80) keys[VK_RETURN] = 0;       /* Alt+Enter */
    }
    return r;
}

/* ---- The main loop (CODE_MAIN_LOOP) never waits: when the message queue is
 * empty it checks the frame timer and goes straight round again, using a whole CPU core.
 * Every pass starts with PeekMessageA, so that is where it now waits - up to 1 ms (the
 * game sets a 1 ms timer resolution), or 50 ms when its window is inactive - waking at
 * once for input. Each pass is also a frame boundary: show the finished frame there. ---- */
BOOL WINAPI My_PeekMessageA(LPMSG m, HWND w, UINT first, UINT last, UINT remove)
{
    BOOL mainLoop = __builtin_return_address(0) == (void *)RET_MAIN_LOOP_PEEK;
    if (mainLoop) { inFrame = 0; warn_if_skipped(); emu_present(FALSE); }
    BOOL r = PeekMessageA(m, w, first, last, remove);
    if (mainLoop && !r) {
        HWND g = game_hwnd();
        MsgWaitForMultipleObjectsEx(0, NULL, g && GetActiveWindow() == g ? 1 : 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        inFrame = 1;                    /* until the next pass: the loop may now run a frame */
    }
    return r;
}

/* ---- GetDiskFreeSpaceA: the game multiplies the results in a signed 32-bit int
 * (CODE_DISK_FREE_MULTIPLY), which overflows above 2 GB free, so its "needs 5MB free" check failed
 * at random. Report at most 1 GB, as Windows' own compatibility fix does. If the drive
 * root can't be queried (Wine's Z:), report the free space of the game folder. ---- */
BOOL WINAPI My_GetDiskFreeSpaceA(LPCSTR root, LPDWORD spc, LPDWORD bps, LPDWORD freeCl, LPDWORD totalCl)
{
    DWORD a, b, c, d;
    BOOL ok = GetDiskFreeSpaceA(root, &a, &b, &c, &d);
    if (!ok) {
        /* Under Wine the root of Z: (Linux's /) often can't be queried. Ask about the game
           folder instead, so the game doesn't take a failed query for a full disk (it then
           deletes its story sprite archives). Reported as 4 KB clusters. */
        ULARGE_INTEGER avail, total;
        if (!GetDiskFreeSpaceExA(gameDir, &avail, &total, NULL)) return FALSE;
        a = 8; b = 512;
        c = avail.QuadPart / 4096 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (DWORD)(avail.QuadPart / 4096);
        d = total.QuadPart / 4096 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (DWORD)(total.QuadPart / 4096);
        ok = TRUE;
    }
    ULONGLONG cluster = (ULONGLONG)a * b, cap = (1ULL << 30) / (cluster ? cluster : 1);
    if (c > cap) c = (DWORD)cap;
    if (d > cap) d = (DWORD)cap;
    if (spc) *spc = a;
    if (bps) *bps = b;
    if (freeCl) *freeCl = c;
    if (totalCl) *totalCl = d;
    return ok;
}

/* ---- Engine task stacks: the engine runs cooperative tasks on 4-8 KB stacks (created
 * via CODE_TASK_CREATE). On Windows 10/11 the sound task's winmm calls go through the modern
 * audio stack and overflow 8 KB, killing the process. Give every task 60 KB, the most
 * that fits the engine's 64 KB reservation (CODE_TASK_STACK_ALLOC). ---- */
static void patch_task_stacks(void)
{
    static const BYTE push1000[] = BYTES_PUSH_1000H, push2000[] = BYTES_PUSH_2000H,
                      pushF000[] = BYTES_PUSH_F000H;
    patch_bytes("sound task stack", PATCH_SOUND_TASK_STACK, push2000, pushF000, sizeof pushF000);
    patch_bytes("task stack 2",     PATCH_TASK_STACK_2,     push1000, pushF000, sizeof pushF000);
    patch_bytes("task stack 3",     PATCH_TASK_STACK_3,     push1000, pushF000, sizeof pushF000);
    patch_bytes("task stack 4",     PATCH_TASK_STACK_4,     push1000, pushF000, sizeof pushF000);
}

/* ---- The printer port (CODE_PRINTER_PORT_INIT) loads the print dialog's driver name + ".DRV" with
 * LoadLibraryA. On Windows NT+ that name is always "winspool", so it loads winspool.drv.
 * Under Wine it is Wine's own driver (e.g. "wineps.drv" -> "wineps.drv.DRV"), the load
 * fails, and the game's failure path frees the port's clip region but still reports success,
 * so it crashes moments later. A failed ".DRV" load is retried without the extra ".DRV",
 * then as winspool.drv - what the game gets on Windows. ---- */
HMODULE WINAPI My_LoadLibraryA(LPCSTR name)
{
    HMODULE h = LoadLibraryA(name);
    int n = name ? lstrlenA(name) : 0;
    if (h || n < 5 || lstrcmpiA(name + n - 4, ".DRV")) return h;
    char shorter[MAX_PATH];
    if (n - 4 < MAX_PATH) {
        lstrcpynA(shorter, name, n - 4 + 1);               /* drop the appended ".DRV" */
        if (strchr(shorter, '.')) h = LoadLibraryA(shorter);
    }
    if (!h) h = LoadLibraryA("winspool.drv");
    return h;
}

/* ---- the printing functions the game imports from WINSPOOL.dll (exported by winspool.def) ---- */
static FARPROC real_winspool(const char *name)
{
    static HMODULE lib;
    if (!lib) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH - 16);
        lstrcpyA(path + n, "\\winspool.drv");
        lib = LoadLibraryA(path);
    }
    return lib ? GetProcAddress(lib, name) : NULL;
}

BOOL WINAPI Proxy_OpenPrinterA(LPSTR name, LPHANDLE printer, LPVOID defaults)
{
    FARPROC f = real_winspool("OpenPrinterA");
    return f ? ((BOOL (WINAPI *)(LPSTR, LPHANDLE, LPVOID))f)(name, printer, defaults) : FALSE;
}

LONG WINAPI Proxy_DocumentPropertiesA(HWND w, HANDLE printer, LPSTR name, PDEVMODEA out, PDEVMODEA in, DWORD mode)
{
    FARPROC f = real_winspool("DocumentPropertiesA");
    return f ? ((LONG (WINAPI *)(HWND, HANDLE, LPSTR, PDEVMODEA, PDEVMODEA, DWORD))f)(w, printer, name, out, in, mode) : -1;
}

BOOL WINAPI Proxy_ClosePrinter(HANDLE printer)
{
    FARPROC f = real_winspool("ClosePrinter");
    return f ? ((BOOL (WINAPI *)(HANDLE))f)(printer) : FALSE;
}

/* Running in a session with no desktop, such as Steam Deck's Game Mode or another gamescope
 * session? There every window is shown scaled to fill the screen, so a borderless full-screen
 * window is what fits; and with no keyboard, Alt+Enter isn't practical. Wine and Proton pass
 * the Linux environment through, where gamescope and Steam announce themselves. */
static BOOL no_desktop(void)
{
    char v[64];
    if (GetEnvironmentVariableA("GAMESCOPE_WAYLAND_DISPLAY", v, sizeof v)) return TRUE;
    if (GetEnvironmentVariableA("XDG_CURRENT_DESKTOP", v, sizeof v) && strstr(v, "gamescope")) return TRUE;
    if (GetEnvironmentVariableA("SteamDeck", v, sizeof v) && strcmp(v, "1") == 0) return TRUE;
    return FALSE;
}

/* A setting from the user folder's orlyfix.ini, else the game folder's, else the default. */
static int setting(const char *key, int def)
{
    int v = GetPrivateProfileIntA("orlyfix", key, def, gameIni);
    return userIni[0] ? (int)GetPrivateProfileIntA("orlyfix", key, v, userIni) : v;
}

/* ---- Loading a saved drawing (CODE_SAVED_DRAWING_LOAD) is an engine task that yields while
 * it waits for the disk. Leaving the screen meanwhile frees the saved-files table
 * (CODE_FREE_SAVED_FILES, from CODE_MENU_LEAVE_SCREEN), and the load then copies the drawing's
 * name out of the freed table (PATCH_SAVED_NAME_COPY) from a null pointer: a race in the
 * original game, easy to hit by clicking during a load. The call is redirected here, and the
 * copy is skipped if the table has gone. ---- */
#define SAVED_FILES_TABLE (*(BYTE *volatile *)VAR_SAVED_FILES)

void * __cdecl copy_saved_name(void *dst, const void *src, size_t n)
{
    if (SAVED_FILES_TABLE) memcpy(dst, src, n);
    return dst;
}

static void patch_saved_name_copy(void)
{
    const DWORD next = PATCH_SAVED_NAME_COPY + REL32_INSN_SIZE;  /* rel32 counts from here */
    BYTE expect[REL32_INSN_SIZE] = { OP_CALL_REL32 }, repl[REL32_INSN_SIZE] = { OP_CALL_REL32 };
    *(DWORD *)(expect + 1) = FN_RTL_MEMCPY - next;
    *(DWORD *)(repl + 1) = (DWORD)copy_saved_name - next;
    patch_bytes("saved drawing name copy", PATCH_SAVED_NAME_COPY, expect, repl, REL32_INSN_SIZE);
}

/* ---- One CPU core, as on the PCs the game was written for. The engine's threads rely on
 * single-CPU timing: e.g. its file worker (CODE_FILE_WORKER, highest priority) signals a request
 * done before clearing it, which only works if the waiting thread can't run in between - on a
 * multi-core PC it can, and the worker then exits and the game freezes (seen when saving). ---- */
static void use_one_cpu(void)
{
    DWORD_PTR proc, sys;
    if (GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && proc)
        SetProcessAffinityMask(GetCurrentProcess(), proc & (~proc + 1));   /* lowest available core */
}

#ifdef CRASHDUMP
/* ---- Build with -DCRASHDUMP: write a minidump to the user folder when the game crashes or
 * gives up - an unhandled exception, its "has caused a fatal error" message box, or a Borland
 * runtime error such as "Pure virtual function called" (PATCH_RTL_FATAL: prints and exits). ---- */
#include <dbghelp.h>
typedef BOOL (WINAPI *PFN_MINIDUMP)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                    PMINIDUMP_EXCEPTION_INFORMATION, PVOID, PVOID);

static void write_dump(EXCEPTION_POINTERS *ep)
{
    static LONG busy;
    if (InterlockedExchange(&busy, 1)) return;
    PFN_MINIDUMP dump = (PFN_MINIDUMP)GetProcAddress(LoadLibraryA("dbghelp.dll"), "MiniDumpWriteDump");
    char path[MAX_PATH];
    wsprintfA(path, "%scrash-%lu.dmp", userDir[0] ? userDir : gameDir, GetTickCount());
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (dump && f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION ei = { GetCurrentThreadId(), ep, FALSE };
        dump(GetCurrentProcess(), GetCurrentProcessId(), f,
             MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo |
             MiniDumpWithIndirectlyReferencedMemory, ep ? &ei : NULL, NULL, NULL);
    }
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    InterlockedExchange(&busy, 0);
}

static LONG WINAPI dump_unhandled(EXCEPTION_POINTERS *ep)
{
    write_dump(ep);
    return EXCEPTION_CONTINUE_SEARCH;                         /* then Windows handles it as usual */
}

int WINAPI My_MessageBoxA(HWND w, LPCSTR text, LPCSTR caption, UINT type)
{
    if (text && strstr(text, "fatal error")) write_dump(NULL);
    return MessageBoxA(w, text, caption, type);
}

/* Replaces the routine at PATCH_RTL_FATAL: dump, then print the message and exit(1) as before. */
void __cdecl rtl_fatal(const char *msg)
{
    write_dump(NULL);
    ((void (__cdecl *)(const char *))FN_RTL_PRINT_ERROR)(msg);
    ((void (__cdecl *)(int))FN_RTL_EXIT)(1);
}

static void crashdump_install(void)
{
    static const BYTE old[] = BYTES_RTL_FATAL_ENTRY;
    BYTE jmp[sizeof old] = { OP_JMP_REL32, 0, 0, 0, 0, OP_NOP };
    *(DWORD *)(jmp + 1) = (DWORD)rtl_fatal - (PATCH_RTL_FATAL + REL32_INSN_SIZE);
    patch_bytes("runtime error dump", PATCH_RTL_FATAL, old, jmp, sizeof old);
    HOOK_IMPORT(MessageBoxA, My_MessageBoxA);
    SetUnhandledExceptionFilter(dump_unhandled);
}
#endif

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)h; (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    userdata_init();
    wsprintfA(gameIni, "%sorlyfix.ini", gameDir);
    if (userDir[0]) wsprintfA(userIni, "%sorlyfix.ini", userDir);
    cfg.fullscreen  = setting("Fullscreen", 2);
    if (cfg.fullscreen != 0 && cfg.fullscreen != 1) cfg.fullscreen = no_desktop();   /* 2: automatic */
    cfg.windowScale = setting("WindowScale", 2);
    cfg.filter      = setting("Filter", 2);
    cfg.vsync       = setting("VSync", 1);
    if (cfg.filter < 0 || cfg.filter > 3) cfg.filter = 2;

    patch_task_stacks();
    patch_saved_name_copy();
    use_one_cpu();
    HOOK_IMPORT(GetDiskFreeSpaceA, My_GetDiskFreeSpaceA);
    HOOK_IMPORT(GetKeyboardState,  My_GetKeyboardState);
    HOOK_IMPORT(PeekMessageA,      My_PeekMessageA);
    HOOK_IMPORT(LoadLibraryA,      My_LoadLibraryA);
    emu_install();
    display_install();
    userdata_install();
#ifdef CRASHDUMP
    crashdump_install();
#endif
    if (strstr(skipped, "PeekMessageA")) warn_if_skipped(); /* otherwise shown from the main loop */
    return TRUE;
}
