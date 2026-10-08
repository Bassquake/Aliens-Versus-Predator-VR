/* Direct3D 9 renderer for the RTX Remix build - see d3d9_backend.h for the overview.
 *
 * Compiled only with AVP_RTX_REMIX (CMake AVP_ENABLE_RTX_REMIX, Windows x86). */

#ifdef AVP_RTX_REMIX

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <string.h>
#include <stdlib.h>

#include "oglfunc.h"
#include "d3d9_backend.h"

/* ------------------------------------------------------------------------------------- */
/* Device                                                                                */

static IDirect3D9            *r9_d3d;
static IDirect3DDevice9      *r9_dev;
static D3DPRESENT_PARAMETERS  r9_pp;
static SDL_Window            *r9_window;
static D3DCAPS9               r9_caps;
static int                    r9_in_scene;
static int                    r9_state_dirty = 1;

/* Tracked GL state. It is applied at draw time (R9_ApplyState), not as each GL call
   arrives, so the order the engine sets state in never matters and the menu blit
   below can change device state freely and just mark it dirty. */
static int      gs_blend_enabled = 0;
static GLenum   gs_blend_src = GL_ONE, gs_blend_dst = GL_ZERO;
static int      gs_depth_test = 0;
static int      gs_depth_write = 1;
static GLenum   gs_depth_func = GL_LESS;
static int      gs_poly_offset = 0;
static float    gs_poly_factor = 0.0f, gs_poly_units = 0.0f;
static float    gs_clear_r, gs_clear_g, gs_clear_b, gs_clear_a;
static int      gs_vp_x, gs_vp_y, gs_vp_w, gs_vp_h;    /* GL convention: y from bottom */
static GLuint   gs_bound_tex = 0;

/* Stage 2a camera (R9_SetCamera). PROJECTION gets D3D9's half-pixel offset added when it
   is applied, since that depends on the viewport. */
static D3DMATRIX r9_view, r9_proj;
static int       r9_have_camera = 0;

static void R9_SetViewportFromGL(void);

static void R9_LogLoadedD3D9(void)
{
    char path[MAX_PATH];
    HMODULE m = GetModuleHandleA("d3d9.dll");
    if (m && GetModuleFileNameA(m, path, sizeof(path)))
        SDL_Log("D3D9: d3d9.dll loaded from %s", path);
}

static int R9_CreateDevice(HWND hwnd, int w, int h)
{
    HRESULT hr;
    DWORD flags = D3DCREATE_FPU_PRESERVE;

    memset(&r9_pp, 0, sizeof(r9_pp));
    r9_pp.Windowed               = TRUE;
    r9_pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    r9_pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    r9_pp.BackBufferWidth        = w;
    r9_pp.BackBufferHeight       = h;
    r9_pp.BackBufferCount        = 1;
    r9_pp.hDeviceWindow          = hwnd;
    r9_pp.EnableAutoDepthStencil = TRUE;
    r9_pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    r9_pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    hr = IDirect3D9_CreateDevice(r9_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                 flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &r9_pp, &r9_dev);
    if (FAILED(hr)) {
        SDL_Log("D3D9: hardware vertex processing refused (0x%08lx), trying software", hr);
        hr = IDirect3D9_CreateDevice(r9_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                     flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &r9_pp, &r9_dev);
    }
    if (FAILED(hr)) {
        SDL_Log("D3D9: CreateDevice failed (0x%08lx)", hr);
        r9_dev = NULL;
        return 0;
    }
    return 1;
}

int R9_Init(SDL_Window *window)
{
    D3DADAPTER_IDENTIFIER9 id;
    HWND hwnd;
    int w = 0, h = 0;

    r9_window = window;
    hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd) {
        SDL_Log("D3D9: no HWND for the window");
        return 0;
    }
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;

    r9_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!r9_d3d) {
        SDL_Log("D3D9: Direct3DCreate9 failed");
        return 0;
    }
    R9_LogLoadedD3D9();
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(r9_d3d, D3DADAPTER_DEFAULT, 0, &id)))
        SDL_Log("D3D9: adapter %s", id.Description);

    if (!R9_CreateDevice(hwnd, w, h)) {
        IDirect3D9_Release(r9_d3d);
        r9_d3d = NULL;
        return 0;
    }
    IDirect3DDevice9_GetDeviceCaps(r9_dev, &r9_caps);
    SDL_Log("D3D9: device ready %dx%d, max anisotropy %lu", w, h, r9_caps.MaxAnisotropy);

    gs_vp_x = 0; gs_vp_y = 0; gs_vp_w = w; gs_vp_h = h;
    r9_state_dirty = 1;
    return 1;
}

/* ------------------------------------------------------------------------------------- */
/* Textures: GL texture names -> D3D9 textures                                            */

typedef struct R9Tex {
    IDirect3DTexture9 *tex;
    int     w, h, levels;
    D3DFORMAT fmt;
    GLint   min_filter, mag_filter, wrap_s, wrap_t;
    float   aniso;
    int     used;
} R9Tex;

static R9Tex *r9_tex;
static int    r9_tex_cap;

static R9Tex *R9_TexFor(GLuint name)
{
    if (name == 0 || (int)name >= r9_tex_cap || !r9_tex[name].used) return NULL;
    return &r9_tex[name];
}

static void APIENTRY shim_GenTextures(GLsizei n, GLuint *out)
{
    int i, slot = 1;
    for (i = 0; i < n; i++) {
        while (slot < r9_tex_cap && r9_tex[slot].used) slot++;
        if (slot >= r9_tex_cap) {
            int ncap = r9_tex_cap ? r9_tex_cap * 2 : 1024;
            r9_tex = (R9Tex *)realloc(r9_tex, ncap * sizeof(R9Tex));
            memset(r9_tex + r9_tex_cap, 0, (ncap - r9_tex_cap) * sizeof(R9Tex));
            r9_tex_cap = ncap;
        }
        memset(&r9_tex[slot], 0, sizeof(R9Tex));
        r9_tex[slot].used = 1;
        r9_tex[slot].min_filter = GL_NEAREST_MIPMAP_LINEAR;   /* GL defaults */
        r9_tex[slot].mag_filter = GL_LINEAR;
        r9_tex[slot].wrap_s = r9_tex[slot].wrap_t = GL_REPEAT;
        r9_tex[slot].aniso = 1.0f;
        out[i] = (GLuint)slot;
        slot++;
    }
}

