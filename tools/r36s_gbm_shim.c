/*
R36S_GBM_SHIM.C (r36s-test experiment only)

A forwarding libgbm.so.1 for the R36S package. Why (on-device rounds
4-5, 2026-10-17): SDL must swap through the GBM the firmware's Mali
driver owns, but the firmware's libgbm is an old Mali/Mesa-era build
missing the plane/modifier entry points a modern SDL references -
gbm_bo_get_offset was the first named ('undefined symbol', exit 127),
with gbm_bo_get_plane_count / _stride_for_plane / _handle_for_plane /
_modifier the same family. SDL 3.2 itself treats exactly those as
OPTIONAL (SDL_kmsdrmsym.h: SDL_KMSDRM_SYM_OPT) and falls back to plain
drmModeAddFB without them; a hard-linked SDL never reaches that
fallback. This shim exports every gbm symbol SDL needs, forwards to
the device's real libgbm (looked up by absolute path, never by soname,
which would find this shim again), and implements the optional family
when the real library lacks it:

  gbm_bo_get_plane_count       -> 1
  gbm_bo_get_offset            -> 0
  gbm_bo_get_stride_for_plane  -> plane 0: the bo's stride, else 0
  gbm_bo_get_handle_for_plane  -> plane 0: the bo's handle, else zero
  gbm_bo_get_modifier          -> DRM_FORMAT_MOD_INVALID (SDL then
                                  calls AddFB2 without the modifiers
                                  flag, i.e. the plain path)

That is exactly true for the single-plane, modifier-less buffer
objects SDL's kmsdrm creates for this game. Everything else forwards
untouched; the buffer objects are the real library's own.
*/

#define _GNU_SOURCE
#include <dlfcn.h>
#include <gbm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void *real_gbm;
static int real_gbm_tried;

static void load_real_gbm(void)
{
	static const char *const paths[] = {
		"/lib/arm-linux-gnueabihf/libgbm.so.1",
		"/usr/lib/arm-linux-gnueabihf/libgbm.so.1",
		"/usr/local/lib/arm-linux-gnueabihf/libgbm.so.1",
	};
	unsigned index;

	if (real_gbm_tried)
		return;
	real_gbm_tried = 1;
	for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++)
	{
		void *handle = dlopen(paths[index], RTLD_NOW | RTLD_GLOBAL);
		void *create;

		if (!handle)
			continue;
		create = dlsym(handle, "gbm_create_device");
		if (create && create != (void *)gbm_create_device)
		{
			fprintf(stderr, "halo-gbm-shim: forwarding to %s\n", paths[index]);
			real_gbm = handle;
			return;
		}
		dlclose(handle); /* (it was this shim again, somehow) */
	}
	fprintf(stderr, "halo-gbm-shim: NO system libgbm found; gbm calls will fail\n");
}

__attribute__((constructor)) static void gbm_shim_initialize(void)
{
	load_real_gbm();
}

static void *real_symbol(const char *name)
{
	load_real_gbm();
	return real_gbm ? dlsym(real_gbm, name) : NULL;
}

#define FORWARD_POINTER(name) (__typeof__(&name))real_symbol(#name)

/* ---------- the plain forwards ---------- */

struct gbm_device *gbm_create_device(int fd)
{
	__typeof__(&gbm_create_device) real = FORWARD_POINTER(gbm_create_device);

	return real ? real(fd) : NULL;
}

void gbm_device_destroy(struct gbm_device *device)
{
	__typeof__(&gbm_device_destroy) real = FORWARD_POINTER(gbm_device_destroy);

	if (real)
		real(device);
}

const char *gbm_device_get_backend_name(struct gbm_device *device)
{
	__typeof__(&gbm_device_get_backend_name) real = FORWARD_POINTER(gbm_device_get_backend_name);

	return real ? real(device) : "halo-shim (no system libgbm)";
}

int gbm_device_is_format_supported(struct gbm_device *device, uint32_t format, uint32_t usage)
{
	__typeof__(&gbm_device_is_format_supported) real = FORWARD_POINTER(gbm_device_is_format_supported);

	return real ? real(device, format, usage) : 0;
}

struct gbm_bo *gbm_bo_create(struct gbm_device *device, uint32_t width, uint32_t height, uint32_t format, uint32_t flags)
{
	__typeof__(&gbm_bo_create) real = FORWARD_POINTER(gbm_bo_create);

	return real ? real(device, width, height, format, flags) : NULL;
}

