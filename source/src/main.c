#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL3/SDL.h>

#ifdef __ANDROID__
#include <SDL3/SDL_main.h>
	#include <sys/system_properties.h>
    #include <jni.h>
    #include <android/native_window_jni.h>
    #include <dlfcn.h>
	#include <khronos/GLES3/gl3.h>
    #include <khronos/EGL/egl.h>
    /* GLES-backed OpenXR — no Vulkan needed */
    #define XR_USE_PLATFORM_ANDROID
    #define XR_USE_GRAPHICS_API_OPENGL_ES
    #include <khronos/openxr/openxr.h>
    #include <khronos/openxr/openxr_platform.h>
	#include <SDL3/SDL_openxr.h>
#elif defined(AVP_PCVR_XLIB)
    /* Linux PCVR (SteamVR, Monado, or any desktop OpenXR runtime), desktop-GL-
     * backed. The loader (libopenxr_loader.so) finds the active runtime through
     * the openxr/1/active_runtime.json manifests under /etc/xdg and ~/.config.
     *
     * NOTE the deliberately missing XR_USE_PLATFORM_XLIB. The Linux binding
     * struct is XrGraphicsBindingOpenGLXlibKHR, which needs Display/GLXFBConfig
     * and therefore <X11/Xlib.h> — and Xlib defines Bool, Status, None, Window,
     * Screen and Cursor, all of which collide with identifiers in the game
     * headers below. Everything X-shaped lives in xr_linux_glx.c instead, and
     * this file sees the binding only as an opaque next-chain pointer. Without
     * a platform macro the header still gives us XrSwapchainImageOpenGLKHR,
     * XrGraphicsRequirementsOpenGLKHR and the requirements entry point, which is
     * all the shared code touches. */
    #include <dlfcn.h>
    #define XR_USE_GRAPHICS_API_OPENGL
    #include <khronos/openxr/openxr.h>
    #include <khronos/openxr/openxr_platform.h>
    #include "xr_linux_glx.h"
#elif defined(AVP_PCVR)
    /* Windows PCVR (SteamVR or any desktop OpenXR runtime), desktop-GL-backed.
     * The loader (openxr_loader.dll) finds the active runtime via the registry.
     * windows.h supplies HDC/HGLRC for XrGraphicsBindingOpenGLWin32KHR;
     * wglGetCurrentDC/-Context come from opengl32 (already linked). Included
     * WITHOUT WIN32_LEAN_AND_MEAN: this file needs JOYINFOEX (mmsystem) and
     * openxr_platform.h's MSFT extension structs need IUnknown (COM). */
    #include <windows.h>
    #include <unknwn.h>
    #define XR_USE_PLATFORM_WIN32
    #define XR_USE_GRAPHICS_API_OPENGL
    #include <khronos/openxr/openxr.h>
    #include <khronos/openxr/openxr_platform.h>
#endif

#include "oglfunc.h"

#if !defined(_MSC_VER)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
#endif

#include "fixer.h"

#include "3dc.h"
#include "platform.h"
#include "inline.h"
#include "gamedef.h"
#include "gameplat.h"
#include "ffstdio.h"
#include "vision.h"
#include "comp_shp.h"
#include "avp_envinfo.h"
#include "stratdef.h"
#include "bh_types.h"
#include "avp_userprofile.h"
#include "pldnet.h"
#include "cdtrackselection.h"
#include "gammacontrol.h"
#include "opengl.h"
#include "avp_menus.h"
#include "avp_mp_config.h"
#include "npcsetup.h"
#include "cdplayer.h"
#include "hud.h"
#include "player.h"
#include "mempool.h"
#include "avpview.h"
#include "consbind.hpp"
#include "progress_bar.h"
#include "scrshot.hpp"
#include "version.h"
#include "fmv.h"

#if defined(__APPLE__)
#include <strings.h>
	#define secure_zero(p, n)  secure_avpzero((p),(n))
#elif defined(__LINUX__)
#include <string.h>
	#define secure_zero(p, n)  secure_avpzero((p),(n))
#else
static inline void secure_avpzero(void* p, size_t n) {
    volatile unsigned char* vp = (volatile unsigned char*)p;
    while (n--) *vp++ = 0;
}
#endif

#if defined(__IPHONEOS__) || defined(__ANDROID__)
#define FIXED_WINDOW_SIZE 1
#endif

#if defined(__IPHONEOS__) || defined(__ANDROID__)
#define USE_OPENGL_ES 1
#endif

void RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(char Ch);
void RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(int wParam);

static bool SDLCALL SDLEventFilter(void* userData, SDL_Event* event);

char LevelName[] = {"predbit6\0QuiteALongNameActually"}; /* the real way to load levels */

int DebouncedGotAnyKey;
unsigned char DebouncedKeyboardInput[MAX_NUMBER_OF_INPUT_KEYS];
int GotJoystick;
int GotMouse;
int JoystickEnabled;
int MouseVelX;
int MouseVelY;

extern int ScanDrawMode;
extern SCREENDESCRIPTORBLOCK ScreenDescriptorBlock;
extern unsigned char KeyboardInput[MAX_NUMBER_OF_INPUT_KEYS];
extern unsigned char GotAnyKey;
extern int NormalFrameTime;
/* True while the user-profile select menu is showing — lets us remap the B
 * button to "delete profile" there (see menu navigation block below). */
extern int VR_OnUserProfileSelectMenu(void);

SDL_Window *window;
SDL_GLContext context;
SDL_Surface *surface;

SDL_Joystick *joy;
#ifdef __ANDROID__
static SDL_Gamepad *gamepad = NULL; /* Quest Touch controllers via gamepad API */
#endif
JOYINFOEX JoystickData;
JOYCAPS JoystickCaps;

// Window configuration and state
static int WindowWidth;
static int WindowHeight;
static int ViewportWidth;
static int ViewportHeight;

enum RENDERING_MODE {
    RENDERING_MODE_SOFTWARE,
    RENDERING_MODE_OPENGL
};

enum RENDERING_MODE RenderingMode;

#if defined(FIXED_WINDOW_SIZE)
static int WantFullscreen = 1;
static int WantFullscreenToggle = 0;
static int WantResolutionChange = 0;
static int WantMouseGrab = 1;
#else
static int WantFullscreen = 1;
static int WantFullscreenToggle = 1;
static int WantResolutionChange = 1;
static int WantMouseGrab = 1;
#endif

// Additional configuration
int WantSound = 1;
static int WantCDRom = 1;
static int WantJoystick = 0;
/* Run a VR-capable build on the flat desktop path (-noxr / --flat / AVP_NO_XR).
   Needed because the "no headset, fall back to flat" path only covers OpenXR
   calls that FAIL: xrCreateSession is allowed to block while the runtime brings
   up a session, and desktop runtimes take that latitude freely. Measured on the
   Oculus runtime with the service already warm but no headset presenting: 40.5
   seconds before it returned success, and it has been seen not to return at all
   — a black screen before the main loop is ever reached.
   There is no timeout in the API, and the call cannot be moved to a worker
   thread because the GL graphics binding requires the context to be current on
   the calling thread. So the only reliable answer is to not make the call. */
static int WantXR = 1;

static GLuint FullscreenTexture;
static GLsizei FullscreenTextureWidth;
static GLsizei FullscreenTextureHeight;

/* originally was "/usr/lib/libGL.so.1:/usr/lib/tls/libGL.so.1:/usr/X11R6/lib/libGL.so" */
static const char * opengl_library = NULL;

static const char * gamedatapath = NULL;

/* ** */

#ifndef AVP_XR
/* -----------------------------------------------------------------------
 * Desktop (non-VR) definitions for the VR / upscaling config + query
 * symbols. On the VR builds (Quest and PCVR — AVP_XR) these live inside the
 * OpenXR block below; on the flat desktop build OpenXR isn't compiled, so the
 * frontend menus, user profile, HUD
 * and renderer (which reference them unconditionally) would fail to link.
 *   - The VR comfort/turn/refresh options are inert on desktop (no headset).
 *   - MSAA is a real desktop setting, and now drives desktop and PCVR as well
 *     as Quest (it wrongly lived in the Android block before).
 * --------------------------------------------------------------------- */
int VRRefreshRateIndex  = 0;
int VRRefreshRateHz     = 0;   /* chosen rate in Hz; 0 = unset. Saved in the profile. */
int VRTurnMode          = 0;
int VRSnapAngleIndex    = 1;
int VRSmoothTurnSpeed   = 5;
int VRSmoothDeadzone    = 4;
int VRVignetteOn        = 1;
int VRVignetteStrength  = 5;
float vr_vignette_strength = 0.0f;
int HUDInsetLevel = 0; /* "Adjust HUD elements": 0=default,1,2 pull HUD toward centre (inert on desktop) */
int ManualReloadEnabled = 0; /* "Manual Reload": 0=off (default), 1=on. Gates the VR knock + desktop R key. */

int MSAASampleIndex = 1;

/* AV Options "Desktop Mirror" (PCVR): 0=every frame (default), 1=every 2nd,
 * 2=every 3rd, 3=off. Defined on every target because the user profile externs
 * it unconditionally; only VR_MirrorEyeToWindow reads it. */
int DesktopMirrorIndex = 0;
int MSAA_SampleCount(void)
{
    switch (MSAASampleIndex) { case 1: return 2; case 2: return 4; default: return 0; }
}

int   VR_IsIn3DMode(void)           { return 0; }
int   VR_SessionActive(void)        { return 0; }
int   VR_HeadsetActive(void)        { return 0; }
int   VR_IsBatterySaverActive(void) { return 0; }
/* 0 = "no headset refresh target", which is the truth on a non-XR build. Both
   callers (the menu and in-game FPS counters) treat >0 as "append /<n> Hz", so
   this is what stops a flat build claiming "/60 Hz" on, say, a 144 Hz monitor.
   It used to return 60.0f. Matches the real implementation further down, which
   already returns 0 when there is no XR frame state — a PCVR exe running flat. */
float VR_GetTargetHz(void)          { return 0.0f; }
int    VR_GetRefreshRateCount(void)          { return 0; }
char **VR_GetRefreshRateLabels(void)         { return 0; }
float  VR_GetRefreshRateByIndex(int i)       { (void)i; return 0.0f; }
int    VR_GetRefreshRateIndexForHz(float hz) { (void)hz; return 0; }
#endif /* !AVP_XR */

#ifdef AVP_XR
/* ========================================================================
 * OpenXR Setup Begin
 * ======================================================================== */

#define CHECK_CREATE(var, thing) { if (!(var)) { SDL_Log("Failed to create %s: %s", thing, SDL_GetError()); return false; } }
#define XR_CHECK(result, msg) do { if (XR_FAILED(result)) { SDL_Log("OpenXR Error: %s (result=%d)", msg, (int)(result)); return false; } } while(0)
#define XR_CHECK_QUIT(result, msg) do { if (XR_FAILED(result)) { SDL_Log("OpenXR Error: %s (result=%d)", msg, (int)(result)); quit(2); return; } } while(0)

/* Graphics-API-specific swapchain image struct + type tag. The GLES and GL
 * variants have an identical layout ({type, next, uint32 image}); only the
 * structure-type enum differs, so one alias keeps the rest of the code shared. */
#ifdef __ANDROID__
typedef XrSwapchainImageOpenGLESKHR AvpXrSwapchainImage;
#define AVP_XR_TYPE_SWAPCHAIN_IMAGE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR
#else /* AVP_PCVR */
typedef XrSwapchainImageOpenGLKHR   AvpXrSwapchainImage;
#define AVP_XR_TYPE_SWAPCHAIN_IMAGE XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR
#endif

/* ========================================================================
 * Math Types and Functions
 * ======================================================================== */

typedef struct { float x, y, z; } Vec3;
typedef struct { float m[16]; } Mat4;

static Mat4 Mat4_Multiply(Mat4 a, Mat4 b)
{
    Mat4 result = {{0}};
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 4; k++) {
                result.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
            }
        }
    }
    return result;
}

static Mat4 Mat4_Translation(float x, float y, float z)
{
    return (Mat4){{ 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }};
}

static Mat4 Mat4_Scale(float s)
{
    return (Mat4){{ s,0,0,0, 0,s,0,0, 0,0,s,0, 0,0,0,1 }};
}

static Mat4 Mat4_RotationY(float rad)
{
    float c = SDL_cosf(rad), s = SDL_sinf(rad);
    return (Mat4){{ c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1 }};
}

static Mat4 Mat4_RotationX(float rad)
{
    float c = SDL_cosf(rad), s = SDL_sinf(rad);
    return (Mat4){{ 1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1 }};
}

/* Convert XrPosef to view matrix (inverted transform) */
static Mat4 Mat4_FromXrPose(XrPosef pose)
{
    float x = pose.orientation.x, y = pose.orientation.y;
    float z = pose.orientation.z, w = pose.orientation.w;
    
    /* Quaternion to rotation matrix columns */
    Vec3 right = { 1-2*(y*y+z*z), 2*(x*y+w*z), 2*(x*z-w*y) };
    Vec3 up = { 2*(x*y-w*z), 1-2*(x*x+z*z), 2*(y*z+w*x) };
    Vec3 fwd = { 2*(x*z+w*y), 2*(y*z-w*x), 1-2*(x*x+y*y) };
    Vec3 pos = { pose.position.x, pose.position.y, pose.position.z };
    
    /* Inverted transform for view matrix */
    float dr = -(right.x*pos.x + right.y*pos.y + right.z*pos.z);
    float du = -(up.x*pos.x + up.y*pos.y + up.z*pos.z);
    float df = -(fwd.x*pos.x + fwd.y*pos.y + fwd.z*pos.z);
    
    return (Mat4){{ right.x,up.x,fwd.x,0, right.y,up.y,fwd.y,0, right.z,up.z,fwd.z,0, dr,du,df,1 }};
}

/* Create asymmetric projection matrix from XR FOV */
static Mat4 Mat4_Projection(XrFovf fov, float nearZ, float farZ)
{
    float tL = SDL_tanf(fov.angleLeft), tR = SDL_tanf(fov.angleRight);
    float tU = SDL_tanf(fov.angleUp), tD = SDL_tanf(fov.angleDown);
    float w = tR - tL, h = tU - tD;
    
    return (Mat4){{
                          2/w, 0, 0, 0,
                          0, 2/h, 0, 0,
                          (tR+tL)/w, (tU+tD)/h, -farZ/(farZ-nearZ), -1,
                          0, 0, -(farZ*nearZ)/(farZ-nearZ), 0
                  }};
}

/* ========================================================================
 * Vertex Data
 * ======================================================================== */

typedef struct {
    float x, y, z;
    float u, v;
} PositionUVVertex;

/* Quad dimensions: 2 m ahead, ~73° horizontal fill (~80% of Quest FOV).
 * To change apparent size without changing distance: scale QUAD_HALF_W/H together.
 * To move the quad further while keeping the same angular size: scale all three
 * proportionally (e.g. multiply everything by 1.5 for 3 m / same fill). */
#define QUAD_HALF_W  2.00f   /* half-width  in metres */
#define QUAD_HALF_H  1.50f  /* half-height in metres (4:3 aspect) */
#define QUAD_DEPTH  -4.00f   /* Z offset from local-space origin */


/* CPU staging: RGB565 pixels from surface, converted to RGBA8 for upload */
#define MENU_W 640
#define MENU_H 480
static Uint8 menu_rgba[MENU_W * MENU_H * 4];  /* RGBA8 conversion buffer */

/* ========================================================================
 * OpenXR Function Pointers (loaded dynamically)
 * ======================================================================== */

static PFN_xrGetInstanceProcAddr pfn_xrGetInstanceProcAddr = NULL;
static PFN_xrEnumerateViewConfigurationViews pfn_xrEnumerateViewConfigurationViews = NULL;
static PFN_xrEnumerateSwapchainImages pfn_xrEnumerateSwapchainImages = NULL;
static PFN_xrCreateReferenceSpace pfn_xrCreateReferenceSpace = NULL;
static PFN_xrDestroySpace pfn_xrDestroySpace = NULL;
static PFN_xrDestroySession pfn_xrDestroySession = NULL;
static PFN_xrDestroyInstance pfn_xrDestroyInstance = NULL;
static PFN_xrPollEvent pfn_xrPollEvent = NULL;
static PFN_xrBeginSession pfn_xrBeginSession = NULL;
static PFN_xrEndSession pfn_xrEndSession = NULL;
static PFN_xrRequestExitSession pfn_xrRequestExitSession = NULL;
static PFN_xrWaitFrame pfn_xrWaitFrame = NULL;
static PFN_xrBeginFrame pfn_xrBeginFrame = NULL;
static PFN_xrEndFrame pfn_xrEndFrame = NULL;
static PFN_xrLocateViews pfn_xrLocateViews = NULL;
static PFN_xrAcquireSwapchainImage pfn_xrAcquireSwapchainImage = NULL;
static PFN_xrWaitSwapchainImage pfn_xrWaitSwapchainImage = NULL;
static PFN_xrReleaseSwapchainImage pfn_xrReleaseSwapchainImage = NULL;
static PFN_xrGetSystem pfn_xrGetSystem = NULL;
static PFN_xrStringToPath pfn_xrStringToPath = NULL;
static PFN_xrCreateActionSet pfn_xrCreateActionSet = NULL;
static PFN_xrCreateAction pfn_xrCreateAction = NULL;
static PFN_xrSuggestInteractionProfileBindings pfn_xrSuggestInteractionProfileBindings = NULL;
static PFN_xrAttachSessionActionSets pfn_xrAttachSessionActionSets = NULL;
static PFN_xrSyncActions pfn_xrSyncActions = NULL;
static PFN_xrGetActionStateVector2f pfn_xrGetActionStateVector2f = NULL;
static PFN_xrGetActionStateBoolean pfn_xrGetActionStateBoolean = NULL;
static PFN_xrCreateActionSpace pfn_xrCreateActionSpace = NULL;
static PFN_xrLocateSpace pfn_xrLocateSpace = NULL;
static PFN_xrApplyHapticFeedback pfn_xrApplyHapticFeedback = NULL;
/* GLES path — instance/session creation, swapchain management */
static PFN_xrCreateInstance pfn_xrCreateInstance = NULL;
static PFN_xrCreateSession  pfn_xrCreateSession  = NULL;
static PFN_xrCreateSwapchain pfn_xrCreateSwapchain = NULL;
static PFN_xrDestroySwapchain pfn_xrDestroySwapchain = NULL;
static PFN_xrEnumerateSwapchainFormats pfn_xrEnumerateSwapchainFormats = NULL;
#ifdef __ANDROID__
typedef XrResult (XRAPI_PTR *PFN_xrGetOpenGLESGraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLESKHR *graphicsRequirements);
static PFN_xrGetOpenGLESGraphicsRequirementsKHR pfn_xrGetOpenGLESGraphicsRequirementsKHR = NULL;
#else /* AVP_PCVR: desktop-GL requirements call (mandatory before xrCreateSession) */
static PFN_xrGetOpenGLGraphicsRequirementsKHR pfn_xrGetOpenGLGraphicsRequirementsKHR = NULL;
#endif
static PFN_xrRequestDisplayRefreshRateFB pfn_xrRequestDisplayRefreshRateFB = NULL;
static PFN_xrGetDisplayRefreshRateFB pfn_xrGetDisplayRefreshRateFB = NULL;
static PFN_xrEnumerateDisplayRefreshRatesFB pfn_xrEnumerateDisplayRefreshRatesFB = NULL;

/* Set at instance creation: whether XR_FB_display_refresh_rate was actually
 * enabled (Quest yes; SteamVR exposes no such extension, so the refresh-rate
 * option is inert there and its pfn_ pointers stay NULL). */
static bool xr_has_refresh_rate_ext = false;
/* XR_BD_controller_interaction: defines Pico's interaction profiles. Enumerated
 * on Android only; stays false everywhere else, where nothing reads it. */
static bool xr_has_bd_controller_ext = false;

/* ========================================================================
 * Global XR State
 * ======================================================================== */

/* OpenXR state */
static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system_id = XR_NULL_SYSTEM_ID;
static XrSession xr_session = XR_NULL_HANDLE;

/* --- Headset refresh rates, enumerated from the runtime --------------------
   XR_FB_display_refresh_rate can report exactly which rates the headset
   supports, so the AV-options row does not hardcode a list that goes stale as
   Meta ships new ones (Quest 3 gaining 144/240 Hz).

   Note the caveat recorded on VR_IsBatterySaverActive: this call reports every
   HARDWARE-supported rate regardless of a Battery Saver cap, which is what makes
   it useless as a cap detector — but it is exactly right for "what can this
   headset actually do", which is what the menu needs. */
#define VR_MAX_REFRESH_RATES 16
static float vr_refresh_rates[VR_MAX_REFRESH_RATES];
static char  vr_refresh_labels[VR_MAX_REFRESH_RATES][12];
static char *vr_refresh_label_ptrs[VR_MAX_REFRESH_RATES];
static int   vr_refresh_rate_count = 0;

int    VR_GetRefreshRateCount(void)  { return vr_refresh_rate_count; }
char **VR_GetRefreshRateLabels(void) { return vr_refresh_label_ptrs; }

float VR_GetRefreshRateByIndex(int i)
{
    if (i < 0 || i >= vr_refresh_rate_count) return 0.0f;
    return vr_refresh_rates[i];
}

/* Nearest enumerated rate to hz, as an index. Used to turn the rate stored in
   the profile back into a slider position — the profile stores the RATE, not an
   index, precisely so a profile carried between headsets with different lists
   still selects the rate the player asked for rather than whatever happens to
   sit at that position. 0 (unset) picks 72 Hz if offered, else the lowest. */
int VR_GetRefreshRateIndexForHz(float hz)
{
    int best = 0, i;
    float bestDelta;

    if (vr_refresh_rate_count <= 0) return 0;

    if (hz <= 0.0f) {
        for (i = 0; i < vr_refresh_rate_count; i++)
            if (vr_refresh_rates[i] > 71.0f && vr_refresh_rates[i] < 73.0f) return i;
        return 0;   /* list is sorted ascending, so [0] is the lowest */
    }

    bestDelta = -1.0f;
    for (i = 0; i < vr_refresh_rate_count; i++) {
        float d = vr_refresh_rates[i] - hz;
        if (d < 0.0f) d = -d;
        if (bestDelta < 0.0f || d < bestDelta) { bestDelta = d; best = i; }
    }
    return best;
}

/* Ask the runtime what it supports, sort ascending and build the menu labels.
   Called once the session is running (the call needs a live XrSession). */
static void vr_enumerate_refresh_rates(void)
{
    uint32_t count = 0, got = 0;
    int i, j;

    if (vr_refresh_rate_count > 0) return;               /* already done */
    if (!pfn_xrEnumerateDisplayRefreshRatesFB || !xr_session) return;

    if (XR_FAILED(pfn_xrEnumerateDisplayRefreshRatesFB(xr_session, 0, &count, NULL))
        || count == 0)
        return;

    if (count > VR_MAX_REFRESH_RATES) count = VR_MAX_REFRESH_RATES;
    if (XR_FAILED(pfn_xrEnumerateDisplayRefreshRatesFB(xr_session, count, &got,
                                                       vr_refresh_rates))
        || got == 0)
        return;
    if (got > VR_MAX_REFRESH_RATES) got = VR_MAX_REFRESH_RATES;

    /* The runtime is not required to return these in order, and the menu reads
       far better ascending. Insertion sort — the list is a handful of entries. */
    for (i = 1; i < (int)got; i++) {
        float v = vr_refresh_rates[i];
        for (j = i - 1; j >= 0 && vr_refresh_rates[j] > v; j--)
            vr_refresh_rates[j + 1] = vr_refresh_rates[j];
        vr_refresh_rates[j + 1] = v;
    }

    for (i = 0; i < (int)got; i++) {
        SDL_snprintf(vr_refresh_labels[i], sizeof(vr_refresh_labels[i]),
                     "%.0f Hz", vr_refresh_rates[i]);
        vr_refresh_label_ptrs[i] = vr_refresh_labels[i];
    }
    vr_refresh_rate_count = (int)got;

    {
        char list[128]; int n = 0;
        for (i = 0; i < vr_refresh_rate_count && n < (int)sizeof(list) - 12; i++)
            n += SDL_snprintf(list + n, sizeof(list) - n, "%s%.0f",
                              i ? ", " : "", vr_refresh_rates[i]);
        SDL_Log("XR: headset supports %d refresh rate(s): %s Hz",
                vr_refresh_rate_count, list);
    }
}
static XrSpace xr_local_space = XR_NULL_HANDLE;

/* Input action state */
static XrActionSet xr_input_action_set = XR_NULL_HANDLE;
static XrAction xr_left_stick_action = XR_NULL_HANDLE;
static XrAction xr_right_stick_action = XR_NULL_HANDLE;
static XrAction xr_x_button_action = XR_NULL_HANDLE;  /* left controller X — menu select */
static XrAction xr_y_button_action = XR_NULL_HANDLE;  /* left controller Y — menu back */
static XrAction xr_menu_button_action = XR_NULL_HANDLE; /* left controller menu — ESC */
static XrAction xr_right_trigger_action = XR_NULL_HANDLE; /* right trigger — fire primary */
static XrAction xr_right_squeeze_action = XR_NULL_HANDLE; /* right grip squeeze — fire secondary */
static XrAction xr_left_squeeze_action  = XR_NULL_HANDLE; /* left grip squeeze — Predator recall disc */
static XrAction xr_a_button_action           = XR_NULL_HANDLE; /* right controller A — operate */
static XrAction xr_left_thumbstick_click_action = XR_NULL_HANDLE; /* left stick click — crouch */
static XrAction xr_b_button_action                    = XR_NULL_HANDLE; /* right controller B — jump */
static XrAction xr_right_thumbstick_click_action       = XR_NULL_HANDLE; /* right stick click — next weapon */
static XrAction xr_left_trigger_action                 = XR_NULL_HANDLE; /* left trigger — throw flare */
static XrAction xr_left_grip_action  = XR_NULL_HANDLE;
static XrAction xr_right_grip_action = XR_NULL_HANDLE;
static XrAction xr_right_haptic_action = XR_NULL_HANDLE; /* right controller vibration output */
static XrAction xr_left_haptic_action  = XR_NULL_HANDLE; /* left controller vibration output */
void XR_Haptic_Left(float amplitude, float duration_ms);  /* defined below; used by the input loop */
static XrSpace  xr_left_grip_space   = XR_NULL_HANDLE;
static XrSpace  xr_right_grip_space  = XR_NULL_HANDLE;

/* Grip poses updated each frame — read by avpview.c to drive hand/weapon position. */
XrPosef xr_grip_pose_left  = { {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f} };
XrPosef xr_grip_pose_right = { {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f} };
int xr_grip_left_valid  = 0;
int xr_grip_right_valid = 0;
int xr_trigger_right_pressed = 0;       /* 1 while right trigger is held */
int xr_grip_right_squeeze_pressed = 0; /* 1 while right grip is squeezed */
int xr_a_button_pressed                = 0; /* 1 while right A button is held */
/* 1 on the right A button's press edge, in BOTH 2D and 3D mode. Drives the
   death-screen restart on PCVR, where A is the only button that restarts. */
int xr_a_button_restart_edge           = 0;
int xr_left_thumbstick_click_pressed   = 0; /* 1 while left stick is clicked */
int xr_b_button_pressed                     = 0; /* 1 while right B button is held */
int xr_right_thumbstick_click_pressed        = 0; /* 1 on right stick up edge (next weapon) */
int xr_right_thumbstick_down_pressed         = 0; /* 1 on right stick down edge (previous weapon) */
int xr_y_button_gameplay_pressed             = 0; /* 1 while Y held in gameplay (vision toggle) */
int xr_y_button_gameplay_edge                = 0; /* 1 on Y press edge */
int xr_y_button_gameplay_tap                 = 0; /* 1 on Y release if it was a short tap (Predator cycle vision mode) */
int xr_y_button_gameplay_long_edge           = 0; /* 1 once when Y is held past the long-press threshold (Predator zoom) */
int xr_menu_button_msg_history_edge          = 0; /* 1 once when left menu button is held past the long-press threshold (message history) */
int xr_x_button_gameplay_pressed             = 0; /* 1 on X press edge in gameplay (taunt) */
int xr_left_trigger_pressed                  = 0; /* 1 on left trigger press edge (throw flare) */
int xr_left_trigger_gameplay_pressed         = 0; /* 1 while the physical left trigger is held (Marine jetpack) */
int xr_left_trigger_gameplay_edge            = 0; /* 1 on physical left trigger press edge (Predator grappling hook) */
int xr_left_squeeze_gameplay_pressed         = 0; /* 1 while the left grip squeeze is held (Predator recall disc) */
static float xr_left_stick_x = 0.0f;
static float xr_left_stick_y = 0.0f;
#ifdef AVP_PCVR
/* 1 while X is still held after its long-press opened the pause menu, so that
 * carried-over hold cannot also confirm a menu entry. Cleared on release. */
static int xr_x_pause_latch = 0;
#endif

/* HMD horizontal heading for locomotion (ONE_FIXED = 65536 scale).
 * Updated each frame from xr_views[0] pose, used by pmove.c to rotate
 * movement velocity in the direction the player is looking. */
int xr_hmd_move_sin = 0;
int xr_hmd_move_cos = 65536; /* ONE_FIXED — default facing +Z */
/* Accumulated snap turn offset in game angle units (0-4095, 4096 = full circle). */
int xr_snap_yaw = 0;
bool xr_enabled = false;   // so you can flip it off quickly if needed
bool xr_session_running = false;

/* ---- VR controller / turning options (Controller Configuration menu) ---- */
/* Turning mode: 0 = snap turn (default), 1 = smooth turn. */
int VRTurnMode = 0;
/* Snap turn angle: index 0=30, 1=45, 2=60, 3=90 degrees. Default 45 (index 1). */
int VRSnapAngleIndex = 1;
/* Smooth turn speed slider 0..10; maps to 60..180 deg/sec. Default 5 (~120 deg/sec). */
int VRSmoothTurnSpeed = 5;
/* Smooth turn deadzone slider 0..10; maps to 0.0..0.5 stick deflection. Default 4 (0.2). */
int VRSmoothDeadzone = 4;
/* Comfort vignette while smooth-turning: on/off + strength 0..10 (tunnel closure). */
int VRVignetteOn = 1;
int VRVignetteStrength = 5;
/* "Adjust HUD elements" (Controller Config): 0=default layout, 1 and 2 pull the
 * HUD progressively toward the centre of view for narrow-FOV headsets.
 * Consumed in AvpShowViewsVR when setting vr_hud_clip_scale. */
int HUDInsetLevel = 0;
/* "Manual Reload" (Controller Config): 0=off (default), 1=on. Gates the VR
 * controller-knock gesture and the desktop R key (checked in PlayerRequestManualReload). */
int ManualReloadEnabled = 0;
/* Current smoothed vignette opacity 0..1, fades in/out as smooth-turn starts/stops.
 * Updated in the input read each frame; consumed by VR_DrawVignette() per eye. */
float vr_vignette_strength = 0.0f;

/* VR display refresh rate setting: 0=72, 1=80, 2=90, 3=120 Hz.
 * Written by the AV options menu; applied at frame begin via xrRequestDisplayRefreshRateFB. */
int VRRefreshRateIndex = 0;
int VRRefreshRateHz    = 0;   /* chosen rate in Hz; 0 = unset. Saved in the profile. */

/* Set by the one-time startup Battery Saver probe (in apply_refresh_rate_if_changed):
 * 1 if requesting 90 Hz didn't take (panel stayed ≤72), i.e. Battery Saver is on even
 * though the saved rate is 72 — the case the live-rate signal can't tell apart.
 * Read by VR_IsBatterySaverActive(). */
static int vr_bs_probe_result = 0;

/* MSAA anti-aliasing setting: 0=off, 1=2x, 2=4x. Default 2x.
 * Written by the AV options menu; read by the VR eye-FBO renderer (avpview.c). */
int MSAASampleIndex = 1;

/* AV Options "Desktop Mirror" (PCVR): 0=every frame (default), 1=every 2nd,
 * 2=every 3rd, 3=off. Defined on every target because the user profile externs
 * it unconditionally; only VR_MirrorEyeToWindow reads it. */
int DesktopMirrorIndex = 0;

/* Map the MSAA menu index to a GL sample count (0/2/4). */
int MSAA_SampleCount(void)
{
    switch (MSAASampleIndex) {
        case 1:  return 2;   /* 2x */
        case 2:  return 4;   /* 4x */
        default: return 0;   /* 0 → no MSAA */
    }
}

static bool xr_should_quit = false;
static bool xr_2d_mode = true;  /* true = show flat game on quad, false = 3D game manages XR */
static XrTime xr_predicted_display_time = 0;

/* Swapchain state — GL/GLES images as OpenXR texture IDs */
typedef struct {
    XrSwapchain                   swapchain;
    AvpXrSwapchainImage          *images;     /* array of GL(ES) texture handles */
    Uint32                         image_count;
    XrExtent2Di                    size;
} VRSwapchain;

/* Which headset family we are driving. Detected on Android from
 * Build.MANUFACTURER in init_xr_instance; there is no detection on PCVR and none
 * is wanted, so QUEST is BOTH the zero value and the explicit initialiser.
 *
 * That ordering is load-bearing, not tidiness. With PICO at 0 an uninitialised
 * global left every desktop build claiming to be a Pico, which suggested
 * /interaction_profiles/pico/neo3_controller to SteamVR: the oculus/touch
 * bindings were never sent, so head tracking kept working and every control went
 * dead — silently, because the suggest result was not checked. The Pico branches
 * below are additionally compiled out unless __ANDROID__, so PCVR cannot reach
 * them even if this were wrong again. */
enum VrHeadset { QUEST, PICO };

static enum VrHeadset vr_headset = QUEST;

