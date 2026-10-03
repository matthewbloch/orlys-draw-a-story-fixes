/* userdata.c - keep the player's files in %APPDATA%\Orly's Draw-A-Story (#included by orlyfix.c).
 *
 * The game reads its data from, and saves into, its own folder. Here, for paths in
 * the game folder:
 *   - reading uses the user folder's copy if there is one, else the game folder's;
 *   - creating goes to the user folder. The engine opens every file in its folder for
 *     exclusive read/write (it uses the result to tell which files are in use or
 *     writable), so an existing game-folder file is opened exactly as the game asks -
 *     failing just as it would - and only copied to the user folder when the game first
 *     actually writes to it; from then on that handle uses the copy;
 *   - deleting, removing a directory or changing attributes acts on the user copy only; for
 *     something only in the game folder it fails with "access denied", as on the game's CD;
 *   - directory listings show both folders, sorted, the user's copy of a name winning;
 *   - entering a folder that only exists in the user folder (the engine saves by changing
 *     into "saved") enters the user copy, and the current directory is reported as the
 *     matching game-folder path.
 * Everything outside the game folder (CD and device probes, temp files) is untouched.
 */

static char gameDir[MAX_PATH], gameDirShort[MAX_PATH], userDir[MAX_PATH];
static CRITICAL_SECTION fsLock;

/* Build with -DFILELOG to log file decisions to orlyfix-files.log in the user folder. */
#ifdef FILELOG
#include <stdarg.h>
static HANDLE flog = INVALID_HANDLE_VALUE;
static void FLOG(const char *fmt, ...)
{
    char buf[1200]; DWORD w, e = GetLastError();
    va_list ap; va_start(ap, fmt);
    int n = wsprintfA(buf, "[%lu] t%lu ", GetTickCount(), GetCurrentThreadId());
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    buf[n++] = '\r'; buf[n++] = '\n';
    if (flog != INVALID_HANDLE_VALUE) WriteFile(flog, buf, n, &w, NULL);
    SetLastError(e);
}
#else
#define FLOG(...) ((void)0)
#endif



static BOOL file_exists(const char *p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* The game folder is read-only, like the original CD: a file or directory that exists only
 * there is never deleted or changed - the game gets "access denied", as from the CD. (On its
 * first run the game deletes its story sprite archives, SPR_*.MHK, expecting to copy them back
 * from the CD when a story needs them.) */
static BOOL game_folder_only(const char *user, const char *game)
{
    if (file_exists(user) || !file_exists(game)) return FALSE;
    SetLastError(ERROR_ACCESS_DENIED);
    return TRUE;
}

static BOOL starts_with_dir(const char *full, const char *dir)
{
    int n = lstrlenA(dir);
    return n && CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, full, n, dir, n) == CSTR_EQUAL;
}

/* If `path` is inside the game folder, give its user-folder and game-folder versions. */
static BOOL map_path(const char *path, char *user, char *game)
{
    char full[MAX_PATH];
    if (!path || !userDir[0] || !GetFullPathNameA(path, MAX_PATH, full, NULL)) return FALSE;
    const char *dir = starts_with_dir(full, gameDir) ? gameDir :
                      starts_with_dir(full, gameDirShort) ? gameDirShort : NULL;
    if (!dir) return FALSE;
    const char *rel = full + lstrlenA(dir);
    if (lstrlenA(userDir) + lstrlenA(rel) >= MAX_PATH) return FALSE;
    lstrcpyA(user, userDir); lstrcatA(user, rel);
    lstrcpyA(game, full);
    return TRUE;
}

/* Create every missing folder above `path` (path itself is a file or the folder to create). */
static void make_parents(const char *path)
{
    char p[MAX_PATH];
    lstrcpyA(p, path);
    for (char *s = p + 3; *s; s++)
        if (*s == '\\') { *s = 0; CreateDirectoryA(p, NULL); *s = '\\'; }
}

/* ---- copy on first write ---- */

