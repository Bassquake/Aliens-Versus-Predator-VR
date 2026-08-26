#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "oglfunc.h"

PFNGLALPHAFUNCPROC		pglAlphaFunc;
PFNGLBINDTEXTUREPROC		pglBindTexture;
PFNGLBLENDFUNCPROC		pglBlendFunc;
PFNGLCLEARPROC			pglClear;
PFNGLCLEARCOLORPROC		pglClearColor;
PFNGLCOLOR4FPROC		pglColor4f;
PFNGLCOLORPOINTERPROC		pglColorPointer;
PFNGLCULLFACEPROC		pglCullFace;
PFNGLDELETETEXTURESPROC		pglDeleteTextures;
PFNGLDEPTHFUNCPROC		pglDepthFunc;
PFNGLDEPTHMASKPROC		pglDepthMask;
PFNGLDEPTHRANGEPROC		pglDepthRange;
PFNGLDISABLEPROC		pglDisable;
PFNGLDISABLECLIENTSTATEPROC	pglDisableClientState;
PFNGLDRAWELEMENTSPROC		pglDrawElements;
PFNGLENABLEPROC			pglEnable;
PFNGLENABLECLIENTSTATEPROC	pglEnableClientState;
PFNGLFRONTFACEPROC		pglFrontFace;
PFNGLGENTEXTURESPROC		pglGenTextures;
PFNGLGETERRORPROC		pglGetError;
PFNGLGETFLOATVPROC		pglGetFloatv;
PFNGLGETINTEGERVPROC		pglGetIntegerv;
PFNGLGETSTRINGPROC		pglGetString;
PFNGLGETTEXPARAMETERFVPROC	pglGetTexParameterfv;
PFNGLHINTPROC			pglHint;
PFNGLPIXELSTOREIPROC		pglPixelStorei;
PFNGLPOLYGONOFFSETPROC		pglPolygonOffset;
PFNGLREADPIXELSPROC		pglReadPixels;
PFNGLSHADEMODELPROC		pglShadeModel;
PFNGLTEXCOORDPOINTERPROC	pglTexCoordPointer;
PFNGLTEXENVFPROC		pglTexEnvf;
PFNGLTEXENVFVPROC		pglTexEnvfv;
PFNGLTEXENVIPROC		pglTexEnvi;
PFNGLTEXIMAGE2DPROC		pglTexImage2D;
PFNGLTEXPARAMETERFPROC		pglTexParameterf;
PFNGLTEXPARAMETERIPROC		pglTexParameteri;
PFNGLTEXSUBIMAGE2DPROC		pglTexSubImage2D;
PFNGLVERTEXPOINTERPROC		pglVertexPointer;
PFNGLVIEWPORTPROC		pglViewport;

int ogl_have_multisample_filter_hint;
int ogl_have_texture_filter_anisotropic;

int ogl_use_multisample_filter_hint;
int ogl_use_texture_filter_anisotropic;