VRSwapchain *vr_swapchains = NULL;
/* Dedicated swapchain for the 2D menu quad layer. Kept separate from the per-eye
 * swapchains because those are rendered with MSAA (glFramebufferTexture2DMultisample)
 * by the 3D path; reusing an MSAA-touched image for the flat menu made the Quest
 * compositor null-deref under Battery Saver when opening the in-game pause menu. */
static VRSwapchain vr_menu_swapchain = {0};
/* Dedicated swapchain for the MP score-table quad layer (floats over the 3D
 * world while dead / showing scores). Same rationale as the menu swapchain —
 * kept separate from the MSAA per-eye images. Fed from avpview.c's vr_score_tex. */
static VRSwapchain vr_score_swapchain = {0};
/* Floating MP scoreboard, produced by avpview.c (AvpShowViewsVR). */
extern int    vr_scoreboard_visible; /* 1 when the score table should float this frame */
extern GLuint vr_score_tex;          /* RGBA offscreen holding the rendered score table */
extern int    vr_score_quad_ready;   /* 1 once vr_score_tex has been drawn this frame */
/* Set by the recenter event (handle_xr_events) so the floating scoreboard re-anchors
 * in front of the new facing; consumed in render_frame. */
static int    vr_score_reanchor = 0;
/* Same, for the 2D menu/intro quad: a recenter while a menu or intro is up re-anchors
 * it in front of the new facing (otherwise it stays put and recenter looks inert). */
static int    vr_menu_reanchor = 0;
XrView *xr_views = NULL;
Uint32 view_count = 0;

/* GLES quad state for 2D menu rendering */
static GLuint quad_program  = 0;
static GLuint quad_vao      = 0;
static GLuint quad_vbo      = 0;
static GLuint quad_ibo      = 0;
static GLint  quad_u_mvp    = -1;
static GLint  quad_u_tex    = -1;
static GLuint menu_gles_tex = 0;
static GLuint menu_fbo_2d   = 0;

#ifdef AVP_PCVR
/* Desktop mirror state (PCVR only) — see VR_MirrorEyeToWindow below. */
static int    vr_mirror_pending = 0;   /* an eye was mirrored; the window needs a swap */
#endif

/* Comfort vignette (peripheral tunnel) drawn over each eye while smooth-turning. */
static GLuint vignette_program = 0;
static GLuint vignette_vao     = 0;
static GLint  vignette_u_fade  = -1;
static GLint  vignette_u_inner = -1;
static GLint  vignette_u_outer = -1;

/* Swapchain color-space management.
 * Quest's compositor treats GL_RGBA8 swapchains as LINEAR, then applies sRGB
 * gamma for display — this double-encodes our already-gamma content (too bright).
 * Fix: use GL_SRGB8_ALPHA8 swapchains so the compositor knows the data is sRGB.
 * But GLES automatically converts linear→sRGB on writes to sRGB FBOs, which
 * would also double-encode. GL_EXT_sRGB_write_control lets us disable that
 * conversion so our already-encoded values pass through unchanged.
 * If the extension is unavailable, fall back to GL_RGBA8 (imperfect but functional). */
static bool  vr_srgb_swapchain       = false;
static bool  has_srgb_write_control  = false;
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9

/* ========================================================================
 * Cleanup and Quit
 * ======================================================================== */

/* Destroy all OpenXR + GLES resources. Safe to call once; every handle is
 * nulled so a second call (or a later SDL_Quit) is a no-op. Does NOT touch
 * SDL or terminate the process — see quit() and the normal exit path. */
static void destroy_xr_resources(void)
{
    /* Wait for GLES to finish */
    if (context) glFinish();

    /* GLES quad resources */
    if (quad_program) { glDeleteProgram(quad_program); quad_program = 0; }
    if (quad_vao)     { glDeleteVertexArrays(1, &quad_vao); quad_vao = 0; }
    if (quad_vbo)     { glDeleteBuffers(1, &quad_vbo); quad_vbo = 0; }
    if (quad_ibo)     { glDeleteBuffers(1, &quad_ibo); quad_ibo = 0; }
    if (menu_gles_tex){ glDeleteTextures(1, &menu_gles_tex); menu_gles_tex = 0; }
    if (menu_fbo_2d)  { glDeleteFramebuffers(1, &menu_fbo_2d); menu_fbo_2d = 0; }
    if (vignette_program) { glDeleteProgram(vignette_program); vignette_program = 0; }
    if (vignette_vao)     { glDeleteVertexArrays(1, &vignette_vao); vignette_vao = 0; }

    /* XR swapchains */
    if (vr_swapchains) {
        for (Uint32 i = 0; i < view_count; i++) {
            SDL_free(vr_swapchains[i].images);
            if (vr_swapchains[i].swapchain && pfn_xrDestroySwapchain)
                pfn_xrDestroySwapchain(vr_swapchains[i].swapchain);
        }
        SDL_free(vr_swapchains);
        vr_swapchains = NULL;
    }

    /* Dedicated 2D-menu swapchain */
    if (vr_menu_swapchain.swapchain) {
        SDL_free(vr_menu_swapchain.images);
        if (pfn_xrDestroySwapchain) pfn_xrDestroySwapchain(vr_menu_swapchain.swapchain);
        vr_menu_swapchain.swapchain   = XR_NULL_HANDLE;
        vr_menu_swapchain.images      = NULL;
        vr_menu_swapchain.image_count = 0;
    }

    /* Dedicated MP scoreboard swapchain */
    if (vr_score_swapchain.swapchain) {
        SDL_free(vr_score_swapchain.images);
        if (pfn_xrDestroySwapchain) pfn_xrDestroySwapchain(vr_score_swapchain.swapchain);
        vr_score_swapchain.swapchain   = XR_NULL_HANDLE;
        vr_score_swapchain.images      = NULL;
        vr_score_swapchain.image_count = 0;
    }

    if (xr_views) { SDL_free(xr_views); xr_views = NULL; }

    if (xr_local_space && pfn_xrDestroySpace) {
        pfn_xrDestroySpace(xr_local_space);
        xr_local_space = XR_NULL_HANDLE;
    }
    if (xr_session && pfn_xrDestroySession) {
        pfn_xrDestroySession(xr_session);
        xr_session = XR_NULL_HANDLE;
    }
    if (xr_instance && pfn_xrDestroyInstance) {
        pfn_xrDestroyInstance(xr_instance);
        xr_instance = XR_NULL_HANDLE;
    }
}

static void quit(int rc)
{
    SDL_Log("Cleaning up...");

    destroy_xr_resources();

    SDL_Quit();
    exit(rc);
}

/* ========================================================================
 * GLES Shader and Quad Pipeline
 * ======================================================================== */

/* GLSL version line: ES 3.0 on Quest, desktop GLSL 3.30 on PCVR. The shader
 * bodies are identical (in/out, layout(location), texture()); desktop GLSL
 * 1.30+ accepts the ES "precision" statements as no-ops. */
#ifdef __ANDROID__
#define AVP_XR_GLSL_VERSION "#version 300 es\n"
#else
#define AVP_XR_GLSL_VERSION "#version 330\n"
#endif

static const char *quad_vs_src =
    AVP_XR_GLSL_VERSION
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

static const char *quad_fs_src =
    AVP_XR_GLSL_VERSION
    "precision mediump float;\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "out vec4 oColor;\n"
    "void main() {\n"
    "    oColor = texture(uTex, vUV);\n"
    "}\n";

static GLuint compile_gles_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetShaderInfoLog(s, sizeof(buf), NULL, buf);
        SDL_Log("Shader compile error: %s", buf);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool create_quad_gles_program(void)
{
    GLuint vs = compile_gles_shader(GL_VERTEX_SHADER,   quad_vs_src);
    GLuint fs = compile_gles_shader(GL_FRAGMENT_SHADER, quad_fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    quad_program = glCreateProgram();
    glAttachShader(quad_program, vs);
    glAttachShader(quad_program, fs);
    glLinkProgram(quad_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(quad_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(quad_program, sizeof(buf), NULL, buf);
        SDL_Log("Quad program link error: %s", buf);
        glDeleteProgram(quad_program); quad_program = 0;
        return false;
    }
    quad_u_mvp = glGetUniformLocation(quad_program, "uMVP");
    quad_u_tex = glGetUniformLocation(quad_program, "uTex");
    SDL_Log("GLES quad program ready (uMVP=%d uTex=%d)", quad_u_mvp, quad_u_tex);
    return true;
}

/* Dummy — kept so load_quad_shader callers below don't break during transition */

/* ========================================================================
 * Comfort Vignette
 * Draws a black peripheral tunnel over the current eye FBO while the player is
 * smooth-turning, to reduce motion sickness. Uses a VBO-less fullscreen
 * triangle (gl_VertexID) so it needs no geometry buffers.
 * ======================================================================== */

static const char *vignette_vs_src =
    AVP_XR_GLSL_VERSION
    "out vec2 vPos;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "    vPos = p * 2.0 - 1.0;\n"            /* -1..1 across the screen */
    "    gl_Position = vec4(vPos, 0.0, 1.0);\n"
    "}\n";

static const char *vignette_fs_src =
    AVP_XR_GLSL_VERSION
    "precision mediump float;\n"
    "in vec2 vPos;\n"
    "uniform float uFade;\n"   /* overall opacity 0..1 */
    "uniform float uInner;\n"  /* radius where darkening starts */
    "uniform float uOuter;\n"  /* radius of full black */
    "out vec4 oColor;\n"
    "void main() {\n"
    "    float r = length(vPos);\n"
    "    float a = smoothstep(uInner, uOuter, r) * uFade;\n"
    "    oColor = vec4(0.0, 0.0, 0.0, a);\n"
    "}\n";

static bool create_vignette_gles_program(void)
{
    GLuint vs = compile_gles_shader(GL_VERTEX_SHADER,   vignette_vs_src);
    GLuint fs = compile_gles_shader(GL_FRAGMENT_SHADER, vignette_fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return false; }
    vignette_program = glCreateProgram();
    glAttachShader(vignette_program, vs);
    glAttachShader(vignette_program, fs);
    glLinkProgram(vignette_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(vignette_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[512];
        glGetProgramInfoLog(vignette_program, sizeof(buf), NULL, buf);
        SDL_Log("Vignette program link error: %s", buf);
        glDeleteProgram(vignette_program); vignette_program = 0;
        return false;
    }
    vignette_u_fade  = glGetUniformLocation(vignette_program, "uFade");
    vignette_u_inner = glGetUniformLocation(vignette_program, "uInner");
    vignette_u_outer = glGetUniformLocation(vignette_program, "uOuter");
    glGenVertexArrays(1, &vignette_vao);
    SDL_Log("GLES vignette program ready");
    return true;
}

/* Draw the comfort vignette into the currently-bound eye FBO. Expects the eye
 * viewport to already be set. No-op unless the vignette is enabled and currently
 * faded in. Saves/restores the GL state it touches. */
void VR_DrawVignette(void)
{
    if (!VRVignetteOn || vr_vignette_strength <= 0.001f) return;
    if (!vignette_program && !create_vignette_gles_program()) return;

    /* Strength 0..10 closes the tunnel: stronger = smaller clear centre. */
    float s     = (float)VRVignetteStrength / 10.0f;     /* 0..1 */
    float inner = 1.05f - s * 0.75f;                     /* 1.05 (subtle) .. 0.30 (strong) */
    float outer = inner + 0.35f;                         /* soft edge width */

    GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean had_cull  = glIsEnabled(GL_CULL_FACE);
    GLboolean had_blend = glIsEnabled(GL_BLEND);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    glUseProgram(vignette_program);
    glUniform1f(vignette_u_fade,  vr_vignette_strength);
    glUniform1f(vignette_u_inner, inner);
    glUniform1f(vignette_u_outer, outer);

    glBindVertexArray(vignette_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

    glDepthMask(GL_TRUE);
    if (!had_blend) glDisable(GL_BLEND);
    if (had_depth)  glEnable(GL_DEPTH_TEST);
    if (had_cull)   glEnable(GL_CULL_FACE);
}

#ifdef AVP_PCVR
/* ========================================================================
 * Desktop mirror (PCVR only)
 *
 * The headset is presented by the OpenXR compositor out of xrEndFrame, and
 * nothing in the XR path ever touched the SDL window — InGameFlipBuffers and
 * FlipBuffers both returned straight after render_frame(), so the monitor
 * stayed black for the whole session. This copies the eye image the game has
 * ALREADY rendered into the window; the swap happens back in those two
 * functions. Nothing is re-rendered and there is no post-processing pass: one
 * glBlitFramebuffer, hardware linear filtering, straight to the back buffer.
 *
 * Deliberately not SteamVR's vr::VRCompositor()->GetMirrorTextureGL: that is
 * OpenVR, which this build does not link (it is a pure OpenXR client), and it
 * would be dead weight under the Oculus runtime, which is what actually serves
 * OpenXR on this machine. Copying our own eye buffer costs one blit and works
 * under every runtime.
 * ======================================================================== */

/* Should the desktop window be updated this frame? Two independent reasons not to,
 * and BOTH callers must respect it — the 3D mirror below and the 2D software
 * present — or the frontend would keep presenting at full rate with the setting
 * off, and the window would flip between fresh menus and a frozen game.
 *
 *   1. The "Desktop Mirror" AV option: every frame / every 2nd / every 3rd / off.
 *      The saving is mostly the SDL_GL_SwapWindow — a desktop present on every VR
 *      frame makes the compositor do work it otherwise would not — with the blit
 *      itself the smaller half.
 *   2. Nobody can see it: minimised, or covered by another window. Free, and it
 *      needs no setting. SDL_WINDOW_OCCLUDED is best-effort per platform — when a
 *      backend never reports it the test is simply inert, never wrong.
 *
 * The frame counter is stepped before the occlusion test so the phase does not
 * drift while the window is hidden. */
static int vr_mirror_frame   = 0;
static int vr_mirror_blanked = 0;   /* window painted black */
static int vr_mirror_hidden  = 0;   /* window taken off the desktop */

static int vr_mirror_is_off(void) { return DesktopMirrorIndex == 3; }

/* "Off" means the window LEAVES THE DESKTOP, not that it stops updating. Merely
 * not presenting leaves it holding whatever frame it last received — the game
 * frozen mid-scene, which reads as a hang; and a black fullscreen window is still
 * a window sitting on top of everything else while you are in the headset. So:
 * paint it black, then hide it.
 *
 * The blank before the hide is insurance, not decoration: it means that whenever
 * the window comes back, it cannot flash the stale frame it was holding before
 * the first mirrored frame lands. Twice, because it is double-buffered — one
 * clear+swap leaves the old frame in the other buffer.
 *
 * Safe with the window hidden: the GL context stays current and valid (the
 * window is hidden, never destroyed), and the eye pass renders to FBOs, so
 * nothing in the frame depends on the window being mapped. Nothing pauses on
 * focus loss either — SDL_EVENT_WINDOW_FOCUS_LOST is an empty case and no code
 * gates on focus. The cost is keyboard input: an unfocused window receives none,
 * so Esc-to-pause is dead while the mirror is off. The VR controller bindings
 * (X hold = pause) are unaffected, which is what you actually use in a headset.
 *
 * Called from both flip paths and cheap to call repeatedly — the flag makes it a
 * no-op until the setting changes. It deliberately does NOT touch
 * vr_mirror_frame: the skip phase is a separate decision. */
static void vr_mirror_park_window(int session_presenting)
{
    if (!vr_mirror_is_off()) {
        /* Turned back on - put the window back. */
        if (vr_mirror_hidden && window != NULL) SDL_ShowWindow(window);
        vr_mirror_hidden  = 0;
        vr_mirror_blanked = 0;
        return;
    }
    if (window == NULL) return;

    /* Blanking and hiding are SEPARATE steps on purpose, and the hide waits for
     * a live session.
     *
     * Blanking happens as soon as the setting is off, including during startup,
     * so no intro or menu content reaches the monitor. Hiding waits until the
     * headset is actually presenting, because until then the window is the only
     * evidence the app is alive: with it gone there is no taskbar entry, no
     * keyboard focus and nothing on screen, so a launch that dies before the
     * session starts - a runtime that never comes up, an xrCreateSession that
     * blocks, a headset that is not connected - is completely invisible and
     * cannot even be closed. A black window during those seconds is the cost of
     * being able to see and kill a failed launch.
     *
     * It is symmetric: if the session later stops, the window comes back rather
     * than leaving an unreachable process. xr_session_running only moves on
     * READY and STOPPING, so this cannot flap during play. */
    if (!vr_mirror_blanked) {

        GLint had_draw = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &had_draw);
        GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
        GLboolean had_srgb    = glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT);
        if (had_scissor) glDisable(GL_SCISSOR_TEST);
        if (had_srgb)    glDisable(GL_FRAMEBUFFER_SRGB_EXT);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        /* Twice: the window is double-buffered, so one clear+swap leaves the
           stale frame in the other buffer, ready to flash back on any later
           swap - including the one that follows a re-show. */
        for (int i = 0; i < 2; i++) {
            glClear(GL_COLOR_BUFFER_BIT);
            SDL_GL_SwapWindow(window);
        }

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)had_draw);
        if (had_srgb)    glEnable(GL_FRAMEBUFFER_SRGB_EXT);
        if (had_scissor) glEnable(GL_SCISSOR_TEST);

        vr_mirror_blanked = 1;
    }

    if (session_presenting) {
        if (!vr_mirror_hidden) {
            SDL_HideWindow(window);
            vr_mirror_hidden = 1;
        }
    } else if (vr_mirror_hidden) {
        SDL_ShowWindow(window);
        vr_mirror_hidden = 0;
    }
}

static int vr_mirror_wanted_this_frame(void)
{
    int period;
    switch (DesktopMirrorIndex) {
        case 1:  period = 2; break;
        case 2:  period = 3; break;
        case 3:  return 0;              /* Off */
        default: period = 1; break;     /* every frame */
    }

    int n = vr_mirror_frame++;
    if (period > 1 && (n % period) != 0) return 0;

    if (window) {
        SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        if (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED))
            return 0;
    }
    return 1;
}

/* Mirror the finished eye image into the desktop window. Called from the eye loop
 * in avpview.c with the FBO the swapchain texture is attached to — after the MSAA
 * resolve, before the texture is detached and released. band_lo/band_hi are the
 * clip-space Y extent of the VR HUD (+1 = top of the eye image); the crop is
 * framed to keep that band on screen.
 *
 * A blit bypasses the fragment pipeline, so there is no blend/depth/program state
 * to save; only the scissor test (which clips both the clear and the blit) and the
 * framebuffer bindings. GL_FRAMEBUFFER_SRGB is forced off for the copy: the eye
 * image is a GL_SRGB8_ALPHA8 swapchain holding values the game has already
 * gamma-encoded (vr_sc_acquire_wait turns the conversion off precisely so they
 * store raw), and letting the driver decode them into the plain RGBA8 back buffer
 * would mirror the game far too dark. */
void VR_MirrorEyeToWindow(GLuint src_fbo, int src_w, int src_h,
                          float band_lo, float band_hi)
{
    if (!src_fbo || src_w <= 0 || src_h <= 0 || window == NULL) return;
    if (!vr_mirror_wanted_this_frame()) return;   /* leaves vr_mirror_pending clear,
                                                     so no swap happens either */

    int win_w = 0, win_h = 0;
    SDL_GetWindowSizeInPixels(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) return;   /* minimised — nothing to mirror to */

    /* The rows the HUD occupies. GL rows count from the bottom and clip_y +1 is the
       top of the image, so row = (clip + 1) * 0.5 * src_h. */
    if (band_hi < band_lo) { float t = band_lo; band_lo = band_hi; band_hi = t; }
    if (band_lo < -1.0f) band_lo = -1.0f;
    if (band_hi >  1.0f) band_hi =  1.0f;
    int band_y0 = (int)((band_lo + 1.0f) * 0.5f * (float)src_h);
    int band_y1 = (int)((band_hi + 1.0f) * 0.5f * (float)src_h + 0.5f);
    if (band_y0 < 0)     band_y0 = 0;
    if (band_y1 > src_h) band_y1 = src_h;
    if (band_y1 <= band_y0) { band_y0 = 0; band_y1 = src_h; }   /* no usable band */
    int band_h = band_y1 - band_y0;

    /* Crop to fill, framed on the HUD rather than on the middle of the eye image.
       A headset eye buffer is roughly square (Quest is 1832x1920 per eye), so
       filling a 16:9 window means showing only about the middle 55% of its height
       — and the VR HUD spans clip_y -0.60..+0.40 (half the image, sitting low),
       so a centre-framed crop cuts the bottom row of HUD elements off. Sizing the
       crop to cover the window and then CENTRING IT ON THE HUD BAND instead shows
       all of the HUD and still fills the screen; the view just sits ~10% lower,
       which is the periphery under your feet rather than anything you aim at.

       The crop comes out of the SOURCE rectangle, so the image is never stretched.
       The headset is not touched by any of this. */
    int fill_h = (int)((double)src_w * (double)win_h / (double)win_w + 0.5);
    int rect_w, rect_h, fills;
    if (fill_h >= band_h) {
        /* Normal case: a full-width crop is already tall enough for the HUD. */
        rect_h = fill_h;
        rect_w = src_w;
        fills  = 1;
        if (rect_h > src_h) {          /* window taller than the eye image */
            rect_h = src_h;
            rect_w = (int)((double)src_h * (double)win_w / (double)win_h + 0.5);
            if (rect_w > src_w) rect_w = src_w;
        }
    } else {
        /* Window is so wide that filling it would cut the HUD. Show the whole band
           and letterbox the remainder — the HUD is worth more than the last few
           percent of screen area. */
        rect_h = band_h;
        rect_w = src_w;
        fills  = 0;
    }

    /* Centre on the band vertically (clamped inside the image), on the eye
       horizontally. */
    int rect_y = (band_y0 + band_y1) / 2 - rect_h / 2;
    if (rect_y < 0) rect_y = 0;
    if (rect_y + rect_h > src_h) rect_y = src_h - rect_h;
    int rect_x = (src_w - rect_w) / 2;

    int dst_x = 0, dst_y = 0, dst_w = win_w, dst_h = win_h;
    if (!fills) {
        dst_h = (int)((double)win_w * (double)rect_h / (double)rect_w + 0.5);
        if (dst_h > win_h) {
            dst_h = win_h;
            dst_w = (int)((double)win_h * (double)rect_w / (double)rect_h + 0.5);
        }
        dst_x = (win_w - dst_w) / 2;
        dst_y = (win_h - dst_h) / 2;
    }

    GLint had_read = 0, had_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &had_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &had_draw);
    GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean had_srgb    = glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT);
    if (had_scissor) glDisable(GL_SCISSOR_TEST);
    if (had_srgb)    glDisable(GL_FRAMEBUFFER_SRGB_EXT);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    /* Only when the blit leaves bars; the filling case covers every pixel. */
    if (!fills) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glBlitFramebuffer(rect_x, rect_y, rect_x + rect_w, rect_y + rect_h,
                      dst_x,  dst_y,  dst_x  + dst_w,  dst_y  + dst_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)had_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)had_draw);
    if (had_srgb)    glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    if (had_scissor) glEnable(GL_SCISSOR_TEST);

    vr_mirror_pending = 1;
}
#endif /* AVP_PCVR */

/* ========================================================================
 * OpenXR Function Loading
 * ======================================================================== */

/* load_xr_functions: called after xr_instance is valid */
static bool load_xr_functions(void)
{
#define XR_LOAD(fn) \
    if (XR_FAILED(pfn_xrGetInstanceProcAddr(xr_instance, #fn, (PFN_xrVoidFunction*)&pfn_##fn))) { \
        SDL_Log("Failed to load " #fn); \
        return false; \
    }

    XR_LOAD(xrEnumerateViewConfigurationViews);
    XR_LOAD(xrEnumerateSwapchainImages);
    XR_LOAD(xrCreateReferenceSpace);
    XR_LOAD(xrDestroySpace);
    XR_LOAD(xrDestroySession);
    XR_LOAD(xrDestroyInstance);
    XR_LOAD(xrPollEvent);
    XR_LOAD(xrBeginSession);
    XR_LOAD(xrEndSession);
    XR_LOAD(xrRequestExitSession);
    XR_LOAD(xrWaitFrame);
    XR_LOAD(xrBeginFrame);
    XR_LOAD(xrEndFrame);
    XR_LOAD(xrLocateViews);
    XR_LOAD(xrAcquireSwapchainImage);
    XR_LOAD(xrWaitSwapchainImage);
    XR_LOAD(xrReleaseSwapchainImage);
    XR_LOAD(xrStringToPath);
    XR_LOAD(xrCreateActionSet);
    XR_LOAD(xrCreateAction);
    XR_LOAD(xrSuggestInteractionProfileBindings);
    XR_LOAD(xrAttachSessionActionSets);
    XR_LOAD(xrSyncActions);
    XR_LOAD(xrGetActionStateVector2f);
    XR_LOAD(xrGetActionStateBoolean);
    XR_LOAD(xrCreateActionSpace);
    XR_LOAD(xrLocateSpace);
    XR_LOAD(xrApplyHapticFeedback);
    XR_LOAD(xrGetSystem);
    XR_LOAD(xrCreateSession);
    XR_LOAD(xrCreateSwapchain);
    XR_LOAD(xrDestroySwapchain);
    XR_LOAD(xrEnumerateSwapchainFormats);
    /* Extensions — not fatal if missing */
#ifdef __ANDROID__
    pfn_xrGetInstanceProcAddr(xr_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn_xrGetOpenGLESGraphicsRequirementsKHR);
#else /* AVP_PCVR */
    pfn_xrGetInstanceProcAddr(xr_instance, "xrGetOpenGLGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn_xrGetOpenGLGraphicsRequirementsKHR);
#endif
    /* Only look these up when the extension was actually enabled — a runtime
     * without XR_FB_display_refresh_rate (SteamVR) fails the lookup anyway,
     * but skipping it keeps the log clean and the intent explicit. */
    if (xr_has_refresh_rate_ext) {
        pfn_xrGetInstanceProcAddr(xr_instance, "xrRequestDisplayRefreshRateFB",
            (PFN_xrVoidFunction*)&pfn_xrRequestDisplayRefreshRateFB);
        pfn_xrGetInstanceProcAddr(xr_instance, "xrGetDisplayRefreshRateFB",
            (PFN_xrVoidFunction*)&pfn_xrGetDisplayRefreshRateFB);
        pfn_xrGetInstanceProcAddr(xr_instance, "xrEnumerateDisplayRefreshRatesFB",
            (PFN_xrVoidFunction*)&pfn_xrEnumerateDisplayRefreshRatesFB);
    }

#undef XR_LOAD

    SDL_Log("XR: all functions loaded");
    return true;
}

static bool init_xr_instance(void)
{
    /* Get xrGetInstanceProcAddr from the OpenXR loader */
#ifdef __ANDROID__
    static void *xr_loader_handle = NULL;
    if (!xr_loader_handle)
        xr_loader_handle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!xr_loader_handle) {
        /* Fall back to SDL wrapper if dlopen fails */
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
    } else {
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(xr_loader_handle, "xrGetInstanceProcAddr");
    }
#elif defined(AVP_PCVR_XLIB) /* Linux PCVR: dlopen the loader staged next to the
       * binary, falling back to a system-wide one. The bundled copy is staged
       * under its SONAME (libopenxr_loader.so.1) as well as the bare name, and
       * that is the name asked for here so the same call also finds a distro
       * package. Prefixing SDL_GetBasePath() makes the bundled copy win even
       * where the exe's rpath does not cover a dlopen. */
    static void *xr_loader_handle = NULL;
    if (!xr_loader_handle) {
        const char *base = SDL_GetBasePath();
        if (base) {
            char path[1024];
            SDL_snprintf(path, sizeof(path), "%slibopenxr_loader.so.1", base);
            xr_loader_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        }
        if (!xr_loader_handle)
            xr_loader_handle = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    }
    if (xr_loader_handle)
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)
            dlsym(xr_loader_handle, "xrGetInstanceProcAddr");
    else
        SDL_Log("XR: libopenxr_loader.so.1 not found (%s)", dlerror());
#else /* AVP_PCVR: load openxr_loader.dll (copied next to the exe by the build).
       * The Khronos loader resolves the active runtime — SteamVR when it is the
       * system OpenXR runtime — via the registry. Loaded dynamically rather than
       * import-linked so a machine with no VR runtime just falls back to flat. */
    static HMODULE xr_loader_handle = NULL;
    if (!xr_loader_handle)
        xr_loader_handle = LoadLibraryA("openxr_loader.dll");
    if (xr_loader_handle)
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)
            (void*)GetProcAddress(xr_loader_handle, "xrGetInstanceProcAddr");
    else
        SDL_Log("XR: openxr_loader.dll not found next to the exe");
#endif
    if (!pfn_xrGetInstanceProcAddr) {
        SDL_Log("XR: no xrGetInstanceProcAddr");
        return false;
    }

#ifdef __ANDROID__
    /* Android requires xrInitializeLoaderKHR before xrCreateInstance */
    {
        PFN_xrInitializeLoaderKHR pfn_xrInitializeLoaderKHR = NULL;
        pfn_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            (PFN_xrVoidFunction*)&pfn_xrInitializeLoaderKHR);
        if (!pfn_xrInitializeLoaderKHR) {
            /* Some loaders expose it via dlsym directly */
            if (xr_loader_handle)
                pfn_xrInitializeLoaderKHR = (PFN_xrInitializeLoaderKHR)
                    dlsym(xr_loader_handle, "xrInitializeLoaderKHR");
        }
        if (pfn_xrInitializeLoaderKHR) {
            JNIEnv *env2 = (JNIEnv*)SDL_GetAndroidJNIEnv();
            JavaVM *vm2 = NULL;
            (*env2)->GetJavaVM(env2, &vm2);
            XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
            loader_info.applicationVM       = vm2;
            loader_info.applicationContext  = (void*)SDL_GetAndroidActivity();
            XrResult lr = pfn_xrInitializeLoaderKHR(
                (const XrLoaderInitInfoBaseHeaderKHR*)&loader_info);
            SDL_Log("XR: xrInitializeLoaderKHR result=%d", (int)lr);
        } else {
            SDL_Log("XR: xrInitializeLoaderKHR not found — proceeding anyway");
        }
    }
#endif /* __ANDROID__ */

    /* xrCreateInstance is available with null handle */
    pfn_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
        (PFN_xrVoidFunction*)&pfn_xrCreateInstance);
    if (!pfn_xrCreateInstance) {
        SDL_Log("XR: no xrCreateInstance");
        return false;
    }

