#ifndef AVPVIEW_H
#define AVPVIEW_H

/* KJL 10:49:41 04/21/97 - avpview.h */
void AvpShowViews(void);
void InitCameraValues(void);
void LightSourcesInRangeOfObject(DISPLAYBLOCK *dptr);
void ReflectObject(DISPLAYBLOCK *dPtr);


extern VIEWDESCRIPTORBLOCK *Global_VDB_Ptr;

#ifdef __ANDROID__
#include <SDL3/SDL.h>
extern int vr_is_rendering;
extern VECTORCH vr_base_world; /* pre-IPD camera position, consistent between eyes */
void AvpShowViewsVR(void);
void VR_InitEyeFBOs(int w, int h);
/* Depth-state helpers called from kzsort.c during VR rendering. */
void VR_DepthBSP(void);
void VR_DepthObjects(void);
void VR_AfterObjects(void);
/* Set to 1 before a new game starts to recalibrate heading and room-scale origin. */
extern int vr_recalibrate;
#endif

#endif
