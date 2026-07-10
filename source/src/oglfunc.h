#ifndef OGLFUNC_H
#define OGLFUNC_H

#if defined(_MSC_VER)
#include <windows.h>
#endif

#if defined(USE_OPENGL_ES)
#include <SDL3/SDL_opengles.h>

// OpenGL compatibility
typedef GLclampf GLclampd;
typedef GLfloat GLdouble;

#else
#include <SDL3/SDL_opengl.h>
#endif

#if !defined(GL_CLAMP_TO_EDGE)
// Originally added by GL_SGIS_texture_edge_clamp; part of OpenGL 1.2 core.
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#if !defined(APIENTRY)
#define APIENTRY
#endif

typedef void (APIENTRY *PFNGLALPHAFUNCPROC)(GLenum, GLclampf);
typedef void (APIENTRY *PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (APIENTRY *PFNGLBLENDFUNCPROC)(GLenum, GLenum);
typedef void (APIENTRY *PFNGLCLEARPROC)(GLbitfield);
typedef void (APIENTRY *PFNGLCLEARCOLORPROC)(GLclampf, GLclampf, GLclampf, GLclampf);
typedef void (APIENTRY *PFNGLCOLOR4FPROC)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLCOLORPOINTERPROC)(GLint, GLenum, GLsizei, const GLvoid *);
typedef void (APIENTRY *PFNGLCULLFACEPROC)(GLenum);
typedef void (APIENTRY *PFNGLDELETETEXTURESPROC)(GLsizei,const GLuint*);
typedef void (APIENTRY *PFNGLDEPTHFUNCPROC)(GLenum);
typedef void (APIENTRY *PFNGLDEPTHMASKPROC)(GLboolean);
typedef void (APIENTRY *PFNGLDEPTHRANGEPROC)(GLclampd, GLclampd);
typedef void (APIENTRY *PFNGLDISABLEPROC)(GLenum);
typedef void (APIENTRY *PFNGLDISABLECLIENTSTATEPROC)(GLenum);
typedef void (APIENTRY *PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const GLvoid *);
typedef void (APIENTRY *PFNGLENABLEPROC)(GLenum);
typedef void (APIENTRY *PFNGLENABLECLIENTSTATEPROC)(GLenum);
typedef void (APIENTRY *PFNGLFRONTFACEPROC)(GLenum);
typedef void (APIENTRY *PFNGLGENTEXTURESPROC)(GLsizei,GLuint*);
typedef GLenum (APIENTRY *PFNGLGETERRORPROC)(void);
typedef void (APIENTRY *PFNGLGETFLOATVPROC)(GLenum, GLfloat *);
typedef void (APIENTRY *PFNGLGETINTEGERVPROC)(GLenum, GLint *);
typedef const GLubyte* (APIENTRY *PFNGLGETSTRINGPROC)(GLenum);
typedef void (APIENTRY *PFNGLGETTEXPARAMETERFVPROC)(GLenum, GLenum, GLfloat*);
typedef void (APIENTRY *PFNGLHINTPROC)(GLenum, GLenum);
typedef void (APIENTRY *PFNGLPIXELSTOREIPROC)(GLenum, GLint);
typedef void (APIENTRY *PFNGLPOLYGONOFFSETPROC)(GLfloat, GLfloat);
typedef void (APIENTRY *PFNGLREADPIXELSPROC)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid *);
typedef void (APIENTRY *PFNGLSHADEMODELPROC)(GLenum);
typedef void (APIENTRY *PFNGLTEXCOORDPOINTERPROC)(GLint, GLenum, GLsizei, const GLvoid *);
typedef void (APIENTRY *PFNGLTEXENVFPROC)(GLenum, GLenum, GLfloat);
typedef void (APIENTRY *PFNGLTEXENVFVPROC)(GLenum, GLenum, const GLfloat *);
typedef void (APIENTRY *PFNGLTEXENVIPROC)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFNGLTEXIMAGE2DPROC)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const GLvoid*);
typedef void (APIENTRY *PFNGLTEXPARAMETERFPROC)(GLenum, GLenum, GLfloat);
typedef void (APIENTRY *PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFNGLTEXSUBIMAGE2DPROC)(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const GLvoid*);
typedef void (APIENTRY *PFNGLVERTEXPOINTERPROC)(GLint, GLenum, GLsizei, const GLvoid *);
typedef void (APIENTRY *PFNGLVIEWPORTPROC)(GLint, GLint, GLsizei, GLsizei);

