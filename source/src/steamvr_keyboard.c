/* SteamVR's keyboard for PCVR menu text fields.
 *
 * The game is a pure OpenXR client, and OpenXR has no keyboard. SteamVR's lives in
 * its older OpenVR interface, so when - and only when - the OpenXR runtime is
 * SteamVR, this opens a second, OpenVR OVERLAY connection beside the OpenXR
 * session, purely to raise that keyboard and read the text back. Every other
 * runtime (Meta Link, VDXR, Monado) keeps the controller grid in avp_menus.c.
 *
 * Verified against SteamVR 2026-10-16 with an OpenXR instance live in the same
 * process (SteamVR's OpenXR runtime IS vrclient, the library OpenVR loads):
 *   - VR_InitInternal2(Overlay) succeeds alongside it.
 *   - The plain ShowKeyboard is refused with KeyboardAlreadyInUse (26): it belongs
 *     to the scene app. An overlay app has to create an overlay - never shown - and
 *     use ShowKeyboardForOverlay, which works, prefill included.
 *   - KeyboardDone arrives on the overlay's queue; KeyboardClosed_Global on the
 *     system queue, so both are drained.
 *
 * Three rules keep it harmless:
 *   - It is never tried unless the runtime calls itself SteamVR. Connecting to
 *     OpenVR with SteamVR not running would START SteamVR.
 *   - openvr_api is loaded at run time, never linked, so a missing library costs
 *     only the keyboard (the grid takes over), never the launch.
 *   - Any failure latches "unavailable" for the session, so it is tried once.
 *
 * Kept free of the game headers: openvr_capi.h typedefs bool, which they would
 * collide with. */
#include <string.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "openvr_capi.h"

#ifdef _WIN32
#define OVR_LIBRARY "openvr_api.dll"
#else
#define OVR_LIBRARY "libopenvr_api.so"
#endif

/* EVREventType values, from openvr.h (the C header has the enum, but by its own
   prefixed names; spelled out here so the intent reads plainly). */
#define OVR_EVENT_KEYBOARD_CLOSED         1200
#define OVR_EVENT_KEYBOARD_DONE           1202
#define OVR_EVENT_KEYBOARD_CLOSED_GLOBAL  1204

/* The exported entry points are VR_CALLTYPE, which is __cdecl on Windows - NOT the
   function tables' __stdcall. Same thing on x64; on x86 a mismatch here would
   unbalance the stack. */
#ifdef _WIN32
#define OVR_EXPORT_CALLTYPE __cdecl
#else
#define OVR_EXPORT_CALLTYPE
#endif
typedef intptr_t (OVR_EXPORT_CALLTYPE *PFN_VR_InitInternal2)(EVRInitError *, EVRApplicationType, const char *);
typedef intptr_t (OVR_EXPORT_CALLTYPE *PFN_VR_GetGenericInterface)(const char *, EVRInitError *);

static char runtime_name[128];
static int  state;                        /* 0 = not tried, 1 = ready, -1 = unavailable */
static int  keyboard_open;
static struct VR_IVROverlay_FnTable *ovr_overlay;
static struct VR_IVRSystem_FnTable  *ovr_system;
static VROverlayHandle_t ovr_handle;

/* main.c, from xrGetInstanceProperties. Before this, nothing is attempted. */
void SteamVRKeyboard_SetRuntimeName(const char *name)
{
	SDL_strlcpy(runtime_name, name ? name : "", sizeof runtime_name);
}

static void *load_fn(SDL_SharedObject *lib, const char *name)
{
	return lib ? (void *)SDL_LoadFunction(lib, name) : NULL;
}

