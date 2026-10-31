/*
HOST_VK_SCREENSHOT_PS5.C

The picture as it went to the TV, written to a file for the audit's eyes
(port/ps5/PS5.md, phase 4): with HALO_PS5_SCREENSHOT=<seconds> in the
environment (environment.txt, host_main.c), every that many seconds after
the first present, the swapchain image the back buffer was just blitted
into is copied, before it is presented, into a host-visible buffer
(vkCmdCopyImageToBuffer, in the frame's own command buffer). Once the
frame's fence has passed (seen at a later present, without waiting), a
thread of the host's, not the game's, writes the buffer to
/data/halo/screenshots/<frame>.ppm: binary P6, the swapchain's size
(3840x2160), the alpha dropped, BGRA turned to RGB. Only one copy is in
flight at a time; a screenshot that comes due while the last is still
being written waits for it. At most 200 files of a run are kept (the oldest
deleted; 24 MB each), and tools/ps5_deploy.py's fetch converts them to PNG
and deletes them from the console, every 5 minutes during a long run
(--minutes), so a run loses none (port/ps5/PS5.md, phase 6).

The copy's cost is measured once, with two timestamps around it on the
GPU, and logged with the file's time on the writer thread: it is the
audit's cost, not the player's.

The hook is host_vk_present.c's (its HALO_PS5 branch), called with the
backend's lock held, on the game's thread.
*/

#include "host.h"
#include "host_vk.h"
#include "ps5_host.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define B host_vkb

#define FOLDER PS5_DATA_FOLDER "/screenshots"
#define KEPT 200
#define CHUNK (1024 * 1024)

int ps5_screenshot_seconds;

enum
{
	IDLE,     /* nothing in flight: a copy may be recorded */
	RECORDED, /* a copy is in a submitted (or to be submitted) frame */
	WRITING,  /* the frame has passed: the writer thread has the buffer */
	OFF,      /* not asked for, or it cannot be made (said why) */
};

static struct
{
	int started;
	int state; /* atomic: the game's thread and the writer's */
	uint64_t next_ns;
	uint64_t presents;
	/* the copy in flight: the submission it is in, its frame, its size and format */
	uint64_t number, frame;
	double at_seconds;
	uint32_t width, height;
	int bgra;
	VkBuffer buffer;
	VkDeviceMemory memory;
	void *mapped;
	VkDeviceSize size;
	int cached;
	/* the copy's time on the GPU, once */
	VkQueryPool timestamps;
	PFN_vkCmdWriteTimestamp write_timestamp;
	PFN_vkGetQueryPoolResults query_results;
	int timed;
	double gpu_ms;
	/* the writer */
	pthread_mutex_t lock;
	pthread_cond_t wake;
	char kept[KEPT][64];
	unsigned written;
} C = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

/* ---------- the writer thread */

static void write_file(void)
{
	char path[128], name[64];
	uint64_t start = ps5_now_ns();
	size_t pixels = (size_t)C.width * C.height, index;
	const unsigned char *source = C.mapped;
	unsigned char *rgb = malloc(pixels * 3);
	FILE *file;
	int header, ok;

	if (!rgb)
	{
		host_logf(HOST_LOG_WARN, "screenshot: no memory for frame %llu's %zu bytes", (unsigned long long)C.frame,
			pixels * 3);
		return;
	}
	for (index = 0; index < pixels; index++)
	{
		const unsigned char *texel = source + index * 4;

		rgb[index * 3 + 0] = C.bgra ? texel[2] : texel[0];
		rgb[index * 3 + 1] = texel[1];
		rgb[index * 3 + 2] = C.bgra ? texel[0] : texel[2];
	}
	snprintf(name, sizeof(name), "%08llu.ppm", (unsigned long long)C.frame);
	snprintf(path, sizeof(path), FOLDER "/%s", name);
	file = fopen(path, "wb");
	if (!file)
	{
		host_logf(HOST_LOG_WARN, "screenshot: %s cannot be written (%s)", path, strerror(errno));
		free(rgb);
		return;
	}
	header = fprintf(file, "P6\n%u %u\n255\n", (unsigned)C.width, (unsigned)C.height);
	ok = header > 0;
	/* a megabyte at a time, with a pause between: one write of the whole picture holds the disk for a second, and the
	game's own reads (the music's) wait behind it */
	for (index = 0; ok && index < pixels * 3; index += CHUNK)
	{
		size_t size = pixels * 3 - index < CHUNK ? pixels * 3 - index : CHUNK;

		ok = fwrite(rgb + index, 1, size, file) == size && fflush(file) == 0;
		usleep(5000);
	}
	ok = fclose(file) == 0 && ok;
	free(rgb);
	if (!ok)
	{
		host_logf(HOST_LOG_WARN, "screenshot: %s was not written whole", path);
		unlink(path);
		return;
	}
	chmod(path, 0666);
	/* the oldest of this run's beyond KEPT goes */
	if (C.written >= KEPT)
	{
		char old[128];

		snprintf(old, sizeof(old), FOLDER "/%s", C.kept[C.written % KEPT]);
		unlink(old);
	}
	snprintf(C.kept[C.written % KEPT], sizeof(C.kept[0]), "%s", name);
	if (C.written == 0)
	{
		char gpu[48];

		if (C.timed)
			snprintf(gpu, sizeof(gpu), "%.3f ms", C.gpu_ms);
		else
			snprintf(gpu, sizeof(gpu), "(not timed)");
		host_logf(HOST_LOG_INFO, "screenshot: %s (frame %llu, %.1f s), %ux%u; its cost, once: the copy of %.1f MB %s on "
			"the GPU (memory %s), and %.1f ms on the writer thread to turn and write %.1f MB", path,
			(unsigned long long)C.frame, C.at_seconds, (unsigned)C.width, (unsigned)C.height, (double)C.size / 1048576.0,
			gpu, C.cached ? "host-cached" : "not host-cached", (ps5_now_ns() - start) / 1e6, (double)pixels * 3 / 1048576.0);
	}
	else
		host_logf(HOST_LOG_INFO, "screenshot: %s (frame %llu, %.1f s)", path, (unsigned long long)C.frame, C.at_seconds);
	C.written++;
}

