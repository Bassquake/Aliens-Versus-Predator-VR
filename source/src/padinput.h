#ifndef _padinput_h_
#define _padinput_h_ 1

/* Game controller support for the FLAT builds (desktop and the non-VR phone).
 *
 * Modelled on the VR binding table in opengl.h - an ACTION is what the game wants
 * ("fire", "crouch"), a SOURCE is a physical control, and the player maps one to the
 * other in Controls > Joystick Configuration. Keeping the two designs alike means the
 * menu code, the profile storage and the consumer sites all read the same way.
 *
 * Built on SDL's GAMEPAD api rather than the raw joystick one the port already had.
 * That matters for the stated goal of "any controller": SDL maps Xbox, DualSense,
 * Switch Pro and the rest onto one standard layout, so "A" is the bottom face button on
 * every pad. The raw joystick path reports bare indices, which differ per device - a
 * binding made on an Xbox pad would land on the wrong button on a DualSense.
 *
 * The raw joystick path is left intact underneath as a fallback for anything SDL has no
 * mapping for (flight sticks, oddities), exactly as before.
 */

typedef enum PAD_ACTION
{
    PAD_ACT_FIRE_PRIMARY = 0,
    PAD_ACT_FIRE_SECONDARY,
    PAD_ACT_JUMP,
    PAD_ACT_CROUCH,
    PAD_ACT_OPERATE,
    PAD_ACT_VISION,
    PAD_ACT_TAUNT,
    PAD_ACT_SPECIAL,        /* Marine jetpack / Predator disc recall */
    PAD_ACT_FLARE,          /* Marine flare, AND the Predator's cloak - the two are
                               mutually exclusive per species, so they share one slot
                               and each screen labels it for its own species. */
    PAD_ACT_NEXT_WEAPON,
    PAD_ACT_PREV_WEAPON,
    PAD_ACT_GRAPPLE,        /* Predator only - the grappling hook */
    PAD_ACT_RELOAD,         /* Marine and Predator - manual reload */
    PAD_ACT_COUNT
} PAD_ACTION;

/* Appended, never inserted: a stored binding is a source INDEX, so putting a new one in
   the middle would silently re-point every saved binding after it. Same rule as
   VR_SOURCE. */
typedef enum PAD_SOURCE
{
    PAD_SRC_NONE = 0,
    PAD_SRC_A,
    PAD_SRC_B,
    PAD_SRC_X,
    PAD_SRC_Y,
    PAD_SRC_LSHOULDER,
    PAD_SRC_RSHOULDER,
    PAD_SRC_LTRIGGER,
    PAD_SRC_RTRIGGER,
    PAD_SRC_LSTICK,         /* stick pressed in */
    PAD_SRC_RSTICK,
    PAD_SRC_DPAD_UP,
    PAD_SRC_DPAD_DOWN,
    PAD_SRC_DPAD_LEFT,
    PAD_SRC_DPAD_RIGHT,
    PAD_SRC_START,
    PAD_SRC_BACK,
    PAD_SRC_COUNT
} PAD_SOURCE;

/* Highest source the binding menus offer.
 *
 * Start and Back are reserved by the frontend - Start opens the in-game menu - so they
 * are not bindable: an action on Start would fire every time the menu was opened. They
 * are deliberately the LAST two entries above so that excluding them is a cap rather
 * than a hole in the middle, which a TEXTSLIDER (base + value) could not express.
 * KEEP THEM LAST if more sources are ever added. */
#define PAD_SRC_LAST_BINDABLE (PAD_SRC_START - 1)

/* One map PER SPECIES, as the VR bindings are: the actions differ (a Marine's jetpack is
   a Predator's recall disc, and only the Marine throws flares), so each gets its own
   screen and its own map. Indexed by I_PLAYER_TYPE - Marine 0, Predator 1, Alien 2.
   Sized by the literal 3 so this header does not need gamedef.h. */
#define PAD_SPECIES_COUNT 3

/* Shipped defaults for the scalar settings.
 *
 * Named once and shared by the three places that have to agree on them: the profile
 * decode (where a zero byte means "written before this option existed" and must come
 * out as the default), Reset To Defaults, and the "(Default)" marker the menu puts
 * beside whichever option is the default. They were literals in each. */
#define PAD_VERT_SENSITIVITY_DEFAULT  10
#define PAD_HORIZ_SENSITIVITY_DEFAULT 10
#define PAD_INVERT_VERTICAL_DEFAULT   0

/* Menu-editable, profile-stored. */
extern int PadBinding[PAD_SPECIES_COUNT][PAD_ACT_COUNT];
extern const int PadBindingDefault[PAD_SPECIES_COUNT][PAD_ACT_COUNT];

/* Right-stick look sensitivity, per axis. 0..20 in the menu, 10 = 1.0x. */
extern int PadVertSensitivity;
extern int PadHorizSensitivity;
extern int PadInvertVertical;

/* Put every binding for every species back to the shipped defaults. */
extern void Pad_ResetBindings(void);

/* State of whatever is bound to this action, 0 when unbound, when no pad is connected,
   or when Use Controller is off. Actions that should fire once per press (weapon cycling,
   flare, taunt, operate) report a press EDGE; the rest report the level. */
extern int Pad_Action(int action);

/* The same binding read as a short tap or as a hold past ~0.5s. Used for the Predator's
   vision button, which taps to cycle vision mode and holds to step the zoom - one
   control doing two jobs, as the headset's Y does.

   Not two actions bound to the same button: pad bindings deliberately ALLOW duplicates
   (see the profile loader), and a shared button simply fires both actions - so a tap
   would cycle the vision mode AND zoom at once. One binding read two ways is what keeps
   them apart. The two signals are mutually exclusive, so a hold never also fires the
   tap. */
extern int Pad_ActionTapShort(int action);
extern int Pad_ActionLong(int action);

/* Stick positions in -1..1 with the deadzone already removed, applied by usr_io.c.
   LEFT stick: X strafes, Y moves (positive = forward).
   RIGHT stick: X turns, Y looks (positive = look down, matching a pulled-back stick). */
extern float PadLookX, PadLookY;
extern float PadMoveX, PadMoveY;
extern void Pad_ApplyLook(float turn, float pitch);
extern void Pad_ApplyMove(float strafe, float forward);

/* 1 for one frame when Start has been held past the long-press threshold: the mission
   log. Read by the per-species message-history sites in usr_io.c, alongside the keyboard
   binding and the headset's equivalent. */
extern int pad_msg_history_edge;

/* 1 while a usable gamepad is connected. */
extern int Pad_IsActive(void);

#endif
