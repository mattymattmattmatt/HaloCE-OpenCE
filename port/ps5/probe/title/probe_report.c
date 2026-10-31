/*
PROBE_REPORT.C

The title probes' report (probe_report.h).
*/

#include "probe_report.h"

#include <ps5platform/kernel.h>
#include <ps5platform/klog.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct notify_request
{
	char useless1[45];
	char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

static FILE *report_file;
static char report_path[128];

void say(const char *format, ...)
{
	char line[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	sceKernelDebugOutText(0, line);
	if (report_file)
	{
		fputs(line, report_file);
		fflush(report_file);
	}
}

void heading(const char *text)
{
	say("\n=== %s ===\n", text);
}

void notify(const char *format, ...)
{
	notify_request_t request;
	va_list arguments;

	memset(&request, 0, sizeof(request));
	va_start(arguments, format);
	vsnprintf(request.message, sizeof(request.message), format, arguments);
	va_end(arguments);
	sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

uint64_t now_us(void)
{
	static uint64_t frequency;

	if (!frequency)
		frequency = sceKernelGetTscFrequency();
	return frequency ? sceKernelReadTsc() / (frequency / 1000000) : 0;
}

void report_open(const char *name)
{
	struct ps5_kernel_sw_version version;

	snprintf(report_path, sizeof(report_path), "/app0/%s", name);
	report_file = fopen(report_path, "w");
	/* the title runs as its own user and FTP as another, which sees only
	what everyone may (PS5 RetroArch's permissions_ps5.cpp) */
	if (report_file)
		chmod(report_path, 0666);
	say("%s: %s\n", name, report_file ? "writing to this file and the kernel log" : "kernel log only");
	memset(&version, 0, sizeof(version));
	version.size = sizeof(version);
	if (sceKernelGetSystemSwVersion(&version) == 0)
		say("system software %.28s (0x%08x)\n", version.text, version.version);
}

void report_close(void)
{
	if (report_file)
		fclose(report_file);
	report_file = NULL;
}