typedef struct {
    HANDLE h;                           /* the handle the game holds: the original, read-only */
    HANDLE copy;                        /* the user-folder copy, once written to */
    DWORD acc, share, flags;            /* what the game asked for */
    char user[MAX_PATH], game[MAX_PATH];
} LAZY_FILE;

#define MAX_LAZY 64
static LAZY_FILE lazy[MAX_LAZY];
static volatile LONG nLazy;

static LAZY_FILE *lazy_find(HANDLE h)
{
    for (int i = 0; i < MAX_LAZY; i++) if (lazy[i].h && lazy[i].h == h) return &lazy[i];
    return NULL;
}

/* Copy a file through an open handle (the game may hold it exclusively), keeping its position. */
static BOOL copy_through_handle(HANDLE src, const char *to)
{
    make_parents(to);
    HANDLE d = CreateFileA(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (d == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_EXISTS;
    BYTE *buf = HeapAlloc(GetProcessHeap(), 0, 65536);    /* not on the stack: engine tasks have 60 KB */
    LONG hi = 0, zero = 0;
    DWORD pos = SetFilePointer(src, 0, &hi, FILE_CURRENT), n, w;
    BOOL ok = buf != NULL;
    SetFilePointer(src, 0, &zero, FILE_BEGIN);
    while (ok) {                                         /* a failed read is a failed copy */
        if (!ReadFile(src, buf, 65536, &n, NULL)) { ok = FALSE; break; }
        if (!n) break;
        ok = WriteFile(d, buf, n, &w, NULL) && w == n;
    }
    SetFilePointer(src, (LONG)pos, &hi, FILE_BEGIN);
    if (buf) HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(d);
    if (!ok) DeleteFileA(to);
    return ok;
}

/* First write: copy the file to the user folder and open the copy at the same position. */
static HANDLE lazy_promote(LAZY_FILE *z)
{
    if (z->copy) return z->copy;
    LONG hi = 0;
    DWORD lo = SetFilePointer(z->h, 0, &hi, FILE_CURRENT);
    if (!copy_through_handle(z->h, z->user) &&
        !(CopyFileA(z->game, z->user, TRUE) || GetLastError() == ERROR_FILE_EXISTS)) return NULL;
    SetFileAttributesA(z->user, GetFileAttributesA(z->user) & ~FILE_ATTRIBUTE_READONLY);
    HANDLE c = CreateFileA(z->user, z->acc, z->share, NULL, OPEN_EXISTING, z->flags, NULL);
    if (c == INVALID_HANDLE_VALUE) return NULL;
    SetFilePointer(c, (LONG)lo, &hi, FILE_BEGIN);
    z->copy = c;
    FLOG("  first write to %lx: now using copy \"%s\" (%lx)", (DWORD)z->h, z->user, (DWORD)c);
    return c;
}

/* The handle an operation should really use. */
static HANDLE route(HANDLE h, BOOL writing)
{
    if (!nLazy) return h;
    EnterCriticalSection(&fsLock);
    LAZY_FILE *z = lazy_find(h);
    HANDLE r = h;
    if (z) {
        r = z->copy ? z->copy : writing ? lazy_promote(z) : z->h;
        if (!r) {                       /* copy failed: fail the write, never touch the game file */
            FLOG("  first write to %lx: copy to \"%s\" FAILED - write refused", (DWORD)h, z->user);
            r = INVALID_HANDLE_VALUE;
        }
    }
    LeaveCriticalSection(&fsLock);
    return r;
}

BOOL WINAPI My_ReadFile(HANDLE h, LPVOID b, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    BOOL ok = ReadFile(route(h, FALSE), b, n, got, ov);
    if (!ok) FLOG("ReadFile %lx (%lu bytes) failed, err %lu", (DWORD)h, n, GetLastError());
    return ok;
}

BOOL WINAPI My_WriteFile(HANDLE h, LPCVOID b, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
    BOOL ok = WriteFile(route(h, TRUE), b, n, put, ov);
    if (!ok) FLOG("WriteFile %lx (%lu bytes) failed, err %lu", (DWORD)h, n, GetLastError());
    return ok;
}

DWORD WINAPI My_SetFilePointer(HANDLE h, LONG lo, PLONG hi, DWORD how)
{ return SetFilePointer(route(h, FALSE), lo, hi, how); }

BOOL WINAPI My_SetEndOfFile(HANDLE h)
{ return SetEndOfFile(route(h, TRUE)); }

DWORD WINAPI My_GetFileType(HANDLE h)
{ return GetFileType(route(h, FALSE)); }

BOOL WINAPI My_CloseHandle(HANDLE h)
{
    HANDLE copy = NULL;
    if (nLazy) {
        EnterCriticalSection(&fsLock);
        LAZY_FILE *z = lazy_find(h);
        if (z) { copy = z->copy; ZeroMemory(z, sizeof *z); InterlockedDecrement(&nLazy); }
        LeaveCriticalSection(&fsLock);
    }
    if (copy) CloseHandle(copy);
    return CloseHandle(h);
}

/* An existing game-folder file opened for writing: open it exactly as the game asked, so it
 * fails (in use, read-only, the running EXE) just as it would, then watch for writes. Only if
 * Windows refuses write access to a file that is not read-only - a protected game folder such
 * as Program Files - is it opened read-only instead. */
static HANDLE open_lazy(const char *user, const char *game, DWORD acc, DWORD share,
                        LPSECURITY_ATTRIBUTES sa, DWORD flags, HANDLE tmpl)
{
    HANDLE h = CreateFileA(game, acc, share, sa, OPEN_EXISTING, flags, tmpl);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED || (GetFileAttributesA(game) & FILE_ATTRIBUTE_READONLY)) {
            SetLastError(e);
            return h;
        }
        h = CreateFileA(game, GENERIC_READ, share | FILE_SHARE_READ, sa, OPEN_EXISTING, flags, tmpl);
        if (h == INVALID_HANDLE_VALUE) return h;
    }
    EnterCriticalSection(&fsLock);
    LAZY_FILE *z = NULL;
    for (int i = 0; !z && i < MAX_LAZY; i++) if (!lazy[i].h) z = &lazy[i];
    if (z) {
        z->h = h; z->copy = NULL; z->acc = acc; z->share = share; z->flags = flags;
        lstrcpyA(z->user, user); lstrcpyA(z->game, game);
        InterlockedIncrement(&nLazy);
        FLOG("  game original %lx, copied to the user folder on first write", (DWORD)h);
    }
    LeaveCriticalSection(&fsLock);
    if (!z) {                           /* table full: copy now and use the copy */
        BOOL ok = copy_through_handle(h, user);
        CloseHandle(h);
        if (!ok) { SetLastError(ERROR_TOO_MANY_OPEN_FILES); return INVALID_HANDLE_VALUE; }
        return CreateFileA(user, acc, share, sa, OPEN_EXISTING, flags, tmpl);
    }
    return h;
}