static void *writer_thread(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&C.lock);
	for (;;)
	{
		while (__atomic_load_n(&C.state, __ATOMIC_ACQUIRE) != WRITING)
			pthread_cond_wait(&C.wake, &C.lock);
		pthread_mutex_unlock(&C.lock);
		write_file();
		pthread_mutex_lock(&C.lock);
		__atomic_store_n(&C.state, IDLE, __ATOMIC_RELEASE);
	}
	return NULL;
}

/* ---------- the game's thread, B.lock held */

static int off(const char *why)
{
	host_logf(HOST_LOG_WARN, "screenshot: none will be taken: %s", why);
	C.state = OFF;
	return 0;
}

/* the copy's buffer: in host-cached memory where there is some (the writer reads all of it), else the renderer's
host-visible kind */
static int buffer_make(void)
{
	const VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
		VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkMemoryRequirements requirements;
	uint32_t type;

	info.size = C.size;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(B.device, &info, NULL, &C.buffer) == VK_SUCCESS)
	{
		vkGetBufferMemoryRequirements(B.device, C.buffer, &requirements);
		type = host_vk_memory_type(requirements.memoryTypeBits, wanted);
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = type;
		if (type != UINT32_MAX && (B.memory.memoryTypes[type].propertyFlags & wanted) == wanted &&
			vkAllocateMemory(B.device, &allocation, NULL, &C.memory) == VK_SUCCESS)
		{
			if (vkBindBufferMemory(B.device, C.buffer, C.memory, 0) == VK_SUCCESS &&
				vkMapMemory(B.device, C.memory, 0, VK_WHOLE_SIZE, 0, &C.mapped) == VK_SUCCESS)
			{
				C.cached = 1;
				return 1;
			}
			vkFreeMemory(B.device, C.memory, NULL);
		}
		vkDestroyBuffer(B.device, C.buffer, NULL);
	}
	C.buffer = VK_NULL_HANDLE;
	C.memory = VK_NULL_HANDLE;
	return host_vk_buffer_make(C.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &C.buffer, &C.memory, &C.mapped);
}

