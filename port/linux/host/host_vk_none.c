/*
HOST_VK_NONE.C

The desktop guest builds' host draws with OpenGL only: the Android app's
Vulkan renderer (port/android/host/host_vk*.c, port/android/VULKAN.md) is not
built here, and the shared host files (host_sdl.c, host_gl.c) find it off.
*/

#include "host.h"

int host_renderer_vulkan;
int host_vk_presenting;
void *host_vk_window;

void host_vk_exit(void)
{
}