#ifdef __ANDROID__
    JNIEnv *env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    JavaVM *vm   = NULL;
    (*env)->GetJavaVM(env, &vm);
    jobject activity = (jobject)SDL_GetAndroidActivity();

    /* Headset family, from Build.MANUFACTURER. Every step is null-checked and the
     * whole thing falls back to QUEST, because the alternative to knowing is the
     * oculus/touch profile, which is the one every Android runtime here supports.
     * The compare is case-INsensitive: Pico has shipped this string as both
     * "Pico" and "PICO" across PUI versions, and a mismatch would silently hand a
     * Pico the Quest bindings. */
    {
        jclass buildClass = (*env)->FindClass(env, "android/os/Build");
        if (buildClass) {
            jfieldID manufacturerField = (*env)->GetStaticFieldID(env, buildClass,
                                            "MANUFACTURER", "Ljava/lang/String;");
            jstring manufacturer = manufacturerField
                ? (jstring)(*env)->GetStaticObjectField(env, buildClass, manufacturerField)
                : NULL;
            const char *mfr = manufacturer
                ? (*env)->GetStringUTFChars(env, manufacturer, NULL)
                : NULL;
            if (mfr) {
                if (SDL_strcasecmp(mfr, "Pico") == 0) vr_headset = PICO;
                SDL_Log("XR: Build.MANUFACTURER=\"%s\" -> %s", mfr,
                        vr_headset == PICO ? "PICO" : "QUEST");
                (*env)->ReleaseStringUTFChars(env, manufacturer, mfr);
            }
            if (manufacturer) (*env)->DeleteLocalRef(env, manufacturer);
            (*env)->DeleteLocalRef(env, buildClass);
        }
        /* A pending exception makes every later JNI call undefined, and the two
         * XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR fields below are JNI handles. */
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }

    XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    android_info.applicationVM       = vm;
    android_info.applicationActivity = activity;

    /* Enumerate rather than assume — same shape as the PCVR branch below, and for
     * the same reason: requesting an extension the runtime does not have FAILS
     * xrCreateInstance outright. This list was written when Android meant Quest,
     * and XR_FB_display_refresh_rate is a META extension: hardcoded, it takes the
     * whole session down on any headset that lacks it, which is a black screen
     * rather than a missing menu row. The two KHR extensions stay unconditional —
     * without them there is no Android XR instance and no GLES rendering at all,
     * so xrCreateInstance failing is the correct outcome. */
    const char *extensions[4];
    Uint32 extension_count = 0;
    xr_has_refresh_rate_ext = false;
    {
        PFN_xrEnumerateInstanceExtensionProperties pfn_xrEnumerateInstanceExtensionProperties = NULL;
        pfn_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            (PFN_xrVoidFunction*)&pfn_xrEnumerateInstanceExtensionProperties);
        if (pfn_xrEnumerateInstanceExtensionProperties) {
            Uint32 avail = 0;
            pfn_xrEnumerateInstanceExtensionProperties(NULL, 0, &avail, NULL);
            XrExtensionProperties *props = SDL_calloc(avail, sizeof(XrExtensionProperties));
            if (props) {
                for (Uint32 i = 0; i < avail; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
                pfn_xrEnumerateInstanceExtensionProperties(NULL, avail, &avail, props);
                for (Uint32 i = 0; i < avail; i++) {
                    if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
                        xr_has_refresh_rate_ext = true;
                    else if (!strcmp(props[i].extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME))
                        xr_has_bd_controller_ext = true;
                }
                SDL_free(props);
            }
        }
    }
    extensions[extension_count++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    extensions[extension_count++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    if (xr_has_refresh_rate_ext)
        extensions[extension_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    /* Pico's interaction profile (/interaction_profiles/pico/neo3_controller) is
     * DEFINED BY this extension, so suggesting bindings for it without enabling
     * the extension is XR_ERROR_PATH_UNSUPPORTED — and that rejects all 17
     * bindings together, not just the profile line. */
    if (xr_has_bd_controller_ext)
        extensions[extension_count++] = XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME;
    SDL_Log("XR: extensions available — FB_display_refresh_rate=%d BD_controller_interaction=%d",
            (int)xr_has_refresh_rate_ext, (int)xr_has_bd_controller_ext);
#else /* AVP_PCVR */
    /* Requesting an extension the runtime doesn't have FAILS xrCreateInstance,
     * so build the list from what the runtime actually offers. Only
     * XR_KHR_opengl_enable is mandatory; XR_FB_display_refresh_rate is a Quest
     * nicety that SteamVR doesn't expose. */
    const char *extensions[2];
    Uint32 extension_count = 0;
    xr_has_refresh_rate_ext = false;
    {
        PFN_xrEnumerateInstanceExtensionProperties pfn_xrEnumerateInstanceExtensionProperties = NULL;
        pfn_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            (PFN_xrVoidFunction*)&pfn_xrEnumerateInstanceExtensionProperties);
        bool have_opengl_ext = false;
        if (pfn_xrEnumerateInstanceExtensionProperties) {
            Uint32 avail = 0;
            pfn_xrEnumerateInstanceExtensionProperties(NULL, 0, &avail, NULL);
            XrExtensionProperties *props = SDL_calloc(avail, sizeof(XrExtensionProperties));
            for (Uint32 i = 0; i < avail; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
            pfn_xrEnumerateInstanceExtensionProperties(NULL, avail, &avail, props);
            for (Uint32 i = 0; i < avail; i++) {
                if (!strcmp(props[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))
                    have_opengl_ext = true;
                else if (!strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
                    xr_has_refresh_rate_ext = true;
            }
            SDL_free(props);
        }
        if (!have_opengl_ext) {
            SDL_Log("XR: runtime does not support XR_KHR_opengl_enable — cannot render");
            return false;
        }
        extensions[extension_count++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
        if (xr_has_refresh_rate_ext)
            extensions[extension_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
    }
#endif

    XrApplicationInfo app_info = {0};
    /* This is what the runtime shows as the running application — SteamVR puts it
       in the VR dashboard, and it wins over the name on a non-Steam shortcut.
       Keep it in step with the SDL window title in SetOGLVideoMode. Limit is
       XR_MAX_APPLICATION_NAME_SIZE (128); SDL_strlcpy truncates safely. */
    SDL_strlcpy(app_info.applicationName, "Aliens Versus Predator: VR",  XR_MAX_APPLICATION_NAME_SIZE);
    app_info.applicationVersion = 1;
    SDL_strlcpy(app_info.engineName, "SDL3", XR_MAX_ENGINE_NAME_SIZE);
    /* Request OpenXR 1.0, not XR_CURRENT_API_VERSION (1.1 in the bundled headers).
     * Older runtimes support ONLY 1.0 and reject a 1.1 request outright:
     * Quest 1 on OS v50 ships a 1.0-only runtime, so xrCreateInstance failed with
     * XR_ERROR_API_VERSION_UNSUPPORTED and the app fell back to no-XR (black screen +
     * 3-dot compositor spinner, dead controllers). This app only uses core 1.0 plus
     * KHR/FB extensions that exist in 1.0, and 1.1 runtimes accept a 1.0 request, so
     * this works on both old and new headsets. */
    app_info.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
    ci.next                     = &android_info;
#endif
    ci.createFlags              = 0;
    ci.applicationInfo          = app_info;
    ci.enabledExtensionCount    = extension_count;
    ci.enabledExtensionNames    = extensions;

    XrResult result = pfn_xrCreateInstance(&ci, &xr_instance);
    if (XR_FAILED(result)) {
        SDL_Log("XR: xrCreateInstance failed: %d", (int)result);
        return false;
    }
    SDL_Log("XR: instance created %p", (void*)xr_instance);

    if (!load_xr_functions()) return false;

    /* Name the runtime we actually bound to. Which one that is comes from the
       loader (XR_RUNTIME_JSON, else the registry ActiveRuntime), never from
       anything this app does, so on a machine with both SteamVR and Oculus
       installed the log is the only reliable way to tell them apart. Note a
       Quest on SteamVR still brings up Meta Link underneath as the transport —
       seeing both start is expected and does NOT mean the wrong one is bound. */
    {
        PFN_xrGetInstanceProperties pfn_props = NULL;

        pfn_xrGetInstanceProcAddr(xr_instance, "xrGetInstanceProperties",
                                  (PFN_xrVoidFunction*)&pfn_props);
        if (pfn_props) {
            XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };

            if (XR_SUCCEEDED(pfn_props(xr_instance, &props))) {
                SDL_Log("XR: runtime \"%s\" version %u.%u.%u", props.runtimeName,
                        (unsigned)XR_VERSION_MAJOR(props.runtimeVersion),
                        (unsigned)XR_VERSION_MINOR(props.runtimeVersion),
                        (unsigned)XR_VERSION_PATCH(props.runtimeVersion));
            }
        }
    }

    XrSystemGetInfo sys_info = { XR_TYPE_SYSTEM_GET_INFO };
    sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = pfn_xrGetSystem(xr_instance, &sys_info, &xr_system_id);
    if (XR_FAILED(result)) {
        SDL_Log("XR: xrGetSystem failed: %d", (int)result);
        return false;
    }
    SDL_Log("XR: system id=%llu", (unsigned long long)xr_system_id);
    return true;
}

static bool create_gles_quad_buffers(void)
{
    PositionUVVertex vertices[4] = {
            /* top-left  */ { -QUAD_HALF_W,  QUAD_HALF_H, QUAD_DEPTH,  0.0f, 0.0f },
            /* top-right */ {  QUAD_HALF_W,  QUAD_HALF_H, QUAD_DEPTH,  1.0f, 0.0f },
            /* bot-right */ {  QUAD_HALF_W, -QUAD_HALF_H, QUAD_DEPTH,  1.0f, 1.0f },
            /* bot-left  */ { -QUAD_HALF_W, -QUAD_HALF_H, QUAD_DEPTH,  0.0f, 1.0f },
    };
    GLushort indices[6] = { 0, 1, 2,  0, 2, 3 };
    
    glGenVertexArrays(1, &quad_vao);
    glBindVertexArray(quad_vao);
    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glGenBuffers(1, &quad_ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(PositionUVVertex),
                          (void*)offsetof(PositionUVVertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(PositionUVVertex),
                          (void*)offsetof(PositionUVVertex, u));
    glBindVertexArray(0);
    SDL_Log("GLES quad buffers ready");
    return true;
}

static bool create_gles_menu_resources(void)
{
    glGenTextures(1, &menu_gles_tex);
    glBindTexture(GL_TEXTURE_2D, menu_gles_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MENU_W, MENU_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &menu_fbo_2d);
    SDL_Log("GLES menu texture + FBO ready");
    return true;
}

/* ========================================================================
 * XR Session Initialization
 * ======================================================================== */

static bool init_xr_session(void)
{
    XrResult result;

#ifdef __ANDROID__
    /* GLES requirements check — required before session creation */
    if (pfn_xrGetOpenGLESGraphicsRequirementsKHR) {
        XrGraphicsRequirementsOpenGLESKHR gfx_reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
        pfn_xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system_id, &gfx_reqs);
        SDL_Log("XR: GLES %d.%d – %d.%d supported",
            XR_VERSION_MAJOR(gfx_reqs.minApiVersionSupported),
            XR_VERSION_MINOR(gfx_reqs.minApiVersionSupported),
            XR_VERSION_MAJOR(gfx_reqs.maxApiVersionSupported),
            XR_VERSION_MINOR(gfx_reqs.maxApiVersionSupported));
    }

    /* Build GLES graphics binding from the current EGL context */
    EGLDisplay egl_disp = eglGetCurrentDisplay();
    EGLContext egl_ctx  = eglGetCurrentContext();
    EGLConfig  egl_cfg  = (EGLConfig)0;
    {
        EGLint cfg_id = 0;
        /* Query config ID from context; fall back to current draw surface if that fails */
        if (eglQueryContext(egl_disp, egl_ctx, EGL_CONFIG_ID, &cfg_id) != EGL_TRUE || cfg_id == 0) {
            EGLSurface egl_surf = eglGetCurrentSurface(EGL_DRAW);
            if (egl_surf != EGL_NO_SURFACE)
                eglQuerySurface(egl_disp, egl_surf, EGL_CONFIG_ID, &cfg_id);
        }
        SDL_Log("XR: EGL cfg_id=%d", (int)cfg_id);
        if (cfg_id != 0) {
            EGLint num = 0;
            eglGetConfigs(egl_disp, NULL, 0, &num);
            if (num > 0) {
                EGLConfig *cfgs = SDL_malloc((size_t)num * sizeof(EGLConfig));
                eglGetConfigs(egl_disp, cfgs, num, &num);
                for (EGLint k = 0; k < num; k++) {
                    EGLint id = 0;
                    eglGetConfigAttrib(egl_disp, cfgs[k], EGL_CONFIG_ID, &id);
                    if (id == cfg_id) { egl_cfg = cfgs[k]; break; }
                }
                SDL_free(cfgs);
            }
        }
    }
    SDL_Log("XR: EGL display=%p ctx=%p cfg=%p", (void*)egl_disp, (void*)egl_ctx, (void*)egl_cfg);

    XrGraphicsBindingOpenGLESAndroidKHR gfx_binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    gfx_binding.display = egl_disp;
    gfx_binding.config  = egl_cfg;
    gfx_binding.context = egl_ctx;
    const void *gfx_next = &gfx_binding;
#else /* AVP_PCVR (both Win32/WGL and Linux/GLX): the spec REQUIRES the
       * graphics-requirements call before xrCreateSession — skipping it fails
       * with XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING. */
    if (pfn_xrGetOpenGLGraphicsRequirementsKHR) {
        XrGraphicsRequirementsOpenGLKHR gfx_reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
        pfn_xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system_id, &gfx_reqs);
        SDL_Log("XR: GL %d.%d – %d.%d required",
            XR_VERSION_MAJOR(gfx_reqs.minApiVersionSupported),
            XR_VERSION_MINOR(gfx_reqs.minApiVersionSupported),
            XR_VERSION_MAJOR(gfx_reqs.maxApiVersionSupported),
            XR_VERSION_MINOR(gfx_reqs.maxApiVersionSupported));
    } else {
        SDL_Log("XR: xrGetOpenGLGraphicsRequirementsKHR missing");
        return false;
    }

#ifdef AVP_PCVR_XLIB
    /* Built in xr_linux_glx.c from the GLX context SDL made current on this
     * thread — see the header block at the top for why the Xlib types are kept
     * out of this file. */
    const void *gfx_next = AvpXrGlxBinding();
    if (!gfx_next)
        return false;
#else /* AVP_PCVR_WIN32 */
    /* SDL exposes no WGL handles directly, but the context it created is
     * current on this thread, so the wglGetCurrent* pair returns exactly it. */
    XrGraphicsBindingOpenGLWin32KHR gfx_binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    gfx_binding.hDC   = wglGetCurrentDC();
    gfx_binding.hGLRC = wglGetCurrentContext();
    SDL_Log("XR: WGL hDC=%p hGLRC=%p", (void*)gfx_binding.hDC, (void*)gfx_binding.hGLRC);
    if (!gfx_binding.hDC || !gfx_binding.hGLRC) {
        SDL_Log("XR: no current WGL context — cannot create session");
        return false;
    }
    const void *gfx_next = &gfx_binding;
#endif
#endif

    /* This call can BLOCK for a long time while the runtime brings up a session
       — 40+ seconds measured on the Oculus runtime with no headset presenting,
       and it has been seen never to return. A log that stops on this line is
       that, not a crash. There is no timeout in the API; run with -noxr to skip
       OpenXR and use the desktop path. */
    SDL_Log("XR: Creating session... (blocks while the runtime starts; -noxr runs flat)");
    XrSessionCreateInfo session_info = { XR_TYPE_SESSION_CREATE_INFO };
    session_info.next     = gfx_next;
    session_info.systemId = xr_system_id;
    result = pfn_xrCreateSession(xr_instance, &session_info, &xr_session);
    SDL_Log("XR: xrCreateSession result=%d session=%p", (int)result, (void*)xr_session);
    XR_CHECK(result, "Failed to create XR session");

    SDL_Log("XR: Creating reference space...");
    XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    space_info.poseInReferenceSpace.orientation.w = 1.0f;

    /* STAGE gives a floor-level origin with room-scale tracking.
     * It requires a valid guardian boundary; fall back to LOCAL if unavailable. */
    space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    result = pfn_xrCreateReferenceSpace(xr_session, &space_info, &xr_local_space);
    if (XR_FAILED(result)) {
        SDL_Log("XR: STAGE space unavailable (%d), falling back to LOCAL", (int)result);
        space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        result = pfn_xrCreateReferenceSpace(xr_session, &space_info, &xr_local_space);
    }
    SDL_Log("XR: xrCreateReferenceSpace result=%d space=%p", (int)result, (void*)xr_local_space);
    XR_CHECK(result, "Failed to create reference space");

    /* --- Input actions: left thumbstick locomotion --- */
    {
        XrActionSetCreateInfo aset_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
        SDL_strlcpy(aset_info.actionSetName,       "gameplay", XR_MAX_ACTION_SET_NAME_SIZE);
        SDL_strlcpy(aset_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);
        aset_info.priority = 0;
        result = pfn_xrCreateActionSet(xr_instance, &aset_info, &xr_input_action_set);
        XR_CHECK(result, "Failed to create action set");

        XrActionCreateInfo act_info = { XR_TYPE_ACTION_CREATE_INFO };
        act_info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
        SDL_strlcpy(act_info.actionName,       "left_stick", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Stick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_stick_action);
        XR_CHECK(result, "Failed to create left_stick action");

        SDL_strlcpy(act_info.actionName,       "right_stick", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Stick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_stick_action);
        XR_CHECK(result, "Failed to create right_stick action");

        act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
        SDL_strlcpy(act_info.actionName,       "x_button", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "X Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_x_button_action);
        XR_CHECK(result, "Failed to create x_button action");

        SDL_strlcpy(act_info.actionName,       "y_button", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Y Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_y_button_action);
        XR_CHECK(result, "Failed to create y_button action");

        SDL_strlcpy(act_info.actionName,       "menu_button", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Menu Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_menu_button_action);
        XR_CHECK(result, "Failed to create menu_button action");

        SDL_strlcpy(act_info.actionName,       "right_trigger", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_trigger_action);
        XR_CHECK(result, "Failed to create right_trigger action");

        SDL_strlcpy(act_info.actionName,       "right_squeeze", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Grip Squeeze", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_squeeze_action);
        XR_CHECK(result, "Failed to create right_squeeze action");

        SDL_strlcpy(act_info.actionName,       "a_button", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "A Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_a_button_action);
        XR_CHECK(result, "Failed to create a_button action");

        SDL_strlcpy(act_info.actionName,       "left_thumbstick_click", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Thumbstick Click", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_thumbstick_click_action);
        XR_CHECK(result, "Failed to create left_thumbstick_click action");

        SDL_strlcpy(act_info.actionName,       "b_button", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "B Button", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_b_button_action);
        XR_CHECK(result, "Failed to create b_button action");

        SDL_strlcpy(act_info.actionName,       "right_thumbstick_click", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Thumbstick Click", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_thumbstick_click_action);
        XR_CHECK(result, "Failed to create right_thumbstick_click action");

        SDL_strlcpy(act_info.actionName,       "left_trigger", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_trigger_action);
        XR_CHECK(result, "Failed to create left_trigger action");

        SDL_strlcpy(act_info.actionName,       "left_squeeze", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Grip Squeeze", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_squeeze_action);
        XR_CHECK(result, "Failed to create left_squeeze action");

        act_info.actionType = XR_ACTION_TYPE_POSE_INPUT;
        SDL_strlcpy(act_info.actionName,       "left_grip", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_grip_action);
        XR_CHECK(result, "Failed to create left_grip action");

        SDL_strlcpy(act_info.actionName,       "right_grip", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Grip Pose", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_grip_action);
        XR_CHECK(result, "Failed to create right_grip action");

        act_info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        SDL_strlcpy(act_info.actionName,       "right_haptic", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Right Haptic", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_right_haptic_action);
        XR_CHECK(result, "Failed to create right_haptic action");

        SDL_strlcpy(act_info.actionName,       "left_haptic", XR_MAX_ACTION_NAME_SIZE);
        SDL_strlcpy(act_info.localizedActionName, "Left Haptic", XR_MAX_LOCALIZED_ACTION_NAME_SIZE);
        result = pfn_xrCreateAction(xr_input_action_set, &act_info, &xr_left_haptic_action);
        XR_CHECK(result, "Failed to create left_haptic action");

        /* Suggest bindings for Touch controller profile */
        XrPath profile_path, left_stick_path, right_stick_path, x_path, y_path, menu_path;
        XrPath left_grip_path, right_grip_path, right_trigger_path, right_squeeze_path, a_path, left_stick_click_path, b_path, right_stick_click_path, left_trigger_path, left_squeeze_path, right_haptic_path, left_haptic_path;
        /* Written as "Pico, else touch" rather than one test per headset so the
         * FALLBACK is the oculus/touch profile — the one every runtime this ships
         * against understands, SteamVR included. The if/else-if pair it replaces
         * left these XrPaths uninitialised for any value matching neither arm.
         * The guard keeps the Pico paths out of the PCVR binary entirely. */
#ifdef __ANDROID__
        if (vr_headset == PICO) {
            /* The Pico Neo3 profile does not accept the bare .../input/trigger
             * form, and its left-hand menu equivalent is a back click. */
            pfn_xrStringToPath(xr_instance, "/interaction_profiles/pico/neo3_controller", &profile_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/left/input/trigger/click", &left_trigger_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/right/input/trigger/click",&right_trigger_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/left/input/back/click",    &menu_path);
        } else
#endif
        {
            pfn_xrStringToPath(xr_instance, "/interaction_profiles/oculus/touch_controller", &profile_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/left/input/trigger",       &left_trigger_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/right/input/trigger",      &right_trigger_path);
            pfn_xrStringToPath(xr_instance, "/user/hand/left/input/menu/click",    &menu_path);
        }
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/thumbstick",        &left_stick_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/thumbstick",       &right_stick_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/x/click",           &x_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/y/click",           &y_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/grip/pose",         &left_grip_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/grip/pose",        &right_grip_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/squeeze",          &right_squeeze_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/a/click",          &a_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/thumbstick/click",  &left_stick_click_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/b/click",           &b_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/thumbstick/click", &right_stick_click_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/squeeze",           &left_squeeze_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/output/haptic",          &right_haptic_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/output/haptic",           &left_haptic_path);

        XrActionSuggestedBinding bindings[17];
        bindings[0].action  = xr_left_stick_action;              bindings[0].binding  = left_stick_path;
        bindings[1].action  = xr_right_stick_action;             bindings[1].binding  = right_stick_path;
        bindings[2].action  = xr_x_button_action;                bindings[2].binding  = x_path;
        bindings[3].action  = xr_y_button_action;                bindings[3].binding  = y_path;
        bindings[4].action  = xr_menu_button_action;             bindings[4].binding  = menu_path;
        bindings[5].action  = xr_left_grip_action;               bindings[5].binding  = left_grip_path;
        bindings[6].action  = xr_right_grip_action;              bindings[6].binding  = right_grip_path;
        bindings[7].action  = xr_right_trigger_action;           bindings[7].binding  = right_trigger_path;
        bindings[8].action  = xr_right_squeeze_action;           bindings[8].binding  = right_squeeze_path;
        bindings[9].action  = xr_a_button_action;                bindings[9].binding  = a_path;
        bindings[10].action = xr_left_thumbstick_click_action;   bindings[10].binding = left_stick_click_path;
        bindings[11].action = xr_b_button_action;                bindings[11].binding = b_path;
        bindings[12].action = xr_right_thumbstick_click_action;  bindings[12].binding = right_stick_click_path;
        bindings[13].action = xr_left_trigger_action;            bindings[13].binding = left_trigger_path;
        bindings[14].action = xr_right_haptic_action;            bindings[14].binding = right_haptic_path;
        bindings[15].action = xr_left_haptic_action;             bindings[15].binding = left_haptic_path;
        bindings[16].action = xr_left_squeeze_action;            bindings[16].binding = left_squeeze_path;
        XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile     = profile_path;
        suggested.countSuggestedBindings = 17;
        suggested.suggestedBindings      = bindings;
        result = pfn_xrSuggestInteractionProfileBindings(xr_instance, &suggested);
        if (XR_FAILED(result)) {
            /* All 17 bindings are accepted or rejected together, so this is the
             * difference between working controllers and a headset that tracks
             * your head and ignores everything else — previously with nothing in
             * the log to say so. Causes: a profile the runtime does not know, a
             * path that profile does not define, or an extension-defined profile
             * whose extension was not enabled at instance creation. */
            SDL_Log("XR: xrSuggestInteractionProfileBindings FAILED (%d) - controllers will be dead",
                    (int)result);
        }

        XrSessionActionSetsAttachInfo attach_info = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attach_info.countActionSets = 1;
        attach_info.actionSets      = &xr_input_action_set;
        result = pfn_xrAttachSessionActionSets(xr_session, &attach_info);
        XR_CHECK(result, "Failed to attach action sets");

        /* Create action spaces for grip poses (must be after xrAttachSessionActionSets) */
        if (pfn_xrCreateActionSpace) {
            XrActionSpaceCreateInfo grip_space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
            grip_space_info.subactionPath = XR_NULL_PATH;
            grip_space_info.poseInActionSpace.orientation.w = 1.0f;

            grip_space_info.action = xr_left_grip_action;
            result = pfn_xrCreateActionSpace(xr_session, &grip_space_info, &xr_left_grip_space);
            if (XR_FAILED(result)) SDL_Log("XR: failed to create left grip space: %d", (int)result);

            /* The right hand holds the weapon, and the Pico grip pose sits at a
             * different angle to the Oculus one - left alone the gun points about
             * 20 degrees off. Applied as a pitch about X on the action space
             * rather than in the aiming code, so everything downstream (muzzle,
             * crosshair, the two-hand reload gesture) follows automatically.
             *
             * A pitch of 0 is the identity quaternion (sin 0 = 0, cos 0 = 1), so
             * Quest and PCVR get bit-for-bit what they had before these two
             * branches were collapsed into one. The guard means the desktop
             * binary cannot pick up a Pico offset even if vr_headset were wrong. */
            float weapon_pitch_rad = 0.0f;
#ifdef __ANDROID__
            if (vr_headset == PICO)
                weapon_pitch_rad = 20.0f * (3.14159265358979323846f / 180.0f);
#endif
            grip_space_info.poseInActionSpace.orientation.x = SDL_sinf(weapon_pitch_rad * 0.5f);
            grip_space_info.poseInActionSpace.orientation.w = SDL_cosf(weapon_pitch_rad * 0.5f);

            grip_space_info.action = xr_right_grip_action;
            result = pfn_xrCreateActionSpace(xr_session, &grip_space_info, &xr_right_grip_space);
            if (XR_FAILED(result)) SDL_Log("XR: failed to create right grip space: %d", (int)result);
        }
    }

    SDL_Log("XR: init_xr_session complete");
    return true;
}

static bool create_swapchains(void)
{
    XrResult result;

    /* Enumerate view configs */
    result = pfn_xrEnumerateViewConfigurationViews(
            xr_instance, xr_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &view_count, NULL);
    XR_CHECK(result, "Failed to count view configs");
    SDL_Log("XR: view_count=%u", (unsigned)view_count);

    XrViewConfigurationView *vcfgs = SDL_calloc(view_count, sizeof(XrViewConfigurationView));
    for (Uint32 i = 0; i < view_count; i++) vcfgs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    result = pfn_xrEnumerateViewConfigurationViews(
            xr_instance, xr_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            view_count, &view_count, vcfgs);
    if (XR_FAILED(result)) { SDL_free(vcfgs); SDL_Log("Failed to enumerate view configs"); return false; }

    vr_swapchains = SDL_calloc(view_count, sizeof(VRSwapchain));
    xr_views      = SDL_calloc(view_count, sizeof(XrView));

    /* Pick swapchain format and set up color-space handling.
     *
     * Quest's compositor treats GL_RGBA8 as LINEAR — it applies sRGB gamma for
     * display, so our already-gamma-encoded game content becomes doubly bright.
     * GL_SRGB8_ALPHA8 explicitly marks content as sRGB; the compositor does
     * correct linearisation for compositing and re-encodes for the display.
     *
     * Problem: GLES auto-converts linear→sRGB on writes to sRGB FBOs, which
     * would double-encode our values. GL_EXT_sRGB_write_control lets us disable
     * that conversion so game values (already gamma-encoded) go in unmodified.
     *
     * Strategy:
     *   - If GL_EXT_sRGB_write_control is available: use GL_SRGB8_ALPHA8.
     *   - Otherwise: fall back to GL_RGBA8 (compositor gamma is wrong, but
     *     better than double-encoding from the sRGB write path). */
    #define GL_RGBA8_FMT        0x8058LL
    #define GL_SRGB8_ALPHA8_FMT 0x8C43LL

    /* Extension check — must happen while our GLES context is current */
#ifdef __ANDROID__
    {
        const char *exts = (const char *)glGetString(GL_EXTENSIONS);
        has_srgb_write_control = exts && strstr(exts, "GL_EXT_sRGB_write_control");
        SDL_Log("XR: GL_EXT_sRGB_write_control=%d", (int)has_srgb_write_control);
    }
#else
    /* Desktop GL has sRGB write control in core: linear→sRGB conversion on
     * writes to sRGB framebuffers only happens while GL_FRAMEBUFFER_SRGB is
     * enabled (default DISABLED — exactly the pass-through we want, and the
     * same enum value 0x8DB9 the EXT path toggles). So the sRGB swapchain
     * strategy works unconditionally here. */
    has_srgb_write_control = true;
#endif

    Uint32 fmt_count = 0;
    pfn_xrEnumerateSwapchainFormats(xr_session, 0, &fmt_count, NULL);
    int64_t *fmts = SDL_calloc(fmt_count, sizeof(int64_t));
    pfn_xrEnumerateSwapchainFormats(xr_session, fmt_count, &fmt_count, fmts);
    for (Uint32 f = 0; f < fmt_count; f++)
        SDL_Log("XR: swapchain fmt[%u]=0x%llx", f, (unsigned long long)fmts[f]);

    int64_t chosen_fmt = 0;
    if (has_srgb_write_control) {
        /* Prefer sRGB — compositor will handle it correctly */
        for (Uint32 f = 0; f < fmt_count; f++)
            if (fmts[f] == GL_SRGB8_ALPHA8_FMT) { chosen_fmt = GL_SRGB8_ALPHA8_FMT; break; }
    }
    if (!chosen_fmt) {
        /* Prefer GL_RGBA8 (linear, hope compositor treats as sRGB) */
        for (Uint32 f = 0; f < fmt_count; f++)
            if (fmts[f] == GL_RGBA8_FMT) { chosen_fmt = GL_RGBA8_FMT; break; }
    }
    if (!chosen_fmt && fmt_count > 0) chosen_fmt = fmts[0];
    if (!chosen_fmt) chosen_fmt = GL_RGBA8_FMT;
    SDL_free(fmts);

    vr_srgb_swapchain = (chosen_fmt == GL_SRGB8_ALPHA8_FMT);
    SDL_Log("XR: chosen swapchain format=0x%llx srgb=%d write_ctrl=%d",
            (unsigned long long)chosen_fmt, (int)vr_srgb_swapchain,
            (int)has_srgb_write_control);

    for (Uint32 i = 0; i < view_count; i++) {
        xr_views[i].type = XR_TYPE_VIEW;
        xr_views[i].pose.orientation.w = 1.0f;

        SDL_Log("XR: eye %u recommended %ux%u", i,
                vcfgs[i].recommendedImageRectWidth, vcfgs[i].recommendedImageRectHeight);

        XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format      = chosen_fmt;
        sci.sampleCount = 1;
        sci.width       = vcfgs[i].recommendedImageRectWidth;
        sci.height      = vcfgs[i].recommendedImageRectHeight;
        sci.faceCount   = 1;
        sci.arraySize   = 1;
        sci.mipCount    = 1;

        result = pfn_xrCreateSwapchain(xr_session, &sci, &vr_swapchains[i].swapchain);
        if (XR_FAILED(result)) {
            SDL_Log("XR: xrCreateSwapchain eye %u failed: %d", i, (int)result);
            SDL_free(vcfgs);
            return false;
        }

        vr_swapchains[i].size.width  = (int32_t)sci.width;
        vr_swapchains[i].size.height = (int32_t)sci.height;

        /* Enumerate GLES swapchain images */
        pfn_xrEnumerateSwapchainImages(vr_swapchains[i].swapchain, 0,
                                       &vr_swapchains[i].image_count, NULL);
        vr_swapchains[i].images = SDL_calloc(vr_swapchains[i].image_count,
                                              sizeof(AvpXrSwapchainImage));
        for (Uint32 j = 0; j < vr_swapchains[i].image_count; j++)
            vr_swapchains[i].images[j].type = AVP_XR_TYPE_SWAPCHAIN_IMAGE;
        pfn_xrEnumerateSwapchainImages(vr_swapchains[i].swapchain,
                                       vr_swapchains[i].image_count,
                                       &vr_swapchains[i].image_count,
                                       (XrSwapchainImageBaseHeader*)vr_swapchains[i].images);
        SDL_Log("XR: eye %u swapchain: %dx%d, %u images (tex[0]=%u)",
                i, sci.width, sci.height, vr_swapchains[i].image_count,
                vr_swapchains[i].image_count > 0 ? vr_swapchains[i].images[0].image : 0);
    }
    SDL_free(vcfgs);

    /* Dedicated 4:3 swapchain for the flat 2D menu quad — never rendered with MSAA,
     * so the compositor doesn't choke on a stale MSAA image under Battery Saver. */
    if (vr_menu_swapchain.swapchain == XR_NULL_HANDLE) {
        XrSwapchainCreateInfo msci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        msci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        msci.format      = chosen_fmt;
        msci.sampleCount = 1;
        msci.width       = 1024;
        msci.height      = 768;   /* 4:3, matches the 640x480 menu surface */
        msci.faceCount   = 1;
        msci.arraySize   = 1;
        msci.mipCount    = 1;
        result = pfn_xrCreateSwapchain(xr_session, &msci, &vr_menu_swapchain.swapchain);
        if (XR_FAILED(result)) {
            SDL_Log("XR: menu xrCreateSwapchain failed: %d", (int)result);
            return false;
        }
        vr_menu_swapchain.size.width  = (int32_t)msci.width;
        vr_menu_swapchain.size.height = (int32_t)msci.height;
        pfn_xrEnumerateSwapchainImages(vr_menu_swapchain.swapchain, 0,
                                       &vr_menu_swapchain.image_count, NULL);
        vr_menu_swapchain.images = SDL_calloc(vr_menu_swapchain.image_count,
                                              sizeof(AvpXrSwapchainImage));
        for (Uint32 j = 0; j < vr_menu_swapchain.image_count; j++)
            vr_menu_swapchain.images[j].type = AVP_XR_TYPE_SWAPCHAIN_IMAGE;
        pfn_xrEnumerateSwapchainImages(vr_menu_swapchain.swapchain,
                                       vr_menu_swapchain.image_count,
                                       &vr_menu_swapchain.image_count,
                                       (XrSwapchainImageBaseHeader*)vr_menu_swapchain.images);
        SDL_Log("XR: menu swapchain: %dx%d, %u images (tex[0]=%u)",
                msci.width, msci.height, vr_menu_swapchain.image_count,
                vr_menu_swapchain.image_count > 0 ? vr_menu_swapchain.images[0].image : 0);
    }

    /* Dedicated 4:3 swapchain for the floating MP score-table quad — same setup as
     * the menu swapchain (non-MSAA, alpha-capable format), presented as an
     * alpha-blended overlay on top of the 3D world when the player is dead/showing
     * scores. Matches the 1024x768 score FBO in avpview.c. */
    if (vr_score_swapchain.swapchain == XR_NULL_HANDLE) {
        XrSwapchainCreateInfo ssci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ssci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ssci.format      = chosen_fmt;
        ssci.sampleCount = 1;
        ssci.width       = 1024;
        ssci.height      = 768;
        ssci.faceCount   = 1;
        ssci.arraySize   = 1;
        ssci.mipCount    = 1;
        result = pfn_xrCreateSwapchain(xr_session, &ssci, &vr_score_swapchain.swapchain);
        if (XR_FAILED(result)) {
            SDL_Log("XR: score xrCreateSwapchain failed: %d", (int)result);
            return false;
        }
        vr_score_swapchain.size.width  = (int32_t)ssci.width;
        vr_score_swapchain.size.height = (int32_t)ssci.height;
        pfn_xrEnumerateSwapchainImages(vr_score_swapchain.swapchain, 0,
                                       &vr_score_swapchain.image_count, NULL);
        vr_score_swapchain.images = SDL_calloc(vr_score_swapchain.image_count,
                                               sizeof(AvpXrSwapchainImage));
        for (Uint32 j = 0; j < vr_score_swapchain.image_count; j++)
            vr_score_swapchain.images[j].type = AVP_XR_TYPE_SWAPCHAIN_IMAGE;
        pfn_xrEnumerateSwapchainImages(vr_score_swapchain.swapchain,
                                       vr_score_swapchain.image_count,
                                       &vr_score_swapchain.image_count,
                                       (XrSwapchainImageBaseHeader*)vr_score_swapchain.images);
        SDL_Log("XR: score swapchain: %dx%d, %u images (tex[0]=%u)",
                ssci.width, ssci.height, vr_score_swapchain.image_count,
                vr_score_swapchain.image_count > 0 ? vr_score_swapchain.images[0].image : 0);
    }

    /* Init GLES quad pipeline + resources (once) */
    if (quad_program == 0) {
        if (!create_quad_gles_program())    return false;
        if (!create_gles_quad_buffers())    return false;
        if (!create_gles_menu_resources())  return false;
    }

    return true;
}

/* ========================================================================
 * XR Event Handling
 * ======================================================================== */

static void handle_xr_events(void)
{
    XrEventDataBuffer event_buffer = { XR_TYPE_EVENT_DATA_BUFFER };
    
    while (pfn_xrPollEvent(xr_instance, &event_buffer) == XR_SUCCESS) {
        switch (event_buffer.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged *state_event =
                        (XrEventDataSessionStateChanged*)&event_buffer;
                
                SDL_Log("Session state changed: %d", state_event->state);
                
                switch (state_event->state) {
                    case XR_SESSION_STATE_READY: {
                        XrSessionBeginInfo begin_info = { XR_TYPE_SESSION_BEGIN_INFO };
                        begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                        
                        XrResult result = pfn_xrBeginSession(xr_session, &begin_info);
                        if (XR_SUCCEEDED(result)) {
                            SDL_Log("XR Session begun!");
                            xr_session_running = true;

                            /* Now that there is a live session, ask the runtime
                               which refresh rates this headset supports and
                               rebuild the AV-options row around them. Also turn
                               the rate saved in the profile back into a slider
                               position (see VR_GetRefreshRateIndexForHz). */
                            {
                                extern int VRRefreshRateHz;
                                extern void PatchRefreshRateMenuFromHeadset(void);
                                vr_enumerate_refresh_rates();
                                if (vr_refresh_rate_count > 0) {
                                    VRRefreshRateIndex =
                                        VR_GetRefreshRateIndexForHz((float)VRRefreshRateHz);
                                    PatchRefreshRateMenuFromHeadset();
                                }
                            }

                            /* Create swapchains now that session is ready */
                            if (!create_swapchains()) {
                                SDL_Log("Failed to create swapchains");
                                xr_should_quit = true;
                            } else {
                                VR_InitEyeFBOs(vr_swapchains[0].size.width,
                                               vr_swapchains[0].size.height);
                            }
                        }
                        break;
                    }
                    case XR_SESSION_STATE_STOPPING:
                        SDL_Log("XR: session STOPPING - ending session (app keeps running)");
                        pfn_xrEndSession(xr_session);
                        xr_session_running = false;
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        SDL_Log("EXIT: XR session state %d (EXITING/LOSS_PENDING) - quitting",
                                (int)state_event->state);
                        xr_should_quit = true;
                        break;
                    default:
                        break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                SDL_Log("EXIT: XR instance loss pending - quitting");
                xr_should_quit = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                /* The runtime recentred our reference space — e.g. the user held
                 * the Meta button to "Reset View". Our cached VR calibration
                 * (heading, room-scale X/Z origin, and ref_head_y which sets the
                 * eyeline height scale) is now relative to the OLD origin, so
                 * re-run the same calibration the first VR frame does. Recapture
                 * happens on the next AvpShowViewsVR when it sees this flag. */
                XrEventDataReferenceSpaceChangePending *rc =
                        (XrEventDataReferenceSpaceChangePending*)&event_buffer;
                SDL_Log("XR reference space recentred (type %d) — recalibrating VR",
                        (int)rc->referenceSpaceType);
                vr_recalibrate = 1;
                vr_score_reanchor = 1; /* re-anchor the floating scoreboard in front */
                vr_menu_reanchor  = 1; /* re-anchor the 2D menu/intro quad in front */
                break;
            }
            default:
                break;
        }

        event_buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

/* Cleanly tear down the OpenXR session before the process exits.
 *
 * Without this the Quest compositor is left holding a live session that was
 * never ended, so once we exit it has nothing to present and the headset shows
 * a black screen until it is restarted. We ask the runtime to exit the session
 * and then pump events so the STOPPING -> EXITING transitions run (xrEndSession
 * is issued from handle_xr_events() on STOPPING). The loop is bounded so a
 * misbehaving runtime can never hang the quit. */
#ifdef AVP_XR
static void shutdown_xr_session(void)
{
    if (!xr_enabled || !xr_session)
        return;

    if (pfn_xrRequestExitSession && xr_session_running) {
        XrResult r = pfn_xrRequestExitSession(xr_session);
        if (XR_FAILED(r))
            SDL_Log("xrRequestExitSession failed (result=%d)", (int)r);
    }

    /* Drive the session through STOPPING/EXITING; ~1s ceiling (200 * 5ms).
     * handle_xr_events() sets xr_should_quit once EXITING arrives. */
    for (int i = 0; i < 200 && !xr_should_quit; i++) {
        handle_xr_events();
        SDL_Delay(5);
    }
}
#endif /* AVP_XR */

/* ========================================================================
 * Rendering helpers
 * ======================================================================== */

/* Upload menu surface pixels to the GLES menu texture */
static void upload_menu_gles_texture(void)
{
    if (!surface || !menu_gles_tex) return;
    const Uint16 *src = (const Uint16 *)surface->pixels;
    Uint8        *dst = menu_rgba;
    for (int i = 0; i < MENU_W * MENU_H; i++) {
        Uint16 p = src[i];
        dst[0] = (Uint8)(((p >> 11) & 0x1F) * 255 / 31);
        dst[1] = (Uint8)(((p >>  5) & 0x3F) * 255 / 63);
        dst[2] = (Uint8)(((p      ) & 0x1F) * 255 / 31);
        dst[3] = 255;
        dst += 4;
    }
    glBindTexture(GL_TEXTURE_2D, menu_gles_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, MENU_W, MENU_H, GL_RGBA, GL_UNSIGNED_BYTE, menu_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/* Acquire+wait on a specific swapchain. Returns image index or UINT32_MAX.
 * On UINT32_MAX the caller must NOT bind the image (it would attach an invalid
 * GL texture and crash the driver — seen on the first menu frames under Battery
 * Saver, whose lowered clocks change frame pacing so the image isn't ready). */
static Uint32 vr_sc_acquire_wait(VRSwapchain *sc)
{
    Uint32 idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(pfn_xrAcquireSwapchainImage(sc->swapchain, &ai, &idx)))
        return UINT32_MAX;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(pfn_xrWaitSwapchainImage(sc->swapchain, &wi))) {
        /* Acquired but not usable — release it so the count stays balanced. */
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        pfn_xrReleaseSwapchainImage(sc->swapchain, &ri);
        return UINT32_MAX;
    }
    /* Disable GLES sRGB write conversion so our already-gamma-encoded values
     * are stored unchanged in the sRGB swapchain texture. Without this, GLES
     * would treat our values as linear and apply an extra gamma step → too bright. */
    if (vr_srgb_swapchain && has_srgb_write_control)
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    return idx;
}

static void vr_sc_release(VRSwapchain *sc)
{
    /* Restore sRGB write conversion before releasing */
    if (vr_srgb_swapchain && has_srgb_write_control)
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    /* No explicit glFlush here: the OpenXR runtime fences the swapchain image on
     * xrReleaseSwapchainImage/xrEndFrame, so a manual flush is redundant. Worse,
     * on Quest under Battery Saver the lowered GPU clock/power state made this
     * mid-frame glFlush null-deref inside the Adreno KGSL submit path (SIGSEGV at
     * +0x30 in libGLESv2_adreno), crashing the very first menu present. Letting
     * the runtime submit at its own sync point avoids that driver path. */
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    pfn_xrReleaseSwapchainImage(sc->swapchain, &ri);
}

/* Per-eye swapchain helpers — called by avpview.c (3D path) */
Uint32 VR_AcquireAndWaitSwapchainImage(int eye) { return vr_sc_acquire_wait(&vr_swapchains[eye]); }
void   VR_ReleaseSwapchainImage(int eye)        { vr_sc_release(&vr_swapchains[eye]); }

GLuint VR_GetSwapchainImageTexture(int eye, Uint32 idx)
{
    return vr_swapchains[eye].images[idx].image;
}

/* Battery Saver detection. Two signals, OR'd together: (1) Android
 * PowerManager.isPowerSaveMode(); (2) the LIVE display refresh rate being capped
 * below what we requested (Battery Saver forces 72 Hz). Signal 2 is the reliable
 * one on Quest — signal 1 may not surface Battery Saver on all OS builds.
 * (xrEnumerateDisplayRefreshRatesFB does NOT work — it lists all hardware-supported
 * rates regardless of the cap.) The AV-options menu uses this to pin the VR
 * refresh-rate option to 72 Hz. Returns 0 on desktop. Throttled + cached because
 * it's polled each frame while the menu is open. */
int VR_IsBatterySaverActive(void)
{
#ifdef __ANDROID__
    static int    cached  = -1;
    static Uint64 last_ms = 0;
    Uint64 now = SDL_GetTicks();
    if (cached >= 0 && (now - last_ms) < 500) return cached;
    last_ms = now;

    /* Signal 1: Android PowerManager.isPowerSaveMode() (may or may not reflect
     * Quest Battery Saver depending on OS build). */
    int psm = 0;
    JNIEnv *env      = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (env && activity) {
        jclass    actCls  = (*env)->GetObjectClass(env, activity);
        jclass    ctxCls  = (*env)->FindClass(env, "android/content/Context");
        jfieldID  pwrFld  = (*env)->GetStaticFieldID(env, ctxCls, "POWER_SERVICE",
                                                     "Ljava/lang/String;");
        jstring   pwrName = (jstring)(*env)->GetStaticObjectField(env, ctxCls, pwrFld);
        jmethodID getSvc  = (*env)->GetMethodID(env, actCls, "getSystemService",
                                                "(Ljava/lang/String;)Ljava/lang/Object;");
        jobject   pm      = (*env)->CallObjectMethod(env, activity, getSvc, pwrName);
        if (pm) {
            jclass    pmCls = (*env)->GetObjectClass(env, pm);
            jmethodID isPSM = (*env)->GetMethodID(env, pmCls, "isPowerSaveMode", "()Z");
            if (isPSM) psm = (*env)->CallBooleanMethod(env, pm, isPSM) ? 1 : 0;
            (*env)->DeleteLocalRef(env, pmCls);
            (*env)->DeleteLocalRef(env, pm);
        }
        (*env)->DeleteLocalRef(env, pwrName);
        (*env)->DeleteLocalRef(env, ctxCls);
        (*env)->DeleteLocalRef(env, actCls);
        (*env)->DeleteLocalRef(env, activity);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }

    /* Signal 2: the LIVE display refresh rate is below what we asked for. Battery
     * Saver caps the panel to 72 Hz even when we've requested 90/120, so
     * "current < requested" means the system is forcing it down. Comparing against
     * the requested rate (not a fixed 72) avoids falsely locking a user who
     * genuinely selected 72 Hz. */
    float actual = 0.0f, requested = 0.0f;
    if (pfn_xrGetDisplayRefreshRateFB && xr_session_running
        && !XR_FAILED(pfn_xrGetDisplayRefreshRateFB(xr_session, &actual)) && actual > 0.0f) {
        static const float fallback[] = { 72.0f, 80.0f, 90.0f, 120.0f };
        int count = VR_GetRefreshRateCount();
        int ri = VRRefreshRateIndex; if (ri < 0) ri = 0;
        if (count > 0) {
            if (ri >= count) ri = count - 1;
            requested = VR_GetRefreshRateByIndex(ri);
        } else {
            if (ri > 3) ri = 3;
            requested = fallback[ri];
        }
    }
    int rate_capped = (actual > 0.0f && requested > 0.0f && actual < requested - 1.0f);

    /* Signal 3: the one-time startup probe (covers "saved rate is 72 + Battery Saver",
     * which signals 1 and 2 can't detect). */
    int result = (psm || rate_capped || vr_bs_probe_result);
    cached = result;
    return result;
#else
    return 0;
#endif
}

int VR_IsIn3DMode(void)
{
    return xr_session_running && !xr_2d_mode;
}

/* Non-zero while an OpenXR session is presenting (2D quad or 3D eyes). The
 * desktop paths (the MSAA target) use this to stand down while the headset owns
 * the frame; the flat build has a stub returning 0. */
int VR_SessionActive(void)
{
    return xr_enabled && xr_session_running;
}

/* "Are we rendering to a headset at all", as opposed to VR_SessionActive()'s
   "is the headset presenting right now". Deliberately does NOT test
   xr_session_running: this answers a question asked once during startup, before
   the runtime has moved the session to READY, and it must not flip afterwards.
   Use it for one-shot setup that depends on the display being an HMD (see the
   gamma bias in gammacontrol.cpp); use VR_SessionActive() for per-frame
   decisions. False on the non-VR Android phone flavor, where XR init is skipped
   entirely (AVP_DISABLE_XR), and on a PCVR exe running flat. */
int VR_HeadsetActive(void)
{
    return xr_enabled ? 1 : 0;
}

void VR_Set2DViewport(void)
{
    /* Called from ThisFramesRenderingHasBegun. Shrinks the GL viewport to exactly
       640x480 so InGameFlipBuffers can do a 1:1 readback with no downscaling. */
    if (xr_enabled && xr_session_running && xr_2d_mode)
        pglViewport(0, 0, 640, 480);
}

XrTime VR_GetDisplayTime(void) { return xr_predicted_display_time; }
XrSpace VR_GetLocalSpace(void) { return xr_local_space; }
XrResult VR_LocateViews(XrViewLocateInfo *info, XrViewState *state,
                        Uint32 count, Uint32 *count_out, XrView *views)
{
    return pfn_xrLocateViews(xr_session, info, state, count, count_out, views);
}

static XrFrameState xr_frame_state = { XR_TYPE_FRAME_STATE };

static void apply_refresh_rate_if_changed(void);  /* defined after render_frame */

void VR_WaitAndBeginFrame(void)
{
    if (!xr_session_running) return;

    /* Apply display refresh rate if the setting changed since last frame. */
    apply_refresh_rate_if_changed();

    XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
    pfn_xrWaitFrame(xr_session, &wait_info, &xr_frame_state);
    xr_predicted_display_time = xr_frame_state.predictedDisplayTime;
    XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
    pfn_xrBeginFrame(xr_session, &begin_info);

    /* Locate views here (in main.c, same pattern as the working 2D path in render_frame)
     * rather than via VR_LocateViews() wrapper from avpview.c, which returns
     * XR_ERROR_VALIDATION_FAILURE (-1) for reasons not yet understood. */
    if (view_count > 0 && xr_local_space != XR_NULL_HANDLE && xr_views != NULL) {
        XrViewState view_state = { XR_TYPE_VIEW_STATE };
        XrViewLocateInfo locate_info = { XR_TYPE_VIEW_LOCATE_INFO };
        locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate_info.space = xr_local_space;
        locate_info.displayTime = xr_predicted_display_time;
        Uint32 view_count_out = 0;
        XrResult result = pfn_xrLocateViews(xr_session, &locate_info, &view_state,
                                            view_count, &view_count_out, xr_views);

        /* Update HMD horizontal heading for locomotion (pmove.c reads xr_hmd_move_sin/cos).
         * Derivation: OpenXR -Z is forward; game +Z is forward; game X = OpenXR X. */
        if (!XR_FAILED(result) && view_count_out > 0) {
            float qx = xr_views[0].pose.orientation.x;
            float qy = xr_views[0].pose.orientation.y;
            float qz = xr_views[0].pose.orientation.z;
            float qw = xr_views[0].pose.orientation.w;
            /* Game forward in world space (projected to horizontal plane) */
            float fwd_x = -2.0f * (qx * qz + qw * qy); /* sin(game_yaw) when level */
            float fwd_z = 1.0f - 2.0f * (qx * qx + qy * qy); /* cos(game_yaw) */
            float mag = SDL_sqrtf(fwd_x * fwd_x + fwd_z * fwd_z);
            if (mag > 0.001f) {
                float nx = fwd_x / mag;
                float nz = fwd_z / mag;
                if (xr_snap_yaw != 0) {
                    float snap_rad = (float)xr_snap_yaw * SDL_PI_F / 2048.0f;
                    float snap_s   = SDL_sinf(snap_rad);
                    float snap_c   = SDL_cosf(snap_rad);
                    float rx = nx * snap_c + nz * snap_s;
                    float rz = -nx * snap_s + nz * snap_c;
                    nx = rx;
                    nz = rz;
                }
                xr_hmd_move_sin = (int)(nx * 65536.0f);
                xr_hmd_move_cos = (int)(nz * 65536.0f);
            }
        }

    }

    /* Locate controller grip spaces */
    if (pfn_xrLocateSpace && xr_predicted_display_time > 0) {
        XrSpaceLocation grip_loc = { XR_TYPE_SPACE_LOCATION };
        xr_grip_left_valid  = 0;
        xr_grip_right_valid = 0;
        if (xr_left_grip_space != XR_NULL_HANDLE) {
            XrResult r = pfn_xrLocateSpace(xr_left_grip_space, xr_local_space,
                                           xr_predicted_display_time, &grip_loc);
            if (!XR_FAILED(r) &&
                (grip_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (grip_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                xr_grip_pose_left  = grip_loc.pose;
                xr_grip_left_valid = 1;
            }
        }
        if (xr_right_grip_space != XR_NULL_HANDLE) {
            XrResult r = pfn_xrLocateSpace(xr_right_grip_space, xr_local_space,
                                           xr_predicted_display_time, &grip_loc);
            if (!XR_FAILED(r) &&
                (grip_loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                (grip_loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                xr_grip_pose_right  = grip_loc.pose;
                xr_grip_right_valid = 1;
            }
        }
    }
}

XrFrameState* VR_GetFrameState(void) { return &xr_frame_state; }

float VR_GetTargetHz(void)
{
    return (xr_frame_state.predictedDisplayPeriod > 0)
           ? (1000000000.0f / (float)xr_frame_state.predictedDisplayPeriod)
           : 0.0f;
}

static void apply_refresh_rate_if_changed(void)
{
    /* One-time startup Battery Saver probe. Only needed when the saved rate is 72 Hz
     * (a >72 selection is already detected by the live-rate signal). We briefly request
     * 90 Hz and, after letting it settle, check whether the panel actually reached it;
     * if not, Battery Saver is capping us → flag it. The normal rate-apply below is
     * held off until the probe finishes so the two don't fight over the rate. */
    static int probe_state  = 0;   /* 0=start, 1=waiting, 2=done */
    static int probe_frames = 0;
    if (probe_state != 2) {
        if (!pfn_xrRequestDisplayRefreshRateFB || !pfn_xrGetDisplayRefreshRateFB
            || VRRefreshRateIndex > 0) {
            probe_state = 2;                 /* nothing to probe; fall through to apply */
        } else if (probe_state == 0) {
            pfn_xrRequestDisplayRefreshRateFB(xr_session, 90.0f);
            probe_frames = 0;
            probe_state  = 1;
            return;                          /* let the request settle; don't apply yet */
        } else {                             /* probe_state == 1: waiting */
            if (++probe_frames < 30) return; /* ~0.4 s at 72 Hz */
            float actual = 0.0f;
            if (!XR_FAILED(pfn_xrGetDisplayRefreshRateFB(xr_session, &actual)) && actual > 0.0f)
                vr_bs_probe_result = (actual < 89.0f) ? 1 : 0; /* asked 90, got less → Battery Saver */
            probe_state = 2;                 /* fall through to apply the real rate now */
        }
    }

    static int vr_applied_refresh = -1;
    if (VRRefreshRateIndex != vr_applied_refresh && pfn_xrRequestDisplayRefreshRateFB) {
        /* Rates come from the headset now, not a hardcoded list. The old fixed
           set survives only as a fallback for a runtime without the extension,
           so the option still does something sensible there. */
        static const float fallback[] = {72.0f, 80.0f, 90.0f, 120.0f};
        int   count = VR_GetRefreshRateCount();
        int   idx   = VRRefreshRateIndex;
        float hz;
        if (idx < 0) idx = 0;
        if (count > 0) {
            if (idx >= count) idx = count - 1;
            hz = VR_GetRefreshRateByIndex(idx);
        } else {
            if (idx > 3) idx = 3;
            hz = fallback[idx];
        }
        /* Persist the RATE, not the index — see VR_GetRefreshRateIndexForHz. */
        VRRefreshRateHz = (int)(hz + 0.5f);
        XrResult rr = pfn_xrRequestDisplayRefreshRateFB(xr_session, hz);
        SDL_Log("XR: set refresh rate %.0f Hz -> %d", hz, (int)rr);
        vr_applied_refresh = VRRefreshRateIndex;
    }
}

static void render_frame(void)
{
    if (!xr_session_running) return;

    XrFrameState frame_state;
    XrResult result;

    if (xr_2d_mode) {
        /* 2D menus: own the full xrWaitFrame/xrBeginFrame here */
        apply_refresh_rate_if_changed();
        frame_state = (XrFrameState){ XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
        result = pfn_xrWaitFrame(xr_session, &wait_info, &frame_state);
        if (XR_FAILED(result)) return;
        xr_frame_state = frame_state;
        xr_predicted_display_time = frame_state.predictedDisplayTime;
        XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
        result = pfn_xrBeginFrame(xr_session, &begin_info);
        if (XR_FAILED(result)) return;
    } else {
        /* 3D game: VR_WaitAndBeginFrame() + AvpShowViewsVR() already ran. */
        frame_state = xr_frame_state;
    }

    XrCompositionLayerProjectionView *proj_views = NULL;
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerQuad quad_layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerQuad score_quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    Uint32 layer_count = 0;
    /* [0] = 3D projection (or 2D menu quad); [1] = floating MP scoreboard overlay. */
    const XrCompositionLayerBaseHeader *layers[2] = {0};

    if (frame_state.shouldRender && view_count > 0 && vr_swapchains != NULL) {

        /* Re-capture head direction each time the menu is opened. */
        static float menu_quad_cx = 0.0f, menu_quad_cy = 1.6f, menu_quad_cz = 0.0f,
                     menu_quad_yaw = 0.0f;
        static int   menu_quad_ready = 0;
        if (!xr_2d_mode) menu_quad_ready = 0;
        /* Recenter (Reset View) while a menu/intro is up: drop the anchor so the
         * quad re-captures the current head facing on this frame and snaps in front
         * of you, instead of staying where it was first opened. */
        if (vr_menu_reanchor) { menu_quad_ready = 0; vr_menu_reanchor = 0; }

        /* Body-lock anchor for the floating scoreboard: captured the first frame
         * the board appears (mirrors menu_quad_ready) and held while it stays up,
         * so you can look around it. Reset when it's hidden or on recenter. */
        static float score_quad_cx = 0.0f, score_quad_cy = 1.6f, score_quad_cz = 0.0f,
                     score_quad_yaw = 0.0f;
        static int   score_quad_ready = 0;
        if (!vr_scoreboard_visible || vr_score_reanchor) score_quad_ready = 0;
        vr_score_reanchor = 0;

        if (xr_2d_mode) {
            /* Locate views for 2D menu */
            XrViewState vs = { XR_TYPE_VIEW_STATE };
            XrViewLocateInfo li = { XR_TYPE_VIEW_LOCATE_INFO };
            li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            li.space       = xr_local_space;
            li.displayTime = frame_state.predictedDisplayTime;
            Uint32 vc_out  = 0;
            result = pfn_xrLocateViews(xr_session, &li, &vs, view_count, &vc_out, xr_views);
            if (XR_FAILED(result)) { SDL_Log("xrLocateViews failed"); goto endFrame; }

            /* On the first frame of each menu open, capture head yaw so the quad
             * appears 2 m ahead of wherever the user is currently facing. */
            if (!menu_quad_ready && vc_out >= 1
                    && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                float qw = xr_views[0].pose.orientation.w;
                float qx = xr_views[0].pose.orientation.x;
                float qy = xr_views[0].pose.orientation.y;
                float qz = xr_views[0].pose.orientation.z;
                /* Forward direction = quaternion * (0,0,-1) projected onto XZ. */
                float fx = -2.0f*(qx*qz + qw*qy);
                float fz =  2.0f*(qx*qx + qy*qy) - 1.0f;
                float len = SDL_sqrtf(fx*fx + fz*fz);
                if (len < 0.001f) { fx = 0.0f; fz = -1.0f; len = 1.0f; }
                fx /= len; fz /= len;
                float hx = (vc_out >= 2)
                    ? (xr_views[0].pose.position.x + xr_views[1].pose.position.x) * 0.5f
                    : xr_views[0].pose.position.x;
                float hz = (vc_out >= 2)
                    ? (xr_views[0].pose.position.z + xr_views[1].pose.position.z) * 0.5f
                    : xr_views[0].pose.position.z;
                float hy = (vc_out >= 2)
                    ? (xr_views[0].pose.position.y + xr_views[1].pose.position.y) * 0.5f
                    : xr_views[0].pose.position.y;
                menu_quad_cx  = hx;
                menu_quad_cy  = hy;
                menu_quad_cz  = hz;
                /* atan2(fx, -fz): yaw=0 → looking -Z, yaw=π/2 → looking +X */
                menu_quad_yaw = SDL_atan2f(fx, -fz);
                menu_quad_ready = 1;
            }
        }

        proj_views = SDL_calloc(view_count, sizeof(XrCompositionLayerProjectionView));

        if (xr_2d_mode) {
            /* Upload the menu pixels, then render the menu FLAT into the dedicated
             * (non-MSAA) menu swapchain. The quad layer below presents that image.
             * Using a separate swapchain — never touched by the 3D MSAA eye pass —
             * is what fixes the in-game pause crash: reusing an MSAA-rendered eye
             * image for the flat menu made the Quest compositor null-deref under
             * Battery Saver. */
            upload_menu_gles_texture();

            VRSwapchain *sc = &vr_menu_swapchain;
            Uint32 idx = vr_sc_acquire_wait(sc);
            if (idx == UINT32_MAX) goto endFrame; /* image not ready — submit nothing */
            GLuint sc_tex = sc->images[idx].image;

            glBindFramebuffer(GL_FRAMEBUFFER, menu_fbo_2d);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, sc_tex, 0);

            /* Guard an incomplete colour target (NULL DRAW_BUFFER0 → Adreno
             * null-deref at submit, seen under Battery Saver). */
            if (sc_tex == 0 ||
                glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, 0, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                vr_sc_release(sc);
                goto endFrame;
            }

            glViewport(0, 0, sc->size.width, sc->size.height);
            glDisable(GL_DEPTH_TEST);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);

            if (quad_program && quad_vao && menu_gles_tex) {
                glUseProgram(quad_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, menu_gles_tex);
                glUniform1i(quad_u_tex, 0);

                /* Fill the whole image with the flat menu: map the quad's metre
                 * coords (±QUAD_HALF_W/H, any z) straight to NDC, keeping the VAO's
                 * UVs (correct orientation). The quad layer's 4:3 size sets aspect. */
                Mat4 mvp = {{ 1.0f/QUAD_HALF_W, 0.0f, 0.0f, 0.0f,
                              0.0f, 1.0f/QUAD_HALF_H, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 1.0f }};
                glUniformMatrix4fv(quad_u_mvp, 1, GL_FALSE, mvp.m);

                glBindVertexArray(quad_vao);
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
                glBindVertexArray(0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glUseProgram(0);
            }

            glEnable(GL_DEPTH_TEST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            vr_sc_release(sc);
        } else {
            /* 3D game: AvpShowViewsVR() already rendered directly to swapchain.
             * Just set up projection views for xrEndFrame. */
            for (Uint32 i = 0; i < view_count; i++) {
                VRSwapchain *sc = &vr_swapchains[i];
                float tan_hx = (SDL_tanf(SDL_fabsf(xr_views[i].fov.angleLeft))
                              + SDL_tanf(SDL_fabsf(xr_views[i].fov.angleRight))) * 0.5f;
                float tan_hy = (SDL_tanf(SDL_fabsf(xr_views[i].fov.angleUp))
                              + SDL_tanf(SDL_fabsf(xr_views[i].fov.angleDown))) * 0.5f;
                XrFovf sym_fov = xr_views[i].fov;
                if (tan_hx > 0.01f && tan_hy > 0.01f) {
                    sym_fov.angleLeft  = -SDL_atanf(tan_hx);
                    sym_fov.angleRight =  SDL_atanf(tan_hx);
                    sym_fov.angleUp    =  SDL_atanf(tan_hy);
                    sym_fov.angleDown  = -SDL_atanf(tan_hy);
                }
                proj_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                proj_views[i].pose = xr_views[i].pose;
                proj_views[i].fov  = sym_fov;
                proj_views[i].subImage.swapchain        = sc->swapchain;
                proj_views[i].subImage.imageRect.offset.x = 0;
                proj_views[i].subImage.imageRect.offset.y = 0;
                proj_views[i].subImage.imageRect.extent  = sc->size;
                proj_views[i].subImage.imageArrayIndex   = 0;
            }
        }

        if (xr_2d_mode) {
            /* Present the 2D menu as a world-locked QUAD layer rather than a stereo
             * PROJECTION layer. The Quest compositor null-derefs on the 2nd composite
             * of our projection layer under Battery Saver (frame 0 submits fine,
             * frame 1 crashes inside xrEndFrame at GL_DRAW_BUFFER0); the quad-layer
             * compositor path is unaffected. The dedicated menu swapchain was filled
             * flat with the menu above; the quad is anchored in local space 2 m ahead of where
             * the head was facing when the menu opened (menu_quad_*), so — like the
             * old projection menu — it stays put in the world as you turn your head.
             *
             * Placement: position = captured head pos + 2 m along captured forward
             * (fwd = (sin yaw, 0, -cos yaw)). A quad layer shows its image on the +Z
             * side of its pose, so orient by a Y rotation of -yaw to point +Z back
             * at the head: quat = (0, -sin(yaw/2), 0, cos(yaw/2)). */
            float hy = menu_quad_yaw * 0.5f;
            VRSwapchain *q = &vr_menu_swapchain;
            quad_layer.layerFlags    = 0;
            quad_layer.space         = xr_local_space;
            quad_layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad_layer.subImage.swapchain          = q->swapchain;
            quad_layer.subImage.imageRect.offset.x = 0;
            quad_layer.subImage.imageRect.offset.y = 0;
            quad_layer.subImage.imageRect.extent   = q->size;
            quad_layer.subImage.imageArrayIndex    = 0;
            quad_layer.pose.orientation.x = 0.0f;
            quad_layer.pose.orientation.y = -SDL_sinf(hy);
            quad_layer.pose.orientation.z = 0.0f;
            quad_layer.pose.orientation.w =  SDL_cosf(hy);
            quad_layer.pose.position.x = menu_quad_cx + 2.0f * SDL_sinf(menu_quad_yaw);
            quad_layer.pose.position.y = menu_quad_cy;
            quad_layer.pose.position.z = menu_quad_cz - 2.0f * SDL_cosf(menu_quad_yaw);
            quad_layer.size.width  = 2.0f;         /* metres (4:3 with height) */
            quad_layer.size.height = 1.5f;
            layers[0]   = (XrCompositionLayerBaseHeader*)&quad_layer;
            layer_count = 1;
        } else {
            layer.space     = xr_local_space;
            layer.viewCount = view_count;
            layer.views     = proj_views;
            layers[0]       = (XrCompositionLayerBaseHeader*)&layer;
            layer_count     = 1;

            /* Floating MP scoreboard: composite the score table (rendered to
             * vr_score_tex by AvpShowViewsVR) as a world-locked quad ON TOP of the
             * 3D projection layer, so it stops being glued to the head. Body-locked
             * to the heading captured when it appeared, so you can look around it. */
            if (vr_scoreboard_visible && vr_score_quad_ready
                && quad_program && quad_vao && menu_fbo_2d && vr_score_tex
                && vr_score_swapchain.swapchain != XR_NULL_HANDLE) {

                /* Capture head pos/yaw the first frame the board appears. */
                if (!score_quad_ready && view_count >= 1) {
                    float qw = xr_views[0].pose.orientation.w;
                    float qx = xr_views[0].pose.orientation.x;
                    float qy = xr_views[0].pose.orientation.y;
                    float qz = xr_views[0].pose.orientation.z;
                    float fx = -2.0f*(qx*qz + qw*qy);
                    float fz =  2.0f*(qx*qx + qy*qy) - 1.0f;
                    float len = SDL_sqrtf(fx*fx + fz*fz);
                    if (len < 0.001f) { fx = 0.0f; fz = -1.0f; len = 1.0f; }
                    fx /= len; fz /= len;
                    score_quad_cx = (view_count >= 2)
                        ? (xr_views[0].pose.position.x + xr_views[1].pose.position.x)*0.5f
                        : xr_views[0].pose.position.x;
                    score_quad_cy = (view_count >= 2)
                        ? (xr_views[0].pose.position.y + xr_views[1].pose.position.y)*0.5f
                        : xr_views[0].pose.position.y;
                    score_quad_cz = (view_count >= 2)
                        ? (xr_views[0].pose.position.z + xr_views[1].pose.position.z)*0.5f
                        : xr_views[0].pose.position.z;
                    score_quad_yaw = SDL_atan2f(fx, -fz);
                    score_quad_ready = 1;
                }

                /* Straight copy vr_score_tex into a score swapchain image (blend OFF
                 * so the premultiplied-alpha text — opaque glyphs, clear elsewhere —
                 * is preserved for the compositor). Reuses menu_fbo_2d as scratch. */
                VRSwapchain *q = &vr_score_swapchain;
                Uint32 idx = vr_sc_acquire_wait(q);
                if (idx != UINT32_MAX) {
                    GLuint sc_tex = q->images[idx].image;
                    glBindFramebuffer(GL_FRAMEBUFFER, menu_fbo_2d);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, sc_tex, 0);
                    if (sc_tex != 0 &&
                        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                        glViewport(0, 0, q->size.width, q->size.height);
                        glDisable(GL_DEPTH_TEST);
                        glDisable(GL_BLEND);
                        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                        glClear(GL_COLOR_BUFFER_BIT);
                        glUseProgram(quad_program);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, vr_score_tex);
                        glUniform1i(quad_u_tex, 0);
                        /* Negative Y: the menu quad's UVs assume a top-down CPU surface,
                         * but the score FBO is GL-rendered (bottom-left origin), so
                         * without the flip the text comes out upside down. */
                        Mat4 mvp = {{ 1.0f/QUAD_HALF_W, 0.0f, 0.0f, 0.0f,
                                      0.0f, -1.0f/QUAD_HALF_H, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 1.0f }};
                        glUniformMatrix4fv(quad_u_mvp, 1, GL_FALSE, mvp.m);
                        glBindVertexArray(quad_vao);
                        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
                        glBindVertexArray(0);
                        glBindTexture(GL_TEXTURE_2D, 0);
                        glUseProgram(0);
                        glEnable(GL_DEPTH_TEST);
                        /* Restore GL_BLEND: the game enables it once at init (main.c
                         * InitialiseRenderer) and thereafter only changes blend FUNC,
                         * assuming it stays enabled. RestoreGameShaderState does NOT
                         * re-enable it, so leaving it off here (from the straight-copy
                         * blit above) collapses every later translucent/additive pass —
                         * world renders black (only bright halos survive), HUD and menu
                         * highlights vanish — and persists across frames. */
                        glEnable(GL_BLEND);
                    }
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                           GL_TEXTURE_2D, 0, 0);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    vr_sc_release(q);

                    if (score_quad_ready) {
                        float shy = score_quad_yaw * 0.5f;
                        /* Premultiplied-alpha blend so the semi-transparent dark panel
                         * (cleared to (0,0,0,SCORE_PANEL_ALPHA) in avpview.c) lets the
                         * world show through, dimmed. */
                        score_quad.layerFlags    = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                        score_quad.space         = xr_local_space;
                        score_quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                        score_quad.subImage.swapchain          = q->swapchain;
                        score_quad.subImage.imageRect.offset.x = 0;
                        score_quad.subImage.imageRect.offset.y = 0;
                        score_quad.subImage.imageRect.extent   = q->size;
                        score_quad.subImage.imageArrayIndex    = 0;
                        score_quad.pose.orientation.x = 0.0f;
                        score_quad.pose.orientation.y = -SDL_sinf(shy);
                        score_quad.pose.orientation.z = 0.0f;
                        score_quad.pose.orientation.w =  SDL_cosf(shy);
                        score_quad.pose.position.x = score_quad_cx + 2.0f * SDL_sinf(score_quad_yaw);
                        score_quad.pose.position.y = score_quad_cy;
                        score_quad.pose.position.z = score_quad_cz - 2.0f * SDL_cosf(score_quad_yaw);
                        score_quad.size.width  = 1.6f;   /* metres (4:3) */
                        score_quad.size.height = 1.2f;
                        layers[layer_count++] = (XrCompositionLayerBaseHeader*)&score_quad;
                    }
                }
            }
        }
    }

    endFrame:;
    XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
    end_info.displayTime          = frame_state.predictedDisplayTime;
    end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end_info.layerCount           = layer_count;
    end_info.layers               = layers;

    pfn_xrEndFrame(xr_session, &end_info);

    if (proj_views) SDL_free(proj_views);
}

/* ========================================================================
 * OpenXR Setup End
 * ======================================================================== */
#endif
/* ** */

static void IngameKeyboardInput_ClearBuffer(void)
{
    // clear the keyboard state
    memset((void*) KeyboardInput, 0, MAX_NUMBER_OF_INPUT_KEYS);
    GotAnyKey = 0;
}

void DirectReadKeyboard()
{
}

void DirectReadMouse()
{
}

void ReadJoysticks()
{
int axes, balls, hats;
    Uint8 hat;

    JoystickData.dwXpos = 32768; /* centred */
    JoystickData.dwYpos = 32768;
    JoystickData.dwRpos = 32768;
    JoystickData.dwUpos = 32768;
    JoystickData.dwVpos = 32768;
    JoystickData.dwPOV = (DWORD) -1;

#ifdef AVP_XR
    /* On VR builds, OpenXR owns the controllers so GotJoystick is never set.
     * Skip the SDL joystick gate and go straight to XR input. */
#else
    if (!GotJoystick) {
        return;
    }
#endif

#ifdef AVP_XR
    /* In VR, OpenXR owns the controllers (on Quest the Android GameController
       API does not even receive axis events while an XrSession is running).
       Read the left thumbstick through the OpenXR action system instead. */
    if (xr_session && xr_session_running && xr_input_action_set && xr_left_stick_action) {
        /* Sync actions to get the current frame's input state.
         * Guard on xr_session_running: xrSyncActions requires xrBeginSession to have
         * been called first (i.e. the session must be in a running state). */
        XrActiveActionSet active = { xr_input_action_set, XR_NULL_PATH };
        XrActionsSyncInfo sync_info = { XR_TYPE_ACTIONS_SYNC_INFO };
        sync_info.countActiveActionSets = 1;
        sync_info.activeActionSets = &active;
        pfn_xrSyncActions(xr_session, &sync_info);
        /* XR_SESSION_NOT_FOCUSED is a success code — session is VISIBLE but not focused.
         * Input won't be active in that case but we still read what we can. */

        XrActionStateGetInfo get_info = { XR_TYPE_ACTION_STATE_GET_INFO };
        get_info.action = xr_left_stick_action;
        XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
        if (XR_SUCCEEDED(pfn_xrGetActionStateVector2f(xr_session, &get_info, &state)) && state.isActive) {
            xr_left_stick_x = state.currentState.x;
            xr_left_stick_y = state.currentState.y;
        }
        /* Convert OpenXR [-1,1] floats to Win95 JOYINFOEX 0..65535 convention. */
        JoystickData.dwXpos = (DWORD)((xr_left_stick_x  * 32767.0f) + 32768.0f);
        JoystickData.dwYpos = (DWORD)((-xr_left_stick_y * 32767.0f) + 32768.0f);

        /* The left stick is delivered by OpenXR, not the SDL gamepad API, so the
         * usr_io.c locomotion consumer (gated on GotJoystick) must be enabled here:
         * on Quest, SDL_EVENT_GAMEPAD_ADDED is unreliable while an XrSession owns the
         * controllers, so GotJoystick may never be set and movement silently dies.
         * Also force dwPOV to "centered" (-1): if no GAMEPAD_ADDED ever initialised
         * JoystickData it is zero-filled, and dwPOV==0 reads as a POV hat pushed
         * forward → phantom auto-walk. (Rudder/trackerball stay gated off.) */
        JoystickData.dwPOV = (DWORD)-1;
        GotJoystick = 1;

        /* Right stick: X turns (snap or smooth, per Controller Config), Y = next weapon. */
        if (xr_right_stick_action) {
            static bool xr_snap_armed = true;
            static bool xr_next_weapon_armed = true;
            static bool xr_prev_weapon_armed = true;
            const float SNAP_THRESHOLD  = 0.6f;
            const float SNAP_REARM_ZONE = 0.3f;
            const float SMOOTH_DEADZONE = (float)VRSmoothDeadzone * 0.05f; /* 0..10 -> 0.0..0.5 */
            /* Snap angles in game units (4096 = full circle): 30/45/60/90 degrees. */
            static const int SNAP_ANGLES[4] = { 341, 512, 683, 1024 };

            XrActionStateGetInfo rget = { XR_TYPE_ACTION_STATE_GET_INFO };
            rget.action = xr_right_stick_action;
            XrActionStateVector2f rstate = { XR_TYPE_ACTION_STATE_VECTOR2F };
            float rx = 0.0f, ry = 0.0f;
            if (XR_SUCCEEDED(pfn_xrGetActionStateVector2f(xr_session, &rget, &rstate)) && rstate.isActive) {
                rx = rstate.currentState.x;
                ry = rstate.currentState.y;
            }

            /* X axis: turning */
            bool smooth_turning = false;
            if (VRTurnMode == 1) {
                /* Smooth turn: continuously accumulate yaw, scaled by stick deflection.
                 * Speed slider 0..100 maps to ~60..180 deg/sec. RealFrameTime is fixed-
                 * point seconds (65536 = 1s). */
                if (rx > SMOOTH_DEADZONE || rx < -SMOOTH_DEADZONE) {
                    extern int RealFrameTime;
                    float dt          = (float)RealFrameTime / 65536.0f;
                    float deg_per_sec = 60.0f + (float)VRSmoothTurnSpeed * 12.0f;
                    float delta_deg   = deg_per_sec * dt * rx;   /* rx carries sign + magnitude */
                    int   delta_units = (int)(delta_deg * 4096.0f / 360.0f);
                    xr_snap_yaw = (xr_snap_yaw + delta_units) & 4095;
                    smooth_turning = true;
                }
                xr_snap_armed = true; /* keep snap re-armed so a mode switch is clean */
            } else {
                /* Snap turn: debounced, configurable angle. */
                int snap_angle = SNAP_ANGLES[(VRSnapAngleIndex >= 0 && VRSnapAngleIndex < 4) ? VRSnapAngleIndex : 1];
                if (xr_snap_armed) {
                    if (rx > SNAP_THRESHOLD) {
                        xr_snap_yaw = (xr_snap_yaw + snap_angle) & 4095;
                        xr_snap_armed = false;
                    } else if (rx < -SNAP_THRESHOLD) {
                        xr_snap_yaw = (xr_snap_yaw - snap_angle) & 4095;
                        xr_snap_armed = false;
                    }
                } else if (rx > -SNAP_REARM_ZONE && rx < SNAP_REARM_ZONE) {
                    xr_snap_armed = true;
                }
            }

            /* Comfort vignette: fade in while smooth-turning, fade out otherwise. */
            {
                extern int RealFrameTime;
                float dt     = (float)RealFrameTime / 65536.0f;
                float target = (VRVignetteOn && smooth_turning) ? 1.0f : 0.0f;
                float step   = 6.0f * dt;   /* ~1/6 s full fade */
                if (vr_vignette_strength < target)
                    vr_vignette_strength = SDL_min(target, vr_vignette_strength + step);
                else
                    vr_vignette_strength = SDL_max(target, vr_vignette_strength - step);
            }

            /* Y axis: stick up → next weapon, stick down → previous weapon
             * (gameplay only, each edge-triggered with its own re-arm dead zone). */
            xr_right_thumbstick_click_pressed = 0;
            xr_right_thumbstick_down_pressed  = 0;
            if (!xr_2d_mode) {
                if (xr_next_weapon_armed && ry > SNAP_THRESHOLD) {
                    xr_right_thumbstick_click_pressed = 1;
                    xr_next_weapon_armed = false;
                } else if (ry < SNAP_REARM_ZONE) {
                    xr_next_weapon_armed = true;
                }

                if (xr_prev_weapon_armed && ry < -SNAP_THRESHOLD) {
                    xr_right_thumbstick_down_pressed = 1;
                    xr_prev_weapon_armed = false;
                } else if (ry > -SNAP_REARM_ZONE) {
                    xr_prev_weapon_armed = true;
                }
            }
        }

        /* Right trigger → primary fire (gameplay only). */
        xr_trigger_right_pressed = 0;
        if (!xr_2d_mode && xr_right_trigger_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo tget = { XR_TYPE_ACTION_STATE_GET_INFO };
            tget.action = xr_right_trigger_action;
            XrActionStateBoolean tstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &tget, &tstate))
                    && tstate.isActive)
                xr_trigger_right_pressed = tstate.currentState ? 1 : 0;
        }

        /* Right grip squeeze → secondary fire (gameplay only). */
        xr_grip_right_squeeze_pressed = 0;
        if (!xr_2d_mode && xr_right_squeeze_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo sget = { XR_TYPE_ACTION_STATE_GET_INFO };
            sget.action = xr_right_squeeze_action;
            XrActionStateBoolean sstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &sget, &sstate))
                    && sstate.isActive)
                xr_grip_right_squeeze_pressed = sstate.currentState ? 1 : 0;
        }

        /* Left grip squeeze → Predator recall disc (gameplay only). Recall_Disc has
         * its own field-charge gate and is safe to call every frame while held, so
         * this mirrors the keyboard binding's held signal. */
        xr_left_squeeze_gameplay_pressed = 0;
        if (!xr_2d_mode && xr_left_squeeze_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo lsget = { XR_TYPE_ACTION_STATE_GET_INFO };
            lsget.action = xr_left_squeeze_action;
            XrActionStateBoolean lsstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &lsget, &lsstate))
                    && lsstate.isActive)
                xr_left_squeeze_gameplay_pressed = lsstate.currentState ? 1 : 0;
        }

        /* A button → operate (gameplay only). Read the raw state ONCE here and
         * derive both signals from it: xr_a_button_pressed keeps its
         * gameplay-only meaning, while xr_a_button_restart_edge is a press edge
         * that also fires in 2D mode. The death screen renders as a 2D quad
         * (xr_2d_mode == true), so a gameplay-gated signal can never see the
         * press that restarts the level. */
        {
            int a_raw = 0;
            if (xr_a_button_action && pfn_xrGetActionStateBoolean) {
                XrActionStateGetInfo aget = { XR_TYPE_ACTION_STATE_GET_INFO };
                aget.action = xr_a_button_action;
                XrActionStateBoolean astate = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &aget, &astate))
                        && astate.isActive)
                    a_raw = astate.currentState ? 1 : 0;
            }
            xr_a_button_pressed = xr_2d_mode ? 0 : a_raw;

            /* Edge, not level: the A press that confirms "restart" is usually
             * still held while the level reloads, and a level-triggered restart
             * would then fire again the moment the player died next. Consumed by
             * CorpseMovement (pmove.c). */
            {
                static int a_restart_prev = 0;
                xr_a_button_restart_edge = (a_raw && !a_restart_prev) ? 1 : 0;
                a_restart_prev = a_raw;
            }
        }

        /* Left thumbstick click → crouch (gameplay only). */
        xr_left_thumbstick_click_pressed = 0;
        if (!xr_2d_mode && xr_left_thumbstick_click_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo lget = { XR_TYPE_ACTION_STATE_GET_INFO };
            lget.action = xr_left_thumbstick_click_action;
            XrActionStateBoolean lstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &lget, &lstate))
                    && lstate.isActive)
                xr_left_thumbstick_click_pressed = lstate.currentState ? 1 : 0;
        }

        /* B button → jump (gameplay only). */
        xr_b_button_pressed = 0;
        if (!xr_2d_mode && xr_b_button_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo bget = { XR_TYPE_ACTION_STATE_GET_INFO };
            bget.action = xr_b_button_action;
            XrActionStateBoolean bstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bstate))
                    && bstate.isActive)
                xr_b_button_pressed = bstate.currentState ? 1 : 0;
        }

        /* Right thumbstick click → throw flare (gameplay only, edge-triggered). */
        {
            static int prev = 0;
            xr_left_trigger_pressed = 0;
            if (!xr_2d_mode && xr_right_thumbstick_click_action && pfn_xrGetActionStateBoolean) {
                XrActionStateGetInfo rget = { XR_TYPE_ACTION_STATE_GET_INFO };
                rget.action = xr_right_thumbstick_click_action;
                XrActionStateBoolean rstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &rget, &rstate))
                        && rstate.isActive) {
                    int cur = rstate.currentState ? 1 : 0;
                    if (cur && !prev)
                        xr_left_trigger_pressed = 1;
                    prev = cur;
                }
            } else {
                prev = 0;
            }
        }

        /* Y button → vision toggle in gameplay (Marine Image Intensifier / Alien Alt
         * Vision use the held signal). The Predator distinguishes a short tap (Cycle
         * Vision Mode) from a long hold (Zoom In): _tap fires on release if the press
         * stayed under the threshold, _long_edge fires once the moment the hold passes
         * it, and a long hold suppresses the tap so zooming never also cycles vision. */
        xr_y_button_gameplay_pressed   = 0;
        xr_y_button_gameplay_edge      = 0;
        xr_y_button_gameplay_tap       = 0;
        xr_y_button_gameplay_long_edge = 0;
        if (!xr_2d_mode && xr_y_button_action && pfn_xrGetActionStateBoolean) {
            static int   y_prev = 0;
            static float y_hold_secs = 0.0f;
            static int   y_long_fired = 0;
            const float  Y_LONG_PRESS_SECS = 0.5f; /* hold past this → zoom, not vision cycle */
            XrActionStateGetInfo yget = { XR_TYPE_ACTION_STATE_GET_INFO };
            yget.action = xr_y_button_action;
            XrActionStateBoolean ystate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &yget, &ystate))
                    && ystate.isActive) {
                int y_cur = ystate.currentState ? 1 : 0;
                xr_y_button_gameplay_pressed = y_cur;
                if (y_cur && !y_prev) {
                    xr_y_button_gameplay_edge = 1;
                    y_hold_secs  = 0.0f;
                    y_long_fired = 0;
                }
                if (y_cur) {
                    extern int RealFrameTime;
                    y_hold_secs += (float)RealFrameTime / 65536.0f;
                    if (!y_long_fired && y_hold_secs >= Y_LONG_PRESS_SECS) {
                        xr_y_button_gameplay_long_edge = 1;
                        y_long_fired = 1;
                    }
                } else if (!y_cur && y_prev) {
                    /* Released: a short press that never became a long-press is a tap. */
                    if (!y_long_fired)
                        xr_y_button_gameplay_tap = 1;
                }
                y_prev = y_cur;
            } else {
                y_prev = 0;
                y_hold_secs = 0.0f;
                y_long_fired = 0;
            }
        }

        /* Mission log (message history) edge. Reset here rather than in the left
         * menu-button block below, because on PCVR the X long-press raises it as
         * well and X is handled first — resetting later would wipe it. */
        xr_menu_button_msg_history_edge = 0;

        /* X button → taunt in gameplay (Marine/Predator/Alien all map to the same
         * StartPlayerTaunt). Edge-triggered, NOT held: X also confirms menu
         * selections (KEY_CR), so the X press that starts the game is still down on
         * the first gameplay frame. Tracking x_prev every frame — including while in
         * 2D/menu mode — means that carried-over hold produces no rising edge, so the
         * player must release and press X again in-game to taunt. (Crouch lives on
         * the left-stick click, xr_left_thumbstick_click_pressed.) */
        xr_x_button_gameplay_pressed = 0;
        {
            static int x_prev = 0;
            int x_cur = 0;
            if (xr_x_button_action && pfn_xrGetActionStateBoolean) {
                XrActionStateGetInfo xget = { XR_TYPE_ACTION_STATE_GET_INFO };
                xget.action = xr_x_button_action;
                XrActionStateBoolean xstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &xget, &xstate))
                        && xstate.isActive)
                    x_cur = xstate.currentState ? 1 : 0;
            }
