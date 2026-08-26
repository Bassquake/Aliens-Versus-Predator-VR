/* Linux PCVR: the Xlib/GLX half of the OpenXR graphics binding.
 *
 * This lives in its own translation unit on purpose. <X11/Xlib.h> and <GL/glx.h>
 * define Bool, Status, None, Window, Screen and Cursor, every one of which
 * collides with an identifier in the game headers main.c pulls in. Confining
 * them here lets main.c include openxr_platform.h *without* XR_USE_PLATFORM_XLIB
 * and deal only in an opaque XrSessionCreateInfo::next pointer.
 *
 * Compiled only for the Linux PCVR target — CMake adds this file to the source
 * list when AVP_ENABLE_PCVR is on and the platform is Linux.
 */

#include <SDL3/SDL.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <khronos/openxr/openxr.h>
#include <khronos/openxr/openxr_platform.h>

#include "xr_linux_glx.h"

/* Static because xrCreateSession reads the struct through the next chain after
 * this function has returned. */
static XrGraphicsBindingOpenGLXlibKHR avp_glx_binding;

/* Recover the FBConfig the current context was created from. Neither SDL nor GLX
 * hands one back directly: the context knows its FBConfig *ID*, so ask for that
 * and then walk the screen's configs for the match. Returns NULL if anything in
 * that chain fails, which is not fatal — see the caller. */
static GLXFBConfig avp_glx_find_fbconfig(Display *dpy, GLXContext ctx)
{
    int fbconfig_id = 0;

    if (glXQueryContext(dpy, ctx, GLX_FBCONFIG_ID, &fbconfig_id) != Success || fbconfig_id == 0)
        return NULL;

    int n = 0;
    GLXFBConfig *cfgs = glXGetFBConfigs(dpy, DefaultScreen(dpy), &n);
    if (!cfgs)
        return NULL;

    GLXFBConfig found = NULL;
    for (int i = 0; i < n; i++) {
        int id = 0;
        if (glXGetFBConfigAttrib(dpy, cfgs[i], GLX_FBCONFIG_ID, &id) == Success && id == fbconfig_id) {
            found = cfgs[i];
            break;
        }
    }

    /* Frees the array, not the configs — those are owned by the Display and stay
     * valid as long as the connection does. */
    XFree(cfgs);
    return found;
}

const void *AvpXrGlxBinding(void)
{
    Display    *dpy  = glXGetCurrentDisplay();
    GLXDrawable draw = glXGetCurrentDrawable();
    GLXContext  ctx  = glXGetCurrentContext();

    if (!dpy || !draw || !ctx) {
        SDL_Log("XR: no current GLX context (display=%p drawable=%lu context=%p) — "
                "is SDL on the x11 video driver?",
                (void *)dpy, (unsigned long)draw, (void *)ctx);
        return NULL;
    }

    GLXFBConfig fbconfig = avp_glx_find_fbconfig(dpy, ctx);
    uint32_t    visualid = 0;

    if (fbconfig) {
        XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbconfig);
        if (vi) {
            visualid = (uint32_t)vi->visualid;
            XFree(vi);
        }
    } else {
        /* Warn rather than bail: the runtimes in practice bind from the drawable
         * and context alone, so this is worth attempting. If a stricter one does
         * validate them, xrCreateSession says so plainly enough. */
        SDL_Log("XR: could not resolve the GLX FBConfig — continuing without it");
    }

    SDL_zero(avp_glx_binding);
    avp_glx_binding.type        = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR;
    avp_glx_binding.xDisplay    = dpy;
    avp_glx_binding.visualid    = visualid;
    avp_glx_binding.glxFBConfig = fbconfig;
    avp_glx_binding.glxDrawable = draw;
    avp_glx_binding.glxContext  = ctx;

    SDL_Log("XR: GLX display=%p drawable=%lu context=%p fbconfig=%p visualid=0x%x",
            (void *)dpy, (unsigned long)draw, (void *)ctx, (void *)fbconfig,
            (unsigned int)visualid);

    return &avp_glx_binding;
}
