/* The renderer's real stream_reserve, stream_spare_get and index_upload
(OpenGL ES, not the web) with a model of buffer storage: a frame streaming
more than its buffers hold must go on in buffers no draw of the frame has
read, never orphan one its earlier draws read, and never write one a queued
frame may still read. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int BOOL;
typedef unsigned GLuint, GLenum;
typedef long GLsizeiptr;
#define TRUE 1
#define FALSE 0
#define HALO_GLES 1
#define GL_ARRAY_BUFFER 1
#define GL_ELEMENT_ARRAY_BUFFER 2
#define GL_STREAM_DRAW 3
#define STREAM_BUFFER_RING 3
#define STREAM_BUFFER_SIZE (16 * 1024 * 1024)
#define INDEX_BUFFER_SIZE (2 * 1024 * 1024)
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define MAXIMUM_BUFFERS 256

static struct
{
	unsigned long frame, buffer_ring;
	GLuint stream_buffer, index_buffer;
	GLuint stream_buffers[STREAM_BUFFER_RING], index_buffers[STREAM_BUFFER_RING];
	unsigned long stream_offset, index_offset, stream_room, index_room;
} device;
static struct { unsigned long stream_overflows, index_overflows, streamed_index_bytes; } stats;

/* each buffer's storage: its size, and the latest frame a draw read it */
static struct { unsigned long size; long read_frame; BOOL exists; } buffers[MAXIMUM_BUFFERS];
static GLuint next_buffer = 1, bound[3];
static unsigned orphans_read, writes_queued, writes_outside, allocations;

static void glGenBuffers(int count, GLuint *names)
{
	int index;

	for (index = 0; index < count; index++)
	{
		CHECK(next_buffer < MAXIMUM_BUFFERS);
		buffers[next_buffer].exists = TRUE;
		buffers[next_buffer].read_frame = -100;
		names[index] = next_buffer++;
	}
}
static void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	GLuint buffer = bound[target];

	(void)data; (void)usage;
	CHECK(buffer && buffers[buffer].exists);
	/* (unsynchronized writes into it would count on the driver renaming it) */
	if (buffers[buffer].read_frame == (long)device.frame)
		orphans_read++;
	buffers[buffer].size = (unsigned long)size;
	buffers[buffer].read_frame = -100;
	allocations++;
}
static void state_array_buffer(GLuint buffer) { bound[GL_ARRAY_BUFFER] = buffer; }
static void state_element_array_buffer(GLuint buffer) { bound[GL_ELEMENT_ARRAY_BUFFER] = buffer; }
static void host_gl_buffer_write(GLenum target, unsigned offset, unsigned size, const void *data)
{
	GLuint buffer = bound[target];
	long read = buffers[buffer].read_frame;

	(void)data;
	if (offset + size > buffers[buffer].size)
		writes_outside++;
	/* read by a frame the GPU may not have finished (the ring's fence) */
	if (read >= 0 && read < (long)device.frame && (long)device.frame - read < STREAM_BUFFER_RING)
		writes_queued++;
}
static void buffer_upload(GLenum target, unsigned long offset, unsigned long size, const void *data)
{
	host_gl_buffer_write(target, (unsigned)offset, (unsigned)size, data);
}
/* (d3d8_gl.c's, with debug.gpu_buffer_writes left at its default) */
static void gles_buffer_write(GLenum target, unsigned long offset, unsigned long size, const void *data)
{
	host_gl_buffer_write(target, (unsigned)offset, (unsigned)size, data);
}

#include "spares.inc"

static void draw(GLuint buffer) { buffers[buffer].read_frame = (long)device.frame; }

static void begin_frame(void)
{
	device.frame++;
	device.buffer_ring = device.frame % STREAM_BUFFER_RING;
#include "rotation.inc"
}

static unsigned char scratch[4 * 1024 * 1024];

int main(int argc, char **argv)
{
	int ring;
	unsigned long frame, piece;
	unsigned long volume = argc > 1 ? strtoul(argv[1], NULL, 0) : 40;

	glGenBuffers(STREAM_BUFFER_RING, device.stream_buffers);
	glGenBuffers(STREAM_BUFFER_RING, device.index_buffers);
	for (ring = 0; ring < STREAM_BUFFER_RING; ring++)
	{
		state_array_buffer(device.stream_buffers[ring]);
		glBufferData(GL_ARRAY_BUFFER, STREAM_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
		state_element_array_buffer(device.index_buffers[ring]);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, INDEX_BUFFER_SIZE, NULL, GL_STREAM_DRAW);
	}
	allocations = 0;
	device.frame = (unsigned long)-1;
	begin_frame();
	for (frame = 0; frame < 30; frame++)
	{
		/* volume MB of vertices in draws of 0.1 to 3.6 MB, and a tenth as
		many indices; every third frame a light one */
		unsigned long total = (frame % 3 == 2 ? 2 : volume) * 1024 * 1024, streamed = 0;

		for (piece = 0; streamed < total; piece++)
		{
			unsigned long size = 100000 + (piece * 7919 % 36) * 100000, offset;

			stream_reserve(size);
			offset = stream_upload(scratch, size);
			CHECK(offset + size <= buffers[device.stream_buffer].size);
			draw(device.stream_buffer);
			offset = index_upload(scratch, size / 10);
			CHECK(offset + size / 10 <= buffers[device.index_buffer].size);
			draw(device.index_buffer);
			streamed += size;
		}
		begin_frame();
	}
	fprintf(stderr, "orphans_read=%u writes_queued=%u writes_outside=%u allocations=%u overflows=%lu/%lu\n",
		orphans_read, writes_queued, writes_outside, allocations, stats.stream_overflows, stats.index_overflows);
	CHECK(orphans_read == 0 && writes_queued == 0 && writes_outside == 0);
	return 0;
}