#ifdef AVP_PCVR
            /* PCVR: SteamVR reserves the left menu button — on Touch-style
             * controllers it arrives as the runtime's SYSTEM button, which
             * OpenXR never delivers to an application, so the tap-to-pause above
             * can never fire here (turning SteamVR's "VR Dashboard on System
             * Button" off just stops SteamVR acting on it; the press still does
             * not reach us). X therefore carries everything that button did, as
             * a three-stage hold — each stage fires AT its threshold (the same
             * fire-on-threshold convention the Y button uses for zoom) so the
             * hold gives feedback as it escalates:
             *     tap (release < 0.5s) → taunt
             *     hold >= 0.5s         → mission log  (was: menu long-press)
             *     hold >= 1.0s         → pause menu   (was: menu tap)
             * Holding through to the pause therefore shows the mission log on
             * the way — harmless, it is a non-modal HUD overlay, and it doubles
             * as the "keep holding" cue. Quest is untouched: its menu button works. */
            {
                static float x_hold_secs = 0.0f;
                static int   x_stage     = 0;   /* 0 = none yet, 1 = log fired, 2 = pause fired */
                /* Only a press that BEGAN in gameplay may drive the stages below.
                 * Without this, an X still held on the way out of a menu starts a
                 * fresh hold the instant 3D resumes, and ~1s later re-opens the very
                 * menu it just dismissed: confirm "Resume Game" with X and keep
                 * holding, or press X on the death screen to restart and keep
                 * holding, and the pause menu reappears on its own. (Zeroing
                 * x_hold_secs in the menu branch is not enough — the accumulator
                 * only needs x_cur, never a rising edge.) It also stopped a
                 * carried-over hold from firing a spurious taunt on release. This
                 * is the mirror of xr_x_pause_latch, which guards the other
                 * direction: gameplay hold -> menu. */
                static int   x_hold_armed = 0;
                const float  X_LOG_HOLD_SECS   = 0.5f;
                const float  X_PAUSE_HOLD_SECS = 1.0f;

                if (xr_2d_mode) {
                    /* Menus: X is select (KEY_CR); no hold tracking here. */
                    x_hold_secs  = 0.0f;
                    x_stage      = 0;
                    x_hold_armed = 0;
                } else {
                    if (x_cur && !x_prev) { x_hold_secs = 0.0f; x_stage = 0; x_hold_armed = 1; }
                    if (x_cur && x_hold_armed) {
                        extern int RealFrameTime;
                        x_hold_secs += (float)RealFrameTime / 65536.0f;
                        if (x_stage < 1 && x_hold_secs >= X_LOG_HOLD_SECS) {
                            /* Same edge the menu button's long hold raised;
                             * usr_io.c turns it into MessageHistory_DisplayPrevious. */
                            xr_menu_button_msg_history_edge = 1;
                            x_stage = 1;
                        }
                        if (x_stage < 2 && x_hold_secs >= X_PAUSE_HOLD_SECS) {
                            /* Exactly the pulse the menu button's tap issues:
                             * AvP_TriggerInGameMenus reads
                             * DebouncedKeyboardInput[FixedInputConfig.PauseGame],
                             * which is KEY_ESCAPE (usr_io.c). */
                            SDL_Log("INPUT: X held %.2fs - opening the pause menu", x_hold_secs);
                            KeyboardInput[KEY_ESCAPE] = 1;
                            DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                            x_stage = 2;
                            /* Hold the menu-select latch until X is released, so
                             * the still-down X that opened the menu doesn't also
                             * confirm the highlighted entry on its first frame. */
                            xr_x_pause_latch = 1;
                        }
                    } else if (x_prev && x_stage == 0 && x_hold_armed) {
                        xr_x_button_gameplay_pressed = 1;   /* short tap → taunt */
                    }
                }
                if (!x_cur) { xr_x_pause_latch = 0; x_hold_armed = 0; }
            }