static HANDLE create_file(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                          DWORD disp, DWORD flags, HANDLE tmpl);
HANDLE WINAPI My_CreateFileA(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                             DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = create_file(name, acc, share, sa, disp, flags, tmpl);
    FLOG("CreateFile \"%s\" acc %lx share %lx disp %lu -> %lx err %lu", name ? name : "", acc, share, disp,
         (DWORD)h, h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
    return h;
}

static HANDLE create_file(LPCSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
                          DWORD disp, DWORD flags, HANDLE tmpl)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return CreateFileA(name, acc, share, sa, disp, flags, tmpl);

    if (file_exists(user)) return CreateFileA(user, acc, share, sa, disp, flags, tmpl);
    BOOL writes = (acc & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE)) ||
                  disp != OPEN_EXISTING;
    if (!writes) return CreateFileA(game, acc, share, sa, disp, flags, tmpl);   /* plain read */
    if (file_exists(game)) {
        switch (disp) {
        case CREATE_NEW:        SetLastError(ERROR_FILE_EXISTS); return INVALID_HANDLE_VALUE;
        case CREATE_ALWAYS:
        case TRUNCATE_EXISTING: make_parents(user);              /* replaced: start a new copy */
                                return CreateFileA(user, acc, share, sa, CREATE_ALWAYS, flags, tmpl);
        default:
            if (acc & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA))
                return open_lazy(user, game, acc, share, sa, flags, tmpl);
            /* Write-only: the copy can't be made through the game's handle later, so make
               it now, before anything has the file open - failing as the original would. */
            if (GetFileAttributesA(game) & FILE_ATTRIBUTE_READONLY) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
            make_parents(user);
            if (!CopyFileA(game, user, TRUE) && GetLastError() != ERROR_FILE_EXISTS) {
                DWORD e = GetLastError();
                FLOG("  write-only open: copy to \"%s\" failed, err %lu", user, e);
                SetLastError(e == ERROR_SHARING_VIOLATION ? ERROR_SHARING_VIOLATION : e);
                return INVALID_HANDLE_VALUE;
            }
            SetFileAttributesA(user, GetFileAttributesA(user) & ~FILE_ATTRIBUTE_READONLY);
            FLOG("  write-only open: copied to \"%s\" first", user);
            return CreateFileA(user, acc, share, sa, OPEN_EXISTING, flags, tmpl);
        }
    }
    if (disp != OPEN_EXISTING && disp != TRUNCATE_EXISTING) make_parents(user);   /* new file */
    return CreateFileA(user, acc, share, sa, disp, flags, tmpl);
}

