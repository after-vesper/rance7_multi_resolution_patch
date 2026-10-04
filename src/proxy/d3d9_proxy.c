/*
 * d3d9.dll proxy for Sengoku Rance (System40) multi-resolution patch.
 *
 * Wraps Direct3DCreate9, hooks IDirect3D9::CreateDevice and
 * IDirect3DDevice9::Present via vtable patching. The game's 800x600
 * backbuffer is blitted (scaled) into an additional swapchain whose
 * backbuffer matches the configured output size, and that swapchain
 * is presented instead. The game window is resized accordingly.
 *
 * Config: MultiRes.ini next to the game exe
 *   [Display]
 *   Width  = 1600
 *   Height = 1200
 *   Filter = 1    ; 0 = point, 1 = linear
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#define GAME_W 800
#define GAME_H 600

static HMODULE g_d3d9;
static int g_outW, g_outH;
static D3DTEXTUREFILTERTYPE g_filter;
static int g_enabled;

static D3DPRESENT_PARAMETERS g_pp;
static HWND g_hwnd;
static IDirect3DSwapChain9 *g_sc;

typedef HRESULT (STDMETHODCALLTYPE *PFN_CreateDevice)(IDirect3D9 *, UINT,
    D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Present)(IDirect3DDevice9 *,
    const RECT *, const RECT *, HWND, const RGNDATA *);
typedef HRESULT (STDMETHODCALLTYPE *PFN_Reset)(IDirect3DDevice9 *,
    D3DPRESENT_PARAMETERS *);

static PFN_CreateDevice g_origCreateDevice;
static PFN_Present g_origPresent;
static PFN_Reset g_origReset;
static int g_d3d9_vt_patched, g_dev_vt_patched;

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

/* Patch a single vtable entry. Vtables live in read-only memory. */
static void *patch_vt(void *obj, int idx, void *fn)
{
    void **vt = *(void ***)obj;
    void *orig;
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old);
    orig = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[idx], sizeof(void *));
    return orig;
}

static void release_swapchain(void)
{
    if (g_sc) {
        g_sc->lpVtbl->Release(g_sc);
        g_sc = NULL;
    }
}

static void ensure_window_size(void)
{
    RECT rc;
    DWORD style, exstyle;
    if (!g_hwnd || !g_enabled)
        return;
    style = GetWindowLongA(g_hwnd, GWL_STYLE);
    exstyle = GetWindowLongA(g_hwnd, GWL_EXSTYLE);
    rc.left = rc.top = 0;
    rc.right = g_outW;
    rc.bottom = g_outH;
    AdjustWindowRectEx(&rc, style, GetMenu(g_hwnd) != NULL, exstyle);
    SetWindowPos(g_hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ensure_swapchain(IDirect3DDevice9 *dev)
{
    D3DPRESENT_PARAMETERS pp;
    if (g_sc || !g_enabled || !g_pp.Windowed)
        return;
    pp = g_pp;
    pp.BackBufferWidth = g_outW;
    pp.BackBufferHeight = g_outH;
    pp.BackBufferCount = 1;
    pp.MultiSampleType = D3DMULTISAMPLE_NONE;
    pp.MultiSampleQuality = 0;
    pp.Windowed = TRUE;
    pp.Flags = 0;
    if (FAILED(dev->lpVtbl->CreateAdditionalSwapChain(dev, &pp, &g_sc)))
        g_sc = NULL;
}

static HRESULT STDMETHODCALLTYPE Present_Hook(IDirect3DDevice9 *dev,
    const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    IDirect3DSurface9 *bb = NULL, *sbb = NULL;
    RECT srect;
    if (g_enabled && g_pp.Windowed) {
        ensure_window_size();
        ensure_swapchain(dev);
        if (g_sc) {
            if (SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev, 0, 0,
                    D3DBACKBUFFER_TYPE_MONO, &bb)) &&
                SUCCEEDED(g_sc->lpVtbl->GetBackBuffer(g_sc, 0,
                    D3DBACKBUFFER_TYPE_MONO, &sbb))) {
                /* Crop to the 800x600 game area even if the game created a
                   larger backbuffer via ini View size. */
                srect.left = srect.top = 0;
                srect.right = g_pp.BackBufferWidth < GAME_W
                              ? g_pp.BackBufferWidth : GAME_W;
                srect.bottom = g_pp.BackBufferHeight < GAME_H
                               ? g_pp.BackBufferHeight : GAME_H;
                dev->lpVtbl->StretchRect(dev, bb, &srect, sbb, NULL,
                                         g_filter);
                sbb->lpVtbl->Release(sbb);
                bb->lpVtbl->Release(bb);
                return g_sc->lpVtbl->Present(g_sc, NULL, NULL, wnd, NULL, 0);
            }
            if (bb) bb->lpVtbl->Release(bb);
            if (sbb) sbb->lpVtbl->Release(sbb);
        }
    }
    return g_origPresent(dev, src, dst, wnd, dirty);
}

