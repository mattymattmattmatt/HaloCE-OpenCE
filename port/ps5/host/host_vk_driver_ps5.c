/*
HOST_VK_DRIVER_PS5.C

The Vulkan driver of the PS5 port (port/ps5/PS5.md, phase 4), in place of
the Android host's host_vk_driver.c, whose driver archives and
libadrenotools a console has no use for: the driver is RADV, built by
PS5_Vulkan from PS5_Mesa (at the commits tools/ps5_build.py pins) and linked
into the title as a static archive, since a title cannot load a library.
The archive's entry point is the ICD's, vk_icdGetInstanceProcAddr (it has
no vkGetInstanceProcAddr: phase 0 B), and it is handed to host_vk.c as the
driver's.

Before that, RADV's environment: its shader cache goes to the data folder
(MESA_SHADER_CACHE_DIR), since PS5_Mesa's platform layer puts it in the
title's folder unless a title says otherwise, and every install replaces
that folder. MESA_DISK_CACHE_DATABASE is left to the platform layer. What
RADV_DEBUG, RADV_PERFTEST and MESA_VK_* the host's environment holds
(environment.txt, host_main.c) reach the driver as they are, and are logged.
*/

#include "host.h"
#include "host_vk.h"
#include "host_vk_driver.h"
#include "ps5_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* the archive's ICD entry point (the loader's interface: with an instance or
NULL and a name, the function) */
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);

#define SHADER_CACHE_FOLDER PS5_DATA_FOLDER "/radv-shader-cache"

/* the commits RADV's archive is built from (tools/ps5_build.py) */
#ifndef HALO_PS5_RADV_PINS
#define HALO_PS5_RADV_PINS "unknown"
#endif

extern char **environ;

static int opened;

PFN_vkGetInstanceProcAddr host_vk_driver_open(const char *setting, char *description, size_t size)
{
	char **entry;

	if (setting && setting[0])
		host_logf(HOST_LOG_WARN, "vk driver: display.vk_driver is \"%s\"; the console has one driver, RADV, linked",
			setting);
	if (!opened)
	{
		/* (0777: the console's FTP server sees only what everyone may) */
		if (mkdir(SHADER_CACHE_FOLDER, 0777) == 0)
			chmod(SHADER_CACHE_FOLDER, 0777);
		setenv("MESA_SHADER_CACHE_DIR", SHADER_CACHE_FOLDER, 1);
		for (entry = environ; entry && *entry; entry++)
		{
			if (!strncmp(*entry, "RADV_", 5) || !strncmp(*entry, "MESA_", 5) || !strncmp(*entry, "ACO_", 4) ||
				!strncmp(*entry, "NIR_", 4))
				host_logf(HOST_LOG_INFO, "vk driver: RADV's environment: %s", *entry);
		}
		opened = 1;
	}
	snprintf(description, size, "RADV, linked (%s)", HALO_PS5_RADV_PINS);
	return (PFN_vkGetInstanceProcAddr)vk_icdGetInstanceProcAddr;
}

int host_vk_driver_verify(char *text, size_t size)
{
	snprintf(text, size, "RADV, linked");
	return 1;
}

void host_vk_driver_close(void)
{
}

/* the guest asks (the main menu shows a driver archive's name under the
version number): none here, as for the phone's own driver on Android */
uint32_t host_vk_driver_name(uint32_t out, uint32_t size)
{
	char *text = (char *)(uintptr_t)out;

	if (!text || !size)
		return 0;
	text[0] = 0;
	return 0;
}

/* VideoOut's display mode, as the present's surface asks it
(host_vk_present.c, host_vk_surface_make): what the console's output
setting gives (3840x2160 at 59.94 Hz on the user's 4K display). 0 before the
instance is made, or without a display or a mode. */
int ps5_vk_display_mode(int *width, int *height, unsigned *millihertz)
{
	VkDisplayPropertiesKHR display;
	VkDisplayModePropertiesKHR mode;
	uint32_t count = 1;
	VkResult result;

	if (!host_vk.instance || !host_vk.physical || !host_vk.vkGetPhysicalDeviceDisplayPropertiesKHR)
		return 0;
	result = host_vk.vkGetPhysicalDeviceDisplayPropertiesKHR(host_vk.physical, &count, &display);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
		return 0;
	count = 1;
	result = host_vk.vkGetDisplayModePropertiesKHR(host_vk.physical, display.display, &count, &mode);
	if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count)
		return 0;
	*width = (int)mode.parameters.visibleRegion.width;
	*height = (int)mode.parameters.visibleRegion.height;
	*millihertz = mode.parameters.refreshRate;
	return 1;
}
