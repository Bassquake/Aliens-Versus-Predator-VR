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
static int WantCDRom = 0;
static int WantJoystick = 0;

static GLuint FullscreenTexture;
static GLsizei FullscreenTextureWidth;
static GLsizei FullscreenTextureHeight;

/* originally was "/usr/lib/libGL.so.1:/usr/lib/tls/libGL.so.1:/usr/X11R6/lib/libGL.so" */
static const char * opengl_library = NULL;

static const char * gamedatapath = NULL;

/* ** */

/* ========================================================================
 * OpenXR Setup Begin
 * ======================================================================== */

#define CHECK_CREATE(var, thing) { if (!(var)) { SDL_Log("Failed to create %s: %s", thing, SDL_GetError()); return false; } }
#define XR_CHECK(result, msg) do { if (XR_FAILED(result)) { SDL_Log("OpenXR Error: %s (result=%d)", msg, (int)(result)); return false; } } while(0)
#define XR_CHECK_QUIT(result, msg) do { if (XR_FAILED(result)) { SDL_Log("OpenXR Error: %s (result=%d)", msg, (int)(result)); quit(2); return; } } while(0)

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
typedef XrResult (XRAPI_PTR *PFN_xrGetOpenGLESGraphicsRequirementsKHR)(
    XrInstance instance, XrSystemId systemId, XrGraphicsRequirementsOpenGLESKHR *graphicsRequirements);
static PFN_xrGetOpenGLESGraphicsRequirementsKHR pfn_xrGetOpenGLESGraphicsRequirementsKHR = NULL;
static PFN_xrRequestDisplayRefreshRateFB pfn_xrRequestDisplayRefreshRateFB = NULL;

/* ========================================================================
 * Global XR State
 * ======================================================================== */

/* OpenXR state */
static XrInstance xr_instance = XR_NULL_HANDLE;
static XrSystemId xr_system_id = XR_NULL_SYSTEM_ID;
static XrSession xr_session = XR_NULL_HANDLE;
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
static XrAction xr_a_button_action           = XR_NULL_HANDLE; /* right controller A — operate */
static XrAction xr_left_thumbstick_click_action = XR_NULL_HANDLE; /* left stick click — crouch */
static XrAction xr_b_button_action                    = XR_NULL_HANDLE; /* right controller B — jump */
static XrAction xr_right_thumbstick_click_action       = XR_NULL_HANDLE; /* right stick click — next weapon */
static XrAction xr_left_trigger_action                 = XR_NULL_HANDLE; /* left trigger — throw flare */
static XrAction xr_left_grip_action  = XR_NULL_HANDLE;
static XrAction xr_right_grip_action = XR_NULL_HANDLE;
static XrAction xr_right_haptic_action = XR_NULL_HANDLE; /* right controller vibration output */
static XrAction xr_left_haptic_action  = XR_NULL_HANDLE; /* left controller vibration output */
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
int xr_left_thumbstick_click_pressed   = 0; /* 1 while left stick is clicked */
int xr_b_button_pressed                     = 0; /* 1 while right B button is held */
int xr_right_thumbstick_click_pressed        = 0; /* 1 on right stick click press edge */
int xr_y_button_gameplay_pressed             = 0; /* 1 while Y held in gameplay (vision toggle) */
int xr_left_trigger_pressed                  = 0; /* 1 on left trigger press edge (throw flare) */
static float xr_left_stick_x = 0.0f;
static float xr_left_stick_y = 0.0f;

/* HMD horizontal heading for locomotion (ONE_FIXED = 65536 scale).
 * Updated each frame from xr_views[0] pose, used by pmove.c to rotate
 * movement velocity in the direction the player is looking. */
int xr_hmd_move_sin = 0;
int xr_hmd_move_cos = 65536; /* ONE_FIXED — default facing +Z */
/* Accumulated snap turn offset in game angle units (0-4095, 4096 = full circle). */
int xr_snap_yaw = 0;
bool xr_enabled = false;   // so you can flip it off quickly if needed
bool xr_session_running = false;

/* VR display refresh rate setting: 0=72, 1=80, 2=90, 3=120 Hz.
 * Written by the AV options menu; applied at frame begin via xrRequestDisplayRefreshRateFB. */
int VRRefreshRateIndex = 0;
static bool xr_should_quit = false;
static bool xr_2d_mode = true;  /* true = show flat game on quad, false = 3D game manages XR */
static XrTime xr_predicted_display_time = 0;

