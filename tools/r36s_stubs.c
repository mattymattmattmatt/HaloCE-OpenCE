/*
R36S_STUBS.C (r36s-test experiment only)

The OpenGL ES renderer's host services (port/linux/src/xgpu.h), which the
Android guest imports from its host and the web build gives in
port/web/src/web_gl_host.c. A native Linux GLES build has no host, so it
gives them directly: extension lookup in the context's own list, buffer
writes as the glBufferSubData they are on a native driver (the Android
host copies through guest memory; nothing here needs that), frame fences
as nothing (a native GL orders a frame's own uploads as WebGL does, which
is what the web build relies on too).

Plus scenario.c's _ReadWriteBarrier: an MSVC intrinsic clang knows on x86
and AArch64, but not on 32-bit ARM (the web build stubs it the same way).
*/

#include <GLES3/gl3.h>
#include <string.h>


int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	if (!name)
		return 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

void host_gl_read_buffer(unsigned int buffer, unsigned int offset, unsigned int size, void *data)
{
	/* (as the Android host: ES has no glGetBufferSubData) */
	const void *mapping;

	glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	mapping = glMapBufferRange(GL_COPY_READ_BUFFER, (GLintptr)offset, (GLsizeiptr)size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(data, mapping, size);
		glUnmapBuffer(GL_COPY_READ_BUFFER);
	}
	else
	{
		memset(data, 0, size);
	}
	glBindBuffer(GL_COPY_READ_BUFFER, 0);
}

void host_gl_buffer_write(unsigned int target, unsigned int offset, unsigned int size, const void *data)
{
	glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void host_gl_fence_frame(unsigned int slot)
{
	(void)slot;
}

void host_gl_wait_frame(unsigned int slot)
{
	(void)slot;
}

void _ReadWriteBarrier(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}