/* the buffer, the timestamps and the thread, at the first present */
static int start(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkQueueFamilyProperties families[16];
	uint32_t family_count = 16;
	pthread_t thread;
	pthread_attr_t attributes;

	C.started = 1;
	if (ps5_screenshot_seconds <= 0)
	{
		C.state = OFF;
		return 0;
	}
	if (B.chain.format != VK_FORMAT_B8G8R8A8_UNORM && B.chain.format != VK_FORMAT_R8G8B8A8_UNORM)
		return off("the swapchain's format is neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM");
	if (host_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(host_vk.physical, B.surface, &caps) != VK_SUCCESS ||
		!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
		return off("the swapchain's images cannot be a transfer source");
	C.width = B.chain.extent.width;
	C.height = B.chain.extent.height;
	C.bgra = B.chain.format == VK_FORMAT_B8G8R8A8_UNORM;
	C.size = (VkDeviceSize)C.width * C.height * 4;
	if (!buffer_make())
		return off("no host-visible buffer for the copy");
	if (mkdir(FOLDER, 0777) == 0)
		chmod(FOLDER, 0777);
	/* the copy's time: two timestamps, where the queue keeps them */
	host_vk.vkGetPhysicalDeviceQueueFamilyProperties(host_vk.physical, &family_count, families);
	C.write_timestamp = (PFN_vkCmdWriteTimestamp)host_vk.vkGetDeviceProcAddr(B.device, "vkCmdWriteTimestamp");
	C.query_results = (PFN_vkGetQueryPoolResults)host_vk.vkGetDeviceProcAddr(B.device, "vkGetQueryPoolResults");
	if (B.family < family_count && families[B.family].timestampValidBits && C.write_timestamp && C.query_results)
	{
		VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };

		info.queryType = VK_QUERY_TYPE_TIMESTAMP;
		info.queryCount = 2;
		if (vkCreateQueryPool(B.device, &info, NULL, &C.timestamps) != VK_SUCCESS)
			C.timestamps = VK_NULL_HANDLE;
	}
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, writer_thread, NULL) != 0)
	{
		pthread_attr_destroy(&attributes);
		return off("its thread could not be started");
	}
	pthread_attr_destroy(&attributes);
	C.next_ns = ps5_now_ns() + (uint64_t)ps5_screenshot_seconds * 1000000000ull;
	host_logf(HOST_LOG_INFO, "screenshot: every %d s from the first present, %ux%u, into " FOLDER " (%.1f MB of "
		"host-visible%s memory for the copy; the GPU's time of it %s)", ps5_screenshot_seconds, (unsigned)C.width,
		(unsigned)C.height, (double)C.size / 1048576.0, C.cached ? ", host-cached" : "",
		C.timestamps ? "measured once" : "not measured: no timestamps");
	C.state = IDLE;
	return 1;
}

void host_vk_screenshot_record(VkCommandBuffer command, VkImage image, VkImageLayout *layout)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkBufferMemoryBarrier after = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
	VkBufferImageCopy region;
	uint64_t now = ps5_now_ns();
	int state;

	C.presents++;
	if (!C.started && !start())
		return;
	state = __atomic_load_n(&C.state, __ATOMIC_ACQUIRE);
	if (state == OFF)
		return;
	/* the frame with the copy has passed (its slot's fence was waited for before the slot was used again): the writer
	has it */
	if (state == RECORDED && B.retired >= C.number)
	{
		if (C.timestamps && !C.timed)
		{
			uint64_t stamps[2];

			if (C.query_results(B.device, C.timestamps, 0, 2, sizeof(stamps), stamps, sizeof(stamps[0]),
				VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] >= stamps[0])
			{
				C.gpu_ms = (double)(stamps[1] - stamps[0]) * host_vk.properties.limits.timestampPeriod / 1e6;
				C.timed = 1;
				host_logf(HOST_LOG_INFO, "screenshot: the copy of %ux%u took %.3f ms on the GPU", (unsigned)C.width,
					(unsigned)C.height, C.gpu_ms);
			}
		}
		pthread_mutex_lock(&C.lock);
		__atomic_store_n(&C.state, WRITING, __ATOMIC_RELEASE);
		pthread_cond_signal(&C.wake);
		pthread_mutex_unlock(&C.lock);
		return;
	}
	if (state != IDLE || now < C.next_ns)
		return;
	if (B.chain.extent.width != C.width || B.chain.extent.height != C.height)
		return;

	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.oldLayout = *layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
		1, &barrier);
	*layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	if (C.timestamps && !C.timed)
	{
		vkCmdResetQueryPool(command, C.timestamps, 0, 2);
		C.write_timestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, C.timestamps, 0);
	}
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = C.width;
	region.imageExtent.height = C.height;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, C.buffer, 1, &region);
	if (C.timestamps && !C.timed)
		C.write_timestamp(command, VK_PIPELINE_STAGE_TRANSFER_BIT, C.timestamps, 1);
	/* (the host reads the buffer once the fence has passed) */
	after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	after.buffer = C.buffer;
	after.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &after, 0,
		NULL);
	/* (the number host_vk_render.c's frame_submit gives this frame) */
	C.number = B.submission + 1;
	C.frame = C.presents;
	C.at_seconds = ps5_seconds();
	C.next_ns = now + (uint64_t)ps5_screenshot_seconds * 1000000000ull;
	__atomic_store_n(&C.state, RECORDED, __ATOMIC_RELEASE);
}