/* Swapchain state — GLES images as OpenXR texture IDs */
typedef struct {
    XrSwapchain                   swapchain;
    XrSwapchainImageOpenGLESKHR  *images;     /* array of GLES texture handles */
    Uint32                         image_count;
    XrExtent2Di                    size;
} VRSwapchain;

VRSwapchain *vr_swapchains = NULL;
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

static void quit(int rc)
{
    SDL_Log("Cleaning up...");

    /* Wait for GLES to finish */
    if (context) glFinish();

    /* GLES quad resources */
    if (quad_program) { glDeleteProgram(quad_program); quad_program = 0; }
    if (quad_vao)     { glDeleteVertexArrays(1, &quad_vao); quad_vao = 0; }
    if (quad_vbo)     { glDeleteBuffers(1, &quad_vbo); quad_vbo = 0; }
    if (quad_ibo)     { glDeleteBuffers(1, &quad_ibo); quad_ibo = 0; }
    if (menu_gles_tex){ glDeleteTextures(1, &menu_gles_tex); menu_gles_tex = 0; }
    if (menu_fbo_2d)  { glDeleteFramebuffers(1, &menu_fbo_2d); menu_fbo_2d = 0; }

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

    SDL_Quit();
    exit(rc);
}

/* ========================================================================
 * GLES Shader and Quad Pipeline
 * ======================================================================== */

static const char *quad_vs_src =
    "#version 300 es\n"
    "layout(location = 0) in vec3 aPos;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "uniform mat4 uMVP;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    gl_Position = uMVP * vec4(aPos, 1.0);\n"
    "    vUV = aUV;\n"
    "}\n";

static const char *quad_fs_src =
    "#version 300 es\n"
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
    pfn_xrGetInstanceProcAddr(xr_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn_xrGetOpenGLESGraphicsRequirementsKHR);
    pfn_xrGetInstanceProcAddr(xr_instance, "xrRequestDisplayRefreshRateFB",
        (PFN_xrVoidFunction*)&pfn_xrRequestDisplayRefreshRateFB);

#undef XR_LOAD

    SDL_Log("XR: all functions loaded");
    return true;
}

