#ifdef __ANDROID__
//#include <SDL3/SDL_openxr.h>
//#include <khronos/openxr/openxr.h>

extern XrView xr_views[];          /* from main.c */
extern int    xr_session_running;  /* from main.c */
extern bool   xr_enabled;          /* from main.c */

/* Convert XR quaternion to game MATRIXCH (ONE_FIXED = 65536 scale) */
static void XrQuatToMatrixCH(const XrQuaternionf *q, MATRIXCH *m)
{
    float x=q->x, y=q->y, z=q->z, w=q->w;
    /* Standard quat→rotation matrix, scaled to ONE_FIXED */
    m->mat11 = (int)((1 - 2*(y*y + z*z)) * ONE_FIXED);
    m->mat12 = (int)((    2*(x*y - z*w)) * ONE_FIXED);
    m->mat13 = (int)((    2*(x*z + y*w)) * ONE_FIXED);
    m->mat21 = (int)((    2*(x*y + z*w)) * ONE_FIXED);
    m->mat22 = (int)((1 - 2*(x*x + z*z)) * ONE_FIXED);
    m->mat23 = (int)((    2*(y*z - x*w)) * ONE_FIXED);
    m->mat31 = (int)((    2*(x*z - y*w)) * ONE_FIXED);
    m->mat32 = (int)((    2*(y*z + x*w)) * ONE_FIXED);
    m->mat33 = (int)((1 - 2*(x*x + y*y)) * ONE_FIXED);
}
#endif