static void APIENTRY shim_DeleteTextures(GLsizei n, const GLuint *names)
{
    int i;
    for (i = 0; i < n; i++) {
        R9Tex *t = R9_TexFor(names[i]);
        if (!t) continue;
        if (t->tex) IDirect3DTexture9_Release(t->tex);
        memset(t, 0, sizeof(*t));
        if (gs_bound_tex == names[i]) gs_bound_tex = 0;
    }
}

static void APIENTRY shim_BindTexture(GLenum target, GLuint name)
{
    (void)target;
    gs_bound_tex = name;
}

/* Copy GL-layout pixels (top row first, as the engine supplies them) into a locked
   D3D9 rectangle. RGBA bytes become D3DFMT_A8R8G8B8 (B,G,R,A in memory); RGB565 goes
   straight into D3DFMT_R5G6B5. */
static void R9_CopyPixels(const R9Tex *t, const D3DLOCKED_RECT *lr, int w, int h,
                          GLenum format, GLenum type, const void *pixels)
{
    int x, y;
    if (t->fmt == D3DFMT_R5G6B5 && type == GL_UNSIGNED_SHORT_5_6_5) {
        const unsigned char *src = (const unsigned char *)pixels;
        for (y = 0; y < h; y++)
            memcpy((unsigned char *)lr->pBits + y * lr->Pitch, src + y * w * 2, w * 2);
        return;
    }
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE) {
        const unsigned char *src = (const unsigned char *)pixels;
        for (y = 0; y < h; y++) {
            unsigned char *dst = (unsigned char *)lr->pBits + y * lr->Pitch;
            const unsigned char *s = src + y * w * 4;
            for (x = 0; x < w; x++, s += 4, dst += 4) {
                dst[0] = s[2]; dst[1] = s[1]; dst[2] = s[0]; dst[3] = s[3];
            }
        }
        return;
    }
    SDL_Log("D3D9: unsupported texture upload format 0x%x/0x%x", format, type);
}

static void APIENTRY shim_TexImage2D(GLenum target, GLint level, GLint internalformat,
                                     GLsizei width, GLsizei height, GLint border,
                                     GLenum format, GLenum type, const GLvoid *pixels)
{
    R9Tex *t = R9_TexFor(gs_bound_tex);
    HRESULT hr;
    (void)target; (void)internalformat; (void)border;

    if (!t || !r9_dev || level != 0 || width <= 0 || height <= 0) return;
    if (t->tex) { IDirect3DTexture9_Release(t->tex); t->tex = NULL; }

    /* RGB565 is the software-surface texture (one level, never mipmapped); everything
       else is an RGBA game texture with a full mip chain, filled by glGenerateMipmap. */
    t->fmt    = (type == GL_UNSIGNED_SHORT_5_6_5) ? D3DFMT_R5G6B5 : D3DFMT_A8R8G8B8;
    t->levels = (t->fmt == D3DFMT_R5G6B5) ? 1 : 0;
    hr = IDirect3DDevice9_CreateTexture(r9_dev, width, height, t->levels, 0, t->fmt,
                                        D3DPOOL_MANAGED, &t->tex, NULL);
    if (FAILED(hr)) {
        SDL_Log("D3D9: CreateTexture %dx%d failed (0x%08lx)", width, height, hr);
        t->tex = NULL;
        return;
    }
    t->w = width; t->h = height;
    t->levels = (int)IDirect3DTexture9_GetLevelCount(t->tex);

    if (pixels) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, 0))) {
            R9_CopyPixels(t, &lr, width, height, format, type, pixels);
            IDirect3DTexture9_UnlockRect(t->tex, 0);
        }
    }
}

static void APIENTRY shim_TexSubImage2D(GLenum target, GLint level, GLint xoff, GLint yoff,
                                        GLsizei width, GLsizei height, GLenum format,
                                        GLenum type, const GLvoid *pixels)
{
    R9Tex *t = R9_TexFor(gs_bound_tex);
    D3DLOCKED_RECT lr;
    RECT rc;
    (void)target;

    if (!t || !t->tex || level != 0 || !pixels) return;
    if (xoff + width > t->w)  width  = t->w - xoff;
    if (yoff + height > t->h) height = t->h - yoff;
    if (width <= 0 || height <= 0) return;
    rc.left = xoff; rc.top = yoff; rc.right = xoff + width; rc.bottom = yoff + height;
    if (SUCCEEDED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, &rc, 0))) {
        R9_CopyPixels(t, &lr, width, height, format, type, pixels);
        IDirect3DTexture9_UnlockRect(t->tex, 0);
    }
}

/* glGenerateMipmap: a 2x2 box filter down the chain, on the CPU. Done by hand rather
   than with D3DUSAGE_AUTOGENMIPMAP so every level holds real data the moment the call
   returns, whatever the driver - and whatever Remix makes of auto-generated levels. */
