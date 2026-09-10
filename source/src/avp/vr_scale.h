#ifndef _vr_scale_h_
#define _vr_scale_h_ 1

/* Melee reach under VR World Scale.
 *
 * Characters are drawn (and now hit-tested) at vr_world_scale, but their attack ranges
 * are plain constants in game units, so a scaled-up alien still had to close to its
 * original distance before it could strike - it appeared to hit from inside its own
 * body, or to stand off and miss.
 *
 * Applied by wrapping the range CONSTANTS rather than the ~29 comparison sites that use
 * them: one edit per range, and any new comparison written later scales for free.
 *
 * Returns the range unchanged outside VR and at scale <= 1, so the flat game and the
 * default configuration are bit-identical to before. */
extern int VR_Reach(int range);

#endif