#else
            if (!xr_2d_mode && x_cur && !x_prev)
                xr_x_button_gameplay_pressed = 1;
#endif
            x_prev = x_cur;
        }

        /* Left trigger → Marine jetpack (held) and Predator grappling hook (press
         * edge). The jetpack thrusts for as long as the trigger is held, so it uses
         * the held signal; the grappling hook fires once per press, so it uses the
         * rising edge. (Throw flare / cloak live on the right thumbstick click and
         * keep the legacy name xr_left_trigger_pressed.) */
        xr_left_trigger_gameplay_pressed = 0;
        xr_left_trigger_gameplay_edge    = 0;
        if (!xr_2d_mode && xr_left_trigger_action && pfn_xrGetActionStateBoolean) {
            static int lt_prev = 0;
            XrActionStateGetInfo ltget = { XR_TYPE_ACTION_STATE_GET_INFO };
            ltget.action = xr_left_trigger_action;
            XrActionStateBoolean ltstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &ltget, &ltstate))
                    && ltstate.isActive) {
                int lt_cur = ltstate.currentState ? 1 : 0;
                xr_left_trigger_gameplay_pressed = lt_cur;
                /* The press edge drives the grappling hook and the confirm haptic,
                 * but both are gated on the player actually owning the ability, so
                 * that decision is made in the gameplay layer (usr_io.c) where the
                 * player status is available — not here. */
                if (lt_cur && !lt_prev)
                    xr_left_trigger_gameplay_edge = 1;
                lt_prev = lt_cur;
            } else {
                lt_prev = 0;
            }
        }

        /* Left menu button. In the 2D menus it's immediate ESC/back. In gameplay a
         * short tap opens the pause menu (fired on release so a hold can be told apart),
         * while holding it past 0.5s instead shows the message history (like F1 in the
         * flat game) and suppresses the pause so a hold never also opens it. */
        /* (xr_menu_button_msg_history_edge is reset above, before the X block —
         * on PCVR the X long-press raises it too, and X is handled first.) */
        if (xr_menu_button_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo mget = { XR_TYPE_ACTION_STATE_GET_INFO };
            XrActionStateBoolean mstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            mget.action = xr_menu_button_action;
            int menu_pressed = 0;
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &mget, &mstate))
                    && mstate.isActive)
                menu_pressed = mstate.currentState ? 1 : 0;

            static int   menu_prev = 0;
            static float menu_hold_secs = 0.0f;
            static int   menu_long_fired = 0;
            const float  MENU_LONG_PRESS_SECS = 0.5f;

            if (xr_2d_mode) {
                /* Menus: unchanged immediate ESC/back. */
                if (menu_pressed && !KeyboardInput[KEY_ESCAPE])
                    DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                KeyboardInput[KEY_ESCAPE] = menu_pressed;
                menu_hold_secs  = 0.0f;
                menu_long_fired = 0;
            } else {
                /* Gameplay: tap = pause (on release), long hold = message history. */
                KeyboardInput[KEY_ESCAPE] = 0; /* don't hold ESC; pulse Debounced on a tap */
                if (menu_pressed && !menu_prev) {
                    menu_hold_secs  = 0.0f;
                    menu_long_fired = 0;
                }
                if (menu_pressed) {
                    extern int RealFrameTime;
                    menu_hold_secs += (float)RealFrameTime / 65536.0f;
                    if (!menu_long_fired && menu_hold_secs >= MENU_LONG_PRESS_SECS) {
                        xr_menu_button_msg_history_edge = 1; /* show previous message once */
                        menu_long_fired = 1;
                    }
                } else if (menu_prev && !menu_long_fired) {
                    /* Released as a short tap → open the pause menu. Pulse KeyboardInput
                     * high as well as the debounced edge so the press registers whether
                     * or not the debounced array is recomputed from edges after this. */
                    KeyboardInput[KEY_ESCAPE] = 1;
                    DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                }
            }
            menu_prev = menu_pressed;
        }

        /* In gameplay mode, set DebouncedGotAnyKey on the rising edge of any
         * face button or trigger so the death-screen "Press any key" works. */
        if (!xr_2d_mode) {
            static int prev_any = 0;
            int cur_any = xr_a_button_pressed | xr_b_button_pressed |
                          xr_trigger_right_pressed | xr_grip_right_squeeze_pressed;
            if (cur_any && !prev_any)
                DebouncedGotAnyKey = 1;
            prev_any = cur_any;
        }

        /* Menu navigation: only active when not in 3D gameplay. */
        if (xr_2d_mode && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo bget = { XR_TYPE_ACTION_STATE_GET_INFO };
            XrActionStateBoolean bstate = { XR_TYPE_ACTION_STATE_BOOLEAN };

            /* X + A buttons → KEY_CR (select). Read both before writing
             * KeyboardInput[KEY_CR] so the debounce check uses the previous
             * frame's combined state rather than X's just-written value. */
            {
                int prev_cr = KeyboardInput[KEY_CR];
                int x_pressed = 0, a_pressed = 0;
                if (xr_x_button_action) {
                    bget.action = xr_x_button_action;
                    XrActionStateBoolean bs = { XR_TYPE_ACTION_STATE_BOOLEAN };
                    if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bs)) && bs.isActive)
                        x_pressed = bs.currentState ? 1 : 0;
                }
                if (xr_a_button_action) {
                    bget.action = xr_a_button_action;
                    XrActionStateBoolean bs = { XR_TYPE_ACTION_STATE_BOOLEAN };
                    if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bs)) && bs.isActive)
                        a_pressed = bs.currentState ? 1 : 0;
                }
                /* On the user-profile select menu only A selects — X is disabled
                 * so the on-screen "Press A / B" prompt matches the controls. */
                if (VR_OnUserProfileSelectMenu())
                    x_pressed = 0;
#ifdef AVP_PCVR
                /* X is still down from the long-press that opened this menu —
                 * ignore it until released (see the X block above). */
                if (xr_x_pause_latch)
                    x_pressed = 0;
#endif
                KeyboardInput[KEY_CR] = x_pressed | a_pressed;
                if (KeyboardInput[KEY_CR] && !prev_cr) {
                    DebouncedKeyboardInput[KEY_CR] = 1;
                    DebouncedGotAnyKey = 1;
                }
                if (KeyboardInput[KEY_CR]) GotAnyKey = 1;
            }

            /* Y button → back (Escape).
             * Menu reads DebouncedKeyboardInput[KEY_ESCAPE] so set that on the press edge,
             * and keep KeyboardInput in sync so repeated reads stay consistent. */
            if (xr_y_button_action) {
                bget.action = xr_y_button_action;
                int y_pressed = 0;
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bstate))
                        && bstate.isActive)
                    y_pressed = bstate.currentState ? 1 : 0;
                /* Y is disabled on the user-profile select menu (no back action
                 * there — only A selects and B deletes). */
                if (VR_OnUserProfileSelectMenu())
                    y_pressed = 0;
                if (y_pressed && !KeyboardInput[KEY_ESCAPE])
                    DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                KeyboardInput[KEY_ESCAPE] = y_pressed;
            }

            /* B button → back (mirrors Y, right controller), EXCEPT on the
             * user-profile select menu where it deletes the highlighted profile
             * (KEY_BACKSPACE, matching the desktop keyboard shortcut). On that
             * menu X and Y are disabled, so only A (select) and B (delete) act.
             *
             * Edge-triggered on b_prev: a B tap lasts several frames, so a level
             * trigger would carry the still-held button into the delete-confirm
             * dialog it just opened and immediately cancel it. Firing only on the
             * rising edge means the menu that was showing when B went down is the
             * one that receives the key. */
            if (xr_b_button_action) {
                static int b_prev = 0;
                bget.action = xr_b_button_action;
                int b_pressed = 0;
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bstate))
                        && bstate.isActive)
                    b_pressed = bstate.currentState ? 1 : 0;

                int b_delete = VR_OnUserProfileSelectMenu();
                if (b_pressed && !b_prev)
                    DebouncedKeyboardInput[b_delete ? KEY_BACKSPACE : KEY_ESCAPE] = 1;
                /* Keep the level state in sync so held reads stay consistent. */
                KeyboardInput[KEY_BACKSPACE] = (b_pressed &&  b_delete) ? 1 : 0;
                if (b_pressed && !b_delete)
                    KeyboardInput[KEY_ESCAPE] |= 1;
                b_prev = b_pressed;
            }

            /* Both sticks → navigate up/down/left/right.
             * IDemandGoForward/GoBackward read KEY_UP/DOWN; IDemandTurnLeft/Right read KEY_LEFT/RIGHT.
             * Drive those directly; the menu's InputIsDebounced + KeyDepressedCounter
             * handle first-press fire and auto-repeat. */
            {
                const float MENU_THRESHOLD = 0.5f;
                int up    = (xr_left_stick_y >  MENU_THRESHOLD) ? 1 : 0;
                int down  = (xr_left_stick_y < -MENU_THRESHOLD) ? 1 : 0;
                int left  = 0;
                int right = 0;

                /* Left stick X axis. */
                if (xr_left_stick_action) {
                    XrActionStateGetInfo lsget = { XR_TYPE_ACTION_STATE_GET_INFO };
                    XrActionStateVector2f lsstate = { XR_TYPE_ACTION_STATE_VECTOR2F };
                    lsget.action = xr_left_stick_action;
                    float lx = 0.0f;
                    if (XR_SUCCEEDED(pfn_xrGetActionStateVector2f(xr_session, &lsget, &lsstate))
                            && lsstate.isActive)
                        lx = lsstate.currentState.x;
                    if (lx >  MENU_THRESHOLD) right = 1;
                    if (lx < -MENU_THRESHOLD) left  = 1;
                }

                /* Right stick X and Y mirror left stick. */
                if (xr_right_stick_action) {
                    XrActionStateGetInfo rsget = { XR_TYPE_ACTION_STATE_GET_INFO };
                    XrActionStateVector2f rsstate = { XR_TYPE_ACTION_STATE_VECTOR2F };
                    rsget.action = xr_right_stick_action;
                    float rx = 0.0f, ry = 0.0f;
                    if (XR_SUCCEEDED(pfn_xrGetActionStateVector2f(xr_session, &rsget, &rsstate))
                            && rsstate.isActive) {
                        rx = rsstate.currentState.x;
                        ry = rsstate.currentState.y;
                    }
                    if (ry >  MENU_THRESHOLD) up    = 1;
                    if (ry < -MENU_THRESHOLD) down  = 1;
                    if (rx >  MENU_THRESHOLD) right = 1;
                    if (rx < -MENU_THRESHOLD) left  = 1;
                }

                if (up    && !KeyboardInput[KEY_UP])    DebouncedKeyboardInput[KEY_UP]    = 1;
                if (down  && !KeyboardInput[KEY_DOWN])  DebouncedKeyboardInput[KEY_DOWN]  = 1;
                if (left  && !KeyboardInput[KEY_LEFT])  DebouncedKeyboardInput[KEY_LEFT]  = 1;
                if (right && !KeyboardInput[KEY_RIGHT]) DebouncedKeyboardInput[KEY_RIGHT] = 1;
                KeyboardInput[KEY_UP]    = up;
                KeyboardInput[KEY_DOWN]  = down;
                KeyboardInput[KEY_LEFT]  = left;
                KeyboardInput[KEY_RIGHT] = right;
            }
        }
        return;
    }