static bool init_xr_instance(void)
{
    /* Get xrGetInstanceProcAddr from the OpenXR loader */
    static void *xr_loader_handle = NULL;
    if (!xr_loader_handle)
        xr_loader_handle = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL);
    if (!xr_loader_handle) {
        /* Fall back to SDL wrapper if dlopen fails */
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)SDL_OpenXR_GetXrGetInstanceProcAddr();
    } else {
        pfn_xrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)dlsym(xr_loader_handle, "xrGetInstanceProcAddr");
    }
    if (!pfn_xrGetInstanceProcAddr) {
        SDL_Log("XR: no xrGetInstanceProcAddr");
        return false;
    }

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

    /* xrCreateInstance is available with null handle */
    pfn_xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance",
        (PFN_xrVoidFunction*)&pfn_xrCreateInstance);
    if (!pfn_xrCreateInstance) {
        SDL_Log("XR: no xrCreateInstance");
        return false;
    }

    JNIEnv *env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    JavaVM *vm   = NULL;
    (*env)->GetJavaVM(env, &vm);
    jobject activity = (jobject)SDL_GetAndroidActivity();

    XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    android_info.applicationVM       = vm;
    android_info.applicationActivity = activity;

    const char *extensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
    };

    XrApplicationInfo app_info = {0};
    SDL_strlcpy(app_info.applicationName, "AvP",  XR_MAX_APPLICATION_NAME_SIZE);
    app_info.applicationVersion = 1;
    SDL_strlcpy(app_info.engineName, "SDL3", XR_MAX_ENGINE_NAME_SIZE);
    app_info.apiVersion = XR_CURRENT_API_VERSION;

    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    ci.next                     = &android_info;
    ci.createFlags              = 0;
    ci.applicationInfo          = app_info;
    ci.enabledExtensionCount    = 3;
    ci.enabledExtensionNames    = extensions;

    XrResult result = pfn_xrCreateInstance(&ci, &xr_instance);
    if (XR_FAILED(result)) {
        SDL_Log("XR: xrCreateInstance failed: %d", (int)result);
        return false;
    }
    SDL_Log("XR: instance created %p", (void*)xr_instance);

    if (!load_xr_functions()) return false;

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

    SDL_Log("XR: Creating GLES session...");
    XrSessionCreateInfo session_info = { XR_TYPE_SESSION_CREATE_INFO };
    session_info.next     = &gfx_binding;
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
        XrPath left_grip_path, right_grip_path, right_trigger_path, right_squeeze_path, a_path, left_stick_click_path, b_path, right_stick_click_path, left_trigger_path, right_haptic_path, left_haptic_path;
        pfn_xrStringToPath(xr_instance, "/interaction_profiles/oculus/touch_controller", &profile_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/thumbstick",        &left_stick_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/thumbstick",       &right_stick_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/x/click",           &x_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/y/click",           &y_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/menu/click",        &menu_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/grip/pose",         &left_grip_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/grip/pose",        &right_grip_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/trigger",          &right_trigger_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/squeeze",          &right_squeeze_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/a/click",          &a_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/thumbstick/click",  &left_stick_click_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/b/click",           &b_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/input/thumbstick/click", &right_stick_click_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/input/trigger",           &left_trigger_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/right/output/haptic",          &right_haptic_path);
        pfn_xrStringToPath(xr_instance, "/user/hand/left/output/haptic",           &left_haptic_path);

        XrActionSuggestedBinding bindings[16];
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
        XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        suggested.interactionProfile     = profile_path;
        suggested.countSuggestedBindings = 16;
        suggested.suggestedBindings      = bindings;
        pfn_xrSuggestInteractionProfileBindings(xr_instance, &suggested);

        XrSessionActionSetsAttachInfo attach_info = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
        attach_info.countActionSets = 1;
        attach_info.actionSets      = &xr_input_action_set;
        result = pfn_xrAttachSessionActionSets(xr_session, &attach_info);
        XR_CHECK(result, "Failed to attach action sets");

        /* Create action spaces for grip poses (must be after xrAttachSessionActionSets) */
        if (pfn_xrCreateActionSpace) {
            XrActionSpaceCreateInfo grip_space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
            grip_space_info.poseInActionSpace.orientation.w = 1.0f;
            grip_space_info.subactionPath = XR_NULL_PATH;

            grip_space_info.action = xr_left_grip_action;
            result = pfn_xrCreateActionSpace(xr_session, &grip_space_info, &xr_left_grip_space);
            if (XR_FAILED(result)) SDL_Log("XR: failed to create left grip space: %d", (int)result);

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
    {
        const char *exts = (const char *)glGetString(GL_EXTENSIONS);
        has_srgb_write_control = exts && strstr(exts, "GL_EXT_sRGB_write_control");
        SDL_Log("XR: GL_EXT_sRGB_write_control=%d", (int)has_srgb_write_control);
    }

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
                                              sizeof(XrSwapchainImageOpenGLESKHR));
        for (Uint32 j = 0; j < vr_swapchains[i].image_count; j++)
            vr_swapchains[i].images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        pfn_xrEnumerateSwapchainImages(vr_swapchains[i].swapchain,
                                       vr_swapchains[i].image_count,
                                       &vr_swapchains[i].image_count,
                                       (XrSwapchainImageBaseHeader*)vr_swapchains[i].images);
        SDL_Log("XR: eye %u swapchain: %dx%d, %u images (tex[0]=%u)",
                i, sci.width, sci.height, vr_swapchains[i].image_count,
                vr_swapchains[i].image_count > 0 ? vr_swapchains[i].images[0].image : 0);
    }
    SDL_free(vcfgs);

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
                        pfn_xrEndSession(xr_session);
                        xr_session_running = false;
                        break;
                    case XR_SESSION_STATE_EXITING:
                    case XR_SESSION_STATE_LOSS_PENDING:
                        xr_should_quit = true;
                        break;
                    default:
                        break;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                xr_should_quit = true;
                break;
            default:
                break;
        }
        
        event_buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

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

/* Per-eye swapchain helpers — called by avpview.c */
Uint32 VR_AcquireAndWaitSwapchainImage(int eye)
{
    Uint32 idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    pfn_xrAcquireSwapchainImage(vr_swapchains[eye].swapchain, &ai, &idx);
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    pfn_xrWaitSwapchainImage(vr_swapchains[eye].swapchain, &wi);
    /* Disable GLES sRGB write conversion so our already-gamma-encoded values
     * are stored unchanged in the sRGB swapchain texture. Without this, GLES
     * would treat our values as linear and apply an extra gamma step → too bright. */
    if (vr_srgb_swapchain && has_srgb_write_control)
        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    return idx;
}