extern PFNGLALPHAFUNCPROC		pglAlphaFunc;
extern PFNGLBINDTEXTUREPROC		pglBindTexture;
extern PFNGLBLENDFUNCPROC		pglBlendFunc;
extern PFNGLCLEARPROC			pglClear;
extern PFNGLCLEARCOLORPROC		pglClearColor;
extern PFNGLCOLOR4FPROC		pglColor4f;
extern PFNGLCOLORPOINTERPROC		pglColorPointer;
extern PFNGLCULLFACEPROC		pglCullFace;
extern PFNGLDELETETEXTURESPROC		pglDeleteTextures;
extern PFNGLDEPTHFUNCPROC		pglDepthFunc;
extern PFNGLDEPTHMASKPROC		pglDepthMask;
extern PFNGLDEPTHRANGEPROC		pglDepthRange;
extern PFNGLDISABLEPROC		pglDisable;
extern PFNGLDISABLECLIENTSTATEPROC	pglDisableClientState;
extern PFNGLDRAWELEMENTSPROC		pglDrawElements;
extern PFNGLENABLEPROC			pglEnable;
extern PFNGLENABLECLIENTSTATEPROC	pglEnableClientState;
extern PFNGLFRONTFACEPROC		pglFrontFace;
extern PFNGLGENTEXTURESPROC		pglGenTextures;
extern PFNGLGETERRORPROC		pglGetError;
extern PFNGLGETFLOATVPROC		pglGetFloatv;
extern PFNGLGETINTEGERVPROC		pglGetIntegerv;
extern PFNGLGETSTRINGPROC		pglGetString;
extern PFNGLGETTEXPARAMETERFVPROC	pglGetTexParameterfv;
extern PFNGLHINTPROC			pglHint;
extern PFNGLPIXELSTOREIPROC		pglPixelStorei;
extern PFNGLPOLYGONOFFSETPROC		pglPolygonOffset;
extern PFNGLREADPIXELSPROC		pglReadPixels;
extern PFNGLSHADEMODELPROC		pglShadeModel;
extern PFNGLTEXCOORDPOINTERPROC	pglTexCoordPointer;
extern PFNGLTEXENVFPROC		pglTexEnvf;
extern PFNGLTEXENVFVPROC		pglTexEnvfv;
extern PFNGLTEXENVIPROC		pglTexEnvi;
extern PFNGLTEXIMAGE2DPROC		pglTexImage2D;
extern PFNGLTEXPARAMETERFPROC		pglTexParameterf;
extern PFNGLTEXPARAMETERIPROC		pglTexParameteri;
extern PFNGLTEXSUBIMAGE2DPROC		pglTexSubImage2D;
extern PFNGLVERTEXPOINTERPROC		pglVertexPointer;
extern PFNGLVIEWPORTPROC		pglViewport;

extern int ogl_have_multisample_filter_hint;
extern int ogl_have_texture_filter_anisotropic;

extern int ogl_use_multisample_filter_hint;
extern int ogl_use_texture_filter_anisotropic;

extern void load_ogl_functions(int mode);

extern int check_for_errors_(const char *file, int line);
#define check_for_errors() check_for_errors_(__FILE__, __LINE__)

/* --- Modern GL (2.0/3.0) entry points --------------------------------------
 * opengl.c's shader + desktop-FSR code calls these by name. On Android they
 * link against libGLESv3; on desktop the Windows opengl32 import library only
 * exports GL 1.1, so load them as function pointers (see load_ogl_functions in
 * oglfunc.c) and remap the plain names onto the pointers. Guarded to non-VR;
 * the VR build keeps the real GLES symbols. */