#endif

    if (joy == NULL) {
        return;
    }

    SDL_UpdateJoysticks();

    axes = SDL_GetNumJoystickAxes(joy);
    balls = SDL_GetNumJoystickBalls(joy);
    hats = SDL_GetNumJoystickHats(joy);

    if (axes > 0) {
        JoystickData.dwXpos = SDL_GetJoystickAxis(joy, 0) + 32768;
    }
    if (axes > 1) {
        JoystickData.dwYpos = SDL_GetJoystickAxis(joy, 1) + 32768;
    }

    if (hats > 0) {
        hat = SDL_GetJoystickHat(joy, 0);
        
        switch (hat) {
            default:
            case SDL_HAT_CENTERED:
                JoystickData.dwPOV = (DWORD) -1;
                break;
            case SDL_HAT_UP:
                JoystickData.dwPOV = 0;
                break;
            case SDL_HAT_RIGHT:
                JoystickData.dwPOV = 9000;
                break;
            case SDL_HAT_DOWN:
                JoystickData.dwPOV = 18000;
                break;
            case SDL_HAT_LEFT:
                JoystickData.dwPOV = 27000;
                break;
            case SDL_HAT_RIGHTUP:
                JoystickData.dwPOV = 4500;
                break;
            case SDL_HAT_RIGHTDOWN:
                JoystickData.dwPOV = 13500;
                break;
            case SDL_HAT_LEFTUP:
                JoystickData.dwPOV = 31500;
                break;
            case SDL_HAT_LEFTDOWN:
                JoystickData.dwPOV = 22500;
                break;
        }
    }
}

/* Trigger a vibration pulse on the right Touch controller.
 * amplitude: 0.0–1.0. duration_ms: pulse length in milliseconds. */
void XR_Haptic_Right(float amplitude, float duration_ms)
{
#ifdef AVP_XR
    if (!pfn_xrApplyHapticFeedback || !xr_session || !xr_right_haptic_action)
        return;
    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = xr_right_haptic_action;
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.duration  = (XrDuration)(duration_ms * 1000000.0f); /* ms → ns */
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    vib.amplitude = amplitude;
    pfn_xrApplyHapticFeedback(xr_session, &info, (XrHapticBaseHeader*)&vib);
#else
    (void)amplitude; (void)duration_ms;
#endif
}

void XR_Haptic_Left(float amplitude, float duration_ms)
{
#ifdef AVP_XR
    if (!pfn_xrApplyHapticFeedback || !xr_session || !xr_left_haptic_action)
        return;
    XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
    info.action = xr_left_haptic_action;
    XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
    vib.duration  = (XrDuration)(duration_ms * 1000000.0f); /* ms → ns */
    vib.frequency = XR_FREQUENCY_UNSPECIFIED;
    vib.amplitude = amplitude;
    pfn_xrApplyHapticFeedback(xr_session, &info, (XrHapticBaseHeader*)&vib);
#else
    (void)amplitude; (void)duration_ms;
#endif
}

/* ** */

unsigned char *GetScreenShot24(int *width, int *height)
{
    unsigned char *buf;
    
    if (surface == NULL) {
        return NULL;
    }
    
    if (RenderingMode == RENDERING_MODE_OPENGL) {
        buf = (unsigned char *)malloc(ViewportWidth * ViewportHeight * 3);
        
        *width = ViewportWidth;
        *height = ViewportHeight;
        
        pglPixelStorei(GL_PACK_ALIGNMENT, 1);
        pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        pglReadPixels(0, 0, ViewportWidth, ViewportHeight, GL_RGB, GL_UNSIGNED_BYTE, buf);
    } else {
        buf = (unsigned char *)malloc(surface->w * surface->h * 3);
        
        unsigned char *ptrd;
        unsigned short int *ptrs;
        int x, y;
        
        if (SDL_MUSTLOCK(surface)) {
            if (SDL_LockSurface(surface) < 0) {
                free(buf);
                return NULL; /* ... */
            }
        }
        
        ptrd = buf;
        for (y = 0; y < surface->h; y++) {
            ptrs = (unsigned short *)(((unsigned char *)surface->pixels) + (surface->h-y-1)*surface->pitch);
            for (x = 0; x < surface->w; x++) {
                unsigned int c;
                
                c = *ptrs;
                ptrd[0] = (c & 0xF800)>>8;
                ptrd[1] = (c & 0x07E0)>>3;
                ptrd[2] = (c & 0x001F)<<3;
                
                ptrs++;
                ptrd += 3;
            }
        }
        
        *width = surface->w;
        *height = surface->h;
        
        if (SDL_MUSTLOCK(surface)) {
            SDL_UnlockSurface(surface);
        }
    }

#if 0
    Uint16 redtable[256], greentable[256], bluetable[256];
	
	if (SDL_GetGammaRamp(redtable, greentable, bluetable) != -1) {
		unsigned char *ptr;
		int i;
		
		ptr = buf;
		for (i = 0; i < surface->w*surface->h; i++) {
			ptr[i*3+0] = redtable[ptr[i*3+0]]>>8;
			ptr[i*3+1] = greentable[ptr[i*3+1]]>>8;
			ptr[i*3+2] = bluetable[ptr[i*3+2]]>>8;
			ptr += 3;
		}
	}
#endif
    
    return buf;
}

/* ** */

PROCESSORTYPES ReadProcessorType()
{
    return PType_PentiumMMX;
}

/* ** */

typedef struct VideoModeStruct
{
    int w;
    int h;
    int available;
} VideoModeStruct;
VideoModeStruct VideoModeList[] = {
        { 	512, 	384,	0	},
        {	640,	480,	0	},
        {	800,	600,	0	},
        {	1024,	768,	0	},
        {	1152,	864,	0	},
        {	1280,   720,	0	},
        {	1280,	960,	0	},
        {	1280,	1024,	0	},
        {	1366,	768,	0	},
        {	1600,	1200,	0	},
        {	1680,	1050,	0	},
        {	1920,	1080,	0	},
        {   2048,   1080,   0   },
        {	2560,	1080,	0	},
        {	2560,	1440,	0	},
        {	2560,	1600,	0	},
        {	2560,	1664,	0	},
        {   2880,   1620,   0   },
        {   3000,   2000,   0   },
        {   3200,   1800,   0   },
        {   3440,   1440,   0   },
        {	3840,	2160,	0	},
        {   4096,   2160,   0   },
        {	4096,	2304,	0	},
        {	4480,	2520,	0	},
        {	5120,	2880,	0	},
        {	6016,	3384,	0	},
        {   7680,   4320,   0   },
        {   8192,   4320,   0   }
};

int CurrentVideoMode;
const int TotalVideoModes = sizeof(VideoModeList) / sizeof(VideoModeList[0]);

/* ---- Video-mode preference, stored in <gamedir>/config.cfg ---------------

   config.cfg is the game's own settings file: the console replays it at
   startup (BatchFileProcessing::Run, davehook.c) and rewrites it with the
   current key bindings on every level exit (KeyBinding::WriteToConfigFile),
   so the resolution belongs there rather than in a second file beside it.

   It is written as a COMMENT. consbtch.cpp skips any line beginning with '#',
   so the command processor never sees it, no console command has to be
   registered for it, and an older build ignores it. A stock config.cfg has no
   such line and simply falls through to the desktop-resolution default below,
   which is also what happens if a user deletes the line by hand.

   Note this stores the RESOLUTION, not the index into VideoModeList. That
   list has gained entries before, and an index would silently come to mean a
   different mode after any future edit to it. */
#define VIDEOMODE_CONFIG_FILE "config.cfg"
#define VIDEOMODE_CONFIG_TAG  "#VIDEOMODE"

/* The desktop mirror rate rides in the same file, for the same reason the video
   mode does: it has to be known BEFORE anything renders. It is also an AV option
   stored in the user profile, but a profile is not loaded until the player picks
   one — so with only the profile copy, the intro FMVs and the profile screen
   itself still reached the monitor with the mirror set to Off, and the screen
   only went dark once a profile was chosen. config.cfg seeds it at startup; the
   profile takes over when it loads (per-profile wins, as with every other AV
   option) and re-seeds this file on "Use these settings". */
#define MIRROR_CONFIG_TAG     "#DESKTOPMIRROR"

/* Does this line carry our setting? Case-insensitive so a hand-edited file
   works either way; the console uppercases everything it reads, we don't. */
static int ConfigLineHasTag(const char *line, const char *tag)
{
    while (*tag) {
        if (toupper((unsigned char)*line) != *tag) return 0;
        line++;
        tag++;
    }
    return (*line == ' ' || *line == '\t');
}

static int VideoModeConfigLine(const char *line)
{
    return ConfigLineHasTag(line, VIDEOMODE_CONFIG_TAG);
}

static int MirrorConfigLine(const char *line)
{
    return ConfigLineHasTag(line, MIRROR_CONFIG_TAG);
}

/* Either of the two lines this file owns. Everything else in config.cfg belongs
   to the game and has to survive a rewrite untouched. */
static int OurConfigLine(const char *line)
{
    return VideoModeConfigLine(line) || MirrorConfigLine(line);
}

/* Seed DesktopMirrorIndex from config.cfg. Leaves it alone if there is no line,
   which already means "every frame". */
void LoadDesktopMirrorPreference(void)
{
    FILE *fp;
    char line[256];

    fp = OpenGameFile(VIDEOMODE_CONFIG_FILE, FILEMODE_READONLY, FILETYPE_CONFIG);
    if (fp == NULL) return;

    while (fgets(line, sizeof(line), fp) != NULL) {
        int v;

        if (!MirrorConfigLine(line)) continue;
        if (sscanf(line + sizeof(MIRROR_CONFIG_TAG) - 1, "%d", &v) != 1) continue;
        if (v >= 0 && v <= 3) DesktopMirrorIndex = v;   /* a later line wins */
    }
    fclose(fp);
}

/* Append the mirror setting to an already-open config.cfg. Same contract as
   VideoMode_WriteConfigLine below, including being called from
   KeyBinding::WriteToConfigFile — which rebuilds that file from nothing on every
   level exit and would otherwise drop it. Index 0 is the default, so an absent
   line already says it and going back to "Every Frame" REMOVES the setting. */
void DesktopMirror_WriteConfigLine(FILE *fp)
{
    if (fp == NULL) return;
    if (DesktopMirrorIndex <= 0 || DesktopMirrorIndex > 3) return;

    fprintf(fp, "%s %d\n", MIRROR_CONFIG_TAG, DesktopMirrorIndex);
}

/* The stored mode as an index into VideoModeList, or -1 if config.cfg has no
   usable line. Availability is NOT checked here — the caller does that. */
static int VideoModeFromConfigFile(void)
{
    FILE *fp;
    char line[256];
    int found = -1;

    fp = OpenGameFile(VIDEOMODE_CONFIG_FILE, FILEMODE_READONLY, FILETYPE_CONFIG);
    if (fp == NULL) return -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        int w, h, i;

        if (!VideoModeConfigLine(line)) continue;
        if (sscanf(line + sizeof(VIDEOMODE_CONFIG_TAG) - 1, "%d %d", &w, &h) != 2) continue;

        for (i = 0; i < TotalVideoModes; i++) {
            if (VideoModeList[i].w == w && VideoModeList[i].h == h) {
                found = i;    /* a later line wins, as with a repeated BIND */
                break;
            }
        }
    }
    fclose(fp);

    return found;
}

/* The mode used when config.cfg carries no preference: the DESKTOP resolution,
   which is what the game actually presents at. (The original fell back to
   640x480, harmless only while the selection was ignored; now that it is
   honoured, that would start a first run in a 640x480 letterbox.)

   Keyed to SDL_GetPrimaryDisplay() rather than the window's display because it
   runs once before the window exists. Both the load and the save go through
   here, which is the point: an ABSENT line means "whatever this returns", so
   if the writer decided "same as native" by any other route the two could
   disagree and a saved choice would come back as something else. */
static int VideoModeDefaultIndex(void)
{
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int i, best = 0;

    if (desktop) {
        for (i = 0; i < TotalVideoModes; i++) {
            if (VideoModeList[i].available &&
                VideoModeList[i].w == desktop->w && VideoModeList[i].h == desktop->h) {
                return i;
            }
        }
    }

    /* Desktop mode not in the list: take the largest available that fits. */
    for (i = 0; i < TotalVideoModes; i++) {
        if (!VideoModeList[i].available) continue;
        if (desktop && (VideoModeList[i].w > desktop->w || VideoModeList[i].h > desktop->h))
            continue;
        best = i;
    }

    return best;
}

/* Append the current mode to an already-open config.cfg. Called from the save
   below AND from KeyBinding::WriteToConfigFile, which rebuilds that file from
   nothing on every level exit and would otherwise drop the setting. */
void VideoMode_WriteConfigLine(FILE *fp)
{
#if !defined(FIXED_WINDOW_SIZE)
    if (fp == NULL) return;

    /* Store nothing when the choice IS the default. An absent line already
       means "native resolution", so the line would be redundant — and leaving
       it out means a later change of desktop resolution, or moving to a
       different monitor, is simply picked up on the next launch rather than
       the game staying pinned to the old display's size. The save below strips
       any previous line before calling this, so going back to native REMOVES
       the setting rather than rewriting it. */
    if (CurrentVideoMode == VideoModeDefaultIndex()) return;

    fprintf(fp, "%s %d %d\n", VIDEOMODE_CONFIG_TAG,
            VideoModeList[CurrentVideoMode].w,
            VideoModeList[CurrentVideoMode].h);
#endif
}

void LoadDeviceAndVideoModePreferences()
{
    int mode = VideoModeFromConfigFile();

    /* Legacy: this used to live in the port's own avp_tempvideo.cfg, as an
       index. Honour it once so an existing choice survives the move; the next
       write of config.cfg (a resolution change, or any level exit) carries it
       over and the old file is then deleted. */
    if (mode < 0) {
        FILE *fp = OpenGameFile("avp_tempvideo.cfg", FILEMODE_READONLY, FILETYPE_CONFIG);

        if (fp != NULL) {
            int old;

            if (fscanf(fp, "%d", &old) == 1 && old >= 0 && old < TotalVideoModes)
                mode = old;
            fclose(fp);
        }
    }

    /* No, or invalid, mode found: fall back to the native resolution. */
    CurrentVideoMode = (mode >= 0 && VideoModeList[mode].available)
                     ? mode
                     : VideoModeDefaultIndex();
}

/* Make the selected resolution actually take effect.

   THIS is what the option was missing. The window is created with
   SDL_WINDOW_FULLSCREEN but no fullscreen mode ever set, and SDL3 treats that as
   borderless-fullscreen-DESKTOP: it ignores the requested size entirely and uses
   whatever the display is already running. So CurrentVideoMode reached
   SDL_CreateWindow and was then silently discarded, on every platform.

   Setting the mode explicitly fixes it: NULL keeps the borderless-desktop
   behaviour (correct when the selection IS the desktop resolution, and the nicer
   option there — no mode switch, instant alt-tab), anything else switches to the
   closest real exclusive mode.

   Safe to call at any time; the resulting resize is picked up by the existing
   SDL_EVENT_WINDOW_RESIZED handler, which updates SDB, the viewport and the MSAA
   target. No-op where the platform fixes the window size for us (Android/iOS). */
void ApplySelectedVideoMode(void)
{
#if !defined(FIXED_WINDOW_SIZE)
    SDL_DisplayID disp;
    const SDL_DisplayMode *desktop;
    int w, h;

    if (!window) return;

    disp    = SDL_GetDisplayForWindow(window);
    desktop = SDL_GetDesktopDisplayMode(disp);
    w = VideoModeList[CurrentVideoMode].w;
    h = VideoModeList[CurrentVideoMode].h;

    if (desktop && desktop->w == w && desktop->h == h) {
        SDL_SetWindowFullscreenMode(window, NULL);
        SDL_Log("video mode: %dx%d (borderless desktop)", w, h);
    } else {
        SDL_DisplayMode closest;
        if (SDL_GetClosestFullscreenDisplayMode(disp, w, h, 0.0f, false, &closest)) {
            SDL_SetWindowFullscreenMode(window, &closest);
            SDL_Log("video mode: %dx%d (exclusive %dx%d @ %.0f Hz)",
                    w, h, closest.w, closest.h, closest.refresh_rate);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            SDL_Log("video mode: %dx%d unavailable, using borderless desktop", w, h);
        }
    }
    SDL_SyncWindow(window);
#endif
}

void SaveDeviceAndVideoModePreferences()
{
    FILE *fp;
    char *contents = NULL;
    size_t contentsLen = 0;
    int fileExists = 0;
    long size = 0;

    /* Read-modify-write. config.cfg is the game's file, not ours — it ships
       with the game and holds the key bindings — so every line that isn't
       ours has to come back out unchanged, and OpenGameFile has no
       update-in-place mode ("wb" truncates). Slurp it, then rewrite it. */
    fp = OpenGameFile(VIDEOMODE_CONFIG_FILE, FILEMODE_READONLY, FILETYPE_CONFIG);
    if (fp != NULL) {
        fileExists = 1;
        if (fseek(fp, 0, SEEK_END) == 0 && (size = ftell(fp)) >= 0 && size < (1024 * 1024)) {
            rewind(fp);
            contents = (char *)malloc((size_t)size + 1);
            if (contents != NULL) {
                contentsLen = fread(contents, 1, (size_t)size, fp);
                contents[contentsLen] = '\0';
            }
        }
        fclose(fp);
    }

    /* The file is there but could not be read back — implausibly large, or out
       of memory. Abandon the save rather than opening it "wb": storing a
       resolution is not worth any chance of wiping the key bindings. An absent
       file is a different case and is fine to create. */
    if (fileExists && contents == NULL) return;

    fp = OpenGameFile(VIDEOMODE_CONFIG_FILE, FILEMODE_WRITEONLY, FILETYPE_CONFIG);
    if (fp == NULL) {
        free(contents);
        return;
    }

    if (contents != NULL) {
        char *p = contents;
        char *end = contents + contentsLen;
        int endedWithNewline = 1;

        /* Walked by length, not by strlen/strchr, so the copy-back is exact
           for any byte the file happens to contain. */
        while (p < end) {
            char *eol = (char *)memchr(p, '\n', (size_t)(end - p));
            size_t len = (eol != NULL) ? (size_t)(eol - p) + 1 : (size_t)(end - p);

            /* Drop any previous copy of either of our lines rather than
               accumulating one per change. */
            if (!OurConfigLine(p)) {
                fwrite(p, 1, len, fp);
                endedWithNewline = (p[len - 1] == '\n');
            }
            if (eol == NULL) break;
            p = eol + 1;
        }

        /* A file not ending in a newline would otherwise absorb our line. */
        if (!endedWithNewline) fputc('\n', fp);

        free(contents);
    }

    VideoMode_WriteConfigLine(fp);
    DesktopMirror_WriteConfigLine(fp);
    fclose(fp);

    /* The setting lives in config.cfg now; retire the file it used to be in. */
    DeleteGameFile("avp_tempvideo.cfg");
}

void PreviousVideoMode2()
{
    int cur = CurrentVideoMode;
    
    do {
        if (cur == 0)
            cur = TotalVideoModes;
        cur--;
        if (cur == CurrentVideoMode)
            return;
    } while(!VideoModeList[cur].available);
    
    CurrentVideoMode = cur;
}

void NextVideoMode2()
{
    int cur = CurrentVideoMode;
    
    do {
        cur++;
        if (cur == TotalVideoModes)
            cur = 0;
        
        if (cur == CurrentVideoMode)
            return;
    } while(!VideoModeList[cur].available);
    
    CurrentVideoMode = cur;
}

char *GetVideoModeDescription2()
{
    return "SDL3";
}

char *GetVideoModeDescription3()
{
    static char buf[64];
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    int w = VideoModeList[CurrentVideoMode].w;
    int h = VideoModeList[CurrentVideoMode].h;

    /* Tag the resolution the monitor is actually running at. This is a plain
       comparison against the desktop mode, NOT VideoModeDefaultIndex(): the
       two differ when the desktop resolution isn't one of the listed modes, and
       there the honest answer is that no entry is native. Hardcoded rather than
       a TEXTSTRING because the shipped language.txt has no such line — as the
       surrounding "SDL3" and "%dx%d" already are. */
    if (desktop && desktop->w == w && desktop->h == h)
        _snprintf(buf, 64, "%dx%d (Native)", w, h);
    else
        _snprintf(buf, 64, "%dx%d", w, h);

    return buf;
}

int InitSDL()
{
    SDL_Log("SDL version: %d.%d.%d", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    SDL_Log("SDL GPU drivers: %s", SDL_GetCurrentVideoDriver());

#ifdef AVP_PCVR_XLIB
    /* XR_KHR_opengl_enable has no usable Wayland binding — openxr_platform.h
       declares XrGraphicsBindingOpenGLWaylandKHR, but no runtime implements it,
       and SteamVR's Linux OpenXR is GLX-only. Pin SDL to x11 so there IS a GLX
       context to hand xrCreateSession; under a Wayland session this goes through
       XWayland, which works. Left alone when SDL_VIDEODRIVER is already set, so
       forcing another backend to test one stays possible. Must precede
       SDL_Init(SDL_INIT_VIDEO) — the driver is chosen there. */
    if (!SDL_getenv("SDL_VIDEODRIVER")) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        SDL_Log("XR: pinned SDL to the x11 video driver (OpenXR GLX binding)");
    }
#endif

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL initialising...");
    
    atexit(SDL_Quit);

    SDL_AddEventWatch(SDLEventFilter, NULL);

#ifdef __ANDROID__
    /* Force the system on-screen keyboard whenever text input is active (menu
       name / multiplayer fields). The default "auto" only shows it when SDL
       thinks no physical keyboard is attached, which misfires on Quest, so the
       keyboard never appeared. Must be set before any SDL_StartTextInput(). */
    SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
#endif

#if 0
    
    // Set Hints BEFORE creating the renderer to force hardware acceleration
    bool is_legacy_device = false;
    #ifdef __ANDROID__
        char sdk_ver_str[PROP_VALUE_MAX];
            if (__system_property_get("ro.build.version.sdk", sdk_ver_str) > 0) {
                int sdk_ver = atoi(sdk_ver_str);
                // Android 7.0 is API 24. We'll consider anything 28 (Android 9) or lower "legacy"
                if (sdk_ver <= 28) {
                    is_legacy_device = true;
                }
            }
    #endif
    if (is_legacy_device) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    } else {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); // 0=Nearest Neighbour, 1=Linear for modern screens
    }
    
	SDL_Rect **SDL_AvailableVideoModes;
	SDL_AvailableVideoModes = SDL_ListModes(NULL, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);
	if (SDL_AvailableVideoModes == NULL)
		return -1;
	
	if (SDL_AvailableVideoModes != (SDL_Rect **)-1) {
		int i, j, foundit;
		
		foundit = 0;
		for (i = 0; i < TotalVideoModes; i++) {
			SDL_Rect **modes = SDL_AvailableVideoModes;
			
			for (j = 0; modes[j]; j++) {
				if (modes[j]->w >= VideoModeList[i].w &&
				    modes[j]->h >= VideoModeList[i].h) {
					if (SDL_VideoModeOK(VideoModeList[i].w, VideoModeList[i].h, 16, SDL_FULLSCREEN | SDL_OPENGL)) {
						/* assume SDL isn't lying to us */
						VideoModeList[i].available = 1;
						
						foundit = 1;
					}
					break;
				}
			}
		}
		if (foundit == 0)
			return -1;
	} else {
		int i, foundit;
		
		foundit = 0;
		for (i = 0; i < TotalVideoModes; i++) {
			if (SDL_VideoModeOK(VideoModeList[i].w, VideoModeList[i].h, 16, SDL_FULLSCREEN | SDL_OPENGL)) {
				/* assume SDL isn't lying to us */
				VideoModeList[i].available = 1;
				
				foundit = 1;
			}
		}
		
		if (foundit == 0)
			return -1;
	}
#endif
    
    /* Mark which of the listed resolutions this display can actually do.
       Previously every entry was marked available unconditionally (the real
       detection above is SDL1-era and compiled out), so the menu offered all 29
       modes up to 8192x4320 whatever the monitor was. */
    {
        int i, j, count = 0, any = 0;
        SDL_DisplayID disp = SDL_GetPrimaryDisplay();
        SDL_DisplayMode **modes = SDL_GetFullscreenDisplayModes(disp, &count);
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(disp);

        for (i = 0; i < TotalVideoModes; i++) VideoModeList[i].available = 0;

        if (modes) {
            for (i = 0; i < TotalVideoModes; i++)
                for (j = 0; j < count; j++)
                    if (modes[j]->w == VideoModeList[i].w &&
                        modes[j]->h == VideoModeList[i].h) {
                        VideoModeList[i].available = 1;
                        break;
                    }
            SDL_free(modes);
        }

        /* Always offer the desktop resolution: it is the borderless-fullscreen
           case and is not guaranteed to appear in the exclusive-mode list. */
        if (desktop)
            for (i = 0; i < TotalVideoModes; i++)
                if (VideoModeList[i].w == desktop->w && VideoModeList[i].h == desktop->h)
                    VideoModeList[i].available = 1;

        for (i = 0; i < TotalVideoModes; i++) any += VideoModeList[i].available;
        if (!any) {
            /* Driver told us nothing usable — fall back to the old behaviour
               rather than leaving the player with an empty list. */
            for (i = 0; i < TotalVideoModes; i++) VideoModeList[i].available = 1;
            any = TotalVideoModes;
        }

        SDL_Log("desktop mode %dx%d; %d of %d listed resolutions usable (driver reported %d modes)",
                desktop ? desktop->w : 0, desktop ? desktop->h : 0,
                any, TotalVideoModes, count);
    }

    LoadDeviceAndVideoModePreferences();
    LoadDesktopMirrorPreference();

#ifdef AVP_XR
    /* On VR builds, always enable controller input and configure left-stick
       locomotion (the XR action system feeds JoystickData in ReadJoysticks). */
    WantJoystick = 1;
    extern void VR_InitJoystickConfig(void);
    VR_InitJoystickConfig();
#endif
#ifdef __ANDROID__
    /* Use the SDL3 gamepad API — Quest Touch controllers are presented as
       Android gamepads, not raw joysticks. SDL_INIT_GAMEPAD implies JOYSTICK. */
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    {
        int gp_count = 0;
        SDL_JoystickID *gp_ids = SDL_GetGamepads(&gp_count);
        if (gp_ids && gp_count > 0) {
            gamepad = SDL_OpenGamepad(gp_ids[0]);
            if (gamepad) {
                GotJoystick = 1;
                JoystickCaps.wCaps = 0;
                JoystickData.dwXpos = 32768;
                JoystickData.dwYpos = 32768;
                JoystickData.dwPOV  = (DWORD) -1;
            }
        }
        SDL_free(gp_ids);
    }
#endif

    if (WantJoystick && !GotJoystick) {
        SDL_InitSubSystem(SDL_INIT_JOYSTICK);

        /* In SDL3, SDL_OpenJoystick takes an instance ID, not an index.
           Query the list of connected devices and open the first one. */
        {
            int joy_count = 0;
            SDL_JoystickID *joy_ids = SDL_GetJoysticks(&joy_count);
            if (joy_ids && joy_count > 0)
                joy = SDL_OpenJoystick(joy_ids[0]);
            SDL_free(joy_ids);
        }
        if (joy) {
            GotJoystick = 1;

            JoystickCaps.wCaps = 0;

            JoystickData.dwXpos = 0;
            JoystickData.dwYpos = 0;
            JoystickData.dwRpos = 0;
            JoystickData.dwUpos = 0;
            JoystickData.dwVpos = 0;
            JoystickData.dwPOV = (DWORD) -1;
        }
    }
    
    Uint32 rmask, gmask, bmask, amask;
    
    // pre-create the software surface in OpenGL RGBA order
    // menus.c assumes RGB565; possible to support both?
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    rmask = 0xff000000;
    gmask = 0x00ff0000;
    bmask = 0x0000ff00;
    amask = 0x000000ff;
#else
    rmask = 0x0000f800;
    gmask = 0x000007e0;
    bmask = 0x0000001f;
    amask = 0x00000000;
#endif
    
    surface = SDL_CreateSurface(640, 480, SDL_PIXELFORMAT_RGB565);
    if (surface == NULL) {
        return -1;
    }

    /* Point ScreenBuffer at the SDL surface so menu backdrop code works on Android.
       Also set BackBufferPitch to the real surface pitch: it's defined in stubs.c
       but never assigned in this build (the DirectDraw path that set it is not
       compiled), so it defaults to 0, which collapses any direct ScreenBuffer
       fill to the top row. menus.c uses surface->pitch directly and is unaffected. */
    {
        extern unsigned char *ScreenBuffer;
        extern long BackBufferPitch;
        ScreenBuffer   = (unsigned char *)surface->pixels;
        BackBufferPitch = surface->pitch;
    }

    return 0;
}

static void SetWindowSize(int PhysicalWidth, int PhysicalHeight, int VirtualWidth, int VirtualHeight)
{
#if !defined(NDEBUG)
    fprintf(stderr, "SetWindowSize(%d,%d,%d,%d); %d\n", PhysicalWidth, PhysicalHeight, VirtualWidth, VirtualHeight, CurrentVideoMode);
#endif
    
    ViewportWidth = PhysicalWidth;
    ViewportHeight = PhysicalHeight;
    
    ScreenDescriptorBlock.SDB_Width     = VirtualWidth;
    ScreenDescriptorBlock.SDB_Height    = VirtualHeight;
    ScreenDescriptorBlock.SDB_CentreX   = VirtualWidth/2;
    ScreenDescriptorBlock.SDB_CentreY   = VirtualHeight/2;
    /* Hor+ default, matching SetupVision's normal lens (prototyp.h). This is only
       the default a freshly allocated VDB inherits (vdb.c copies SDB->VDB);
       SetupVision overrides VDB_ProjX per species at level start, and re-widens it
       for the Alien. Left as VirtualWidth/2 this silently reintroduced the old
       zoomed-in FOV for any VDB created after a window resize. */
    ScreenDescriptorBlock.SDB_ProjX     = AVP_PROJX_NORMAL(VirtualWidth, VirtualHeight);
    ScreenDescriptorBlock.SDB_ProjY     = VirtualHeight/2;
    ScreenDescriptorBlock.SDB_ClipLeft  = 0;
    ScreenDescriptorBlock.SDB_ClipRight = VirtualWidth;
    ScreenDescriptorBlock.SDB_ClipUp    = 0;
    ScreenDescriptorBlock.SDB_ClipDown  = VirtualHeight;
    
    if (window != NULL) {
        SDL_SetWindowSize(window, PhysicalWidth, PhysicalHeight);
    }
}

static int SetSoftVideoMode(int Width, int Height, int Depth)
{
    //TODO: clear surface
    
    RenderingMode = RENDERING_MODE_SOFTWARE;
    ScanDrawMode = ScanDrawD3DHardwareRGB;
    GotMouse = 1;
    
    // reset input
    IngameKeyboardInput_ClearBuffer();
    
    SetWindowSize(ViewportWidth, ViewportHeight, Width, Height);
    
    return 0;
}

/* ** */
static bool SDLCALL SDLEventFilter(void* userData, SDL_Event* event) {
    (void) userData;
    
    //printf("SDLEventFilter: %d\n", event->type);
    
    switch (event->type) {
        case SDL_EVENT_TERMINATING:
            SDL_Log("EXIT: SDL_EVENT_TERMINATING - leaving the main loop");
            AvP.MainLoopRunning = 0; /* TODO */
            break;
    }
    
    return true;
}

static int InitSDLVideo(void) {
    return 0;
}

// ------------------------------------------------------------
// CINEMA SCREEN SHADER (simple textured quad)
// ------------------------------------------------------------

typedef struct {
    GLuint program;
    GLint uTexture;
} CinemaShader_t;

CinemaShader_t CinemaShader;
GLuint CinemaQuadVAO = 0;
GLuint CinemaQuadVBO = 0;

static const char* cinema_vs =
        "#version 100\n"
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vUV = aUV;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";

static const char* cinema_fs =
        "#version 100\n"
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTexture;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTexture, vUV);\n"
        "}\n";

static GLuint compile_shader(GLenum type, const char* src)
{
    SDL_Log("compile_shader: type=%s, first line: %.40s",
            type == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT",
            src);
    
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        SDL_Log("Shader compile error (%s): %s",
                type == GL_VERTEX_SHADER ? "VERTEX" : "FRAGMENT",
                log);
    }
    
    return s;
}

void InitCinemaShader(void)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, cinema_vs);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, cinema_fs);
    
    CinemaShader.program = glCreateProgram();
    glAttachShader(CinemaShader.program, vs);
    glAttachShader(CinemaShader.program, fs);
    
    // Bind BEFORE linking — slots 0 & 1
    glBindAttribLocation(CinemaShader.program, 0, "aPos");
    glBindAttribLocation(CinemaShader.program, 1, "aUV");
    
    glLinkProgram(CinemaShader.program);
    
    GLint linked = 0;
    glGetProgramiv(CinemaShader.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        glGetProgramInfoLog(CinemaShader.program, sizeof(log), NULL, log);
        SDL_Log("CinemaShader link error: %s", log);
    }
    
    glDeleteShader(vs);
    glDeleteShader(fs);
    
    CinemaShader.uTexture = glGetUniformLocation(CinemaShader.program, "uTexture");
    SDL_Log("XR: CinemaShader initialized, uTexture=%d", CinemaShader.uTexture);
    
    // No VAO/VBO — FlipBuffers() supplies verts directly via glVertexAttribPointer
}