DWORD WINAPI My_GetFileAttributesA(LPCSTR name)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return GetFileAttributesA(name);
    DWORD a = GetFileAttributesA(user);
    return a != INVALID_FILE_ATTRIBUTES ? a : GetFileAttributesA(game);
}

BOOL WINAPI My_SetFileAttributesA(LPCSTR name, DWORD attr)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return SetFileAttributesA(name, attr);
    if (game_folder_only(user, game)) return FALSE;
    return SetFileAttributesA(user, attr);
}


BOOL WINAPI My_DeleteFileA(LPCSTR name)
{
    FLOG("DeleteFile \"%s\"", name ? name : "");
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return DeleteFileA(name);
    if (game_folder_only(user, game)) return FALSE;
    return DeleteFileA(user);
}

BOOL WINAPI My_CreateDirectoryA(LPCSTR name, LPSECURITY_ATTRIBUTES sa)
{
    FLOG("CreateDirectory \"%s\"", name ? name : "");
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return CreateDirectoryA(name, sa);
    if (file_exists(game) || file_exists(user)) { SetLastError(ERROR_ALREADY_EXISTS); return FALSE; }
    make_parents(user);
    return CreateDirectoryA(user, sa);
}

BOOL WINAPI My_RemoveDirectoryA(LPCSTR name)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(name, user, game)) return RemoveDirectoryA(name);
    if (game_folder_only(user, game)) return FALSE;
    return RemoveDirectoryA(user);
}

/* ---- merged directory listings: both folders, sorted like NTFS, user copy wins ---- */

typedef struct { int n, next; WIN32_FIND_DATAA e[1]; } MERGED_FIND;   /* e[] allocated to fit */

#define MAX_FINDS 32
static MERGED_FIND *finds[MAX_FINDS];

static int find_index(MERGED_FIND *m, const char *name)
{
    for (int i = 0; i < m->n; i++) if (lstrcmpiA(m->e[i].cFileName, name) == 0) return i;
    return -1;
}

/* Add every entry of one folder's search; the user folder goes first, so its entries win. */
static MERGED_FIND *add_search(MERGED_FIND *m, const char *pattern)
{
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return m;
    do {
        if (find_index(m, d.cFileName) >= 0) continue;
        if (m->n % 64 == 0 && m->n) {
            MERGED_FIND *g = HeapReAlloc(GetProcessHeap(), 0, m, sizeof *m + (m->n + 64) * sizeof d);
            if (!g) break;
            m = g;
        }
        m->e[m->n++] = d;
    } while (FindNextFileA(h, &d));
    FindClose(h);
    return m;
}

