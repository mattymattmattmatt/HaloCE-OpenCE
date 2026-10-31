/*
PS5 GLSLANG PROBE

The Vulkan renderer writes its shaders as GLSL at run time, from the game's
shader keys, and compiles them with glslang (port/android/host/
host_vk_shaders.c, host_vk_glslang_compile). On Android glslang is a shared
library loaded with dlopen; on the PS5 it would be linked into the title.
This asks whether glslang, built with the title toolchain, runs on the
console and how fast, with the calls and the input the renderer makes:

1. glslang_initialize_process: its time, and memory before and after:
   flexible memory, direct memory's largest free block and the platform
   heap. The title is linked as the host will be (PS5_Vulkan's RADV link
   recipe, without RADV), which sends malloc to the platform layer's heap in
   direct memory, so that is where glslang's allocations show.
2. Each of the six shaders of port/android/probe/ (copied into /app0 by
   title-files.list), ten times: create, preprocess, parse, a program, link,
   SPIR-V, delete. The first compile of the first shader on its own (the
   cold start), then per shader the min and median of the rest, and the
   SPIR-V's size in words. On a failure, the info logs whole.
3. Each shader's last SPIR-V written to /app0/spirv/<name>.spv, to be pulled
   over FTP and checked with spirv-val on the PC.
4. The same figures after the compiles, and after glslang_finalize_process.

Build: port/ps5/probe/title/build.sh port/ps5/probe/glslang (build-extra.sh
       builds glslang first)
Run:   port/ps5/probe/title/deploy.sh 192.168.2.157 PPSA99105 and start
       "Halo PS5 glslang probe" from the home screen.

The report goes to the kernel log and /app0/glslang-probe.txt (over FTP,
/data/homebrew/PPSA99105/glslang-probe.txt).
*/

#include "probe_report.h"

#include "glslang/Include/glslang_c_interface.h"
#include "glslang/Public/resource_limits_c.h"

#include <ps5platform/heap.h>
#include <ps5platform/kernel.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MEGABYTE 0x100000ULL
#define RUNS 10

static const char *const shader_names[] = {
	"pass.vert", "pass.frag", "vs_typical.vert", "ps_typical.frag", "vs_large.vert", "ps_large.frag",
};

static size_t flexible_available(void)
{
	size_t available = 0;

	sceKernelAvailableFlexibleMemorySize(&available);
	return available;
}

static void say_memory(const char *when)
{
	size_t available = flexible_available();

	struct ps5_heap_stats heap;
	int64_t direct_start = 0;
	size_t direct_largest = 0;

	memset(&heap, 0, sizeof(heap));
	ps5_heap_stats(&heap);
	sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0x10000, &direct_start, &direct_largest);
	say("%s: flexible memory available %.1f MB; direct memory's largest free block %.1f MB; the platform heap "
		"(malloc, wrapped as the RADV link wraps it) holds %.1f MB, at most %.1f MB, %llu fallbacks to libc\n",
		when, (double)available / MEGABYTE, (double)direct_largest / MEGABYTE, (double)heap.mapped_bytes / MEGABYTE,
		(double)heap.peak_bytes / MEGABYTE, heap.libc_fallbacks);
}

/* a long log, a line at a time (say's line is 1 KB) */
static void say_log(const char *what, const char *text)
{
	say("%s:\n", what);
	while (text && *text)
	{
		const char *end = strchr(text, '\n');
		int length = end ? (int)(end - text) : (int)strlen(text);

		say("  %.*s\n", length > 900 ? 900 : length, text);
		text += length + (end ? 1 : 0);
	}
}

static char *read_file(const char *path)
{
	FILE *file = fopen(path, "rb");
	long size;
	char *text;

	if (!file)
		return NULL;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = malloc((size_t)size + 1);
	if (text && fread(text, 1, (size_t)size, file) != (size_t)size)
	{
		free(text);
		text = NULL;
	}
	if (text)
		text[size] = 0;
	fclose(file);
	return text;
}

