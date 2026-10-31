/*
HOST_GL_NONE.C

The console has no OpenGL. The game's Vulkan image still calls a few GL
functions from the files it shares with the GL renderer (Android's
host_gl.c counts them under debug.gpu_stats): each hostgl_* import
(host_loader.c asks host_gl_resolve for the name without its prefix) is
answered by a function of its own that counts its calls and returns 0, so
that the names called are known: logged every 10 s under debug.gpu_stats
(host_gl_frame, from the stand-in swap), and once when the game ends
(ps5_gl_report), for phase 4.

The host_gl_* imports (the eight, and debug.gl_debug's logger) are the
Vulkan image's answers of Android's host_gl.c: no strings but the
renderer's name, no extensions, buffers read as zeros, no fences.
*/

#include "host.h"
#include "ps5_host.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int host_gl_statistics;

/* one function a name: the GL imports the image has (157 in phase 2's) fit */
#define GL_NAMES 256

static const char *gl_names[GL_NAMES];
static int gl_names_used;
static uint64_t gl_calls[GL_NAMES], gl_calls_total[GL_NAMES];

/* gl_noop_<a><b><c><d>, the digits base four: number a * 64 + b * 16 + c * 4 + d */
#define GL_NOOP(a, b, c, d) \
	static long gl_noop_##a##b##c##d(void) \
	{ \
		__atomic_add_fetch(&gl_calls[(a) * 64 + (b) * 16 + (c) * 4 + (d)], 1, __ATOMIC_RELAXED); \
		return 0; \
	}
#define GL_NOOP4(a, b, c) GL_NOOP(a, b, c, 0) GL_NOOP(a, b, c, 1) GL_NOOP(a, b, c, 2) GL_NOOP(a, b, c, 3)
#define GL_NOOP16(a, b) GL_NOOP4(a, b, 0) GL_NOOP4(a, b, 1) GL_NOOP4(a, b, 2) GL_NOOP4(a, b, 3)
#define GL_NOOP64(a) GL_NOOP16(a, 0) GL_NOOP16(a, 1) GL_NOOP16(a, 2) GL_NOOP16(a, 3)
GL_NOOP64(0)
GL_NOOP64(1)
GL_NOOP64(2)
GL_NOOP64(3)

#define GL_ENTRY(a, b, c, d) gl_noop_##a##b##c##d,
#define GL_ENTRY4(a, b, c) GL_ENTRY(a, b, c, 0) GL_ENTRY(a, b, c, 1) GL_ENTRY(a, b, c, 2) GL_ENTRY(a, b, c, 3)
#define GL_ENTRY16(a, b) GL_ENTRY4(a, b, 0) GL_ENTRY4(a, b, 1) GL_ENTRY4(a, b, 2) GL_ENTRY4(a, b, 3)
#define GL_ENTRY64(a) GL_ENTRY16(a, 0) GL_ENTRY16(a, 1) GL_ENTRY16(a, 2) GL_ENTRY16(a, 3)

/* in order: the one at index i counts gl_calls[i] */
static long (*const gl_noops[GL_NAMES])(void) = { GL_ENTRY64(0) GL_ENTRY64(1) GL_ENTRY64(2) GL_ENTRY64(3) };

void *host_gl_resolve(const char *name)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	void *function = NULL;
	int index;

	pthread_mutex_lock(&lock);
	for (index = 0; index < gl_names_used && strcmp(gl_names[index], name); index++)
		;
	if (index == gl_names_used && gl_names_used < GL_NAMES)
		gl_names[gl_names_used++] = strdup(name);
	if (index < gl_names_used && gl_names[index])
		function = (void *)gl_noops[index];
	pthread_mutex_unlock(&lock);
	if (!function)
		host_logf(HOST_LOG_WARN, "GL: no room to count %s (%d names)", name, GL_NAMES);
	return function;
}

/* the calls since the last report, one line each, into the totals */
static void gl_report(unsigned frames)
{
	int index, called = 0;

	for (index = 0; index < gl_names_used; index++)
	{
		uint64_t calls = __atomic_exchange_n(&gl_calls[index], 0, __ATOMIC_RELAXED);

		gl_calls_total[index] += calls;
		if (!calls || !frames)
			continue;
		called++;
		host_logf(HOST_LOG_INFO, "vk gl call: %s %llu calls in %u frames (%.1f a frame)", gl_names[index],
			(unsigned long long)calls, frames, (double)calls / frames);
	}
	if (frames)
		host_logf(HOST_LOG_INFO, "vk gl calls: %d of the %d GL imports called in %u frames", called, gl_names_used,
			frames);
}

/* once a frame, from the stand-in swap (host_sdl_ps5.c) */
void host_gl_frame(void)
{
	static unsigned frames;
	static struct timespec last;
	struct timespec now;

	frames++;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!last.tv_sec)
		last = now;
	if (now.tv_sec - last.tv_sec >= 10)
	{
		gl_report(host_gl_statistics ? frames : 0);
		frames = 0;
		last = now;
	}
}

void ps5_gl_report(void)
{
	char line[1024];
	size_t used;
	int index, called = 0;

	gl_report(0);
	used = (size_t)snprintf(line, sizeof(line), "GL names the Vulkan image called (of %d resolved):", gl_names_used);
	for (index = 0; index < gl_names_used; index++)
	{
		if (!gl_calls_total[index])
			continue;
		called++;
		if (used < sizeof(line))
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %s %llu", gl_names[index],
				(unsigned long long)gl_calls_total[index]);
	}
	if (!called && used < sizeof(line))
		snprintf(line + used, sizeof(line) - used, " none");
	ps5_log_line(line);
}

/* ---------- the host_gl_* imports, as Android's host_gl.c answers them
under Vulkan */

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	(void)name;
	if (!size)
		return;
	buffer[0] = 0;
	/* (the platform layer prints "OpenGL %s on %s" of what it is told) */
	if (index < 0)
		strncpy(buffer, "Vulkan", size - 1);
	buffer[size - 1] = 0;
}

int host_gl_has_extension(const char *name)
{
	(void)name;
	return 0;
}

void host_gl_read_buffer(uint32_t buffer, uint32_t offset, uint32_t size, void *data)
{
	(void)buffer;
	(void)offset;
	memset(data, 0, size);
}

void host_gl_fence_frame(uint32_t slot)
{
	(void)slot;
}

void host_gl_wait_frame(uint32_t slot)
{
	(void)slot;
}

void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	(void)target;
	(void)offset;
	(void)size;
	(void)data;
}

int host_gl_map_results(uint32_t buffer, uint32_t size)
{
	(void)buffer;
	(void)size;
	return 0;
}

void host_gl_read_results(void *data, uint32_t size)
{
	memset(data, 0, size);
}

/* debug.gl_debug's logger of the driver's messages (the desktop renderer's):
no driver, no messages */
void host_gl_debug_output(void)
{
	static int logged;

	if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
		host_logf(HOST_LOG_INFO, "GL: debug.gl_debug asked for the driver's messages; there is no GL driver");
}
