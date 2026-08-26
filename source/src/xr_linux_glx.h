#ifndef AVP_XR_LINUX_GLX_H
#define AVP_XR_LINUX_GLX_H

/* Linux PCVR only (AVP_PCVR_XLIB). See xr_linux_glx.c. */

#ifdef __cplusplus
extern "C" {
#endif

/* Builds an XrGraphicsBindingOpenGLXlibKHR describing the GLX context that is
 * current on the calling thread, and hands it back as the opaque pointer
 * XrSessionCreateInfo::next wants — so main.c never has to see an Xlib type.
 * Returns NULL, having logged why, when there is no current GLX context.
 *
 * The storage is static, so the pointer stays valid for the xrCreateSession
 * call that consumes it. */
const void *AvpXrGlxBinding(void);

#ifdef __cplusplus
}
#endif

#endif /* AVP_XR_LINUX_GLX_H */
