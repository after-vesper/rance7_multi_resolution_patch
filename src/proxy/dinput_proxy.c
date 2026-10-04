/*
 * dinput.dll proxy for Sengoku Rance (System40) multi-resolution patch.
 *
 * The windowed renderer of this game is GDI-based: the engine draws the
 * 800x600 main surface into a DIB section and blits it to the window DC
 * with BitBlt. This proxy forwards DirectInput exports to the real
 * dinput.dll and patches the exe's IAT entry for GDI32!BitBlt so that
 * the final blit is stretched to the configured output size.
 * The game window is resized to match.
 *
 * Config: MultiRes.ini next to the game exe
 *   [Display]
 *   Width  = 1600
 *   Height = 1200
 *   Filter = 1    ; 0 = nearest (COLORONCOLOR), 1 = smooth (HALFTONE)
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#define GAME_W 800
#define GAME_H 600

static HMODULE g_dinput;
static int g_enabled;
static int g_outW, g_outH, g_smooth;
static HWND g_hwnd;
static int g_hooked;

typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PFN_ScreenToClient)(HWND, LPPOINT);
typedef BOOL (WINAPI *PFN_SetCursorPos)(int, int);
typedef BOOL (WINAPI *PFN_GetCursorPos)(LPPOINT);
typedef BOOL (WINAPI *PFN_ClientToScreen)(HWND, LPPOINT);
static PFN_BitBlt g_origBitBlt;
static PFN_ScreenToClient g_origScreenToClient;
static PFN_SetCursorPos g_origSetCursorPos;
static PFN_GetCursorPos g_origGetCursorPos;
static PFN_ClientToScreen g_origClientToScreen;

static void logmsg(const char *s)
{
    HANDLE f = CreateFileA("multires_log.txt", GENERIC_WRITE, 0, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, s, lstrlenA(s), &n, NULL);
    WriteFile(f, "\r\n", 2, &n, NULL);
    CloseHandle(f);
}

static void load_config(void)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\') n--;
    path[n] = 0;
    lstrcatA(path, "MultiRes.ini");
    g_outW = GetPrivateProfileIntA("Display", "Width", GAME_W, path);
    g_outH = GetPrivateProfileIntA("Display", "Height", GAME_H, path);
    g_smooth = GetPrivateProfileIntA("Display", "Filter", 1, path);
    g_enabled = g_outW > GAME_W && g_outH > GAME_H;
}

/* --- window management -------------------------------------------------- */

static BOOL CALLBACK find_game_window(HWND hwnd, LPARAM lp)
{
    char cls[64];
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        return TRUE;
    if (GetClassNameA(hwnd, cls, sizeof(cls)) &&
        lstrcmpA(cls, "Sys40WindowClass") == 0) {
        *(HWND *)lp = hwnd;
        return FALSE;
    }
    return TRUE;
}

/* Subclass the game window so mouse-message client coords arrive in the
   800x600 logical space. Without this the game reads raw client coords
   (e.g. as the start point of cursor warps), mixing coordinate spaces. */
static WNDPROC g_origWndProc;

static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_enabled) {
        switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_MOUSEHOVER: {
            RECT rc;
            short x, y;
            GetClientRect(hwnd, &rc);
            x = (short)LOWORD(lp);
            y = (short)HIWORD(lp);
            x = (short)MulDiv(x, GAME_W, rc.right);
            y = (short)MulDiv(y, GAME_H, rc.bottom);
            lp = MAKELPARAM((WORD)x, (WORD)y);
            break;
        }
        default:
            break;
        }
    }
    return CallWindowProcA(g_origWndProc, hwnd, msg, wp, lp);
}