static void R9_GenerateMips(R9Tex *t)
{
    int lv;
    if (!t || !t->tex || t->fmt != D3DFMT_A8R8G8B8) return;
    for (lv = 1; lv < t->levels; lv++) {
        D3DSURFACE_DESC sd, dd;
        D3DLOCKED_RECT  sl, dl;
        int x, y;
        IDirect3DTexture9_GetLevelDesc(t->tex, lv - 1, &sd);
        IDirect3DTexture9_GetLevelDesc(t->tex, lv, &dd);
        if (FAILED(IDirect3DTexture9_LockRect(t->tex, lv - 1, &sl, NULL, D3DLOCK_READONLY)))
            return;
        if (FAILED(IDirect3DTexture9_LockRect(t->tex, lv, &dl, NULL, 0))) {
            IDirect3DTexture9_UnlockRect(t->tex, lv - 1);
            return;
        }
        for (y = 0; y < (int)dd.Height; y++) {
            int y0 = y * 2, y1 = (y * 2 + 1 < (int)sd.Height) ? y * 2 + 1 : y * 2;
            const unsigned char *r0 = (const unsigned char *)sl.pBits + y0 * sl.Pitch;
            const unsigned char *r1 = (const unsigned char *)sl.pBits + y1 * sl.Pitch;
            unsigned char *d = (unsigned char *)dl.pBits + y * dl.Pitch;
            for (x = 0; x < (int)dd.Width; x++) {
                int x0 = x * 2, x1 = (x * 2 + 1 < (int)sd.Width) ? x * 2 + 1 : x * 2, c;
                for (c = 0; c < 4; c++)
                    d[x * 4 + c] = (unsigned char)((r0[x0 * 4 + c] + r0[x1 * 4 + c] +
                                                    r1[x0 * 4 + c] + r1[x1 * 4 + c] + 2) >> 2);
            }
        }
        IDirect3DTexture9_UnlockRect(t->tex, lv);
        IDirect3DTexture9_UnlockRect(t->tex, lv - 1);
    }
}

static void APIENTRY shim_GenerateMipmap(GLenum target)
{
    (void)target;
    R9_GenerateMips(R9_TexFor(gs_bound_tex));
}

static void APIENTRY shim_TexParameteri(GLenum target, GLenum pname, GLint param)
{
    R9Tex *t = R9_TexFor(gs_bound_tex);
    (void)target;
    if (!t) return;
    switch (pname) {
        case GL_TEXTURE_MIN_FILTER: t->min_filter = param; break;
        case GL_TEXTURE_MAG_FILTER: t->mag_filter = param; break;
        case GL_TEXTURE_WRAP_S:     t->wrap_s = param;     break;
        case GL_TEXTURE_WRAP_T:     t->wrap_t = param;     break;
        default: break;
    }
}

static void APIENTRY shim_TexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    R9Tex *t = R9_TexFor(gs_bound_tex);
    (void)target;
    if (!t) return;
    if (pname == GL_TEXTURE_MAX_ANISOTROPY_EXT) t->aniso = param;
    else shim_TexParameteri(target, pname, (GLint)param);
}

/* ------------------------------------------------------------------------------------- */
/* Fixed-function state                                                                  */

static D3DBLEND R9_Blend(GLenum f)
{
    switch (f) {
        case GL_ZERO:                return D3DBLEND_ZERO;
        case GL_ONE:                 return D3DBLEND_ONE;
        case GL_SRC_COLOR:           return D3DBLEND_SRCCOLOR;
        case GL_ONE_MINUS_SRC_COLOR: return D3DBLEND_INVSRCCOLOR;
        case GL_SRC_ALPHA:           return D3DBLEND_SRCALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return D3DBLEND_INVSRCALPHA;
        case GL_DST_ALPHA:           return D3DBLEND_DESTALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return D3DBLEND_INVDESTALPHA;
        case GL_DST_COLOR:           return D3DBLEND_DESTCOLOR;
        case GL_ONE_MINUS_DST_COLOR: return D3DBLEND_INVDESTCOLOR;
        default:                     return D3DBLEND_ONE;
    }
}

static DWORD R9_FloatBits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

static void R9_ApplyState(void)
{
    IDirect3DDevice9 *d = r9_dev;
    if (!r9_state_dirty) return;
    r9_state_dirty = 0;

    IDirect3DDevice9_SetRenderState(d, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_FOGENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SPECULARENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SHADEMODE, D3DSHADE_GOURAUD);

    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, gs_blend_enabled ? TRUE : FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_SRCBLEND,  R9_Blend(gs_blend_src));
    IDirect3DDevice9_SetRenderState(d, D3DRS_DESTBLEND, R9_Blend(gs_blend_dst));

    /* The game shader discards below alpha 0.01: keep 3/255 and up. */
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHAREF, 3);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);

    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, gs_depth_test ? D3DZB_TRUE : D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZWRITEENABLE, gs_depth_write ? TRUE : FALSE);
    /* GL_NEVER..GL_ALWAYS (0x200..0x207) line up with D3DCMP_NEVER..D3DCMP_ALWAYS (1..8). */
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZFUNC, (DWORD)(gs_depth_func - GL_NEVER + D3DCMP_NEVER));

    /* glPolygonOffset's units are multiples of the smallest resolvable depth step,
       2^-24 for the D24 buffer; D3D9's bias is in depth units directly. */
    IDirect3DDevice9_SetRenderState(d, D3DRS_DEPTHBIAS,
        R9_FloatBits(gs_poly_offset ? gs_poly_units / 16777216.0f : 0.0f));
    IDirect3DDevice9_SetRenderState(d, D3DRS_SLOPESCALEDEPTHBIAS,
        R9_FloatBits(gs_poly_offset ? gs_poly_factor : 0.0f));

    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(d, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    R9_SetViewportFromGL();

    if (r9_have_camera) {
        /* The engine places pixel centres as GL does; D3D9's sit half a pixel away. The
           pre-transformed path subtracts 0.5 per vertex, and this is the same correction
           in clip space: -1/W of w on x, +1/H of w on y (w = Z is _34's term). */
        D3DMATRIX ident, proj = r9_proj;
        float W = (float)(gs_vp_w > 0 ? gs_vp_w : 1), H = (float)(gs_vp_h > 0 ? gs_vp_h : 1);
        proj._31 -= proj._34 / W;
        proj._32 += proj._34 / H;
        proj._41 -= proj._44 / W;
        proj._42 += proj._44 / H;
        memset(&ident, 0, sizeof(ident));
        ident._11 = ident._22 = ident._33 = ident._44 = 1.0f;
        IDirect3DDevice9_SetTransform(d, D3DTS_WORLD, &ident);
        IDirect3DDevice9_SetTransform(d, D3DTS_VIEW, &r9_view);
        IDirect3DDevice9_SetTransform(d, D3DTS_PROJECTION, &proj);
        IDirect3DDevice9_SetRenderState(d, D3DRS_CLIPPING, TRUE);
    }
}