void gbm_bo_destroy(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_destroy) real = FORWARD_POINTER(gbm_bo_destroy);

	if (real)
		real(bo);
}

uint32_t gbm_bo_get_width(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_width) real = FORWARD_POINTER(gbm_bo_get_width);

	return real ? real(bo) : 0;
}

uint32_t gbm_bo_get_height(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_height) real = FORWARD_POINTER(gbm_bo_get_height);

	return real ? real(bo) : 0;
}

uint32_t gbm_bo_get_stride(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_stride) real = FORWARD_POINTER(gbm_bo_get_stride);

	return real ? real(bo) : 0;
}

uint32_t gbm_bo_get_format(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_format) real = FORWARD_POINTER(gbm_bo_get_format);

	return real ? real(bo) : 0;
}

union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_handle) real = FORWARD_POINTER(gbm_bo_get_handle);
	union gbm_bo_handle handle;

	memset(&handle, 0, sizeof(handle));
	return real ? real(bo) : handle;
}

int gbm_bo_write(struct gbm_bo *bo, const void *buffer, size_t count)
{
	__typeof__(&gbm_bo_write) real = FORWARD_POINTER(gbm_bo_write);

	return real ? real(bo, buffer, count) : -1;
}

struct gbm_device *gbm_bo_get_device(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_device) real = FORWARD_POINTER(gbm_bo_get_device);

	return real ? real(bo) : NULL;
}

void gbm_bo_set_user_data(struct gbm_bo *bo, void *data, void (*destroy)(struct gbm_bo *, void *))
{
	__typeof__(&gbm_bo_set_user_data) real = FORWARD_POINTER(gbm_bo_set_user_data);

	if (real)
		real(bo, data, destroy);
}

void *gbm_bo_get_user_data(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_user_data) real = FORWARD_POINTER(gbm_bo_get_user_data);

	return real ? real(bo) : NULL;
}

struct gbm_surface *gbm_surface_create(struct gbm_device *device, uint32_t width, uint32_t height, uint32_t format, uint32_t flags)
{
	__typeof__(&gbm_surface_create) real = FORWARD_POINTER(gbm_surface_create);

	return real ? real(device, width, height, format, flags) : NULL;
}

void gbm_surface_destroy(struct gbm_surface *surface)
{
	__typeof__(&gbm_surface_destroy) real = FORWARD_POINTER(gbm_surface_destroy);

	if (real)
		real(surface);
}

struct gbm_bo *gbm_surface_lock_front_buffer(struct gbm_surface *surface)
{
	__typeof__(&gbm_surface_lock_front_buffer) real = FORWARD_POINTER(gbm_surface_lock_front_buffer);

	return real ? real(surface) : NULL;
}

void gbm_surface_release_buffer(struct gbm_surface *surface, struct gbm_bo *bo)
{
	__typeof__(&gbm_surface_release_buffer) real = FORWARD_POINTER(gbm_surface_release_buffer);

	if (real)
		real(surface, bo);
}

/* ---------- the optional plane/modifier family (see the header) ---------- */

uint64_t gbm_bo_get_modifier(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_modifier) real = FORWARD_POINTER(gbm_bo_get_modifier);

	return real ? real(bo) : (uint64_t)0x00ffffffffffffffULL; /* DRM_FORMAT_MOD_INVALID */
}

int gbm_bo_get_plane_count(struct gbm_bo *bo)
{
	__typeof__(&gbm_bo_get_plane_count) real = FORWARD_POINTER(gbm_bo_get_plane_count);

	return real ? real(bo) : 1;
}

uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane)
{
	__typeof__(&gbm_bo_get_offset) real = FORWARD_POINTER(gbm_bo_get_offset);

	(void)plane;
	return real ? real(bo, plane) : 0;
}

uint32_t gbm_bo_get_stride_for_plane(struct gbm_bo *bo, int plane)
{
	__typeof__(&gbm_bo_get_stride_for_plane) real = FORWARD_POINTER(gbm_bo_get_stride_for_plane);

	if (real)
		return real(bo, plane);
	return plane == 0 ? gbm_bo_get_stride(bo) : 0;
}

union gbm_bo_handle gbm_bo_get_handle_for_plane(struct gbm_bo *bo, int plane)
{
	__typeof__(&gbm_bo_get_handle_for_plane) real = FORWARD_POINTER(gbm_bo_get_handle_for_plane);
	union gbm_bo_handle handle;

	memset(&handle, 0, sizeof(handle));
	if (real)
		return real(bo, plane);
	return plane == 0 ? gbm_bo_get_handle(bo) : handle;
}