#ifndef __ANDROID__
/* Modern GL (2.0/3.0) entry points — see oglfunc.h. Loaded in load_ogl_functions. */
PFNGLACTIVETEXTUREPROC            pfn_glActiveTexture;
PFNGLATTACHSHADERPROC             pfn_glAttachShader;
PFNGLBINDATTRIBLOCATIONPROC       pfn_glBindAttribLocation;
PFNGLBINDBUFFERPROC               pfn_glBindBuffer;
PFNGLBINDFRAMEBUFFERPROC          pfn_glBindFramebuffer;
PFNGLBINDRENDERBUFFERPROC         pfn_glBindRenderbuffer;
PFNGLBINDVERTEXARRAYPROC          pfn_glBindVertexArray;
PFNGLBUFFERDATAPROC               pfn_glBufferData;
PFNGLCOMPILESHADERPROC            pfn_glCompileShader;
PFNGLCREATEPROGRAMPROC            pfn_glCreateProgram;
PFNGLCREATESHADERPROC             pfn_glCreateShader;
PFNGLDELETEFRAMEBUFFERSPROC       pfn_glDeleteFramebuffers;
PFNGLDELETERENDERBUFFERSPROC      pfn_glDeleteRenderbuffers;
PFNGLDELETESHADERPROC             pfn_glDeleteShader;
PFNGLDISABLEVERTEXATTRIBARRAYPROC pfn_glDisableVertexAttribArray;
PFNGLENABLEVERTEXATTRIBARRAYPROC  pfn_glEnableVertexAttribArray;
PFNGLFRAMEBUFFERRENDERBUFFERPROC  pfn_glFramebufferRenderbuffer;
PFNGLFRAMEBUFFERTEXTURE2DPROC     pfn_glFramebufferTexture2D;
PFNGLGENBUFFERSPROC               pfn_glGenBuffers;
PFNGLGENFRAMEBUFFERSPROC          pfn_glGenFramebuffers;
PFNGLGENRENDERBUFFERSPROC         pfn_glGenRenderbuffers;
PFNGLGENERATEMIPMAPPROC           pfn_glGenerateMipmap;
PFNGLGETATTRIBLOCATIONPROC        pfn_glGetAttribLocation;
PFNGLGETPROGRAMINFOLOGPROC        pfn_glGetProgramInfoLog;
PFNGLGETPROGRAMIVPROC             pfn_glGetProgramiv;
PFNGLGETSHADERINFOLOGPROC         pfn_glGetShaderInfoLog;
PFNGLGETSHADERIVPROC              pfn_glGetShaderiv;
PFNGLGETUNIFORMLOCATIONPROC       pfn_glGetUniformLocation;
PFNGLLINKPROGRAMPROC              pfn_glLinkProgram;
PFNGLRENDERBUFFERSTORAGEPROC      pfn_glRenderbufferStorage;
PFNGLSHADERSOURCEPROC             pfn_glShaderSource;
PFNGLUNIFORM1IPROC                pfn_glUniform1i;
PFNGLUNIFORM2FPROC                pfn_glUniform2f;
PFNGLUSEPROGRAMPROC               pfn_glUseProgram;
PFNGLVERTEXATTRIBPOINTERPROC      pfn_glVertexAttribPointer;
PFNGLDELETEPROGRAMPROC            pfn_glDeleteProgram;
PFNGLGENVERTEXARRAYSPROC          pfn_glGenVertexArrays;
PFNGLDELETEVERTEXARRAYSPROC       pfn_glDeleteVertexArrays;
PFNGLDELETEBUFFERSPROC            pfn_glDeleteBuffers;
PFNGLUNIFORM1FPROC                pfn_glUniform1f;
PFNGLUNIFORMMATRIX4FVPROC         pfn_glUniformMatrix4fv;
PFNGLCHECKFRAMEBUFFERSTATUSPROC   pfn_glCheckFramebufferStatus;
PFNGLBLITFRAMEBUFFERPROC          pfn_glBlitFramebuffer;
PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC pfn_glRenderbufferStorageMultisample;
#endif /* !__ANDROID__ */

static void dummyfunc()
{
}

#define LoadOGLProc_(type, func, name) {                    \
	if (!mode) p##func = (type) dummyfunc; else			\
	p##func = (type) SDL_GL_GetProcAddress(#name);			\
	if (p##func == NULL) {						\
		if (!ogl_missing_func) ogl_missing_func = #func;	\
	}								\
}

#define LoadOGLProc(type, func)						\
	LoadOGLProc_(type, func, func)

#define LoadOGLProc2(type, func1, func2)					\
	LoadOGLProc_(type, func1, func1); \
	if (p##func1 == NULL) { \
		ogl_missing_func = NULL; \
		LoadOGLProc_(type, func1, func2); \
	}

static int check_token(const char *string, const char *token)
{
	const char *s = string;
	int len = strlen(token);
	
	while ((s = strstr(s, token)) != NULL) {
		const char *next = s + len;
		
		if ((s == string || *(s-1) == ' ') &&
			(*next == 0 || *next == ' ')) {
			
			return 1;
		}
		
		s = next;
	}
	
	return 0;
}