void R9_SetCamera(const float view[16], const float proj[16])
{
    memcpy(&r9_view, view, sizeof(r9_view));
    memcpy(&r9_proj, proj, sizeof(r9_proj));
    r9_have_camera = 1;
    r9_state_dirty = 1;
}

int R9_WorldEnabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = SDL_getenv("AVP_REMIX_WORLD");
        enabled = !(e && e[0] == '0');
        SDL_Log("D3D9: world-space level geometry %s (AVP_REMIX_WORLD)", enabled ? "ON" : "OFF");
    }
    return enabled;
}

static void R9_SetViewportFromGL(void)
{
    D3DVIEWPORT9 vp;
    int x = gs_vp_x, y = (int)r9_pp.BackBufferHeight - (gs_vp_y + gs_vp_h);
    int w = gs_vp_w, h = gs_vp_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)r9_pp.BackBufferWidth)  w = (int)r9_pp.BackBufferWidth - x;
    if (y + h > (int)r9_pp.BackBufferHeight) h = (int)r9_pp.BackBufferHeight - y;
    if (w <= 0 || h <= 0) return;
    vp.X = x; vp.Y = y; vp.Width = w; vp.Height = h; vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(r9_dev, &vp);
}

/* Sampler state comes from the bound texture's GL parameters, as GL keeps them. */
static void R9_ApplySampler(const R9Tex *t)
{
    IDirect3DDevice9 *d = r9_dev;
    DWORD mag, min, mip = D3DTEXF_NONE, maxAniso = 1;

    mag = (t->mag_filter == GL_NEAREST) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    switch (t->min_filter) {
        case GL_NEAREST:                min = D3DTEXF_POINT;  break;
        case GL_LINEAR:                 min = D3DTEXF_LINEAR; break;
        case GL_NEAREST_MIPMAP_NEAREST: min = D3DTEXF_POINT;  mip = D3DTEXF_POINT;  break;
        case GL_NEAREST_MIPMAP_LINEAR:  min = D3DTEXF_POINT;  mip = D3DTEXF_LINEAR; break;
        case GL_LINEAR_MIPMAP_NEAREST:  min = D3DTEXF_LINEAR; mip = D3DTEXF_POINT;  break;
        default:                        min = D3DTEXF_LINEAR; mip = D3DTEXF_LINEAR; break;
    }
    if (t->levels <= 1) mip = D3DTEXF_NONE;
    if (min == D3DTEXF_LINEAR && t->aniso > 1.0f && r9_caps.MaxAnisotropy > 1) {
        maxAniso = (DWORD)t->aniso;
        if (maxAniso > r9_caps.MaxAnisotropy) maxAniso = r9_caps.MaxAnisotropy;
        min = D3DTEXF_ANISOTROPIC;
    }
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, mag);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, min);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, mip);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAXANISOTROPY, maxAniso);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU,
        t->wrap_s == GL_REPEAT ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV,
        t->wrap_t == GL_REPEAT ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
}

/* Each frame's scene starts from a cleared target. GL kept the previous frame's pixels,
   and the engine relies on that - it never clears colour in normal play, trusting the
   world to cover every pixel - but a D3DSWAPEFFECT_DISCARD back buffer is undefined after
   a Present. The scene begins on the frame's FIRST draw or clear, ahead of it, so a fill
   the game asks for (ColourFillBackBuffer) still lands on top of this. */
static void R9_EnsureScene(void)
{
    if (!r9_in_scene && r9_dev) {
        IDirect3DDevice9_BeginScene(r9_dev);
        r9_in_scene = 1;
        R9_ApplyState();
        IDirect3DDevice9_Clear(r9_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
                               D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    }
}

static void APIENTRY shim_BlendFunc(GLenum s, GLenum dfac)
{
    gs_blend_src = s; gs_blend_dst = dfac; r9_state_dirty = 1;
}
static void APIENTRY shim_DepthFunc(GLenum f)      { gs_depth_func = f; r9_state_dirty = 1; }
static void APIENTRY shim_DepthMask(GLboolean m)   { gs_depth_write = m ? 1 : 0; r9_state_dirty = 1; }
static void APIENTRY shim_PolygonOffset(GLfloat f, GLfloat u)
{
    gs_poly_factor = f; gs_poly_units = u; r9_state_dirty = 1;
}
static void R9_SetCap(GLenum cap, int on)
{
    switch (cap) {
        case GL_BLEND:               gs_blend_enabled = on; break;
        case GL_DEPTH_TEST:          gs_depth_test = on;    break;
        case GL_POLYGON_OFFSET_FILL: gs_poly_offset = on;   break;
        default: return;               /* GL_CULL_FACE etc: culling is always off */
    }
    r9_state_dirty = 1;
}
static void APIENTRY shim_Enable(GLenum cap)  { R9_SetCap(cap, 1); }
static void APIENTRY shim_Disable(GLenum cap) { R9_SetCap(cap, 0); }

static void APIENTRY shim_Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    gs_vp_x = x; gs_vp_y = y; gs_vp_w = w; gs_vp_h = h;
    r9_state_dirty = 1;
}

static void APIENTRY shim_ClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
    gs_clear_r = r; gs_clear_g = g; gs_clear_b = b; gs_clear_a = a;
}

static DWORD R9_ByteClamp(float f)
{
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return (DWORD)(f * 255.0f + 0.5f);
}

static void APIENTRY shim_Clear(GLbitfield mask)
{
    DWORD flags = 0;
    D3DCOLOR col;
    if (!r9_dev) return;
    if (mask & GL_COLOR_BUFFER_BIT) flags |= D3DCLEAR_TARGET;
    if (mask & GL_DEPTH_BUFFER_BIT) flags |= D3DCLEAR_ZBUFFER;
    if (mask & GL_STENCIL_BUFFER_BIT) flags |= D3DCLEAR_STENCIL;
    if (!flags) return;
    R9_EnsureScene();  /* the frame's first clear begins the scene (see R9_EnsureScene) */
    R9_ApplyState();   /* D3D9 clears the current viewport, as GL clears the scissor */
    col = D3DCOLOR_ARGB(R9_ByteClamp(gs_clear_a), R9_ByteClamp(gs_clear_r),
                        R9_ByteClamp(gs_clear_g), R9_ByteClamp(gs_clear_b));
    IDirect3DDevice9_Clear(r9_dev, 0, NULL, flags, col, 1.0f, 0);
}