/* NTFS order: names compared character by character after upper-casing. */
static int cmp_names(const WIN32_FIND_DATAA *a, const WIN32_FIND_DATAA *b)
{
    const unsigned char *p = (const unsigned char *)a->cFileName, *q = (const unsigned char *)b->cFileName;
    for (;; p++, q++) {
        int x = (*p >= 'a' && *p <= 'z') ? *p - 32 : *p, y = (*q >= 'a' && *q <= 'z') ? *q - 32 : *q;
        if (x != y || !x) return x - y;
    }
}

HANDLE WINAPI My_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA out)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (!map_path(pattern, user, game)) return FindFirstFileA(pattern, out);

    MERGED_FIND *m = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *m + 64 * sizeof(WIN32_FIND_DATAA));
    if (!m) return FindFirstFileA(pattern, out);
    m = add_search(m, user);
    m = add_search(m, game);
    FLOG("FindFirst \"%s\" -> %d entries", pattern, m->n);
    if (!m->n) { HeapFree(GetProcessHeap(), 0, m); SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    for (int i = 1; i < m->n; i++)                       /* insertion sort: folders are small */
        for (int j = i; j > 0 && cmp_names(&m->e[j - 1], &m->e[j]) > 0; j--) {
            WIN32_FIND_DATAA t = m->e[j]; m->e[j] = m->e[j - 1]; m->e[j - 1] = t;
        }

    EnterCriticalSection(&fsLock);
    int slot = -1;
    for (int i = 0; i < MAX_FINDS && slot < 0; i++) if (!finds[i]) { finds[i] = m; slot = i; }
    LeaveCriticalSection(&fsLock);
    if (slot < 0) { HeapFree(GetProcessHeap(), 0, m); return FindFirstFileA(pattern, out); }
    *out = m->e[m->next++];
    return (HANDLE)m;
}

static int find_slot(HANDLE h)
{
    for (int i = 0; i < MAX_FINDS; i++) if (finds[i] && (HANDLE)finds[i] == h) return i;
    return -1;
}

BOOL WINAPI My_FindNextFileA(HANDLE h, LPWIN32_FIND_DATAA out)
{
    EnterCriticalSection(&fsLock);
    int slot = find_slot(h);
    LeaveCriticalSection(&fsLock);
    if (slot < 0) return FindNextFileA(h, out);
    MERGED_FIND *m = finds[slot];
    if (m->next >= m->n) { SetLastError(ERROR_NO_MORE_FILES); return FALSE; }
    *out = m->e[m->next++];
    return TRUE;
}

BOOL WINAPI My_FindClose(HANDLE h)
{
    EnterCriticalSection(&fsLock);
    int slot = find_slot(h);
    MERGED_FIND *m = slot >= 0 ? finds[slot] : NULL;
    if (m) finds[slot] = NULL;
    LeaveCriticalSection(&fsLock);
    if (!m) return FindClose(h);
    HeapFree(GetProcessHeap(), 0, m);
    return TRUE;
}

/* ---- current directory ---- */

static BOOL dir_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

BOOL WINAPI My_SetCurrentDirectoryA(LPCSTR dir)
{
    char user[MAX_PATH], game[MAX_PATH];
    if (map_path(dir, user, game) && !dir_exists(game) && dir_exists(user)) {
        FLOG("SetCurrentDirectory \"%s\" -> user folder \"%s\"", dir, user);
        return SetCurrentDirectoryA(user);
    }
    return SetCurrentDirectoryA(dir);
}

/* Inside the user folder, report the matching game-folder path. */
DWORD WINAPI My_GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    char cur[MAX_PATH], slashed[MAX_PATH + 1], v[2 * MAX_PATH];
    DWORD r = GetCurrentDirectoryA(MAX_PATH, cur);
    if (!r || r >= MAX_PATH || !userDir[0]) return GetCurrentDirectoryA(size, buf);
    wsprintfA(slashed, "%s\\", cur);
    if (!starts_with_dir(slashed, userDir)) return GetCurrentDirectoryA(size, buf);
    wsprintfA(v, "%s%s", gameDir, slashed + lstrlenA(userDir));
    int n = lstrlenA(v);
    if (n > 3 && v[n - 1] == '\\') v[--n] = 0;             /* no trailing slash, like Windows */
    if (!buf || size <= (DWORD)n) return n + 1;
    lstrcpyA(buf, v);
    return n;
}