void VR_ReleaseSwapchainImage(int eye)
{
    /* Restore sRGB write conversion before releasing */
    if (vr_srgb_swapchain && has_srgb_write_control)
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);
    glFlush(); /* ensure GLES commands reach GPU before compositor reads */
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    pfn_xrReleaseSwapchainImage(vr_swapchains[eye].swapchain, &ri);
}

GLuint VR_GetSwapchainImageTexture(int eye, Uint32 idx)
{
    return vr_swapchains[eye].images[idx].image;
}

int VR_IsIn3DMode(void)
{
    return xr_session_running && !xr_2d_mode;
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
    static int vr_applied_refresh = -1;
    if (VRRefreshRateIndex != vr_applied_refresh && pfn_xrRequestDisplayRefreshRateFB) {
        static const float rates[] = {72.0f, 80.0f, 90.0f, 120.0f};
        int idx = VRRefreshRateIndex;
        if (idx < 0) idx = 0;
        if (idx > 3) idx = 3;
        XrResult rr = pfn_xrRequestDisplayRefreshRateFB(xr_session, rates[idx]);
        SDL_Log("XR: set refresh rate %.0f Hz -> %d", rates[idx], (int)rr);
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
    Uint32 layer_count = 0;
    const XrCompositionLayerBaseHeader *layers[1] = {0};

    if (frame_state.shouldRender && view_count > 0 && vr_swapchains != NULL) {

        /* Re-capture head direction each time the menu is opened. */
        static float menu_quad_cx = 0.0f, menu_quad_cz = 0.0f, menu_quad_yaw = 0.0f;
        static int   menu_quad_ready = 0;
        if (!xr_2d_mode) menu_quad_ready = 0;

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
                menu_quad_cx  = hx;
                menu_quad_cz  = hz;
                /* atan2(fx, -fz): yaw=0 → looking -Z, yaw=π/2 → looking +X */
                menu_quad_yaw = SDL_atan2f(fx, -fz);
                menu_quad_ready = 1;
            }
        }

        proj_views = SDL_calloc(view_count, sizeof(XrCompositionLayerProjectionView));

        if (xr_2d_mode) {
            /* Upload menu pixels to GLES texture once per frame */
            upload_menu_gles_texture();

            /* Render menu quad into each eye's swapchain image via GLES FBO */
            for (Uint32 i = 0; i < view_count; i++) {
                VRSwapchain *sc = &vr_swapchains[i];
                Uint32 idx = VR_AcquireAndWaitSwapchainImage((int)i);
                GLuint sc_tex = sc->images[idx].image;

                /* Attach swapchain image as FBO color target */
                glBindFramebuffer(GL_FRAMEBUFFER, menu_fbo_2d);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, sc_tex, 0);

                glViewport(0, 0, sc->size.width, sc->size.height);
                glDisable(GL_DEPTH_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                if (quad_program && quad_vao && menu_gles_tex) {
                    glUseProgram(quad_program);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, menu_gles_tex);
                    glUniform1i(quad_u_tex, 0);

                    Mat4 view_matrix = Mat4_FromXrPose(xr_views[i].pose);
                    Mat4 proj_matrix = Mat4_Projection(xr_views[i].fov, 0.05f, 100.0f);
                    float eye_y = (view_count >= 2)
                        ? (xr_views[0].pose.position.y + xr_views[1].pose.position.y) * 0.5f
                        : 1.6f;
                    /* Rotate the model-space -Z offset to align with head forward,
                     * then translate to the head position.
                     * Note: Mat4_Multiply(A,B) = B*A, so arg order is reversed. */
                    Mat4 model = Mat4_Multiply(
                        Mat4_RotationY(-menu_quad_yaw),
                        Mat4_Translation(menu_quad_cx, eye_y, menu_quad_cz)
                    );
                    Mat4 mv    = Mat4_Multiply(model, view_matrix);
                    Mat4 mvp   = Mat4_Multiply(mv, proj_matrix);
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

                VR_ReleaseSwapchainImage((int)i);

                /* Projection view for this eye */
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

        layer.space     = xr_local_space;
        layer.viewCount = view_count;
        layer.views     = proj_views;
        layers[0]       = (XrCompositionLayerBaseHeader*)&layer;
        layer_count     = 1;
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

#ifdef __ANDROID__
    /* On Android/Quest, OpenXR owns the controllers so GotJoystick is never set.
     * Skip the SDL joystick gate and go straight to XR input. */
#else
    if (!GotJoystick) {
        return;
    }
#endif

#ifdef __ANDROID__
    /* On Quest, OpenXR owns the Touch controllers — the Android GameController
       API does not receive axis events while an XrSession is running.
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

        /* Right stick: debounced 45° snap turns (X) + next weapon on stick up (Y). */
        if (xr_right_stick_action) {
            static bool xr_snap_armed = true;
            static bool xr_next_weapon_armed = true;
            const float SNAP_THRESHOLD  = 0.6f;
            const float SNAP_REARM_ZONE = 0.3f;
            const int   SNAP_ANGLE      = 512; /* 45° in game units (4096 = full circle) */

            XrActionStateGetInfo rget = { XR_TYPE_ACTION_STATE_GET_INFO };
            rget.action = xr_right_stick_action;
            XrActionStateVector2f rstate = { XR_TYPE_ACTION_STATE_VECTOR2F };
            float rx = 0.0f, ry = 0.0f;
            if (XR_SUCCEEDED(pfn_xrGetActionStateVector2f(xr_session, &rget, &rstate)) && rstate.isActive) {
                rx = rstate.currentState.x;
                ry = rstate.currentState.y;
            }

            /* X axis: snap turns */
            if (xr_snap_armed) {
                if (rx > SNAP_THRESHOLD) {
                    xr_snap_yaw = (xr_snap_yaw + SNAP_ANGLE) & 4095;
                    xr_snap_armed = false;
                } else if (rx < -SNAP_THRESHOLD) {
                    xr_snap_yaw = (xr_snap_yaw - SNAP_ANGLE) & 4095;
                    xr_snap_armed = false;
                }
            } else if (rx > -SNAP_REARM_ZONE && rx < SNAP_REARM_ZONE) {
                xr_snap_armed = true;
            }

            /* Y axis: stick up → next weapon (gameplay only, edge-triggered). */
            xr_right_thumbstick_click_pressed = 0;
            if (!xr_2d_mode) {
                if (xr_next_weapon_armed && ry > SNAP_THRESHOLD) {
                    xr_right_thumbstick_click_pressed = 1;
                    xr_next_weapon_armed = false;
                } else if (ry < SNAP_REARM_ZONE) {
                    xr_next_weapon_armed = true;
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

        /* A button → operate (gameplay only). */
        xr_a_button_pressed = 0;
        if (!xr_2d_mode && xr_a_button_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo aget = { XR_TYPE_ACTION_STATE_GET_INFO };
            aget.action = xr_a_button_action;
            XrActionStateBoolean astate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &aget, &astate))
                    && astate.isActive)
                xr_a_button_pressed = astate.currentState ? 1 : 0;
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

        /* Y button → vision toggle in gameplay (Image Intensifier / Cloak / Alt Vision). */
        xr_y_button_gameplay_pressed = 0;
        if (!xr_2d_mode && xr_y_button_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo yget = { XR_TYPE_ACTION_STATE_GET_INFO };
            yget.action = xr_y_button_action;
            XrActionStateBoolean ystate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &yget, &ystate))
                    && ystate.isActive)
                xr_y_button_gameplay_pressed = ystate.currentState ? 1 : 0;
        }

        /* Left trigger: unbound (throw flare moved to right thumbstick click). */

        /* Left menu button → ESC in all modes (opens/closes pause menu in-game,
         * acts as back in the 2D menus). */
        if (xr_menu_button_action && pfn_xrGetActionStateBoolean) {
            XrActionStateGetInfo mget = { XR_TYPE_ACTION_STATE_GET_INFO };
            XrActionStateBoolean mstate = { XR_TYPE_ACTION_STATE_BOOLEAN };
            mget.action = xr_menu_button_action;
            int menu_pressed = 0;
            if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &mget, &mstate))
                    && mstate.isActive)
                menu_pressed = mstate.currentState ? 1 : 0;
            if (menu_pressed && !KeyboardInput[KEY_ESCAPE])
                DebouncedKeyboardInput[KEY_ESCAPE] = 1;
            KeyboardInput[KEY_ESCAPE] = menu_pressed;
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
                if (y_pressed && !KeyboardInput[KEY_ESCAPE])
                    DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                KeyboardInput[KEY_ESCAPE] = y_pressed;
            }

            /* B button → back (mirrors Y, right controller). */
            if (xr_b_button_action) {
                bget.action = xr_b_button_action;
                int b_pressed = 0;
                if (XR_SUCCEEDED(pfn_xrGetActionStateBoolean(xr_session, &bget, &bstate))
                        && bstate.isActive)
                    b_pressed = bstate.currentState ? 1 : 0;
                if (b_pressed && !KeyboardInput[KEY_ESCAPE])
                    DebouncedKeyboardInput[KEY_ESCAPE] = 1;
                KeyboardInput[KEY_ESCAPE] |= b_pressed;
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
#ifdef __ANDROID__
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
#ifdef __ANDROID__
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

void LoadDeviceAndVideoModePreferences()
{
    FILE *fp;
    int mode;
    
    fp = OpenGameFile("avp_tempvideo.cfg", FILEMODE_READONLY, FILETYPE_CONFIG);
    
    if (fp != NULL) {
        // fullscreen mode (0=window,1=fullscreen,2=fullscreen desktop)
        // window width
        // window height
        // fullscreen width
        // fullscreen height
        // fullscreen desktop aspect ratio n
        // fullscreen desktop aspect ratio d
        // fullscreen desktop scale n
        // fullscreen desktop scale d
        // multisample number of samples (0/2/4)
        if (fscanf(fp, "%d", &mode) == 1) {
            fclose(fp);
            
            if (mode >= 0 && mode < TotalVideoModes && VideoModeList[mode].available) {
                CurrentVideoMode = mode;
                return;
            }
        } else {
            fclose(fp);
        }
    }
    
    /* No, or invalid, mode found */
    
    /* Try 640x480 first */
    if (VideoModeList[1].available) {
        CurrentVideoMode = 1;
    } else {
        int i;
        
        for (i = 0; i < TotalVideoModes; i++) {
            if (VideoModeList[i].available) {
                CurrentVideoMode = i;
                break;
            }
        }
    }
}

void SaveDeviceAndVideoModePreferences()
{
    FILE *fp;
    
    fp = OpenGameFile("avp_tempvideo.cfg", FILEMODE_WRITEONLY, FILETYPE_CONFIG);
    if (fp != NULL) {
        fprintf(fp, "%d\n", CurrentVideoMode);
        fclose(fp);
    }
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
    
    _snprintf(buf, 64, "%dx%d", VideoModeList[CurrentVideoMode].w, VideoModeList[CurrentVideoMode].h);
    
    return buf;
}

int InitSDL()
{
    SDL_Log("SDL version: %d.%d.%d", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
    SDL_Log("SDL GPU drivers: %s", SDL_GetCurrentVideoDriver());
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "SDL initialising...");
    
    atexit(SDL_Quit);
    
    SDL_AddEventWatch(SDLEventFilter, NULL);

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
    
    {
        int i;
        
        for (i = 0; i < TotalVideoModes; i++) {
            //if (SDL_VideoModeOK(VideoModeList[i].w, VideoModeList[i].h, 16, SDL_FULLSCREEN | SDL_OPENGL)) {
            /* assume SDL isn't lying to us */
            VideoModeList[i].available = 1;
            
            //foundit = 1;
            //}
        }
    }
    
    LoadDeviceAndVideoModePreferences();

#ifdef __ANDROID__
    /* On Quest, always enable controller input and configure left-stick locomotion. */
    WantJoystick = 1;
    extern void VR_InitJoystickConfig(void);
    VR_InitJoystickConfig();

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

    /* Point ScreenBuffer at the SDL surface so menu backdrop code works on Android */
    {
        extern unsigned char *ScreenBuffer;
        ScreenBuffer = (unsigned char *)surface->pixels;
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
    ScreenDescriptorBlock.SDB_ProjX     = VirtualWidth/2;
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
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        
        // These should be configurable video options.
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
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
        
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        if (strstr(renderer, "SwiftShader") || strstr(renderer, "software")) {
            SDL_Log("WARNING: Software rendering detected!");
        } else {
            SDL_Log("Hardware rendering confirmed: %s", renderer);
        }
        
        // These should be configurable video options.
        SDL_GL_SetSwapInterval(1);
        
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
        
        /* ---- Native GLES OpenXR initialisation ---- */
#ifdef __ANDROID__
        if (!xr_enabled) {
            /* Quest's VR shell launches us with a plain MAIN+LAUNCHER intent —
             * the com.oculus.intent.category.VR category is NOT propagated to
             * the Activity's Intent, even though the system is in IMMERSIVE
             * mode and the compositor is waiting for XR frames. So we can't
             * use the intent to decide whether to init XR. The manifest already
             * declares this as a VR app; always attempt OpenXR init and fall
             * back to 2D if it fails. */
            SDL_Log("XR: attempting OpenXR init (manifest-declared VR app)");
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
            SDL_Log("XR: GLES native path active");
        }
        xr_init_done:;
#endif
        /* ---- end OpenXR init ---- */
        
    }
    
    SDL_GetWindowSize(window, &Width, &Height);
    
#ifdef __ANDROID__
    /* Use 640x480 virtual coordinates on Android so 2D HUD/progress-screen text
       (designed for 640x480 virtual space) normalises to correct NDC without glyph
       downscaling. VR 3D mode (AvpShowViewsVR) overrides SDB to eye FBO size. */
    SetWindowSize(Width, Height, 640, 480);
#else
    SetWindowSize(Width, Height, Width, Height);
#endif

    int NewWidth, NewHeight;
    SDL_GetWindowSize(window, &Width, &Height);
    if (Width != NewWidth || Height != NewHeight) {
        //printf("Failed to change size: %d,%d vs. %d,%d\n", Width, Height, NewWidth, NewHeight);
        //Width = NewWidth;
        //Height = NewHeight;
        //SetWindowSize(Width, Height, Width, Height);
    }
    
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

static void handle_keypress(int key, int unicode, int press)
{
    if (key == -1)
        return;
    
    // Nasty hack to allow temporary character entry by TCH68k on Github
    
    if ((key == KEY_LEFTSHIFT) || (key == KEY_RIGHTSHIFT))
    {
        ShiftDown = press;
    }
    else if (press) {
        if ((key >= KEY_A) && (key <= KEY_Z))
        {
            RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(65 + (key - KEY_A) + ShiftAddition[ShiftDown ^ CapsLockOn]);
        }
        else if ((key >= KEY_0) && (key <= KEY_9))
        {
            RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(48 + (key - KEY_0)); /* TODO: Shift numbers -> symbols */
        }
        else if ((key >= KEY_NUMPAD0) && (key <= KEY_NUMPAD9))
        {
            RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(48 + (key - KEY_NUMPAD0));
        }
        else if (false) /* TODO: other symbols */
        {
            // BOO!
        }
        else switch (key) {
                case KEY_CAPS:
                    CapsLockOn ^= 1;
                    break;
                case KEY_CR:
                    SDL_StartTextInput(NULL);
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
                    SDL_StartTextInput(NULL);
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

void CheckForWindowsMessages()
{
    SDL_Event event;
    float x, y, wantmouse;
    int buttons;

    GotAnyKey = 0;
    DebouncedGotAnyKey = 0;
    secure_avpzero(DebouncedKeyboardInput, sizeof DebouncedKeyboardInput);

#ifdef __ANDROID__
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
                SDL_StartTextInput(NULL);
                int unicode = event.text.text[0]; //TODO convert to utf-32
                if (unicode && !(unicode & 0xFF80)) {
                    RE_ENTRANT_QUEUE_WinProc_AddMessage_WM_CHAR(unicode);
                    KeyboardEntryQueue_Add(unicode);
                }
            }
                break;
            case SDL_EVENT_KEY_DOWN:
                SDL_StartTextInput(NULL);
                if (event.key.key == SDLK_PRINTSCREEN) {
                    if (HavePrintScn == 0)
                        GotPrintScn = 1;
                    HavePrintScn = 1;
                } else {
                    handle_keypress(KeySymToKey(event.key.key), 0, 1);
                }
                break;
            case SDL_EVENT_KEY_UP:
                SDL_StartTextInput(NULL);
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
                break;
            case SDL_EVENT_QUIT:
                AvP.MainLoopRunning = 0; /* TODO */
                exit(0); //TODO
                SDL_StopTextInput(NULL);
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

void InGameFlipBuffers(void)
{
#if !defined(NDEBUG)
    check_for_errors();
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR)
        SDL_Log("GL error: 0x%04X", err);
#endif

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
            return;
        }
        /* XR session not running (e.g. 2D panel mode) — fall through to SDL swap */
    }

    SDL_GL_SwapWindow(window);
}

void FlipBuffers()
{
    // Always let the game render the menu into surface->pixels first
    // (the existing GL upload below keeps the flat window working too)

    if (xr_enabled) {
        handle_xr_events();
        if (xr_session_running && view_count > 0 && vr_swapchains != NULL) {
            render_frame();
            return;
        }
        /* XR session not running (e.g. 2D panel mode) — fall through to SDL swap */
    }
    
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
    
    SDL_GL_SwapWindow(window);
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
/*
{ "loadrifs",	1,	NULL,	'l' },
{ "server",	0,	someval,	1 },
{ "client",	1,	someval,	2 },
*/
        { NULL,		0,	NULL,	0 },
};
#endif

static const char *usage_string =
        "Aliens vs Predator Linux - http://www.icculus.org/avp/\n"
        "Based on Rebellion Developments AvP Gold source\n"
        "      [-h | --help]           Display this help message\n"
        "      [-v | --version]        Display the game version\n"
        "      [-f | --fullscreen]     Run the game fullscreen\n"
        "      [-w | --windowed]       Run the game in a window\n"
        "      [-s | --nosound]        Do not access the soundcard\n"
        "      [-c | --nocdrom]        Do not access the CD-ROM\n"
        "      [-j | --nojoy]          Do not access the joystick\n"
        "      [-p | --datapath] [x]   Look at [x] for game files\n"
        "      [-g | --withgl] [x]     Use [x] instead of /usr/lib/libGL.so.1 for OpenGL\n"
;

int main(int argc, char *argv[])
{
    //NEEDED?
    //SDL_GLContext g_MainGLContext = NULL;
    //g_MainGLContext = SDL_GL_CreateContext(window);
    SDL_Log("Attempting to create window with SDL3...");
#if !defined(_MSC_VER)
    int c;
    
    opterr = 0;
    while ((c = getopt_long(argc, argv, "hvfwscdg:p:", getopt_long_options, NULL)) != -1) {
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
#endif
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
        
        vr_recalibrate = 1;   // recalibrate heading + room-scale on first VR frame
        xr_2d_mode = false;   // 3D game starting — stop quad rendering
        SDL_Log("*** xr_2d_mode set to FALSE — game starting ***");
        
        while(AvP.MainLoopRunning) {
#ifdef __ANDROID__
            if (xr_should_quit) break;
#endif
            CheckForWindowsMessages();
            
            switch(AvP.GameMode) {
                case I_GM_Playing:
                    if ((!menusActive || (AvP.Network!=I_No_Network && !netGameData.skirmishMode)) && !AvP.LevelCompleted) {
                        /* TODO: print some debugging stuff */
                        
                        DoAllShapeAnimations();
                        
                        UpdateGame();

#ifdef __ANDROID__
                        if (xr_enabled && xr_session_running) {
                            VR_WaitAndBeginFrame();
                            AvpShowViewsVR();
                            InGameFlipBuffers();
                        } else {
#endif
                            AvpShowViews();
                            InGameFlipBuffers();
#ifdef __ANDROID__
                        }
#endif
                        
#ifdef __ANDROID__
                        if (!VR_IsIn3DMode())
#endif
                        MaintainHUD();

                        CheckCDAndChooseTrackIfNeeded();
                        
                        if(InGameMenusAreRunning() && ( (AvP.Network!=I_No_Network && netGameData.skirmishMode) || (AvP.Network==I_No_Network)) ) {
                            SoundSys_StopAll();
                        }
                    } else {
                        ReadUserInput();

                        SoundSys_Management();

                        FlushD3DZBuffer();

#ifdef __ANDROID__
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

#ifdef __ANDROID__
                    /* Submit the 2D menu frame to the VR compositor, then restore
                     * 3D mode if the menu was just dismissed. */
                    if (xr_enabled && xr_session_running && xr_2d_mode) {
                        InGameFlipBuffers();
                        if (!menusActive)
                            xr_2d_mode = false;
                    }
#endif
                    //InGameFlipBuffers();
                    
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
                    fprintf(stderr, "AvP.MainLoopRunning: gamemode = %d\n", AvP.GameMode);
                    exit(EXIT_FAILURE);
            }
            
            if (AvP.RestartLevel) {
                AvP.RestartLevel = 0;
                AvP.LevelCompleted = 0;
                
                FixCheatModesInUserProfile(UserProfilePtr);
                
                RestartLevel();
            }
        }
        
        xr_2d_mode = true;    // back to menus
        
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
    
    return 0;
}