static void APIENTRY shim_GetFloatv(GLenum pname, GLfloat *out)
{
    if (!out) return;
    if (pname == GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT)
        *out = (GLfloat)(r9_caps.MaxAnisotropy ? r9_caps.MaxAnisotropy : 1);
    else
        *out = 0.0f;
}

static void APIENTRY shim_GetIntegerv(GLenum pname, GLint *out) { (void)pname; if (out) *out = 0; }
static GLenum APIENTRY shim_GetError(void) { return GL_NO_ERROR; }
static const GLubyte *APIENTRY shim_GetString(GLenum name)
{
    switch (name) {
        case GL_VENDOR:     return (const GLubyte *)"AvP D3D9 backend";
        case GL_RENDERER:   return (const GLubyte *)"Direct3D 9";
        case GL_VERSION:    return (const GLubyte *)"2.1 (D3D9 shim)";
        case GL_EXTENSIONS: return (const GLubyte *)"GL_EXT_texture_filter_anisotropic";
        default:            return (const GLubyte *)"";
    }
}

static void APIENTRY shim_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h,
                                     GLenum format, GLenum type, GLvoid *dst)
{
    if (format == GL_RGB && type == GL_UNSIGNED_BYTE)
        R9_ReadPixelsRGB(x, y, w, h, (unsigned char *)dst);
}

static void APIENTRY shim_DrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *idx)
{
    static int warned;
    (void)mode; (void)count; (void)type; (void)idx;
    if (!warned) { warned = 1; SDL_Log("D3D9: unexpected glDrawElements - draw ignored"); }
}

/* Everything else the fixed-function table holds: accepted and ignored. */
static void APIENTRY shim_AlphaFunc(GLenum a, GLclampf b) { (void)a; (void)b; }
static void APIENTRY shim_Color4f(GLfloat a, GLfloat b, GLfloat c, GLfloat d) { (void)a; (void)b; (void)c; (void)d; }
static void APIENTRY shim_Pointer(GLint a, GLenum b, GLsizei c, const GLvoid *d) { (void)a; (void)b; (void)c; (void)d; }
static void APIENTRY shim_Enum(GLenum a) { (void)a; }
static void APIENTRY shim_DepthRange(GLclampd a, GLclampd b) { (void)a; (void)b; }
static void APIENTRY shim_GetTexParameterfv(GLenum a, GLenum b, GLfloat *c) { (void)a; (void)b; if (c) *c = 0.0f; }
static void APIENTRY shim_EnumEnum(GLenum a, GLenum b) { (void)a; (void)b; }
static void APIENTRY shim_PixelStorei(GLenum a, GLint b) { (void)a; (void)b; }
static void APIENTRY shim_TexEnvf(GLenum a, GLenum b, GLfloat c) { (void)a; (void)b; (void)c; }
static void APIENTRY shim_TexEnvfv(GLenum a, GLenum b, const GLfloat *c) { (void)a; (void)b; (void)c; }
static void APIENTRY shim_TexEnvi(GLenum a, GLenum b, GLint c) { (void)a; (void)b; (void)c; }

/* ------------------------------------------------------------------------------------- */
/* Shader-path stubs. The engine's GLSL programs have no D3D9 counterpart: the draws that
   used them call R9_DrawBatch / R9_PresentSurface565 directly, so the rest of that path
   (program setup, attribute pointers, uniforms, the MSAA FBO) only needs to be harmless.
   Every stub matches its PFN type exactly - these are __stdcall on x86, where a mismatched
   argument list would unbalance the stack. MSAA's two entry points are deliberately left
   NULL, which is how MSAA_BeginFrame recognises "unsupported" and stands down. */

static void   APIENTRY st_u(GLuint a) { (void)a; }
static void   APIENTRY st_e(GLenum a) { (void)a; }
static void   APIENTRY st_uu(GLuint a, GLuint b) { (void)a; (void)b; }
static void   APIENTRY st_eu(GLenum a, GLuint b) { (void)a; (void)b; }
static void   APIENTRY st_genN(GLsizei n, GLuint *o) { int i; for (i = 0; i < n; i++) o[i] = (GLuint)(i + 1); }
static void   APIENTRY st_delN(GLsizei n, const GLuint *o) { (void)n; (void)o; }
static void   APIENTRY st_BindAttribLocation(GLuint a, GLuint b, const GLchar *c) { (void)a; (void)b; (void)c; }
static void   APIENTRY st_BufferData(GLenum a, GLsizeiptr b, const void *c, GLenum d) { (void)a; (void)b; (void)c; (void)d; }
static GLuint APIENTRY st_CreateProgram(void) { return 1; }
static GLuint APIENTRY st_CreateShader(GLenum a) { (void)a; return 1; }
static void   APIENTRY st_FramebufferRenderbuffer(GLenum a, GLenum b, GLenum c, GLuint d) { (void)a; (void)b; (void)c; (void)d; }
static void   APIENTRY st_FramebufferTexture2D(GLenum a, GLenum b, GLenum c, GLuint d, GLint e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
static GLint  APIENTRY st_GetLocation(GLuint a, const GLchar *b) { (void)a; (void)b; return -1; }
static void   APIENTRY st_GetInfoLog(GLuint a, GLsizei b, GLsizei *c, GLchar *d) { (void)a; if (c) *c = 0; if (d && b > 0) d[0] = 0; }
static void   APIENTRY st_GetIv(GLuint a, GLenum b, GLint *c) { (void)a; (void)b; if (c) *c = 1; }
static void   APIENTRY st_RenderbufferStorage(GLenum a, GLenum b, GLsizei c, GLsizei d) { (void)a; (void)b; (void)c; (void)d; }
static void   APIENTRY st_ShaderSource(GLuint a, GLsizei b, const GLchar *const *c, const GLint *d) { (void)a; (void)b; (void)c; (void)d; }
static void   APIENTRY st_Uniform1i(GLint a, GLint b) { (void)a; (void)b; }
static void   APIENTRY st_Uniform2f(GLint a, GLfloat b, GLfloat c) { (void)a; (void)b; (void)c; }
static void   APIENTRY st_Uniform1f(GLint a, GLfloat b) { (void)a; (void)b; }
static void   APIENTRY st_VertexAttribPointer(GLuint a, GLint b, GLenum c, GLboolean d, GLsizei e, const void *f) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; }
static void   APIENTRY st_UniformMatrix4fv(GLint a, GLsizei b, GLboolean c, const GLfloat *d) { (void)a; (void)b; (void)c; (void)d; }
static GLenum APIENTRY st_CheckFramebufferStatus(GLenum a) { (void)a; return 0; }