static int SetOGLVideoMode(int Width, int Height)
{

//#ifdef __ANDROID__
    //SDL_Log("DEBUG: Android detected, skipping SetOGLVideoMode");
    //return 0; // Simply return: the window is already initialized by SDL3
//#endif
    
    int oldflags;
    int flags;
    
    RenderingMode = RENDERING_MODE_OPENGL;
    ScanDrawMode = ScanDrawD3DHardwareRGB;
    GotMouse = 1;

#if defined(FIXED_WINDOW_SIZE)
    // SDL3 returns a pointer to the mode, or NULL on failure
    const SDL_DisplayMode* dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (dm) {
        Width = dm->w;
        Height = dm->h;
    }
#endif
    
    if (window == NULL) {
        load_ogl_functions(0);
        
        flags = SDL_WINDOW_OPENGL;

#if defined(FIXED_WINDOW_SIZE)
        flags |= SDL_WINDOW_BORDERLESS;
        flags |= SDL_WINDOW_FULLSCREEN; // SDL3 uses this for all fullscreen modes
#else
        if (WantFullscreen) {
            flags |= SDL_WINDOW_FULLSCREEN;
        }
        
        // the game doesn't properly support window resizing
        //flags |= SDL_WINDOW_RESIZABLE;
#endif
        
        // reset input
        IngameKeyboardInput_ClearBuffer();
        
        // force restart the video system
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        SDL_InitSubSystem(SDL_INIT_VIDEO);
        
        // set OpenGL attributes first
#if defined(USE_OPENGL_ES)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2); // Upgraded to 2
    	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
        // These should be configurable video options.
        // If user requests 8bpp, try that, else fall back to 5.
        // Same with depth.  Try 32, 24, 16.
#ifdef __ANDROID__
        //ANDROID SPECIFIC SETTINGS (MAYBE NOT NEEDED)
        //SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
        //SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        //SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
        //SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
        //SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        //SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        //DEFAULT SETTINGS WORKS OK
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        //THESE NEEDS TO BE SET TO 0 IN ORDER TO RUN
        // These should be configurable video options.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
#else
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        /* 24-bit depth: 16-bit over AvP's large world coordinates z-fights badly
           (faces of objects flicker/drop out depending on view angle). The VR
           swapchain uses higher-precision depth, which is why VR looks correct. */
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

        /* Deliberately NO multisampling on the default framebuffer. Desktop MSAA
           is done with our own multisampled FBO plus a blit resolve (opengl.c),
           which the menu slider can change at any time; baking samples into the
           GL context here would instead need a context rebuild to change, and
           would double up with the FBO. */
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
#endif
        window = SDL_CreateWindow("Aliens Versus Predator: VR",
                                  WindowWidth,
                                  WindowHeight,
                                  flags);
        
        //if (window == NULL) {
        //	fprintf(stderr, "(OpenGL) SDL SDL_CreateWindow failed: %s\n", SDL_GetError());
        //	exit(EXIT_FAILURE);
        //}
        if (!window) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_CreateWindow failed: %s", SDL_GetError());
            return -1;  // or bubble the error up instead of exit()
        }
        context = SDL_GL_CreateContext(window);
        if (context == NULL) {
            fprintf(stderr, "(OpenGL) SDL SDL_GL_CreateContext failed: %s\n", SDL_GetError());
            exit(EXIT_FAILURE);
        }
        SDL_GL_MakeCurrent(window, context);
        
        // Check rendering type
        SDL_Log("GL_VENDOR:   %s", glGetString(GL_VENDOR));
        SDL_Log("GL_RENDERER: %s", glGetString(GL_RENDERER));
        SDL_Log("GL_VERSION:  %s", glGetString(GL_VERSION));
        {
            int gotDepth = 0, gotDouble = 0;
            SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &gotDepth);
            SDL_GL_GetAttribute(SDL_GL_DOUBLEBUFFER, &gotDouble);
            SDL_Log("GL context depth buffer: %d bits, double-buffered: %d", gotDepth, gotDouble);
        }
        
        /* Name the software rasterisers explicitly. This used to test only for
           "SwiftShader" and "software", which misses every Mesa one — llvmpipe,
           softpipe, swrast, lavapipe — so a software fallback was cheerfully
           reported as "Hardware rendering confirmed".

           That is not cosmetic. A 32-bit build on a machine with only 64-bit GPU
           drivers silently lands on llvmpipe: it cannot vsync, so the frontend
           free-runs at several hundred fps while gameplay crawls, and the only
           clue was an FPS counter reading a number that looked wrong. */
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        if (!renderer) renderer = "(null)";
        if (strstr(renderer, "SwiftShader") || strstr(renderer, "software")
         || strstr(renderer, "llvmpipe")    || strstr(renderer, "softpipe")
         || strstr(renderer, "swrast")      || strstr(renderer, "lavapipe")) {
            SDL_Log("WARNING: SOFTWARE rendering (%s) - no GPU driver for this "
                    "build's architecture. Expect low frame rates and no vsync.",
                    renderer);
        } else {
            SDL_Log("Hardware rendering confirmed: %s", renderer);
        }
        
        // These should be configurable video options.
        /* Check the result: this request is honoured on Windows but drivers and
           compositors are free to refuse it (Mesa with vblank_mode=0, some X11
           and Wayland setups). Silently ignoring a refusal leaves the frontend
           free-running — the menu is a 640x480 blit plus one quad, so it will
           happily spin at several hundred fps, burning a core to draw a static
           screen. Log it so an uncapped frame rate is explainable rather than
           mysterious. */
#ifdef AVP_PCVR
        /* PCVR: the mirror window must never pace the headset — with vsync on, a
           60 Hz monitor caps xrWaitFrame-driven rendering at 60 fps. The runtime
           owns frame pacing, so the desktop swap chain runs at interval 0.
           This branch matters on every call AFTER the first: SetOGLVideoMode is
           re-entered on a video mode change, the XR init below runs only once
           (it is guarded on !xr_enabled), and without this the request for
           interval 1 above would quietly put vsync back and cap the headset. */
        if (xr_enabled) {
            if (!SDL_GL_SetSwapInterval(0))
                SDL_Log("WARNING: could not disable vsync for the mirror window (%s)",
                        SDL_GetError());
            else
                SDL_Log("vsync disabled (mirror window; the XR runtime paces frames)");
        } else
#endif
        if (!SDL_GL_SetSwapInterval(1)) {
            SDL_Log("WARNING: vsync request refused (%s) - frame rate is uncapped",
                    SDL_GetError());
        } else {
            SDL_Log("vsync enabled (swap interval 1)");
        }
        /* The window was created SDL_WINDOW_FULLSCREEN with no mode set, which
           SDL3 resolves to borderless desktop. Apply the saved selection now. */
        ApplySelectedVideoMode();
        {
            /* The rate vsync is capping to, and what the FPS counter shows after
               the "/" on flat builds. */
            extern float Platform_GetDisplayRefreshHz(void);
            float hz = Platform_GetDisplayRefreshHz();
            if (hz > 0.0f) SDL_Log("display refresh: %.0f Hz", hz);
            else           SDL_Log("display refresh: unknown");
        }
        
        load_ogl_functions(1);
        
        // Only compile shaders once — they survive across SetOGLVideoMode calls
        static int shadersInitialized = 0;
        if (!shadersInitialized) {
            InitGameShader();
            InitCinemaShader();
            shadersInitialized = 1;
        }
        
        SDL_GetWindowSize(window, &Width, &Height);
        pglViewport(0, 0, Width, Height);
        
        // create fullscreen window texture
        pglGenTextures(1, &FullscreenTexture);
        
        pglBindTexture(GL_TEXTURE_2D, FullscreenTexture);
        
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        FullscreenTextureWidth = 1024;
        FullscreenTextureHeight = 512;
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, FullscreenTextureWidth, FullscreenTextureHeight, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
        
        /* ---- Native OpenXR initialisation ---- */
#ifdef AVP_XR
#ifndef AVP_DISABLE_XR   /* phone (non-VR) build: never touch OpenXR; xr_enabled stays
                           false, so the loop uses AvpShowViews() + SDL_GL_SwapWindow */
        if (!xr_enabled && !WantXR) {
            /* Asked for flat explicitly. Skip OpenXR entirely rather than
               relying on the failure fallback below — see WantXR. */
            SDL_Log("XR: OpenXR init skipped (-noxr / --flat / AVP_NO_XR) — flat desktop path");
        } else if (!xr_enabled) {
            /* Quest's VR shell launches us with a plain MAIN+LAUNCHER intent —
             * the com.oculus.intent.category.VR category is NOT propagated to
             * the Activity's Intent, even though the system is in IMMERSIVE
             * mode and the compositor is waiting for XR frames. So we can't
             * use the intent to decide whether to init XR. The manifest already
             * declares this as a VR app; always attempt OpenXR init and fall
             * back to 2D if it fails. (On PCVR the same shape applies: try
             * OpenXR; a machine with no runtime/headset falls back to flat.) */
            SDL_Log("XR: attempting OpenXR init");
            if (!init_xr_instance()) {
                SDL_Log("XR: init_xr_instance failed");
                goto xr_init_done;
            }
            if (!init_xr_session()) {
                SDL_Log("XR: init_xr_session failed — destroying instance so compositor stops waiting");
                if (xr_instance && pfn_xrDestroyInstance) {
                    pfn_xrDestroyInstance(xr_instance);
                    xr_instance = XR_NULL_HANDLE;
                }
                goto xr_init_done;
            }
            xr_enabled = true;
            SDL_Log("XR: native path active");
#ifdef AVP_PCVR
            /* The mirror window must never pace the headset: with vsync on, a
             * 60 Hz monitor caps xrWaitFrame-driven rendering at 60 fps. The
             * runtime paces frames from here on. This is the first-init case —
             * the vsync request above ran while xr_enabled was still false; a
             * later SetOGLVideoMode takes the xr_enabled branch up there. */
            if (!SDL_GL_SetSwapInterval(0))
                SDL_Log("WARNING: could not disable vsync for the mirror window (%s)",
                        SDL_GetError());
            else
                SDL_Log("vsync disabled (mirror window; the XR runtime paces frames)");
#endif
        }
        xr_init_done:;
#else
        SDL_Log("XR: disabled at build time (phone/non-VR variant) — flat render path");
#endif /* AVP_DISABLE_XR */
#endif /* AVP_XR */
        /* ---- end OpenXR init ---- */

    }

    SDL_GetWindowSize(window, &Width, &Height);

#if defined(__ANDROID__) && !defined(AVP_DISABLE_XR)
    /* Quest: 640x480 virtual coordinates so 2D HUD/progress-screen text (designed
       for 640x480 virtual space) normalises to correct NDC without glyph
       downscaling. Safe here because VR 3D mode (AvpShowViewsVR) overrides SDB to
       the eye FBO size before rendering the world, so the 3D aspect comes from the
       eye buffer, not from this. */
    SetWindowSize(Width, Height, 640, 480);
#elif defined(__ANDROID__)
    /* Phone/tablet (AVP_DISABLE_XR): native virtual size, exactly like desktop.
       This flavor renders the world through the FLAT AvpShowViews() path, which —
       unlike AvpShowViewsVR — never overrides SDB. Left at 640x480 the world was
       projected for 4:3 and then stretched across the real window (1280x672 on a
       Galaxy S7, ~1.9:1), i.e. squashed vertically. The menus looked right
       throughout because they are composited into the fixed 640x480 software
       surface and presented pillarboxed by FlipBuffers, independent of SDB. */
    SetWindowSize(Width, Height, Width, Height);
#elif defined(AVP_PCVR)
    /* Same 640x480 virtual space as Quest while the headset is active (the 2D
       menu/progress readback path assumes it); normal desktop sizing when XR
       init failed and we're running flat. */
    if (xr_enabled) {
        SetWindowSize(Width, Height, 640, 480);
    } else {
        SetWindowSize(Width, Height, Width, Height);
        MSAA_SetOutputSize(Width, Height); /* desktop MSAA target (window) resolution */
    }
#else
    SetWindowSize(Width, Height, Width, Height);
    MSAA_SetOutputSize(Width, Height); /* desktop MSAA target (window) resolution */
#endif

    pglEnable(GL_BLEND);
    pglBlendFunc(GL_SRC_ALPHA, GL_ONE);
    
    pglEnable(GL_DEPTH_TEST);
    pglDepthFunc(GL_LEQUAL);
    pglDepthMask(GL_TRUE);
    pglDepthRange(0.0, 1.0);
    
    //pglEnable(GL_TEXTURE_2D);// Stops GL error 0x0500
    
    pglDisable(GL_CULL_FACE);
    
    pglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    InitOpenGL();
    
    return 0;
}

int InitialiseWindowsSystem(HANDLE hInstance, int nCmdShow, int WinInitMode)
{
    return 0;
}

int ExitWindowsSystem()
{
    if (joy != NULL) {
        SDL_CloseJoystick(joy);
    }
    
    if (FullscreenTexture != 0) {
        pglDeleteTextures(1, &FullscreenTexture);
    }
    FullscreenTexture = 0;
    
    load_ogl_functions(0);
    
    if (surface != NULL) {
        SDL_DestroySurface(surface);
    }
    surface = NULL;
    
    if (context != NULL) {
        SDL_GL_DestroyContext(context);
    }
    context = NULL;
    
    if (window != NULL) {
        SDL_DestroyWindow(window);
    }
    window = NULL;
    
    return 0;
}

static int GotPrintScn, HavePrintScn;

static int KeySymToKey(int keysym)
{
    switch(keysym) {
        case SDLK_ESCAPE:
            return KEY_ESCAPE;
        
        case SDLK_0:
            return KEY_0;
        case SDLK_1:
            return KEY_1;
        case SDLK_2:
            return KEY_2;
        case SDLK_3:
            return KEY_3;
        case SDLK_4:
            return KEY_4;
        case SDLK_5:
            return KEY_5;
        case SDLK_6:
            return KEY_6;
        case SDLK_7:
            return KEY_7;
        case SDLK_8:
            return KEY_8;
        case SDLK_9:
            return KEY_9;
        
        case SDLK_A:
            return KEY_A;
        case SDLK_B:
            return KEY_B;
        case SDLK_C:
            return KEY_C;
        case SDLK_D:
            return KEY_D;
        case SDLK_E:
            return KEY_E;
        case SDLK_F:
            return KEY_F;
        case SDLK_G:
            return KEY_G;
        case SDLK_H:
            return KEY_H;
        case SDLK_I:
            return KEY_I;
        case SDLK_J:
            return KEY_J;
        case SDLK_K:
            return KEY_K;
        case SDLK_L:
            return KEY_L;
        case SDLK_M:
            return KEY_M;
        case SDLK_N:
            return KEY_N;
        case SDLK_O:
            return KEY_O;
        case SDLK_P:
            return KEY_P;
        case SDLK_Q:
            return KEY_Q;
        case SDLK_R:
            return KEY_R;
        case SDLK_S:
            return KEY_S;
        case SDLK_T:
            return KEY_T;
        case SDLK_U:
            return KEY_U;
        case SDLK_V:
            return KEY_V;
        case SDLK_W:
            return KEY_W;
        case SDLK_X:
            return KEY_X;
        case SDLK_Y:
            return KEY_Y;
        case SDLK_Z:
            return KEY_Z;
        
        case SDLK_LEFT:
            return KEY_LEFT;
        case SDLK_RIGHT:
            return KEY_RIGHT;
        case SDLK_UP:
            return KEY_UP;
        case SDLK_DOWN:
            return KEY_DOWN;
        case SDLK_RETURN:
            return KEY_CR;
        case SDLK_TAB:
            return KEY_TAB;
        case SDLK_INSERT:
            return KEY_INS;
        case SDLK_DELETE:
            return KEY_DEL;
        case SDLK_END:
            return KEY_END;
        case SDLK_HOME:
            return KEY_HOME;
        case SDLK_PAGEUP:
            return KEY_PAGEUP;
        case SDLK_PAGEDOWN:
            return KEY_PAGEDOWN;
        case SDLK_BACKSPACE:
            return KEY_BACKSPACE;
        case SDLK_COMMA:
            return KEY_COMMA;
        case SDLK_PERIOD:
            return KEY_FSTOP;
        case SDLK_SPACE:
            return KEY_SPACE;
        
        case SDLK_LSHIFT:
            return KEY_LEFTSHIFT;
        case SDLK_RSHIFT:
            return KEY_RIGHTSHIFT;
        case SDLK_LALT:
            return KEY_LEFTALT;
        case SDLK_RALT:
            return KEY_RIGHTALT;
        case SDLK_LCTRL:
            return KEY_LEFTCTRL;
        case SDLK_RCTRL:
            return KEY_RIGHTCTRL;
        
        case SDLK_CAPSLOCK:
            return KEY_CAPS;
        case SDLK_NUMLOCKCLEAR:
            return KEY_NUMLOCK;
        case SDLK_SCROLLLOCK:
            return KEY_SCROLLOK;
        
        case SDLK_KP_0:
            return KEY_NUMPAD0;
        case SDLK_KP_1:
            return KEY_NUMPAD1;
        case SDLK_KP_2:
            return KEY_NUMPAD2;
        case SDLK_KP_3:
            return KEY_NUMPAD3;
        case SDLK_KP_4:
            return KEY_NUMPAD4;
        case SDLK_KP_5:
            return KEY_NUMPAD5;
        case SDLK_KP_6:
            return KEY_NUMPAD6;
        case SDLK_KP_7:
            return KEY_NUMPAD7;
        case SDLK_KP_8:
            return KEY_NUMPAD8;
        case SDLK_KP_9:
            return KEY_NUMPAD9;
        case SDLK_KP_MINUS:
            return KEY_NUMPADSUB;
        case SDLK_KP_PLUS:
            return KEY_NUMPADADD;
        case SDLK_KP_PERIOD:
            return KEY_NUMPADDEL;
        case SDLK_KP_ENTER:
            return KEY_NUMPADENTER;
        case SDLK_KP_DIVIDE:
            return KEY_NUMPADDIVIDE;
        case SDLK_KP_MULTIPLY:
            return KEY_NUMPADMULTIPLY;
        
        case SDLK_LEFTBRACKET:
            return KEY_LBRACKET;
        case SDLK_RIGHTBRACKET:
            return KEY_RBRACKET;
        case SDLK_SEMICOLON:
            return KEY_SEMICOLON;
        case SDLK_APOSTROPHE:
            return KEY_APOSTROPHE;
        case SDLK_GRAVE:
            return KEY_GRAVE;
        case SDLK_BACKSLASH:
            return KEY_BACKSLASH;
        case SDLK_SLASH:
            return KEY_SLASH;
/*		case SDLK_
			return KEY_CAPITAL; */
        case SDLK_MINUS:
            return KEY_MINUS;
        case SDLK_EQUALS:
            return KEY_EQUALS;
        case SDLK_LGUI:
            return KEY_LWIN;
        case SDLK_RGUI:
            return KEY_RWIN;
/*		case SDLK_
			return KEY_APPS; */
        
        case SDLK_F1:
            return KEY_F1;
        case SDLK_F2:
            return KEY_F2;
        case SDLK_F3:
            return KEY_F3;
        case SDLK_F4:
            return KEY_F4;
        case SDLK_F5:
            return KEY_F5;
        case SDLK_F6:
            return KEY_F6;
        case SDLK_F7:
            return KEY_F7;
        case SDLK_F8:
            return KEY_F8;
        case SDLK_F9:
            return KEY_F9;
        case SDLK_F10:
            return KEY_F10;
        case SDLK_F11:
            return KEY_F11;
        case SDLK_F12:
            return KEY_F12;

/* finish foreign keys */
        
        default:
            return -1;
    }
}

char ShiftDown = 0;
char CapsLockOn = 0;
const char ShiftAddition[2] = { 32, 0 };

/* Keep SDL text input enabled so TEXT_INPUT events (and their correct
   shift/caps handling) keep arriving for the console and menu text fields.
   Desktop leaves this on permanently, which is free there — there is no
   on-screen keyboard.

   On Android it is NOT free: SDL_StartTextInput raises the system IME. Calling
   it from the event loop, on every key down/up, pinned the soft keyboard open
   over the running game — it covered roughly two thirds of the screen, resized
   the GL surface down to a strip, and swallowed the keystrokes it sat on, which
   is why only a few keys (modifiers, some numpad) appeared to reach gameplay
   with a Bluetooth keyboard attached.

   So on Android, Platform_SetTextInputActive() is the SOLE owner of IME state:
   the menu code drives it each frame from ActUponUsersInput and turns it on
   only while a text field is actually focused. Do not re-enable it from here. */
static void KeepTextInputAlive(void)
{
#ifndef __ANDROID__
    SDL_StartTextInput(window);
#endif
}

static void handle_keypress(int key, int unicode, int press)
{
    if (key == -1)
        return;
    
    if ((key == KEY_LEFTSHIFT) || (key == KEY_RIGHTSHIFT))
    {
        ShiftDown = press;
    }
    else if (press) {
        /* Printable characters (letters, digits, symbols) are delivered by
           SDL_EVENT_TEXT_INPUT, which honours the real keyboard layout and
           shift/caps-lock state. This path used to *also* synthesise a WM_CHAR
           from the raw key code, feeding the console a second copy of every
           letter/digit -- that produced the doubled "aa" in the command
           console (and "aA" when this path's shift/caps guess disagreed with
           the OS). Only the editing keys below, which TEXT_INPUT does not
           generate, are handled here now. */
        switch (key) {
                case KEY_CAPS:
                    CapsLockOn ^= 1;
                    break;
                case KEY_CR:
                    KeepTextInputAlive();
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR('\r');
                    break;
                case KEY_BACKSPACE:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_BACK);
                    break;
                case KEY_END:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_END);
                    break;
                case KEY_HOME:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_HOME);
                    break;
                case KEY_LEFT:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_LEFT);
                    break;
                case KEY_UP:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_UP);
                    break;
                case KEY_RIGHT:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_RIGHT);
                    break;
                case KEY_DOWN:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_DOWN);
                    break;
                case KEY_INS:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_INSERT);
                    break;
                case KEY_DEL:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_DELETE);
                    break;
                case KEY_TAB:
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_KEYDOWN(VK_TAB);
                    break;
                default:
                    KeepTextInputAlive();
                    break;
            }
    }
    
    if (press && !KeyboardInput[key]) {
        DebouncedKeyboardInput[key] = 1;
        DebouncedGotAnyKey = 1;
    }

    if (press)
        GotAnyKey = 1;
    KeyboardInput[key] = press;
}

/* Refresh rate of the display the game window is currently on, for the FPS
   counter's "/<n> Hz" on flat builds. 0 when unknown, which the callers treat as
   "show fps only".

   Queried live rather than cached so dragging the window to a second monitor
   with a different rate is picked up. It is only called while the counter is
   actually being drawn, so the cost is irrelevant.

   This is the flat-path counterpart to VR_GetTargetHz(): a headset's rate comes
   from the OpenXR runtime, a monitor's from SDL. Callers prefer the former when
   a session is live. */
float Platform_GetDisplayRefreshHz(void)
{
    SDL_DisplayID id;
    const SDL_DisplayMode *mode;

    if (!window) return 0.0f;

    id = SDL_GetDisplayForWindow(window);
    if (!id) return 0.0f;

    mode = SDL_GetCurrentDisplayMode(id);
    if (!mode || mode->refresh_rate <= 0.0f) return 0.0f;

    return mode->refresh_rate;
}

/* Show/hide the system on-screen keyboard for menu text entry. Idempotent —
   only calls into SDL when the active state actually changes. On Quest this
   brings up the Meta system keyboard so a name / multiplayer IP / etc. can be
   typed; the resulting SDL_EVENT_TEXT_INPUT events feed KeyboardEntryQueue_Add.
   Driven each frame from the menu code (ActUponUsersInput). */
void Platform_SetTextInputActive(int active)
{
    static int current = -1;
    if (active == current) return;
    current = active;
    if (active)
        SDL_StartTextInput(window);
    else
        SDL_StopTextInput(window);
}

void CheckForWindowsMessages()
{
    SDL_Event event;
    float x, y, wantmouse;
    int buttons;

    GotAnyKey = 0;
    DebouncedGotAnyKey = 0;
    secure_avpzero(DebouncedKeyboardInput, sizeof DebouncedKeyboardInput);

#ifdef AVP_XR
    /* Process XR session state events before input is read this frame.
     * Without this, handle_xr_events() would only run in FlipBuffers()
     * (after ReadUserInput), so xrSyncActions would see stale session state. */
    if (xr_enabled) handle_xr_events();
#endif

    wantmouse =	(SDL_GetWindowRelativeMouseMode(window) == true);
    
    // "keyboard" events that don't have an up event
    KeyboardInput[KEY_MOUSEWHEELUP] = 0;
    KeyboardInput[KEY_MOUSEWHEELDOWN] = 0;
    
    while (SDL_PollEvent(&event)) {
        switch(event.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                break;
            /* A touch counts as "any key".
             *
             * On a touch-only device nothing could otherwise satisfy the
             * `while(!DebouncedGotAnyKey);` waits (the loading screen at the end
             * of Start_Progress_Bar, the intro logos, the credits):
             * DebouncedGotAnyKey is raised only by a real key event in
             * handle_keypress, by a joystick button, or by the XR controller
             * block above — and that block is compiled out on the phone, which
             * sets AVP_DISABLE_XR. The mouse-button cases above are empty stubs
             * and there is no other touch handling in the tree.
             *
             * Deliberately hooked to FINGER_DOWN rather than to the mouse
             * cases: SDL synthesises mouse events from touch, so this is enough
             * for a phone, while desktop mouse behaviour is left exactly as it
             * was. That matters because DebouncedGotAnyKey also dismisses the
             * completed-level stats screen (hud.c) and the death screen
             * (pmove.c) — making a click "any key" would let a reflexive click
             * after dying restart the level instantly.
             *
             * Both flags are set: CheckForWindowsMessages clears them at the
             * top of each frame, and GotAnyKey on its own is what lets a touch
             * skip the intro logos (avp_intro.cpp).
             *
             * NOTE: this is a usability gap for touch-only devices, NOT the
             * cause of the phone "stuck on Press any key to continue" report —
             * that was the missing present further down (see the InGameFlipBuffers
             * comment in the game loop). With a keyboard attached the prompt
             * always worked; the display just never updated afterwards. */
            case SDL_EVENT_FINGER_DOWN:
                if (!GotAnyKey)
                    DebouncedGotAnyKey = 1;
                GotAnyKey = 1;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (wantmouse) {
                    if (event.wheel.y < 0) {
                        handle_keypress(KEY_MOUSEWHEELDOWN, 0, 1);
                    } else if (event.wheel.y > 0) {
                        handle_keypress(KEY_MOUSEWHEELUP, 0, 1);
                    }
                }
                break;
            case SDL_EVENT_TEXT_INPUT: {
                KeepTextInputAlive();
                int unicode = event.text.text[0]; //TODO convert to utf-32
                if (unicode && !(unicode & 0xFF80)) {
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(unicode);
                    KeyboardEntryQueue_Add(unicode);
                }
            }
                break;
            case SDL_EVENT_KEY_DOWN:
                KeepTextInputAlive();
                if (event.key.key == SDLK_PRINTSCREEN) {
                    if (HavePrintScn == 0)
                        GotPrintScn = 1;
                    HavePrintScn = 1;
                } else {
                    handle_keypress(KeySymToKey(event.key.key), 0, 1);
                }
                break;
            case SDL_EVENT_KEY_UP:
                KeepTextInputAlive();
                if (event.key.key == SDLK_PRINTSCREEN) {
                    GotPrintScn = 0;
                    HavePrintScn = 0;
                } else {
                    handle_keypress(KeySymToKey(event.key.key), 0, 0);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // disable mouse grab?
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                //printf("test, %d,%d\n", event.window.data1, event.window.data2);
                /* Hiding and re-showing a fullscreen window can emit a
                   degenerate size; SetWindowSize and the MSAA target both
                   divide by these. */
                if (event.window.data1 <= 0 || event.window.data2 <= 0) break;
                WindowWidth = event.window.data1;
                WindowHeight = event.window.data2;
                if (RenderingMode == RENDERING_MODE_SOFTWARE) {
                    SetWindowSize(WindowWidth, WindowHeight, 640, 480);
                } else {
                    SetWindowSize(WindowWidth, WindowHeight, WindowWidth, WindowHeight);
                }
                if (pglViewport != NULL) {
                    pglViewport(0, 0, WindowWidth, WindowHeight);
                }
#ifndef __ANDROID__
                MSAA_SetOutputSize(WindowWidth, WindowHeight); /* rebuild the MSAA target at the new size */
#endif
                break;
            case SDL_EVENT_QUIT:
                SDL_Log("EXIT: SDL_EVENT_QUIT received - terminating");
                AvP.MainLoopRunning = 0; /* TODO */
#ifdef AVP_XR
                /* End the XR session before the process goes away. Exiting with a
                   live session leaves the compositor holding one it can never get
                   a frame from — the same reason the normal exit path calls this
                   (see destroy_xr_resources). This used to be a bare exit(0). */
                destroy_xr_resources();
#endif
                SDL_StopTextInput(window);
                exit(0); //TODO
                break;
#ifdef __ANDROID__
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!gamepad) {
                    gamepad = SDL_OpenGamepad(event.gdevice.which);
                    if (gamepad) {
                        GotJoystick = 1;
                        JoystickCaps.wCaps = 0;
                        JoystickData.dwXpos = 32768;
                        JoystickData.dwYpos = 32768;
                        JoystickData.dwPOV  = (DWORD) -1;
                    }
                }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (gamepad && SDL_GetGamepadID(gamepad) == event.gdevice.which) {
                    SDL_CloseGamepad(gamepad);
                    gamepad = NULL;
                    GotJoystick = 0;
                }
                break;
#endif
            case SDL_EVENT_JOYSTICK_ADDED:
                /* Open the first controller that connects if we don't have one yet. */
                if (WantJoystick && !joy && !GotJoystick) {
                    joy = SDL_OpenJoystick(event.jdevice.which);
                    if (joy) {
                        GotJoystick = 1;
                        JoystickCaps.wCaps = 0;
                        JoystickData.dwXpos = 32768;
                        JoystickData.dwYpos = 32768;
                        JoystickData.dwRpos = 0;
                        JoystickData.dwUpos = 0;
                        JoystickData.dwVpos = 0;
                        JoystickData.dwPOV = (DWORD) -1;
                    }
                }
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                if (joy && SDL_GetJoystickID(joy) == event.jdevice.which) {
                    SDL_CloseJoystick(joy);
                    joy = NULL;
                    GotJoystick = 0;
                }
                break;
        }
    }
    
    buttons = SDL_GetRelativeMouseState(&x, &y);
    
    if (wantmouse) {
        if (buttons & SDL_BUTTON_MASK(1))
            handle_keypress(KEY_LMOUSE, 0, 1);
        else
            handle_keypress(KEY_LMOUSE, 0, 0);
        if (buttons & SDL_BUTTON_MASK(2))
            handle_keypress(KEY_MMOUSE, 0, 1);
        else
            handle_keypress(KEY_MMOUSE, 0, 0);
        if (buttons & SDL_BUTTON_MASK(3))
            handle_keypress(KEY_RMOUSE, 0, 1);
        else
            handle_keypress(KEY_RMOUSE, 0, 0);
        
        MouseVelX = DIV_FIXED(x, NormalFrameTime);
        MouseVelY = DIV_FIXED(y, NormalFrameTime);
    } else {
        KeyboardInput[KEY_LMOUSE] = 0;
        KeyboardInput[KEY_MMOUSE] = 0;
        KeyboardInput[KEY_RMOUSE] = 0;
        MouseVelX = 0;
        MouseVelY = 0;
    }
    
    if (GotJoystick) {
        float numbuttons;
        int x;
        
        SDL_UpdateJoysticks();
        
        numbuttons = SDL_GetNumJoystickButtons(joy);
        if (numbuttons > 16) numbuttons = 16;
        
        for (x = 0; x < numbuttons; x++) {
            if (SDL_GetJoystickButton(joy, x)) {
                GotAnyKey = 1;
                if (!KeyboardInput[KEY_JOYSTICK_BUTTON_1+x]) {
                    KeyboardInput[KEY_JOYSTICK_BUTTON_1+x] = 1;
                    DebouncedKeyboardInput[KEY_JOYSTICK_BUTTON_1+x] = 1;
                }
            } else {
                KeyboardInput[KEY_JOYSTICK_BUTTON_1+x] = 0;
            }
        }
    }

//#warning Redo WantX, need to split it out better so fullscreen can temporary set relative without clobbering user setting
    if ((KeyboardInput[KEY_LEFTALT]||KeyboardInput[KEY_RIGHTALT]) && DebouncedKeyboardInput[KEY_CR]) {
        if (WantFullscreenToggle != 0) {
            int displayMode = SDL_GetWindowFlags(window);
            //printf("Current window mode:%08x\n", displayMode);
            if ((displayMode & (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_FULLSCREEN)) != 0) {
                SDL_SetWindowFullscreen(window, 0);
            } else {
                SDL_SetWindowFullscreen(window, WantResolutionChange ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN);
            }
            
            displayMode = SDL_GetWindowFlags(window);
            //printf("New window mode:%08x\n", displayMode);
            if ((displayMode & (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_FULLSCREEN)) != 0) {
                SDL_SetWindowRelativeMouseMode(window, true);
                WantFullscreen = 1;
            } else {
                SDL_SetWindowRelativeMouseMode(window, WantMouseGrab ? true : false);
                WantFullscreen = 0;
            }
        }
    }
    
    if (KeyboardInput[KEY_LEFTCTRL] && DebouncedKeyboardInput[KEY_G]) {
        int IsWindowed = (SDL_GetWindowFlags(window) & (SDL_WINDOW_FULLSCREEN|SDL_WINDOW_FULLSCREEN)) == 0;
        
        if (IsWindowed) {
            WantMouseGrab = WantMouseGrab != 0 ? 0 : 1;
            if (WantMouseGrab != 0) {
                SDL_SetWindowRelativeMouseMode(window, true);
            } else {
                SDL_SetWindowRelativeMouseMode(window, false);
            }
            WantMouseGrab = (SDL_GetWindowRelativeMouseMode(window) == true);
        }
    }
    
    // a second reset of relative mouse state because
    // enabling relative mouse mode moves the mouse
    SDL_SetWindowRelativeMouseMode(window, true);
    SDL_GetRelativeMouseState(NULL, NULL);
    
    if (GotPrintScn) {
        GotPrintScn = 0;
        
        ScreenShot();
    }
}