void load_ogl_functions(int mode)
{
	const char* ogl_missing_func;
	const char* ext;

	ogl_missing_func = NULL;
	
	LoadOGLProc(PFNGLALPHAFUNCPROC, glAlphaFunc);
	LoadOGLProc(PFNGLBINDTEXTUREPROC, glBindTexture);
	LoadOGLProc(PFNGLBLENDFUNCPROC, glBlendFunc);
	LoadOGLProc(PFNGLCLEARPROC, glClear);
	LoadOGLProc(PFNGLCLEARCOLORPROC, glClearColor);
	LoadOGLProc(PFNGLCOLOR4FPROC, glColor4f);
	LoadOGLProc(PFNGLCOLORPOINTERPROC, glColorPointer);
	LoadOGLProc(PFNGLCULLFACEPROC, glCullFace);
	LoadOGLProc(PFNGLDELETETEXTURESPROC, glDeleteTextures);
	LoadOGLProc(PFNGLDEPTHFUNCPROC, glDepthFunc);
	LoadOGLProc(PFNGLDEPTHMASKPROC, glDepthMask);
	LoadOGLProc2(PFNGLDEPTHRANGEPROC, glDepthRange, glDepthRangef);
	LoadOGLProc(PFNGLDISABLEPROC, glDisable);
	LoadOGLProc(PFNGLDISABLECLIENTSTATEPROC, glDisableClientState);
	LoadOGLProc(PFNGLDRAWELEMENTSPROC, glDrawElements);
	LoadOGLProc(PFNGLENABLEPROC, glEnable);
	LoadOGLProc(PFNGLENABLECLIENTSTATEPROC, glEnableClientState);
	LoadOGLProc(PFNGLFRONTFACEPROC, glFrontFace);
	LoadOGLProc(PFNGLGENTEXTURESPROC, glGenTextures);
	LoadOGLProc(PFNGLGETERRORPROC, glGetError);
	LoadOGLProc(PFNGLGETFLOATVPROC, glGetFloatv);
	LoadOGLProc(PFNGLGETINTEGERVPROC, glGetIntegerv);
	LoadOGLProc(PFNGLGETSTRINGPROC, glGetString);
	LoadOGLProc(PFNGLGETTEXPARAMETERFVPROC, glGetTexParameterfv);
	LoadOGLProc(PFNGLHINTPROC, glHint);
	LoadOGLProc(PFNGLPIXELSTOREIPROC, glPixelStorei);
	LoadOGLProc(PFNGLPOLYGONOFFSETPROC, glPolygonOffset);
	LoadOGLProc(PFNGLREADPIXELSPROC, glReadPixels);
	LoadOGLProc(PFNGLSHADEMODELPROC, glShadeModel);
	LoadOGLProc(PFNGLTEXCOORDPOINTERPROC, glTexCoordPointer);
	LoadOGLProc(PFNGLTEXENVFPROC, glTexEnvf);
	LoadOGLProc(PFNGLTEXENVFVPROC, glTexEnvfv);
	LoadOGLProc(PFNGLTEXENVIPROC, glTexEnvi);
	LoadOGLProc(PFNGLTEXIMAGE2DPROC, glTexImage2D);
	LoadOGLProc(PFNGLTEXPARAMETERFPROC, glTexParameterf);
	LoadOGLProc(PFNGLTEXPARAMETERIPROC, glTexParameteri);
	LoadOGLProc(PFNGLTEXSUBIMAGE2DPROC, glTexSubImage2D);
	LoadOGLProc(PFNGLVERTEXPOINTERPROC, glVertexPointer);
	LoadOGLProc(PFNGLVIEWPORTPROC, glViewport);

#ifndef __ANDROID__
	/* Modern GL (2.0/3.0) entry points used by the shader + FSR paths. Only
	   meaningful with a real GL context (mode != 0); left NULL in software mode. */
	if (mode) {
		#define LOAD_GL_EXT(t, n) pfn_##n = (t)SDL_GL_GetProcAddress(#n)
		LOAD_GL_EXT(PFNGLACTIVETEXTUREPROC,            glActiveTexture);
		LOAD_GL_EXT(PFNGLATTACHSHADERPROC,             glAttachShader);
		LOAD_GL_EXT(PFNGLBINDATTRIBLOCATIONPROC,       glBindAttribLocation);
		LOAD_GL_EXT(PFNGLBINDBUFFERPROC,               glBindBuffer);
		LOAD_GL_EXT(PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer);
		LOAD_GL_EXT(PFNGLBINDRENDERBUFFERPROC,         glBindRenderbuffer);
		LOAD_GL_EXT(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray);
		LOAD_GL_EXT(PFNGLBUFFERDATAPROC,               glBufferData);
		LOAD_GL_EXT(PFNGLCOMPILESHADERPROC,            glCompileShader);
		LOAD_GL_EXT(PFNGLCREATEPROGRAMPROC,            glCreateProgram);
		LOAD_GL_EXT(PFNGLCREATESHADERPROC,             glCreateShader);
		LOAD_GL_EXT(PFNGLDELETEFRAMEBUFFERSPROC,       glDeleteFramebuffers);
		LOAD_GL_EXT(PFNGLDELETERENDERBUFFERSPROC,      glDeleteRenderbuffers);
		LOAD_GL_EXT(PFNGLDELETESHADERPROC,             glDeleteShader);
		LOAD_GL_EXT(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray);
		LOAD_GL_EXT(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray);
		LOAD_GL_EXT(PFNGLFRAMEBUFFERRENDERBUFFERPROC,  glFramebufferRenderbuffer);
		LOAD_GL_EXT(PFNGLFRAMEBUFFERTEXTURE2DPROC,     glFramebufferTexture2D);
		LOAD_GL_EXT(PFNGLGENBUFFERSPROC,               glGenBuffers);
		LOAD_GL_EXT(PFNGLGENFRAMEBUFFERSPROC,          glGenFramebuffers);
		LOAD_GL_EXT(PFNGLGENRENDERBUFFERSPROC,         glGenRenderbuffers);
		LOAD_GL_EXT(PFNGLGENERATEMIPMAPPROC,           glGenerateMipmap);
		LOAD_GL_EXT(PFNGLGETATTRIBLOCATIONPROC,        glGetAttribLocation);
		LOAD_GL_EXT(PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog);
		LOAD_GL_EXT(PFNGLGETPROGRAMIVPROC,             glGetProgramiv);
		LOAD_GL_EXT(PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog);
		LOAD_GL_EXT(PFNGLGETSHADERIVPROC,              glGetShaderiv);
		LOAD_GL_EXT(PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation);
		LOAD_GL_EXT(PFNGLLINKPROGRAMPROC,              glLinkProgram);
		LOAD_GL_EXT(PFNGLRENDERBUFFERSTORAGEPROC,      glRenderbufferStorage);
		LOAD_GL_EXT(PFNGLSHADERSOURCEPROC,             glShaderSource);
		LOAD_GL_EXT(PFNGLUNIFORM1IPROC,                glUniform1i);
		LOAD_GL_EXT(PFNGLUNIFORM2FPROC,                glUniform2f);
		LOAD_GL_EXT(PFNGLUSEPROGRAMPROC,               glUseProgram);
		LOAD_GL_EXT(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer);
		LOAD_GL_EXT(PFNGLDELETEPROGRAMPROC,            glDeleteProgram);
		LOAD_GL_EXT(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays);
		LOAD_GL_EXT(PFNGLDELETEVERTEXARRAYSPROC,       glDeleteVertexArrays);
		LOAD_GL_EXT(PFNGLDELETEBUFFERSPROC,            glDeleteBuffers);
		LOAD_GL_EXT(PFNGLUNIFORM1FPROC,                glUniform1f);
		LOAD_GL_EXT(PFNGLUNIFORMMATRIX4FVPROC,         glUniformMatrix4fv);
		LOAD_GL_EXT(PFNGLCHECKFRAMEBUFFERSTATUSPROC,   glCheckFramebufferStatus);
		LOAD_GL_EXT(PFNGLBLITFRAMEBUFFERPROC,          glBlitFramebuffer);
		LOAD_GL_EXT(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample);
		#undef LOAD_GL_EXT
	}
#endif /* !__ANDROID__ */

	if (!mode) {
		return;
	}
	
	if (ogl_missing_func) {
		fprintf(stderr, "Unable to load OpenGL Library: missing function %s\n", ogl_missing_func);
		exit(EXIT_FAILURE);
	}
	
#if !defined(NDEBUG)
	printf("GL_VENDOR: %s\n", pglGetString(GL_VENDOR));
	printf("GL_RENDERER: %s\n", pglGetString(GL_RENDERER));
	printf("GL_VERSION: %s\n", pglGetString(GL_VERSION));
	//printf("GL_SHADING_LANGUAGE_VERSION: %s\n", pglGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("GL_EXTENSIONS: %s\n", pglGetString(GL_EXTENSIONS));
#endif

	ext = (const char *) pglGetString(GL_EXTENSIONS);

	ogl_have_multisample_filter_hint = check_token(ext, "GL_NV_multisample_filter_hint");
	ogl_have_texture_filter_anisotropic = check_token(ext, "GL_EXT_texture_filter_anisotropic");

	ogl_use_multisample_filter_hint = ogl_have_multisample_filter_hint;
	ogl_use_texture_filter_anisotropic = ogl_have_texture_filter_anisotropic;
}

int check_for_errors_(const char *file, int line)
{
	GLenum error;
	int diderror = 0;
	
	while ((error = pglGetError()) != GL_NO_ERROR) {
		fprintf(stderr, "OPENGL ERROR: %04X (%s:%d)\n", error, file, line);
		
		diderror = 1;
	}
	
	return diderror;
}