void R9_InstallGLShim(void)
{
    pglAlphaFunc          = shim_AlphaFunc;
    pglBindTexture        = shim_BindTexture;
    pglBlendFunc          = shim_BlendFunc;
    pglClear              = shim_Clear;
    pglClearColor         = shim_ClearColor;
    pglColor4f            = shim_Color4f;
    pglColorPointer       = shim_Pointer;
    pglCullFace           = shim_Enum;
    pglDeleteTextures     = shim_DeleteTextures;
    pglDepthFunc          = shim_DepthFunc;
    pglDepthMask          = shim_DepthMask;
    pglDepthRange         = shim_DepthRange;
    pglDisable            = shim_Disable;
    pglDisableClientState = shim_Enum;
    pglDrawElements       = shim_DrawElements;
    pglEnable             = shim_Enable;
    pglEnableClientState  = shim_Enum;
    pglFrontFace          = shim_Enum;
    pglGenTextures        = shim_GenTextures;
    pglGetError           = shim_GetError;
    pglGetFloatv          = shim_GetFloatv;
    pglGetIntegerv        = shim_GetIntegerv;
    pglGetString          = shim_GetString;
    pglGetTexParameterfv  = shim_GetTexParameterfv;
    pglHint               = shim_EnumEnum;
    pglPixelStorei        = shim_PixelStorei;
    pglPolygonOffset      = shim_PolygonOffset;
    pglReadPixels         = shim_ReadPixels;
    pglShadeModel         = shim_Enum;
    pglTexCoordPointer    = shim_Pointer;
    pglTexEnvf            = shim_TexEnvf;
    pglTexEnvfv           = shim_TexEnvfv;
    pglTexEnvi            = shim_TexEnvi;
    pglTexImage2D         = shim_TexImage2D;
    pglTexParameterf      = shim_TexParameterf;
    pglTexParameteri      = shim_TexParameteri;
    pglTexSubImage2D      = shim_TexSubImage2D;
    pglVertexPointer      = shim_Pointer;
    pglViewport           = shim_Viewport;

    pfn_glActiveTexture            = st_e;
    pfn_glAttachShader             = st_uu;
    pfn_glBindAttribLocation       = st_BindAttribLocation;
    pfn_glBindBuffer               = st_eu;
    pfn_glBindFramebuffer          = st_eu;
    pfn_glBindRenderbuffer         = st_eu;
    pfn_glBindVertexArray          = st_u;
    pfn_glBufferData               = st_BufferData;
    pfn_glCompileShader            = st_u;
    pfn_glCreateProgram            = st_CreateProgram;
    pfn_glCreateShader             = st_CreateShader;
    pfn_glDeleteFramebuffers       = st_delN;
    pfn_glDeleteRenderbuffers      = st_delN;
    pfn_glDeleteShader             = st_u;
    pfn_glDisableVertexAttribArray = st_u;
    pfn_glEnableVertexAttribArray  = st_u;
    pfn_glFramebufferRenderbuffer  = st_FramebufferRenderbuffer;
    pfn_glFramebufferTexture2D     = st_FramebufferTexture2D;
    pfn_glGenBuffers               = st_genN;
    pfn_glGenFramebuffers          = st_genN;
    pfn_glGenRenderbuffers         = st_genN;
    pfn_glGenerateMipmap           = shim_GenerateMipmap;
    pfn_glGetAttribLocation        = st_GetLocation;
    pfn_glGetProgramInfoLog        = st_GetInfoLog;
    pfn_glGetProgramiv             = st_GetIv;
    pfn_glGetShaderInfoLog         = st_GetInfoLog;
    pfn_glGetShaderiv              = st_GetIv;
    pfn_glGetUniformLocation       = st_GetLocation;
    pfn_glLinkProgram              = st_u;
    pfn_glRenderbufferStorage      = st_RenderbufferStorage;
    pfn_glShaderSource             = st_ShaderSource;
    pfn_glUniform1i                = st_Uniform1i;
    pfn_glUniform2f                = st_Uniform2f;
    pfn_glUseProgram               = st_u;
    pfn_glVertexAttribPointer      = st_VertexAttribPointer;
    pfn_glDeleteProgram            = st_u;
    pfn_glGenVertexArrays          = st_genN;
    pfn_glDeleteVertexArrays       = st_delN;
    pfn_glDeleteBuffers            = st_delN;
    pfn_glUniform1f                = st_Uniform1f;
    pfn_glUniformMatrix4fv         = st_UniformMatrix4fv;
    pfn_glCheckFramebufferStatus   = st_CheckFramebufferStatus;
    pfn_glBlitFramebuffer                 = NULL;   /* MSAA: unsupported here */
    pfn_glRenderbufferStorageMultisample  = NULL;

    ogl_have_texture_filter_anisotropic = ogl_use_texture_filter_anisotropic = 1;
    ogl_have_multisample_filter_hint    = ogl_use_multisample_filter_hint    = 0;
}

/* ------------------------------------------------------------------------------------- */
/* Drawing                                                                               */