static HRESULT STDMETHODCALLTYPE Reset_Hook(IDirect3DDevice9 *dev,
    D3DPRESENT_PARAMETERS *pp)
{
    HRESULT hr;
    release_swapchain();
    hr = g_origReset(dev, pp);
    if (SUCCEEDED(hr) && pp)
        g_pp = *pp;
    return hr;
}

static HRESULT STDMETHODCALLTYPE CreateDevice_Hook(IDirect3D9 *d3d,
    UINT adapter, D3DDEVTYPE devtype, HWND focus, DWORD flags,
    D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    HRESULT hr = g_origCreateDevice(d3d, adapter, devtype, focus, flags, pp, out);
    if (FAILED(hr) || !out || !*out)
        return hr;
    g_pp = *pp;
    g_hwnd = pp->hDeviceWindow ? pp->hDeviceWindow : focus;
    logmsg("CreateDevice ok");
    if (!g_dev_vt_patched) {
        g_origReset = patch_vt(*out, 16, Reset_Hook);
        g_origPresent = patch_vt(*out, 17, Present_Hook);
        g_dev_vt_patched = 1;
        logmsg("device vtable patched");
    }
    ensure_window_size();
    return hr;
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
    g_filter = GetPrivateProfileIntA("Display", "Filter", 1, path)
               ? D3DTEXF_LINEAR : D3DTEXF_POINT;
    g_enabled = (g_outW > GAME_W || g_outH > GAME_H) &&
                g_outW >= GAME_W && g_outH >= GAME_H;
}

IDirect3D9 *STDMETHODCALLTYPE Direct3DCreate9(UINT version)
{
    typedef IDirect3D9 *(STDMETHODCALLTYPE *PFN_Direct3DCreate9)(UINT);
    static PFN_Direct3DCreate9 orig;
    IDirect3D9 *d3d;
    char sys[MAX_PATH];

    if (!g_d3d9) {
        GetSystemDirectoryA(sys, MAX_PATH);
        lstrcatA(sys, "\\d3d9.dll");
        g_d3d9 = LoadLibraryA(sys);
        if (!g_d3d9)
            return NULL;
        orig = (PFN_Direct3DCreate9)GetProcAddress(g_d3d9,
                                                 "Direct3DCreate9");
        if (!orig)
            return NULL;
        load_config();
    }
    d3d = orig(version);
    logmsg("Direct3DCreate9 called");
    if (d3d && !g_d3d9_vt_patched) {
        g_origCreateDevice = patch_vt(d3d, 16, CreateDevice_Hook);
        g_d3d9_vt_patched = 1;
        logmsg("IDirect3D9 vtable patched");
    }
    return d3d;
}

static void loginit(void){ logmsg("proxy loaded"); }
BOOL WINAPI DllMainCRTStartup(HINSTANCE inst, DWORD reason, void *res)
{
    (void)inst; (void)res;
    if (reason == DLL_PROCESS_ATTACH) loginit();
    return TRUE;
}
