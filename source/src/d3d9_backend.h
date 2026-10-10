#ifndef D3D9_BACKEND_H
#define D3D9_BACKEND_H

/* Direct3D 9 renderer for the RTX Remix build (CMake AVP_ENABLE_RTX_REMIX, Windows x86).
 *
 * The engine's rendering reaches the GPU through two kinds of call, and this replaces both:
 *
 *  - The FIXED-FUNCTION-STYLE GL entry points (pglBlendFunc, pglTexImage2D, pglClear, ...).
 *    These are function POINTERS (oglfunc.h), so the Remix build simply points them at D3D9
 *    implementations (R9_InstallGLShim, called from load_ogl_functions) - no call site in
 *    opengl.c, fmv.c or main.c changes. GL texture names map to D3D9 textures, and blend /
 *    depth / filter state is tracked and applied at draw time.
 *
 *  - The SHADER path (the game shader in opengl.c, the cinema shader in main.c). Those calls
 *    are pointed at harmless stubs, and the handful of places that actually draw call this
 *    backend directly instead: FlushTriangleBuffers -> R9_DrawBatch, PresentSoftwareSurface
 *    -> R9_PresentSurface565, and the buffer swap -> R9_Present.
 *
 * Geometry still arrives as the engine's CPU-projected clip coordinates, drawn as D3D9
 * pre-transformed (XYZRHW) vertices with fixed-function texture stages that reproduce the
 * game shader exactly (modulate; specular second pass; alpha test at 0.01). Remix treats
 * pre-transformed geometry as UI, so it captures the frame but cannot path-trace it - that
 * needs world-space geometry with SetTransform matrices, the next stage of this work (see
 * "RTX Remix" in CLAUDE.md). */

#ifdef AVP_RTX_REMIX

#include <SDL3/SDL.h>

/* Must match VertexArray in opengl.c field for field (checked there). */
typedef struct R9GameVertex {
    float v[4];            /* clip-space position (x*w, y*w, z*w, w) */
    float t[2];            /* texture coordinates */
    unsigned char c[4];    /* diffuse RGBA */
    unsigned char s[4];    /* specular RGBA (second pass) */
} R9GameVertex;

int  R9_Init(SDL_Window *window);          /* 1 on success */
void R9_Shutdown(void);
void R9_InstallGLShim(void);               /* point the pgl* / pfn_gl* pointers at D3D9 */

/* Draw one batch from opengl.c's triangle buffers. specularPass selects the second
   (specular) pass of the game shader. worldSpace: 0 = clip-space vertices drawn
   pre-transformed, 1 = world-space vertices, 2 = clip-space vertices to be UNPROJECTED
   into world space through the current camera (see R9_Unproject). */
void R9_DrawBatch(const R9GameVertex *verts, int nverts,
                  const unsigned short *indices, int ntris, int specularPass, int worldSpace);

/* Stage 2a, world-space geometry. worldSpace batches carry world positions in v[0..2] and
   are transformed by the camera from R9_SetCamera: row-major D3D9 VIEW and PROJECTION
   matrices (row vectors, y up), built in TranslationSetup (kshape.c). R9_WorldEnabled is
   the A/B switch: on unless the AVP_REMIX_WORLD environment variable is "0". */
void R9_SetCamera(const float view[16], const float proj[16]);
int  R9_WorldEnabled(void);

/* Marks where the frame's screen-space overlay begins (the view model, then the HUD).
   Remix composites its path-traced image at the first draw it classes as UI and
   rasterizes everything after it on top, unchanged. Pre-transformed draws do not count
   as UI by default (rtx.preTransformedVerticesIsUI), and turning that on would fire at
   the frame's FIRST one - the sky, or any stray effect inside the world pass. So the
   backend issues one invisible orthographic draw here instead, which Remix's
   orthographicIsUI test always catches. Once per scene; a no-op with the world path off. */
void R9_BeginOverlay(void);

/* The frame's dynamic lights, as D3D9 point lights that Remix converts into its own.
   The game lights only through vertex colours, which Remix discards as baked lighting,
   so without these nothing the game lights (muzzle flashes, explosions, flares, the
   level's own lights) lights anything under Remix. pos is world space (the same space as
   the world-path vertices), rgb 0..1 and more, range in world units. */
#define R9_MAX_LIGHTS 256
void R9_SetLights(int n, const float (*pos)[3], const float (*rgb)[3], const float *range);

/* The 640x480 RGB565 software surface (menus, FMVs, loading screens), letterboxed into
   the window, then presented. */
void R9_PresentSurface565(const void *pixels, int pitch, int viewW, int viewH);

void R9_Present(void);

/* Readback for screenshots: bottom-up rows, as glReadPixels returns them. */
void R9_ReadPixelsRGB(int x, int y, int w, int h, unsigned char *dst);

#endif /* AVP_RTX_REMIX */
#endif /* D3D9_BACKEND_H */