typedef struct R9TLVertex {
    float x, y, z, rhw;
    D3DCOLOR diffuse, specular;
    float u, v;
} R9TLVertex;
#define R9_TL_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

static R9TLVertex r9_tl[4096];

typedef struct R9WorldVertex {
    float x, y, z;
    D3DCOLOR diffuse, specular;
    float u, v;
} R9WorldVertex;
#define R9_WORLD_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1)

static R9WorldVertex r9_wv[4096];

void R9_DrawBatch(const R9GameVertex *verts, int nverts,
                  const unsigned short *indices, int ntris, int specularPass, int worldSpace)
{
    IDirect3DDevice9 *d = r9_dev;
    const R9Tex *t;
    float vx, vy, vw, vh;
    int i;

    if (!d || nverts <= 0 || ntris <= 0) return;
    if (nverts > (int)(sizeof(r9_tl) / sizeof(r9_tl[0]))) nverts = sizeof(r9_tl) / sizeof(r9_tl[0]);

    R9_EnsureScene();
    R9_ApplyState();

    /* The engine's clip coordinates become D3D9 screen coordinates exactly as GL's
       viewport transform would place them (y flipped, depth -1..1 -> 0..1), less
       D3D9's half-pixel offset so texels land where GL puts them. rhw keeps texture
       and colour interpolation perspective-correct. */
    vx = (float)gs_vp_x;
    vy = (float)((int)r9_pp.BackBufferHeight - (gs_vp_y + gs_vp_h));
    vw = (float)gs_vp_w;
    vh = (float)gs_vp_h;
    if (worldSpace) {
        for (i = 0; i < nverts; i++) {
            const R9GameVertex *s = &verts[i];
            R9WorldVertex *o = &r9_wv[i];
            o->x = s->v[0]; o->y = s->v[1]; o->z = s->v[2];
            o->diffuse  = D3DCOLOR_ARGB(s->c[3], s->c[0], s->c[1], s->c[2]);
            o->specular = D3DCOLOR_ARGB(s->s[3], s->s[0], s->s[1], s->s[2]);
            o->u = s->t[0];
            o->v = s->t[1];
        }
    } else
    for (i = 0; i < nverts; i++) {
        const R9GameVertex *s = &verts[i];
        R9TLVertex *o = &r9_tl[i];
        float w = (s->v[3] != 0.0f) ? s->v[3] : 1e-6f, iw = 1.0f / w;
        o->x   = vx + (s->v[0] * iw * 0.5f + 0.5f) * vw - 0.5f;
        o->y   = vy + (0.5f - s->v[1] * iw * 0.5f) * vh - 0.5f;
        o->z   = s->v[2] * iw * 0.5f + 0.5f;
        o->rhw = iw;
        o->diffuse  = D3DCOLOR_ARGB(s->c[3], s->c[0], s->c[1], s->c[2]);
        o->specular = D3DCOLOR_ARGB(s->s[3], s->s[0], s->s[1], s->s[2]);
        o->u = s->t[0];
        o->v = s->t[1];
    }

    /* The game shader as texture stages: pass 0 is texel * colour; the specular pass
       replaces RGB with the specular colour and keeps texel alpha * specular alpha. */
    t = R9_TexFor(gs_bound_tex);
    if (t && t->tex) {
        IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)t->tex);
        R9_ApplySampler(t);
    } else {
        t = NULL;
        IDirect3DDevice9_SetTexture(d, 0, NULL);
    }
    {
        DWORD col = specularPass ? D3DTA_SPECULAR : D3DTA_DIFFUSE;
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, t && !specularPass ? D3DTA_TEXTURE : col);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG2, col);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP,
            t && !specularPass ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG1, t ? D3DTA_TEXTURE : col);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAARG2, col);
        IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, t ? D3DTOP_MODULATE : D3DTOP_SELECTARG1);
    }

    if (worldSpace && r9_have_camera) {
        IDirect3DDevice9_SetFVF(d, R9_WORLD_FVF);
        IDirect3DDevice9_DrawIndexedPrimitiveUP(d, D3DPT_TRIANGLELIST, 0, nverts, ntris,
                                                indices, D3DFMT_INDEX16, r9_wv, sizeof(R9WorldVertex));
    } else if (!worldSpace) {
        IDirect3DDevice9_SetFVF(d, R9_TL_FVF);
        IDirect3DDevice9_DrawIndexedPrimitiveUP(d, D3DPT_TRIANGLELIST, 0, nverts, ntris,
                                                indices, D3DFMT_INDEX16, r9_tl, sizeof(R9TLVertex));
    }
}

/* ------------------------------------------------------------------------------------- */
/* Present, resize and device loss                                                       */

static void R9_ResetDevice(int w, int h)
{
    HRESULT hr;
    if (w > 0) r9_pp.BackBufferWidth  = w;
    if (h > 0) r9_pp.BackBufferHeight = h;
    /* Everything this backend creates is in D3DPOOL_MANAGED or SYSTEMMEM, so nothing
       has to be released before a Reset. */
    hr = IDirect3DDevice9_Reset(r9_dev, &r9_pp);
    if (FAILED(hr))
        SDL_Log("D3D9: Reset %ux%u failed (0x%08lx)", r9_pp.BackBufferWidth, r9_pp.BackBufferHeight, hr);
    else
        SDL_Log("D3D9: device reset to %ux%u", r9_pp.BackBufferWidth, r9_pp.BackBufferHeight);
    r9_state_dirty = 1;
}

void R9_Present(void)
{
    HRESULT hr;
    int w = 0, h = 0;

    if (!r9_dev) return;
    if (r9_in_scene) {
        IDirect3DDevice9_EndScene(r9_dev);
        r9_in_scene = 0;
    }
    hr = IDirect3DDevice9_Present(r9_dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST) {
        if (IDirect3DDevice9_TestCooperativeLevel(r9_dev) == D3DERR_DEVICENOTRESET)
            R9_ResetDevice(0, 0);
        return;
    }

    /* The game resizes the window on a video-mode change; follow it with the back buffer. */
    SDL_GetWindowSizeInPixels(r9_window, &w, &h);
    if (w > 0 && h > 0 && ((UINT)w != r9_pp.BackBufferWidth || (UINT)h != r9_pp.BackBufferHeight))
        R9_ResetDevice(w, h);
}