static int steamvr_init(void)
{
	SDL_SharedObject *lib = NULL;
	PFN_VR_InitInternal2 init;
	PFN_VR_GetGenericInterface get;
	EVRInitError err = EVRInitError_VRInitError_None;
	char fn[128];

	if (state) return state > 0;
	state = -1;

	if (SDL_strncmp(runtime_name, "SteamVR", 7) != 0) {
		SDL_Log("SteamVR keyboard: runtime is \"%s\", using the controller grid", runtime_name);
		return 0;
	}

	{
		/* Next to the exe first, so the bundled copy wins, then the system's. */
		const char *base = SDL_GetBasePath();
		char path[1024];
		if (base) {
			SDL_snprintf(path, sizeof path, "%s%s", base, OVR_LIBRARY);
			lib = SDL_LoadObject(path);
		}
		if (!lib) lib = SDL_LoadObject(OVR_LIBRARY);
	}
	init = (PFN_VR_InitInternal2)load_fn(lib, "VR_InitInternal2");
	get  = (PFN_VR_GetGenericInterface)load_fn(lib, "VR_GetGenericInterface");
	if (!init || !get) {
		SDL_Log("SteamVR keyboard: %s not found, using the controller grid", OVR_LIBRARY);
		return 0;
	}

	init(&err, EVRApplicationType_VRApplication_Overlay, NULL);
	if (err != EVRInitError_VRInitError_None) {
		SDL_Log("SteamVR keyboard: OpenVR init failed (%d), using the controller grid", (int)err);
		return 0;
	}

	SDL_snprintf(fn, sizeof fn, "FnTable:%s", IVROverlay_Version);
	ovr_overlay = (struct VR_IVROverlay_FnTable *)get(fn, &err);
	SDL_snprintf(fn, sizeof fn, "FnTable:%s", IVRSystem_Version);
	ovr_system  = (struct VR_IVRSystem_FnTable *)get(fn, &err);
	if (!ovr_overlay || !ovr_system) {
		SDL_Log("SteamVR keyboard: OpenVR interfaces unavailable, using the controller grid");
		return 0;
	}
	if (ovr_overlay->CreateOverlay("com.bassquake.avpvr.keyboard", "AvP keyboard", &ovr_handle)
	    != EVROverlayError_VROverlayError_None) {
		SDL_Log("SteamVR keyboard: could not create its overlay, using the controller grid");
		return 0;
	}

	SDL_Log("SteamVR keyboard: ready");
	state = 1;
	return 1;
}

/* Open the keyboard over a field. title: shown above it. Returns 1 if SteamVR's
   keyboard is now up, 0 if the caller should use its own (the grid). */
int SteamVRKeyboard_Open(const char *existing, int maxChars, const char *title)
{
	EVROverlayError e;
	if (keyboard_open) return 1;
	if (!steamvr_init()) return 0;
	e = ovr_overlay->ShowKeyboardForOverlay(ovr_handle,
		EGamepadTextInputMode_k_EGamepadTextInputModeNormal,
		EGamepadTextInputLineMode_k_EGamepadTextInputLineModeSingleLine,
		EKeyboardFlags_KeyboardFlag_Modal,
		(char *)(title ? title : ""), (uint32_t)(maxChars > 0 ? maxChars : 1),
		(char *)(existing ? existing : ""), 0);
	if (e != EVROverlayError_VROverlayError_None) {
		/* Per field, not latched: "in use" by another app can be momentary. */
		SDL_Log("SteamVR keyboard: ShowKeyboardForOverlay failed (%d), using the controller grid", (int)e);
		return 0;
	}
	keyboard_open = 1;
	return 1;
}

/* Call each frame while open. Returns 0 while the keyboard is up, 1 when the player
   pressed Done (text in out), -1 when it was closed without Done. */
int SteamVRKeyboard_Poll(char *out, int outSize)
{
	struct VREvent_t ev;
	int result = 0;

	if (!keyboard_open) return -1;
	while (ovr_overlay->PollNextOverlayEvent(ovr_handle, &ev, sizeof ev)) {
		if (ev.eventType == OVR_EVENT_KEYBOARD_DONE && result == 0) {
			ovr_overlay->GetKeyboardText(out, (uint32_t)outSize);
			out[outSize - 1] = '\0';
			result = 1;
		} else if (ev.eventType == OVR_EVENT_KEYBOARD_CLOSED && result == 0) {
			result = -1;
		}
	}
	/* Our own connection's queue: nothing else reads it, so draining it costs
	   nobody an event. */
	while (ovr_system->PollNextEvent(&ev, sizeof ev)) {
		if (ev.eventType == OVR_EVENT_KEYBOARD_CLOSED_GLOBAL && result == 0
		    && ev.data.keyboard.overlayHandle == ovr_handle)
			result = -1;
	}
	if (result) keyboard_open = 0;
	return result;
}

/* The field ended some other way (Back, leaving the menu): take the keyboard down. */
void SteamVRKeyboard_Close(void)
{
	if (!keyboard_open) return;
	ovr_overlay->HideKeyboard();
	keyboard_open = 0;
}