/* ---- set-up ---- */


/* Game folder from the EXE's path; user folder %APPDATA%\Orly's Draw-A-Story\ (created). */
static void userdata_init(void)
{
    InitializeCriticalSection(&fsLock);
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!GetLongPathNameA(exe, gameDir, MAX_PATH)) lstrcpyA(gameDir, exe);
    char *slash = strrchr(gameDir, '\\');
    if (slash) slash[1] = 0;
    if (!GetShortPathNameA(gameDir, gameDirShort, MAX_PATH)) gameDirShort[0] = 0;


    char appdata[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    if (!n || n >= MAX_PATH - 40) return;                 /* no user folder: everything stays as it was */
    wsprintfA(userDir, "%s\\Orly's Draw-A-Story\\", appdata);
    make_parents(userDir);
#ifdef FILELOG
    char lp[MAX_PATH];
    wsprintfA(lp, "%sorlyfix-files.log", userDir);               /* the game folder may be read-only */
    flog = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    FLOG("game folder \"%s\", user folder \"%s\"", gameDir, userDir);
#endif

    /* Remove unmodified copies of game files (left by a version that copied on open). */
    char pat[MAX_PATH], u[MAX_PATH], g[MAX_PATH];
    WIN32_FIND_DATAA f;
    WIN32_FILE_ATTRIBUTE_DATA a;
    wsprintfA(pat, "%s*", userDir);
    HANDLE fh = FindFirstFileA(pat, &f);
    if (fh == INVALID_HANDLE_VALUE) return;
    do {
        if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wsprintfA(g, "%s%s", gameDir, f.cFileName);
        if (GetFileAttributesExA(g, GetFileExInfoStandard, &a) &&
            a.nFileSizeLow == f.nFileSizeLow && a.nFileSizeHigh == f.nFileSizeHigh &&
            CompareFileTime(&a.ftLastWriteTime, &f.ftLastWriteTime) == 0) {
            wsprintfA(u, "%s%s", userDir, f.cFileName);
            SetFileAttributesA(u, FILE_ATTRIBUTE_NORMAL);
            DeleteFileA(u);
        }
    } while (FindNextFileA(fh, &f));
    FindClose(fh);
}

static void userdata_install(void)
{
    if (!userDir[0]) return;
    HOOK_IMPORT(CreateFileA,        My_CreateFileA);
    HOOK_IMPORT(GetFileAttributesA, My_GetFileAttributesA);
    HOOK_IMPORT(SetFileAttributesA, My_SetFileAttributesA);
    HOOK_IMPORT(DeleteFileA,        My_DeleteFileA);
    HOOK_IMPORT(CreateDirectoryA,   My_CreateDirectoryA);
    HOOK_IMPORT(RemoveDirectoryA,   My_RemoveDirectoryA);
    HOOK_IMPORT(FindFirstFileA,     My_FindFirstFileA);
    HOOK_IMPORT(FindNextFileA,      My_FindNextFileA);
    HOOK_IMPORT(FindClose,          My_FindClose);
    HOOK_IMPORT(ReadFile,           My_ReadFile);
    HOOK_IMPORT(WriteFile,          My_WriteFile);
    HOOK_IMPORT(SetFilePointer,     My_SetFilePointer);
    HOOK_IMPORT(SetEndOfFile,       My_SetEndOfFile);
    HOOK_IMPORT(GetFileType,        My_GetFileType);
    HOOK_IMPORT(CloseHandle,        My_CloseHandle);
    HOOK_IMPORT(SetCurrentDirectoryA, My_SetCurrentDirectoryA);
    HOOK_IMPORT(GetCurrentDirectoryA, My_GetCurrentDirectoryA);
}