typedef struct R9QuadVertex { float x, y, z, rhw, u, v; } R9QuadVertex;

static IDirect3DTexture9 *menuTex;

void R9_PresentSurface565(const void *pixels, int pitch, int viewW, int viewH)
{
    IDirect3DDevice9 *d = r9_dev;
    D3DLOCKED_RECT lr;
    D3DVIEWPORT9 vp;
    float x0, x1, y0, y1, a, b, W, H, s1, t1;
    R9QuadVertex q[4];
    int y;

    if (!d || !pixels) return;
    if (!menuTex &&
        FAILED(IDirect3DDevice9_CreateTexture(d, 1024, 512, 1, 0, D3DFMT_R5G6B5,
                                              D3DPOOL_MANAGED, &menuTex, NULL))) {
        menuTex = NULL;
        return;
    }
    if (SUCCEEDED(IDirect3DTexture9_LockRect(menuTex, 0, &lr, NULL, 0))) {
        for (y = 0; y < 480; y++)
            memcpy((unsigned char *)lr.pBits + y * lr.Pitch,
                   (const unsigned char *)pixels + y * pitch, 640 * 2);
        IDirect3DTexture9_UnlockRect(menuTex, 0);
    }

    /* Same letterbox as the GL path: the 640x480 image scaled to fit, centred. */
    W = (float)(viewW > 0 ? viewW : (int)r9_pp.BackBufferWidth);
    H = (float)(viewH > 0 ? viewH : (int)r9_pp.BackBufferHeight);
    a = H * 640.0f / 480.0f;
    b = W * 480.0f / 640.0f;
    if (a <= W) { x0 = (W - a) * 0.5f; x1 = x0 + a; y0 = 0.0f; y1 = H; }
    else        { y0 = (H - b) * 0.5f; y1 = y0 + b; x0 = 0.0f; x1 = W; }
    s1 = 640.0f / 1024.0f;
    t1 = 480.0f / 512.0f;

    q[0].x = x0 - 0.5f; q[0].y = y0 - 0.5f; q[0].u = 0.0f; q[0].v = 0.0f;
    q[1].x = x1 - 0.5f; q[1].y = y0 - 0.5f; q[1].u = s1;   q[1].v = 0.0f;
    q[2].x = x0 - 0.5f; q[2].y = y1 - 0.5f; q[2].u = 0.0f; q[2].v = t1;
    q[3].x = x1 - 0.5f; q[3].y = y1 - 0.5f; q[3].u = s1;   q[3].v = t1;
    for (y = 0; y < 4; y++) { q[y].z = 0.0f; q[y].rhw = 1.0f; }

    vp.X = 0; vp.Y = 0; vp.Width = r9_pp.BackBufferWidth; vp.Height = r9_pp.BackBufferHeight;
    vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
    IDirect3DDevice9_SetViewport(d, &vp);
    IDirect3DDevice9_Clear(d, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    R9_EnsureScene();
    IDirect3DDevice9_SetRenderState(d, D3DRS_ZENABLE, D3DZB_FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_ALPHATESTENABLE, FALSE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(d, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetTexture(d, 0, (IDirect3DBaseTexture9 *)menuTex);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(d, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetFVF(d, D3DFVF_XYZRHW | D3DFVF_TEX1);
    IDirect3DDevice9_DrawPrimitiveUP(d, D3DPT_TRIANGLESTRIP, 2, q, sizeof(R9QuadVertex));
    IDirect3DDevice9_SetTextureStageState(d, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);

    r9_state_dirty = 1;   /* the game's tracked state goes back on at the next draw */
    R9_Present();
}

void R9_ReadPixelsRGB(int x, int y, int w, int h, unsigned char *dst)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DLOCKED_RECT lr;
    int row, col, H = (int)r9_pp.BackBufferHeight;

    memset(dst, 0, (size_t)w * h * 3);
    if (!r9_dev) return;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(r9_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        return;
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(r9_dev, r9_pp.BackBufferWidth,
            r9_pp.BackBufferHeight, r9_pp.BackBufferFormat, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(r9_dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        /* GL rows run bottom-up from (x, y); D3D9 surfaces run top-down. */
        for (row = 0; row < h; row++) {
            int sy = H - 1 - (y + row);
            const unsigned char *s;
            unsigned char *o = dst + (size_t)row * w * 3;
            if (sy < 0 || sy >= H) continue;
            s = (const unsigned char *)lr.pBits + sy * lr.Pitch;
            for (col = 0; col < w; col++) {
                int sx = x + col;
                if (sx < 0 || sx >= (int)r9_pp.BackBufferWidth) continue;
                o[col * 3 + 0] = s[sx * 4 + 2];
                o[col * 3 + 1] = s[sx * 4 + 1];
                o[col * 3 + 2] = s[sx * 4 + 0];
            }
        }
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

void R9_Shutdown(void)
{
    int i;
    for (i = 0; i < r9_tex_cap; i++)
        if (r9_tex[i].used && r9_tex[i].tex) IDirect3DTexture9_Release(r9_tex[i].tex);
    free(r9_tex);
    r9_tex = NULL;
    if (menuTex) { IDirect3DTexture9_Release(menuTex); menuTex = NULL; }
    if (r9_dev) {
        /* Unbind before the release, so the device holds no reference to anything. */
        if (r9_in_scene) IDirect3DDevice9_EndScene(r9_dev);
        IDirect3DDevice9_SetTexture(r9_dev, 0, NULL);
    }
    r9_tex_cap = 0;
    if (r9_dev) { IDirect3DDevice9_Release(r9_dev); r9_dev = NULL; }
    if (r9_d3d) { IDirect3D9_Release(r9_d3d); r9_d3d = NULL; }
    r9_in_scene = 0;
}

#endif /* AVP_RTX_REMIX */
