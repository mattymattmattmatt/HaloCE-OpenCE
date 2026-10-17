/*
GL_FUNCTIONS.C

Run-time resolution of the OpenGL entry points listed in gl.h.
*/

#include "platform.h"
#define GL_FUNCTIONS_DEFINE
#include "gl.h"

#include <SDL3/SDL.h>

#define GL_DEFINE_FUNCTION(name) __typeof__(&name) halo_##name;
GL_FUNCTIONS(GL_DEFINE_FUNCTION)
#ifdef HALO_GLES
GL_OPTIONAL_FUNCTIONS(GL_DEFINE_FUNCTION)
#endif

int gl_functions_load(void)
{
	int success = TRUE;

#ifdef HALO_WEB
/* WebGL 2 is OpenGL ES 3.0: the 3.1 and 3.2 functions it lacks
(glCopyImageSubData, glDrawElementsBaseVertex, glMemoryBarrier) stay NULL,
and the renderer calls them only where its capabilities say the context
has them (xgpu_capabilities, d3d8_gl.c) */
#define GL_MISSING_FAILS 0
#else
#define GL_MISSING_FAILS 1
#endif
#define GL_LOAD_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name); \
	if (!halo_##name) \
	{ \
		if (GL_MISSING_FAILS) \
			platform_log("OpenGL function %s is unavailable", #name); \
		success = success && !GL_MISSING_FAILS; \
	}
	GL_FUNCTIONS(GL_LOAD_FUNCTION)
#undef GL_LOAD_FUNCTION
#if defined(HALO_GLES) && !defined(HALO_WEB)
	/* a missing one is no failure: the renderer uses each only where the
	context lists its extension (gl_initialize, d3d8_gl.c). WebGL has none of
	these, and its table is not asked */
#define GL_LOAD_OPTIONAL_FUNCTION(name) \
	halo_##name = (__typeof__(halo_##name))SDL_GL_GetProcAddress(#name);
	GL_OPTIONAL_FUNCTIONS(GL_LOAD_OPTIONAL_FUNCTION)
#undef GL_LOAD_OPTIONAL_FUNCTION
#endif
#if defined(HALO_GLES) && !defined(HALO_WEB) && !defined(HALO_ARM64_GUEST)
	/* native GLES (the R36S build): a driver whose SDL_GL_GetProcAddress
	hands back nothing for an entry point the renderer calls during
	initialisation would otherwise be met as a call through a null
	pointer (on-device, 2026-10-17: Mali r13p0, crash at (nil) right
	after the capabilities print, no 'unavailable' line - so either a
	later step's pointer or a bogus non-null address; name the critical
	ones here so the next log says which). Failing cleanly beats
	crashing at pc=0. */
	{
		struct { const char *name; void *pointer; } const critical[] = {
			{ "glGenVertexArrays", (void *)halo_glGenVertexArrays },
			{ "glBindVertexArray", (void *)halo_glBindVertexArray },
			{ "glGenBuffers", (void *)halo_glGenBuffers },
			{ "glBindBuffer", (void *)halo_glBindBuffer },
			{ "glBufferData", (void *)halo_glBufferData },
			{ "glBufferSubData", (void *)halo_glBufferSubData },
			{ "glCopyBufferSubData", (void *)halo_glCopyBufferSubData },
			{ "glGenSamplers", (void *)halo_glGenSamplers },
			{ "glBindSampler", (void *)halo_glBindSampler },
			{ "glGenQueries", (void *)halo_glGenQueries },
			{ "glVertexAttrib4fv", (void *)halo_glVertexAttrib4fv },
			{ "glMemoryBarrier", (void *)halo_glMemoryBarrier },
			{ "glInvalidateFramebuffer", (void *)halo_glInvalidateFramebuffer },
			{ "glRenderbufferStorageMultisample", (void *)halo_glRenderbufferStorageMultisample },
			{ "glBlitFramebuffer", (void *)halo_glBlitFramebuffer },
			{ "glCopyImageSubData", (void *)halo_glCopyImageSubData },
		};
		unsigned index;

		for (index = 0; index < sizeof(critical) / sizeof(critical[0]); index++)
		{
			if (!critical[index].pointer)
			{
				platform_log("gl init: required function %s resolved to NULL; refusing to start rather than call it",
					critical[index].name);
				success = FALSE;
			}
		}
	}
#endif
	return success;
}