/* Present the 640x480 software menu/progress surface to the desktop window,
 * letterboxed. Split out of FlipBuffers so the PCVR mirror can reuse it: in VR
 * the 2D screens are composited into a headset quad layer by render_frame(),
 * which leaves the window untouched, so both flip paths call this to put the
 * same picture on the monitor. Ends in SDL_GL_SwapWindow.
 *
 * Sets the viewport explicitly rather than inheriting it: the VR menu path
 * leaves it sized to the menu swapchain. */
static void PresentSoftwareSurface(void)
{
    if (surface == NULL) return;

#ifdef AVP_PCVR
    /* Turn sRGB write conversion off for the whole present. vr_sc_release()
       ENABLES GL_FRAMEBUFFER_SRGB on its way out of render_frame(), so on the VR
       menu path we arrive here with it on — and the menu surface holds values
       that are already gamma-encoded, exactly like the eye images. Left on, the
       driver encodes them a second time on the way into the back buffer and the
       frontend mirrors washed out and far too bright, while gameplay (whose blit
       forces this off) looks correct. Flat builds never enable it at all. */
    GLboolean had_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB_EXT);
    if (had_srgb) glDisable(GL_FRAMEBUFFER_SRGB_EXT);
#endif

    glViewport(0, 0, ViewportWidth, ViewportHeight);

    // RESET STATE for software blit - prevent PBO/VAO/VBO issues
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    pglPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    // Upload software surface
    pglBindTexture(GL_TEXTURE_2D, FullscreenTexture);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    
    pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 640, 480,
                     GL_RGB, GL_UNSIGNED_SHORT_5_6_5, surface->pixels);
    
    // Letterbox coords
    GLfloat x0, x1, y0, y1, s0, s1, t0, t1;
    
    GLfloat a = ViewportHeight * 640.0f / 480.0f;
    GLfloat b = ViewportWidth  * 480.0f / 640.0f;
    
    if (a <= ViewportWidth) {
        
        y0 = -1.0f; y1 = 1.0f;
        
        
        x1 = 1.0f - (ViewportWidth - a) / ViewportWidth;
        x0 = -x1;
    } else {
        
        x0 = -1.0f; x1 = 1.0f;
        
        
        y1 = 1.0f - (ViewportHeight - b) / ViewportHeight;
        y0 = -y1;
    }
    
    s0 = 0.0f; s1 = 640.0f / (float)FullscreenTextureWidth;
    
    t0 = 0.0f; t1 = 480.0f / (float)FullscreenTextureHeight;
    
    GLfloat verts[6 * 4] = {
            x0, y0,  s0, t1,
            x1, y0,  s1, t1,
            x1, y1,  s1, t0,
            x0, y0,  s0, t1,
            x1, y1,  s1, t0,
            x0, y1,  s0, t0,
    };
    
    /* Restored after the blit. The engine enables GL_BLEND once at init and only
       ever changes blend FUNC afterwards, so leaving it off here would collapse
       every later translucent/additive pass — which now matters because the VR
       menu path reaches this code and then goes back to rendering the world. */
    GLboolean had_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean had_blend = glIsEnabled(GL_BLEND);

    pglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_BLEND);
    
    // Disable all attribs to start clean
    for (int i = 0; i < 8; i++) glDisableVertexAttribArray(i);
    
    glUseProgram(CinemaShader.program);
    glActiveTexture(GL_TEXTURE0);
    pglBindTexture(GL_TEXTURE_2D, FullscreenTexture);
    glUniform1i(CinemaShader.uTexture, 0);
    
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), &verts[0]);
    
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), &verts[2]);
    
    glDrawArrays(GL_TRIANGLES, 0, 6);
    
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    
    // Restore game shader state for next frame
    RestoreGameShaderState();
    if (had_depth) pglEnable(GL_DEPTH_TEST);
    if (had_blend) pglEnable(GL_BLEND);
#ifdef AVP_PCVR
    if (had_srgb) glEnable(GL_FRAMEBUFFER_SRGB_EXT);
#endif
    
    SDL_GL_SwapWindow(window);
}

void InGameFlipBuffers(void)
{
#if !defined(NDEBUG)
    check_for_errors();
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        SDL_Log("GL error: 0x%04X", err);
#endif
#ifdef AVP_XR
    if (xr_enabled) {
        handle_xr_events();
        if (xr_session_running && view_count > 0 && vr_swapchains != NULL) {
            if (xr_2d_mode) {
                /* Progress screen — 1:1 readback from the 640x480 viewport that
                   VR_Set2DViewport set in ThisFramesRenderingHasBegun.
                   No downscaling; only a Y-flip (GL origin is bottom-left). */
                if (RenderingMode == RENDERING_MODE_OPENGL && surface != NULL) {
                    static Uint8 *readback_buf = NULL;
                    if (!readback_buf)
                        readback_buf = malloc(640 * 480 * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, 640, 480, GL_RGBA, GL_UNSIGNED_BYTE, readback_buf);
                    Uint16 *dst = (Uint16 *)surface->pixels;
                    for (int y = 0; y < 480; y++) {
                        const Uint8 *row = readback_buf + (479 - y) * 640 * 4;
                        for (int x = 0; x < 640; x++) {
                            const Uint8 *p = row + x * 4;
                            *dst++ = ((p[0]>>3)<<11)|((p[1]>>2)<<5)|(p[2]>>3);
                        }
                    }
                    /* Restore native viewport for subsequent frames */
                    pglViewport(0, 0, ViewportWidth, ViewportHeight);
                }
            }
            /* Always call render_frame — it knows which mode to use */
            render_frame();
#ifdef AVP_PCVR
            /* Everything below runs AFTER render_frame(), i.e. after xrEndFrame
               has submitted this frame to the compositor. Nothing that can block
               on a desktop presentation lock — the swap, and the one-shot
               blank+hide when the setting is turned off — may precede the submit,
               or a monitor-side stall would delay headset frame delivery. The
               only mirror work that runs before it is the eye blit in
               avpview.c, which is a GPU copy with no present call in it and
               cannot be moved later: it reads the swapchain image, which is the
               runtime's again the moment we release it. */
            vr_mirror_park_window(1);   /* xrEndFrame just presented: safe to hide */

            /* Desktop mirror: xrEndFrame presented to the headset, the window
               still holds last frame. 2D screens live in a headset quad layer,
               so put the same software surface on the monitor; 3D frames were
               blitted into the window by VR_MirrorEyeToWindow during the eye
               pass and only need the swap. Skipping the swap when no eye was
               mirrored (swapchain image not ready) leaves the last good frame
               up rather than flipping to an undefined back buffer. */
            if (xr_2d_mode) {
                vr_mirror_pending = 0;
                if (vr_mirror_wanted_this_frame()) PresentSoftwareSurface();
            } else if (vr_mirror_pending) {
                vr_mirror_pending = 0;
                SDL_GL_SwapWindow(window);
            }
#endif
            return;
        }
#ifdef AVP_PCVR
        /* No submit happens on this path (the session is not running), so there
           is nothing to stay behind - and nothing is presenting to the headset
           yet either, so this blanks the window but deliberately does NOT hide
           it. See vr_mirror_park_window. */
        vr_mirror_park_window(0);

        /* Before the session reaches READY there are no eye images yet, so the
           flat present below drives the monitor directly — which is how the intro
           FMVs and the profile screen still reached it with the mirror off. Bail
           here instead. Keyed on xr_enabled (we are inside that test), so a PCVR
           exe running flat, with no runtime or on -noxr, is untouched: there the
           window IS the game. */
        if (vr_mirror_is_off()) {
            /* Resolve before bailing. MSAA_BeginFrame ran this frame — opengl.c
               only stands it down while VR_SessionActive(), which is false until
               the session is READY — so returning with the multisampled FBO still
               bound would leak it into the next frame. Resolve it onto the back
               buffer as usual and simply never present it. */
            MSAA_Resolve();
            return;
        }
#endif
        /* XR session not running (e.g. 2D panel mode) — fall through to SDL swap */
    }
#endif

#ifndef __ANDROID__
    /* Desktop: if this frame was rendered into the multisampled target, resolve
       it onto the backbuffer before presenting. No-op when MSAA is off. */
    MSAA_Resolve();
#endif

    SDL_GL_SwapWindow(window);
}

void FlipBuffers()
{
    // Always let the game render the menu into surface->pixels first
    // (the existing GL upload below keeps the flat window working too)

#ifndef __ANDROID__
    /* Safety net: this is the 2D/menu present path. If an in-game frame was begun
       (multisampled FBO bound) but we ended up here, drop it back to the
       backbuffer so the menu draws to the window, not the FBO. */
    MSAA_AbortFrame();
#endif
#ifdef AVP_XR
    if (xr_enabled) {
        handle_xr_events();
        if (xr_session_running && view_count > 0 && vr_swapchains != NULL) {
            render_frame();
#ifdef AVP_PCVR
            /* Everything below runs AFTER render_frame(), i.e. after xrEndFrame
               has submitted this frame to the compositor. Nothing that can block
               on a desktop presentation lock — the swap, and the one-shot
               blank+hide when the setting is turned off — may precede the submit,
               or a monitor-side stall would delay headset frame delivery. The
               only mirror work that runs before it is the eye blit in
               avpview.c, which is a GPU copy with no present call in it and
               cannot be moved later: it reads the swapchain image, which is the
               runtime's again the moment we release it. */
            vr_mirror_park_window(1);   /* xrEndFrame just presented: safe to hide */

            /* Desktop mirror: xrEndFrame presented to the headset, the window
               still holds last frame. 2D screens live in a headset quad layer,
               so put the same software surface on the monitor; 3D frames were
               blitted into the window by VR_MirrorEyeToWindow during the eye
               pass and only need the swap. Skipping the swap when no eye was
               mirrored (swapchain image not ready) leaves the last good frame
               up rather than flipping to an undefined back buffer. */
            if (xr_2d_mode) {
                vr_mirror_pending = 0;
                if (vr_mirror_wanted_this_frame()) PresentSoftwareSurface();
            } else if (vr_mirror_pending) {
                vr_mirror_pending = 0;
                SDL_GL_SwapWindow(window);
            }
#endif
            return;
        }
#ifdef AVP_PCVR
        /* No submit happens on this path (the session is not running), so there
           is nothing to stay behind - and nothing is presenting to the headset
           yet either, so this blanks the window but deliberately does NOT hide
           it. See vr_mirror_park_window. */
        vr_mirror_park_window(0);

        /* Before the session reaches READY there are no eye images yet, so the
           flat present below drives the monitor directly — which is how the intro
           FMVs and the profile screen still reached it with the mirror off. Bail
           here instead. Keyed on xr_enabled (we are inside that test), so a PCVR
           exe running flat, with no runtime or on -noxr, is untouched: there the
           window IS the game. */
        if (vr_mirror_is_off()) return;
#endif
        /* XR session not running (e.g. 2D panel mode) — fall through to SDL swap */
    }
#endif
    
    PresentSoftwareSurface();
}

char *AvpCDPath = 0;

#if !defined(_MSC_VER)
static const struct option getopt_long_options[] = {
        { "help",	0,	NULL,	'h' },
        { "version",	0,	NULL,	'v' },
        { "fullscreen",	0,	NULL,	'f' },
        { "windowed",	0,	NULL,	'w' },
        { "nosound",	0,	NULL,	's' },
        { "nocdrom",	0,	NULL,	'c' },
        { "nojoy",	0,	NULL,	'j' },
        { "debug",	0,	NULL,	'd' },
        { "withgl",	1,	NULL,	'g' },
        { "datapath",	1,	NULL,	'p' },
        { "noxr",	0,	NULL,	'X' },
        { "flat",	0,	NULL,	'X' },
/*
{ "loadrifs",	1,	NULL,	'l' },
{ "server",	0,	someval,	1 },
{ "client",	1,	someval,	2 },
*/
        { NULL,		0,	NULL,	0 },
};
#endif

/* Printed by --help on every platform now, so no "Linux" label (same reason as
   AvPVersionString in version.c). Keep in step with BOTH parsers below — the getopt
   one and the manual MSVC one. */
static const char *usage_string =
        "Aliens vs Predator - http://www.icculus.org/avp/\n"
        "Based on Rebellion Developments AvP Gold source\n"
        "      [-h | --help]           Display this help message\n"
        "      [-v | --version]        Display the game version\n"
        "      [-f | --fullscreen]     Run the game fullscreen\n"
        "      [-w | --windowed]       Run the game in a window\n"
        "      [-s | --nosound]        Do not access the soundcard\n"
        "      [-c | --nocdrom]        Do not access the CD-ROM\n"
        "      [-j | --nojoy]          Do not access the joystick\n"
        "      [-d | --debug]          Enable the debugging/cheat console commands\n"
        "      [-p | --datapath] [x]   Look at [x] for game files\n"
        "      [-g | --withgl] [x]     Accepted and ignored (legacy dlopen-libGL option)\n"
        "      [--noxr | --flat]       VR builds: skip OpenXR, run on the desktop\n"
;

int main(int argc, char *argv[])
{
    /* Unbuffer the diagnostic streams. Redirecting to a file — the documented way
       to capture a log — makes stdout fully buffered, so a hard crash discards up
       to 4 KB of the most recent output: exactly the lines naming where it died.
       Costs nothing at these volumes and makes a truncated log mean "it stopped
       here" rather than "the buffer was lost". */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    //NEEDED?
    //SDL_GLContext g_MainGLContext = NULL;
    //g_MainGLContext = SDL_GL_CreateContext(window);
    SDL_Log("Attempting to create window with SDL3...");
#if !defined(_MSC_VER)
    int c;
    
    opterr = 0;
    while ((c = getopt_long(argc, argv, "hvfwscdjg:p:X", getopt_long_options, NULL)) != -1) {
        switch(c) {
            case 'h':
                printf("%s", usage_string);
                exit(EXIT_SUCCESS);
            case 'v':
                printf("%s", AvPVersionString);
                exit(EXIT_SUCCESS);
            case 'f':
                WantFullscreen = 1;
                break;
            case 'w':
                WantFullscreen = 0;
                break;
            case 's':
                WantSound = 0;
                break;
            case 'c':
                WantCDRom = 0;
                break;
            case 'j':
                WantJoystick = 0;
                break;
            case 'd': {
                extern int DebuggingCommandsActive;
                DebuggingCommandsActive = 1;
            }
                break;
            case 'X':
                WantXR = 0;
                break;
            case 'g':
                opengl_library = optarg;
                break;
            case 'p':
                gamedatapath = optarg;
                break;
            default:
                printf("%s", usage_string);
                exit(EXIT_FAILURE);
        }
    }
#else
    /* MSVC has no getopt, so the block above is compiled out on Windows.
       Without a replacement, every command-line switch -- including -debug --
       was silently ignored, leaving DebuggingCommandsActive at 0 so console
       cheat codes like GOD did nothing. Scan argv manually for the flags we
       support on Windows. Accept -debug, -d and --debug for the debug switch. */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            /* Informational switches exit before SDL/InitGameDirectories, matching the
               getopt path above, so the game never opens a window for them. */
            printf("%s", usage_string);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("%s", AvPVersionString);
            exit(EXIT_SUCCESS);
        } else if ((!strcmp(a, "-g") || !strcmp(a, "--withgl")) && (i + 1 < argc)) {
            opengl_library = argv[++i];
        } else if (!strcmp(a, "-f") || !strcmp(a, "--fullscreen")) {
            WantFullscreen = 1;
        } else if (!strcmp(a, "-w") || !strcmp(a, "--windowed")) {
            WantFullscreen = 0;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--nosound")) {
            WantSound = 0;
        } else if (!strcmp(a, "-c") || !strcmp(a, "--nocdrom")) {
            WantCDRom = 0;
        } else if (!strcmp(a, "-j") || !strcmp(a, "--nojoy")) {
            WantJoystick = 0;
        } else if (!strcmp(a, "-d") || !strcmp(a, "-debug") || !strcmp(a, "--debug")) {
            extern int DebuggingCommandsActive;
            DebuggingCommandsActive = 1;
        } else if ((!strcmp(a, "-p") || !strcmp(a, "--datapath")) && (i + 1 < argc)) {
            gamedatapath = argv[++i];
        } else if (!strcmp(a, "-noxr") || !strcmp(a, "--noxr") ||
                   !strcmp(a, "-flat")  || !strcmp(a, "--flat")) {
            WantXR = 0;
        }
    }
#endif

    /* Same switch as an environment variable, for launchers and shortcuts that
       cannot pass arguments. Any value counts, including an empty one. */
    if (SDL_getenv("AVP_NO_XR") != NULL)
        WantXR = 0;
    SDL_Log("BOOT: InitSDL done");
    //SDL_Log("DEBUG: argv[0] is %s", (argv[0] ? argv[0] : "NULL"));
    //SDL_Log("DEBUG: gamedatapath is %s", (gamedatapath ? gamedatapath : "NULL"));
#ifdef __ANDROID__
    if (gamedatapath == NULL)
        gamedatapath = SDL_GetAndroidExternalStoragePath();
#endif
    InitGameDirectories(argv[0], gamedatapath);
    SDL_Log("BOOT: InitGameDirectories done");
    
    if (InitSDL() == -1) {
        fprintf(stderr, "Could not find a sutable resolution!\n");
        fprintf(stderr, "At least 512x384 is needed.  Does OpenGL work?\n");
        exit(EXIT_FAILURE);
    }
    
    LoadCDTrackList();
    
    SetFastRandom();

#if MARINE_DEMO
    ffInit("fastfile/mffinfo.txt","fastfile/");
#elif ALIEN_DEMO
    ffInit("alienfastfile/ffinfo.txt","alienfastfile/");
#else
    ffInit("fastfile/ffinfo.txt","fastfile/");
    SDL_Log("BOOT: ffInit done");
#endif
    InitGame();
    SDL_Log("BOOT: InitGame done");
    
    WindowWidth = VideoModeList[CurrentVideoMode].w;
    WindowHeight = VideoModeList[CurrentVideoMode].h;

    SetOGLVideoMode(0, 0);
    SDL_Log("BOOT: SetOGLVideoMode done");
    SetSoftVideoMode(640, 480, 16);

    InitialVideoMode();
    SDL_Log("BOOT: InitialVideoMode done");

    /* Env_List can probably be removed */
    Env_List[0]->main = LevelName;

    InitialiseSystem();
    SDL_Log("BOOT: InitialiseSystem done");
    InitialiseRenderer();
    SDL_Log("BOOT: InitialiseRenderer done");

    LoadKeyConfiguration();

    SoundSys_Start();
    SDL_Log("BOOT: SoundSys_Start done");
    if (WantCDRom) CDDA_Start();

    {
        extern void PatchCDVolumeMenuForNoMusic(void);
        if (!CDDA_HasMusicFiles()) PatchCDVolumeMenuForNoMusic();
    }

    /* Drop "Video Card & Resolution" where it cannot do anything (see the note
       on PatchOutVideoModeMenu). XR init has already run by this point, so
       VR_HeadsetActive() is valid — which keeps the row for a PCVR exe running
       flat, where it genuinely sizes the window. */
    {
        extern void PatchOutVideoModeMenu(void);
#ifdef __ANDROID__
        /* Phone and Quest alike: the window is created FULLSCREEN, so the
           selected resolution is ignored either way. */
        PatchOutVideoModeMenu();
#else
        if (VR_HeadsetActive()) PatchOutVideoModeMenu();
#endif
    }

    InitTextStrings();
    SDL_Log("BOOT: InitTextStrings done");

    BuildMultiplayerLevelNameArray();

    ChangeDirectDrawObject();
    SDL_Log("BOOT: ChangeDirectDrawObject done");
    AvP.LevelCompleted = 0;
    LoadSounds("PLAYER");
    SDL_Log("BOOT: LoadSounds done");
    
    /* is this still neccessary? */
    AvP.CurrentEnv = AvP.StartingEnv = 0;

#if ALIEN_DEMO
    AvP.PlayerType = I_Alien;
	SetLevelToLoad(AVP_ENVIRONMENT_INVASION_A);
#elif PREDATOR_DEMO
    AvP.PlayerType = I_Predator;
	SetLevelToLoad(AVP_ENVIRONMENT_INVASION_P);
#elif MARINE_DEMO
    AvP.PlayerType = I_Marine;
	SetLevelToLoad(AVP_ENVIRONMENT_INVASION);
#endif

#if !(ALIEN_DEMO|PREDATOR_DEMO|MARINE_DEMO)
    //SDL_Log("DEBUG REACHED HERE BEFORE 'while AvP_MainMenus()' in main.c***");
    while (AvP_MainMenus())
        //SDL_Log("DEBUG REACHED HERE AFTER 'while AvP_MainMenus()' in main.c***");
#else
        if (AvP_MainMenus())
#endif
    {
        int menusActive = 0;
        int thisLevelHasBeenCompleted = 0;
        
        /* turn off any special effects */
        d3d_light_ctrl.ctrl = LCCM_NORMAL;
        
        SetOGLVideoMode(0, 0);
        
        InitialiseGammaSettings(RequestedGammaSetting);
        
        start_of_loaded_shapes = load_precompiled_shapes();
        
        SDL_Log("About to call InitCharacter");
        InitCharacter();
        
        LoadRifFile(); /* sets up a map */
        
        AssignAllSBNames();
        
        StartGame();
        
        ffcloseall();
        
        AvP.MainLoopRunning = 1;
        
        ScanImagesForFMVs();
        
        ResetFrameCounter();
        
        Game_Has_Loaded();
        
        ResetFrameCounter();
        
        if(AvP.Network!=I_No_Network)
        {
            /*Need to choose a starting position for the player , but first we must look
            through the network messages to find out which generator spots are currently clear*/
            netGameData.myGameState = NGS_Playing;
            MinimalNetCollectMessages();
            TeleportNetPlayerToAStartingPosition(Player->ObStrategyBlock,1);
        }
        
        IngameKeyboardInput_ClearBuffer();
#ifdef AVP_XR
        vr_recalibrate = 1;   // recalibrate heading + room-scale on first VR frame
        xr_2d_mode = false;   // 3D game starting — stop quad rendering
        SDL_Log("*** xr_2d_mode set to FALSE — game starting ***");
#endif
        while(AvP.MainLoopRunning) {
#ifdef AVP_XR
            if (xr_should_quit) {
                SDL_Log("EXIT: xr_should_quit set - leaving the game loop");
                break;
            }
#endif
            CheckForWindowsMessages();
            
            switch(AvP.GameMode) {
                case I_GM_Playing:
                    if ((!menusActive || (AvP.Network!=I_No_Network && !netGameData.skirmishMode)) && !AvP.LevelCompleted) {
                        /* TODO: print some debugging stuff */
                        
                        DoAllShapeAnimations();
                        
                        UpdateGame();

#ifdef AVP_XR
                        /* Death "Level not completed" screen: once the death sequence
                         * has settled (see deathFadeLevel gate below), present the stats
                         * on the world-anchored 2D quad — like the menus — instead of the
                         * head-locked HUD, so they stay fixed in front of where you were
                         * looking and you can look around them. Render the flat frame +
                         * "Level not completed" stats to the 640x480 surface, which the
                         * common 2D flip (further down) reads back and submits as the
                         * quad. render_frame() owns xrWaitFrame/Begin in 2D mode, so we
                         * must NOT call VR_WaitAndBeginFrame()/InGameFlipBuffers() here.
                         * The quad pose is snapshotted on the first 2D frame (see
                         * render_frame), i.e. facing where you were looking when the
                         * death-cam finished. */
                        extern int deathFadeLevel;   /* ONE_FIXED at death, ramps to 0 as the death-cam settles */
                        PLAYER_STATUS *vrDeathPS = (Player && Player->ObStrategyBlock)
                            ? (PLAYER_STATUS *)Player->ObStrategyBlock->SBdataptr : NULL;
                        /* Only switch to the world-anchored stats panel once the death
                           sequence has finished (deathFadeLevel == 0). While it is still
                           playing, fall through to the normal 3D render so the
                           head-locked death-cam drop plays exactly as it always did —
                           playing it flat on the quad is too jarring. */
                        int vrDeathScreen = (xr_enabled && xr_session_running)
                                         && (AvP.Network == I_No_Network)
                                         && vrDeathPS && !vrDeathPS->IsAlive
                                         && deathFadeLevel == 0;
                        if ((xr_enabled && xr_session_running) && InGameMenusAreRunning()) {
                            /* Multiplayer: the pause menu is up but the loop stays in this
                               gameplay branch to keep the network game simulating (a live
                               net game can't pause). Render the flat 2D menu instead of the
                               3D scene — mirrors the single-player menu branch — so the menu
                               quad presented by the xr_2d_mode block below is actually shown.
                               (Single player never reaches here: it takes the menu-only
                               branch, where menusActive gates this whole block off.) */
                            xr_2d_mode = true;
                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                            glClear(GL_COLOR_BUFFER_BIT);
                            ThisFramesRenderingHasBegun();
                        } else if (vrDeathScreen) {
                            /* Render the flat frame (death-cam view) into the 640x480
                               surface. This sets up the full per-frame render state
                               (Global_VDB_Ptr, pipeline, etc.) that MaintainHUD /
                               DoStatisticsScreen rely on — skipping it left those
                               pointers null and crashed. AvpShowViews() calls
                               ThisFramesRenderingHasBegun() internally, so the 2D
                               viewport is set up too. MaintainHUD() below then draws
                               the fade + stats, and the common 2D flip submits the
                               world-anchored quad. */
                            xr_2d_mode = true;
                            AvpShowViews();
                        } else if (xr_enabled && xr_session_running) {
                            VR_WaitAndBeginFrame();
                            AvpShowViewsVR();
                            InGameFlipBuffers();
                        } else {
#endif
                            /* Desktop (and Android when XR isn't running): when the pause
                               menu is up don't re-render the 3D scene or present here — set
                               the frame up like the menu-only branch so D3D_FadeDownScreen
                               accumulates to black over the frozen frame (re-rendering a
                               fresh game frame every time never darkens, and presenting both
                               the game and the menu strobes). The menu is drawn and presented
                               once after AvP_InGameMenus() below. Matters for network games,
                               where the loop stays in this branch while the menu is open. */
                            if (InGameMenusAreRunning()) {
                                ThisFramesRenderingHasBegun();
                            } else {
                                AvpShowViews();
                                /* No present here — the frame is presented after
                                   MaintainHUD() below so the HUD + weapon are included.
                                   Presenting here would swap them away (they'd be drawn to
                                   the next back buffer and overdrawn by the next world
                                   render before it's ever shown). */
                            }
#ifdef AVP_XR
                        }
#endif

                        /* Skip the HUD while the pause menu is up (matches the single-player
                           menu branch, which never calls MaintainHUD) so it isn't drawn over
                           the live HUD in a network game. */
                        if (!InGameMenusAreRunning()
#ifdef AVP_XR
                            && !VR_IsIn3DMode()
#endif
                           )
                        {
                            extern void ShowGameFrameRate(void);
                            MaintainHUD();
                            /* "Show FPS" for the flat path. The VR eye pass draws
                               its own counter in AvpShowViewsVR, and VR 3D mode
                               skips this whole branch, so there is no double draw. */
                            ShowGameFrameRate();
                        }

                        CheckCDAndChooseTrackIfNeeded();
                        
                        if(InGameMenusAreRunning() && ( (AvP.Network!=I_No_Network && netGameData.skirmishMode) || (AvP.Network==I_No_Network)) ) {
                            SoundSys_StopAll();
                        }
                    } else {
                        ReadUserInput();

                        SoundSys_Management();

                        FlushD3DZBuffer();

#ifdef AVP_XR
                        /* In VR: switch to 2D quad mode so render_frame() handles
                         * xrWaitFrame/Begin/End and displays the menu as a flat overlay.
                         * Clear FB 0 to black so the menu draws on a clean background. */
                        if (xr_enabled && xr_session_running) {
                            xr_2d_mode = true;
                            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                            glClear(GL_COLOR_BUFFER_BIT);
                        }
#endif
                        ThisFramesRenderingHasBegun();
                    }

                    menusActive = AvP_InGameMenus();
                    if (AvP.RestartLevel) menusActive=0;

                    if (AvP.LevelCompleted) {
                        SoundSys_FadeOutFast();
                        DoCompletedLevelStatisticsScreen();
                        thisLevelHasBeenCompleted = 1;
                    }

                    ThisFramesRenderingHasFinished();

#ifdef AVP_XR
                    /* Submit the 2D menu frame to the VR compositor, then restore
                     * 3D mode if the menu was just dismissed. */
                    if (xr_enabled && xr_session_running && xr_2d_mode) {
                        InGameFlipBuffers();
                        if (!menusActive)
                            xr_2d_mode = false;
                    }
                    /* An AVP_XR build that is NOT presenting through a headset has to
                       present here, exactly like the desktop build below. Two targets
                       reach this: a PCVR exe running flat (headset/runtime absent, so XR
                       init failed) and the non-VR Android "android" flavor, which sets
                       AVP_DISABLE_XR and never brings up a session at all.

                       This was gated on AVP_PCVR, which silently excluded the phone:
                       AVP_XR is defined for EVERY Android build, AVP_PCVR only for
                       desktop, so on the phone no branch here presented and the gameplay
                       branch above deliberately does not present either. The result was a
                       game loop that ran and rendered normally while the display stayed
                       frozen on the last loading-screen frame — it looked like "Press any
                       key to continue" had hung, when the level had in fact started.

                       When XR IS running, the presents above / in the gameplay branch
                       already happened — a second InGameFlipBuffers here would run
                       xrWaitFrame twice per game frame — so the condition stays keyed on
                       there being no live session. */
                    else if (!xr_enabled || !xr_session_running) {
                        InGameFlipBuffers();
                    }
#else
                    //InGameFlipBuffers();
                    /* Single desktop present for the whole frame, AFTER AvpShowViews,
                       MaintainHUD and AvP_InGameMenus, so the world, HUD, weapon and any
                       pause menu are all included in the presented frame. The gameplay
                       branch above deliberately does not present, which is what makes the
                       HUD/weapon visible and keeps the pause-menu background from strobing
                       (exactly one swap per frame). */
                    InGameFlipBuffers();
#endif

                    FrameCounterHandler();
                    {
                        PLAYER_STATUS *playerStatusPtr = (PLAYER_STATUS *) (Player->ObStrategyBlock->SBdataptr);
                        
                        if (!menusActive && playerStatusPtr->IsAlive && !AvP.LevelCompleted) {
                            DealWithElapsedTime();
                        }
                    }
                    break;
                
                case I_GM_Menus:
                    AvP.GameMode = I_GM_Playing;
                    break;
                default:
                    SDL_Log("EXIT: unexpected AvP.GameMode = %d", AvP.GameMode);
                    fprintf(stderr, "AvP.MainLoopRunning: gamemode = %d\n", AvP.GameMode);
                    exit(EXIT_FAILURE);
            }

            if (AvP.RestartLevel) {
                AvP.RestartLevel = 0;
                AvP.LevelCompleted = 0;

                FixCheatModesInUserProfile(UserProfilePtr);

                /* Bracketed because a restart is the one place a long, XR-silent
                   gap opens mid-session: if the app disappears here, the log shows
                   which side of RestartLevel it went. */
#ifdef AVP_XR
                SDL_Log("RESTART: RestartLevel() begin (xr_enabled=%d session_running=%d 2d_mode=%d)",
                        (int)xr_enabled, (int)xr_session_running, (int)xr_2d_mode);
#else
                SDL_Log("RESTART: RestartLevel() begin");
#endif
                RestartLevel();
                SDL_Log("RESTART: RestartLevel() done (Player=%p sb=%p)",
                        (void*)Player,
                        (void*)(Player ? Player->ObStrategyBlock : NULL));
            }
        }
        /* Catch-all: the level loop has ended and we are heading back to the
           frontend. MainLoopRunning==0 means something cleared it — the EXIT:
           lines above name which site — while a nonzero value means we left via
           a break instead. Logged unconditionally so no silent exit escapes. */
#ifdef AVP_XR
        SDL_Log("EXIT: level loop ended (MainLoopRunning=%d xr_should_quit=%d LevelCompleted=%d)",
                (int)AvP.MainLoopRunning, (int)xr_should_quit, (int)AvP.LevelCompleted);
#else
        SDL_Log("EXIT: level loop ended (MainLoopRunning=%d LevelCompleted=%d)",
                (int)AvP.MainLoopRunning, (int)AvP.LevelCompleted);
#endif
#ifdef AVP_XR
        xr_2d_mode = true;    // back to menus
#endif

        AvP.LevelCompleted = thisLevelHasBeenCompleted;
        
        FixCheatModesInUserProfile(UserProfilePtr);
        
        ReleaseAllFMVTextures();
        
        CONSBIND_WriteKeyBindingsToConfigFile();
        
        DeInitialisePlayer();
        
        DeallocatePlayersMirrorImage();
        
        KillHUD();
        
        Destroy_CurrentEnvironment();
        
        DeallocateAllImages();
        
        EndNPCs();
        
        ExitGame();
        
        SoundSys_StopAll();
        
        SoundSys_ResetFadeLevel();
        
        CDDA_Stop();
        
        if (AvP.Network != I_No_Network) {
            EndAVPNetGame();
        }
        
        ClearMemoryPool();

/* go back to menu mode */
#if !(ALIEN_DEMO|PREDATOR_DEMO|MARINE_DEMO)
        SetSoftVideoMode(640, 480, 16);
#endif
    }
    
    SoundSys_StopAll();
    SoundSys_RemoveAll();
    
    ExitSystem();

    CDDA_End();
    ClearMemoryPool();

#ifdef AVP_XR
    /* End the OpenXR session and destroy all XR/GLES resources so the Quest
     * compositor isn't left holding a live session (which otherwise leaves the
     * headset on a black screen).
     *
     * Crucially we do NOT call exit() here: returning from main() lets
     * SDLMain.run() finish the Activity through the normal Android lifecycle
     * (onDestroy -> SDL_Quit). Killing the process with exit() from this
     * native thread instead tears the Activity down out of order, which makes
     * Meta's OVRMetricsToolClient crash on shutdown
     * ("IllegalArgumentException: Service not registered"). */
    shutdown_xr_session();
    destroy_xr_resources();
#endif

    return 0;
}