static int compare_times(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* one compile as host_vk_glslang_compile makes it; the time in us, or 0 on
a failure (its logs said); the SPIR-V kept in *words when asked */
static uint64_t compile(const char *name, const char *source, int fragment, uint32_t **words, size_t *count)
{
	glslang_input_t input;
	glslang_shader_t *shader;
	glslang_program_t *program;
	glslang_stage_t stage = fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
	const int messages = GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT;
	uint64_t start = now_us(), took = 0;

	memset(&input, 0, sizeof(input));
	input.language = GLSLANG_SOURCE_GLSL;
	input.stage = stage;
	input.client = GLSLANG_CLIENT_VULKAN;
	input.client_version = GLSLANG_TARGET_VULKAN_1_0;
	input.target_language = GLSLANG_TARGET_SPV;
	input.target_language_version = GLSLANG_TARGET_SPV_1_0;
	input.code = source;
	input.default_version = 450;
	input.default_profile = GLSLANG_NO_PROFILE;
	input.messages = (glslang_messages_t)messages;
	input.resource = glslang_default_resource();
	shader = glslang_shader_create(&input);
	if (!glslang_shader_preprocess(shader, &input))
	{
		say("%s: preprocess failed\n", name);
		say_log("shader info log", glslang_shader_get_info_log(shader));
		glslang_shader_delete(shader);
		return 0;
	}
	if (!glslang_shader_parse(shader, &input))
	{
		say("%s: parse failed\n", name);
		say_log("shader info log", glslang_shader_get_info_log(shader));
		glslang_shader_delete(shader);
		return 0;
	}
	program = glslang_program_create();
	glslang_program_add_shader(program, shader);
	if (!glslang_program_link(program, messages))
	{
		say("%s: link failed\n", name);
		say_log("program info log", glslang_program_get_info_log(program));
	}
	else
	{
		glslang_program_SPIRV_generate(program, stage);
		*count = glslang_program_SPIRV_get_size(program);
		if (!*count)
			say("%s: no SPIR-V\n", name);
		else
		{
			took = now_us() - start;
			if (took == 0)
				took = 1;
			if (words)
			{
				free(*words);
				*words = malloc(*count * 4);
				if (*words)
					memcpy(*words, glslang_program_SPIRV_get_ptr(program), *count * 4);
			}
		}
	}
	glslang_program_delete(program);
	glslang_shader_delete(shader);
	return took;
}

static void write_spirv(const char *name, const uint32_t *words, size_t count)
{
	char path[128];
	char base[64];
	FILE *file;
	char *dot;

	snprintf(base, sizeof(base), "%s", name);
	dot = strrchr(base, '.');
	/* pass.vert -> pass_vert.spv: two stages share a base name */
	if (dot)
		*dot = '_';
	snprintf(path, sizeof(path), "/app0/spirv/%s.spv", base);
	file = fopen(path, "wb");
	if (!file)
	{
		say("%s: %s\n", path, strerror(errno));
		return;
	}
	if (fwrite(words, 4, count, file) != count)
		say("%s: short write\n", path);
	fclose(file);
	chmod(path, 0666);
	say("%s: %zu words written\n", path, count);
}

int main(void)
{
	uint64_t start;
	size_t index;
	int ok;

	report_open("glslang-probe.txt");
	notify("Halo PS5 glslang probe starts");

	heading("1. glslang_initialize_process");
	say_memory("before");
	start = now_us();
	ok = glslang_initialize_process();
	say("glslang_initialize_process: %d in %llu us\n", ok, (unsigned long long)(now_us() - start));
	say_memory("after");

	heading("2. the six shaders, ten compiles each, and 3. their SPIR-V written");
	if (mkdir("/app0/spirv", 0777) != 0 && errno != EEXIST)
		say("mkdir /app0/spirv: %s\n", strerror(errno));
	chmod("/app0/spirv", 0777);
	for (index = 0; index < sizeof(shader_names) / sizeof(shader_names[0]); index++)
	{
		const char *name = shader_names[index];
		int fragment = strstr(name, ".frag") != NULL;
		char path[128];
		char *source;
		uint64_t times[RUNS];
		uint32_t *words = NULL;
		size_t count = 0;
		int run, good = 0;

		snprintf(path, sizeof(path), "/app0/%s", name);
		source = read_file(path);
		if (!source)
		{
			say("%s: %s\n", path, strerror(errno));
			continue;
		}
		for (run = 0; run < RUNS; run++)
		{
			uint64_t took = compile(name, source, fragment, run == RUNS - 1 ? &words : NULL, &count);

			if (!took)
				break;
			if (index == 0 && run == 0)
				say("the first compile of all (%s, cold): %llu us\n", name, (unsigned long long)took);
			times[good++] = took;
		}
		if (good == RUNS)
		{
			/* the rest: all but the first */
			qsort(times + 1, RUNS - 1, sizeof(times[0]), compare_times);
			say("%s: %zu bytes of GLSL, %zu words of SPIR-V; first %llu us, then min %llu us, median %llu us "
				"(of %d)\n",
				name, strlen(source), count, (unsigned long long)times[0], (unsigned long long)times[1],
				(unsigned long long)times[1 + (RUNS - 1) / 2], RUNS - 1);
			if (words)
				write_spirv(name, words, count);
		}
		else
			say("%s: failed on compile %d of %d\n", name, good + 1, RUNS);
		free(words);
		free(source);
	}

	heading("4. memory after");
	say_memory("after all compiles");
	glslang_finalize_process();
	say_memory("after glslang_finalize_process");

	heading("done");
	notify("Halo PS5 glslang probe is done: read glslang-probe.txt over FTP");
	report_close();
	return 0;
}