static void ensure_window_size(void)
{
    RECT rc;
    if (!g_hwnd)
        return;
    GetClientRect(g_hwnd, &rc);
    if (rc.right == g_outW && rc.bottom == g_outH)
        return;
    rc.left = rc.top = 0;
    rc.right = g_outW;
    rc.bottom = g_outH;
    AdjustWindowRectEx(&rc, GetWindowLongA(g_hwnd, GWL_STYLE),
                       GetMenu(g_hwnd) != NULL,
                       GetWindowLongA(g_hwnd, GWL_EXSTYLE));
    SetWindowPos(g_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* --- BitBlt hook --------------------------------------------------------- */

static BOOL WINAPI BitBlt_Hook(HDC dst, int x, int y, int cx, int cy,
                               HDC src, int sx, int sy, DWORD rop)
{
    if (g_enabled && cx > 0 && cy > 0 &&
        sx >= 0 && sy >= 0 && sx + cx <= GAME_W && sy + cy <= GAME_H) {
        HWND w;
        if (!g_hwnd)
            EnumWindows(find_game_window, (LPARAM)&g_hwnd);
        w = WindowFromDC(dst);
        if (w && g_hwnd && w == g_hwnd) {
            if (!g_origWndProc)
                g_origWndProc = (WNDPROC)SetWindowLongPtrA(
                    g_hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
            RECT rc;
            int dx, dy, dw, dh;
            ensure_window_size();
            GetClientRect(w, &rc);
            /* Scale the partial-update rect proportionally so dirty-rect
               blits land at the right place. */
            dx = MulDiv(x, rc.right, GAME_W);
            dy = MulDiv(y, rc.bottom, GAME_H);
            dw = MulDiv(x + cx, rc.right, GAME_W) - dx;
            dh = MulDiv(y + cy, rc.bottom, GAME_H) - dy;
            SetStretchBltMode(dst, g_smooth ? HALFTONE : COLORONCOLOR);
            if (g_smooth)
                SetBrushOrgEx(dst, dx, dy, NULL);
            return StretchBlt(dst, dx, dy, dw, dh,
                              src, sx, sy, cx, cy, rop);
        }
    }
    return g_origBitBlt(dst, x, y, cx, cy, src, sx, sy, rop);
}

/* Convert the scaled client coordinate back to the 800x600 game space. */
static BOOL WINAPI ScreenToClient_Hook(HWND hwnd, LPPOINT pt)
{
    BOOL r;
    r = g_origScreenToClient(hwnd, pt);
    /* Inputs are already logical screen coords once GetCursorPos is
       hooked, so the client-space result is logical too. Do not rescale
       here or the position would be converted twice. */
    (void)g_hwnd;
    return r;
}

/* The game auto-moves the cursor with SetCursorPos using positions in
   the unscaled 800x600 space. Re-map screen coordinates that fall inside
   the game window to the scaled position. */
static BOOL WINAPI SetCursorPos_Hook(int x, int y)
{
    if (g_enabled && g_hwnd) {
        POINT pt;
        pt.x = x;
        pt.y = y;
        g_origScreenToClient(g_hwnd, &pt);
        {
            /* The game emits warp targets in 800x600 logical space, but
               glides interpolate from the real cursor position, so values
               may exceed the logical range. Scale everything uniformly;
               out-of-range results are clamped by SetCursorPos anyway. */
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            pt.x = MulDiv(pt.x, rc.right, GAME_W);
            pt.y = MulDiv(pt.y, rc.bottom, GAME_H);
            g_origClientToScreen(g_hwnd, &pt);
            return g_origSetCursorPos(pt.x, pt.y);
        }
    }
    return g_origSetCursorPos(x, y);
}

/* Report the cursor position in 800x600 logical space (expressed as
   screen coords) so the game computes consistent positions even through
   paths that use raw screen coordinates. */
static BOOL WINAPI GetCursorPos_Hook(LPPOINT pt)
{
    BOOL r = g_origGetCursorPos(pt);
    if (r && g_enabled && g_hwnd) {
        RECT rc;
        POINT c = *pt;
        g_origScreenToClient(g_hwnd, &c);
        GetClientRect(g_hwnd, &rc);
        c.x = MulDiv(c.x, GAME_W, rc.right);
        c.y = MulDiv(c.y, GAME_H, rc.bottom);
        g_origClientToScreen(g_hwnd, &c);
        *pt = c;
    }
    return r;
}

static void patch_import_thunk(PIMAGE_IMPORT_DESCRIPTOR imp,
                               BYTE *base, const char *dllname,
                               FARPROC real, void *hook,
                               const char *label)
{
    for (; imp->Name; imp++) {
        const char *name = (const char *)(base + imp->Name);
        if (lstrcmpiA(name, dllname) == 0)
            break;
    }
    if (!imp->Name) { logmsg("import dll not found"); logmsg(dllname); return; }
    {
        DWORD *thunk = (DWORD *)(base + imp->FirstThunk);
        DWORD i;
        for (i = 0; thunk[i]; i++) {
            if (thunk[i] == (DWORD)(UINT_PTR)real) {
                DWORD old;
                VirtualProtect(&thunk[i], sizeof(DWORD),
                               PAGE_EXECUTE_READWRITE, &old);
                thunk[i] = (DWORD)(UINT_PTR)hook;
                VirtualProtect(&thunk[i], sizeof(DWORD), old, &old);
                FlushInstructionCache(GetCurrentProcess(),
                                      &thunk[i], sizeof(DWORD));
                logmsg(label);
                return;
            }
        }
    }
    logmsg("thunk not found");
    logmsg(dllname);
}

static void hook_module(HMODULE mod, const char *dllname, FARPROC real,
                        void *hook, const char *label)
{
    PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)mod;
    PIMAGE_NT_HEADERS nt;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    if (!mod || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    nt = (PIMAGE_NT_HEADERS)((BYTE *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress)
        return;
    imp = (PIMAGE_IMPORT_DESCRIPTOR)((BYTE *)mod +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress);
    patch_import_thunk(imp, (BYTE *)mod, dllname, real, hook, label);
}

/* Patch BitBlt/ScreenToClient thunks in every loaded module: mouse and
   blit code may live in engine DLLs (Sys42VM, SACT2, ...), not just exe. */
static void hook_iat(void)
{
    HANDLE snap;
    MODULEENTRY32 me;
    HMODULE gdi, u32;
    FARPROC realBitBlt, realS2C, realSCP, realGCP;

    gdi = GetModuleHandleA("GDI32.dll");
    u32 = GetModuleHandleA("USER32.dll");
    realBitBlt = gdi ? GetProcAddress(gdi, "BitBlt") : NULL;
    realS2C = u32 ? GetProcAddress(u32, "ScreenToClient") : NULL;
    realSCP = u32 ? GetProcAddress(u32, "SetCursorPos") : NULL;
    realGCP = u32 ? GetProcAddress(u32, "GetCursorPos") : NULL;
    if (realBitBlt) g_origBitBlt = (PFN_BitBlt)realBitBlt;
    if (realS2C) g_origScreenToClient = (PFN_ScreenToClient)realS2C;
    if (realSCP) g_origSetCursorPos = (PFN_SetCursorPos)realSCP;
    if (realGCP) g_origGetCursorPos = (PFN_GetCursorPos)realGCP;
    if (u32)
        g_origClientToScreen =
            (PFN_ClientToScreen)GetProcAddress(u32, "ClientToScreen");

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                    GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        hook_module(GetModuleHandleA(NULL), "GDI32.dll", realBitBlt,
                    BitBlt_Hook, "BitBlt IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realS2C,
                    ScreenToClient_Hook, "ScreenToClient IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realSCP,
                    SetCursorPos_Hook, "SetCursorPos IAT hooked");
        hook_module(GetModuleHandleA(NULL), "USER32.dll", realGCP,
                    GetCursorPos_Hook, "GetCursorPos IAT hooked");
        return;
    }
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (realBitBlt)
                hook_module(me.hModule, "GDI32.dll", realBitBlt,
                            BitBlt_Hook, "BitBlt IAT hooked");
            if (realS2C)
                hook_module(me.hModule, "USER32.dll", realS2C,
                            ScreenToClient_Hook, "ScreenToClient IAT hooked");
            if (realSCP)
                hook_module(me.hModule, "USER32.dll", realSCP,
                            SetCursorPos_Hook, "SetCursorPos IAT hooked");
            if (realGCP)
                hook_module(me.hModule, "USER32.dll", realGCP,
                            GetCursorPos_Hook, "GetCursorPos IAT hooked");
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    logmsg("IAT scan done");
}

/* --- dinput.dll forwards ------------------------------------------------- */

static void load_real(void)
{
    char sys[MAX_PATH];
    if (g_dinput) return;
    GetSystemDirectoryA(sys, MAX_PATH);
    lstrcatA(sys, "\\dinput.dll");
    g_dinput = LoadLibraryA(sys);
}

#define FORWARD(fn) \
    load_real(); \
    if (!g_dinput) return E_FAIL; \
    return ((PFN_##fn)GetProcAddress(g_dinput, #fn))

typedef HRESULT (WINAPI *PFN_DirectInputCreateA)(HINSTANCE, DWORD, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DirectInputCreateW)(HINSTANCE, DWORD, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DirectInputCreateEx)(HINSTANCE, DWORD, REFIID, LPVOID *, LPVOID);
typedef HRESULT (WINAPI *PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID *);
typedef HRESULT (WINAPI *PFN_DllCanUnloadNow)(void);
typedef HRESULT (WINAPI *PFN_DllRegisterServer)(void);
typedef HRESULT (WINAPI *PFN_DllUnregisterServer)(void);

HRESULT WINAPI DirectInputCreateA(HINSTANCE a, DWORD b, LPVOID *c, LPVOID d)
{ FORWARD(DirectInputCreateA)(a, b, c, d); }

HRESULT WINAPI DirectInputCreateW(HINSTANCE a, DWORD b, LPVOID *c, LPVOID d)
{ FORWARD(DirectInputCreateW)(a, b, c, d); }

HRESULT WINAPI DirectInputCreateEx(HINSTANCE a, DWORD b, REFIID c, LPVOID *d, LPVOID e)
{
    load_real();
    if (!g_dinput) return E_FAIL;
    return ((PFN_DirectInputCreateEx)GetProcAddress(g_dinput,
        "DirectInputCreateEx"))(a, b, c, d, e);
}

HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID *c)
{ FORWARD(DllGetClassObject)(a, b, c); }

HRESULT WINAPI DllCanUnloadNow(void)
{ FORWARD(DllCanUnloadNow)(); }

HRESULT WINAPI DllRegisterServer(void)
{ FORWARD(DllRegisterServer)(); }

HRESULT WINAPI DllUnregisterServer(void)
{ FORWARD(DllUnregisterServer)(); }

BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, void *res)
{
    (void)inst; (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        logmsg("dinput proxy loaded");
        if (!g_hooked) {
            g_hooked = 1;
            load_config();
            hook_iat();
        }
    }
    return TRUE;
}