#ifndef __ANDROID__
extern PFNGLACTIVETEXTUREPROC            pfn_glActiveTexture;
extern PFNGLATTACHSHADERPROC             pfn_glAttachShader;
extern PFNGLBINDATTRIBLOCATIONPROC       pfn_glBindAttribLocation;
extern PFNGLBINDBUFFERPROC               pfn_glBindBuffer;
extern PFNGLBINDFRAMEBUFFERPROC          pfn_glBindFramebuffer;
extern PFNGLBINDRENDERBUFFERPROC         pfn_glBindRenderbuffer;
extern PFNGLBINDVERTEXARRAYPROC          pfn_glBindVertexArray;
extern PFNGLBUFFERDATAPROC               pfn_glBufferData;
extern PFNGLCOMPILESHADERPROC            pfn_glCompileShader;
extern PFNGLCREATEPROGRAMPROC            pfn_glCreateProgram;
extern PFNGLCREATESHADERPROC             pfn_glCreateShader;
extern PFNGLDELETEFRAMEBUFFERSPROC       pfn_glDeleteFramebuffers;
extern PFNGLDELETERENDERBUFFERSPROC      pfn_glDeleteRenderbuffers;
extern PFNGLDELETESHADERPROC             pfn_glDeleteShader;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC pfn_glDisableVertexAttribArray;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC  pfn_glEnableVertexAttribArray;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC  pfn_glFramebufferRenderbuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC     pfn_glFramebufferTexture2D;
extern PFNGLGENBUFFERSPROC               pfn_glGenBuffers;
extern PFNGLGENFRAMEBUFFERSPROC          pfn_glGenFramebuffers;
extern PFNGLGENRENDERBUFFERSPROC         pfn_glGenRenderbuffers;
extern PFNGLGENERATEMIPMAPPROC           pfn_glGenerateMipmap;
extern PFNGLGETATTRIBLOCATIONPROC        pfn_glGetAttribLocation;
extern PFNGLGETPROGRAMINFOLOGPROC        pfn_glGetProgramInfoLog;
extern PFNGLGETPROGRAMIVPROC             pfn_glGetProgramiv;
extern PFNGLGETSHADERINFOLOGPROC         pfn_glGetShaderInfoLog;
extern PFNGLGETSHADERIVPROC              pfn_glGetShaderiv;
extern PFNGLGETUNIFORMLOCATIONPROC       pfn_glGetUniformLocation;
extern PFNGLLINKPROGRAMPROC              pfn_glLinkProgram;
extern PFNGLRENDERBUFFERSTORAGEPROC      pfn_glRenderbufferStorage;
extern PFNGLSHADERSOURCEPROC             pfn_glShaderSource;
extern PFNGLUNIFORM1IPROC                pfn_glUniform1i;
extern PFNGLUNIFORM2FPROC                pfn_glUniform2f;
extern PFNGLUSEPROGRAMPROC               pfn_glUseProgram;
extern PFNGLVERTEXATTRIBPOINTERPROC      pfn_glVertexAttribPointer;

#define glActiveTexture            pfn_glActiveTexture
#define glAttachShader             pfn_glAttachShader
#define glBindAttribLocation       pfn_glBindAttribLocation
#define glBindBuffer               pfn_glBindBuffer
#define glBindFramebuffer          pfn_glBindFramebuffer
#define glBindRenderbuffer         pfn_glBindRenderbuffer
#define glBindVertexArray          pfn_glBindVertexArray
#define glBufferData               pfn_glBufferData
#define glCompileShader            pfn_glCompileShader
#define glCreateProgram            pfn_glCreateProgram
#define glCreateShader             pfn_glCreateShader
#define glDeleteFramebuffers       pfn_glDeleteFramebuffers
#define glDeleteRenderbuffers      pfn_glDeleteRenderbuffers
#define glDeleteShader             pfn_glDeleteShader
#define glDisableVertexAttribArray pfn_glDisableVertexAttribArray
#define glEnableVertexAttribArray  pfn_glEnableVertexAttribArray
#define glFramebufferRenderbuffer  pfn_glFramebufferRenderbuffer
#define glFramebufferTexture2D     pfn_glFramebufferTexture2D
#define glGenBuffers               pfn_glGenBuffers
#define glGenFramebuffers          pfn_glGenFramebuffers
#define glGenRenderbuffers         pfn_glGenRenderbuffers
#define glGenerateMipmap           pfn_glGenerateMipmap
#define glGetAttribLocation        pfn_glGetAttribLocation
#define glGetProgramInfoLog        pfn_glGetProgramInfoLog
#define glGetProgramiv             pfn_glGetProgramiv
#define glGetShaderInfoLog         pfn_glGetShaderInfoLog
#define glGetShaderiv              pfn_glGetShaderiv
#define glGetUniformLocation       pfn_glGetUniformLocation
#define glLinkProgram              pfn_glLinkProgram
#define glRenderbufferStorage      pfn_glRenderbufferStorage
#define glShaderSource             pfn_glShaderSource
#define glUniform1i                pfn_glUniform1i
#define glUniform2f                pfn_glUniform2f
#define glUseProgram               pfn_glUseProgram
#define glVertexAttribPointer      pfn_glVertexAttribPointer
#endif /* !__ANDROID__ */

#endif
