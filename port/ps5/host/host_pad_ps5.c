/*
HOST_PAD_PS5.C

The DualSense's gyroscope for Gyro Aim (port/ps5/PS5.md, phase 7, step 3):
host_sdl_gamepad_gyro, which the guest asks for each local player's pad
(port/linux/src/xinput_sdl.c's halo_linux_touch_look, the Switch's path) for
the turn since its last ask, in radians, and to which it applies
input.gyro_aim, the sensitivities and the inverts itself. The Switch's
host_sdl2.c is the model.

What probe 1 found shapes this (port/ps5/probe/results/pad-probe-run2.txt,
-run3.txt):

- pacbrew's SDL2 opens each signed-in user's pad (scePadOpen) and names the
  joystick after the user, but never turns the motion sensor on. So a pad is
  matched to its user by that name (sceUserServiceGetLoginUserIdList,
  sceUserServiceGetUserName), and scePadGetHandle(user, 0, 0) answers the
  handle SDL2 holds; scePadSetMotionSensorState turns the sensor on, once.
- The pad's state, scePadReadState, has a new sample about 235 times a
  second, SDL2 reading it too. It is read at each ask (the game asks every
  frame), and its angular velocity counts for the time since the sample
  before, by the samples' own timestamps: host_rules_ps5.c's ps5_gyro_turn
  (the axes, their signs, the noise floor and the gap limit, tested on the
  PC); so there is no thread. The first build drained scePadRead's queue,
  which probe 1 found at 600 samples a second while nothing else read the
  pad; in the title SDL2's own reads of the state empty it, and the host
  saw 6 samples a second (port/ps5/PS5.md, phase 7, step 3).
- The library's dead band is left as it starts, off: turned on, it zeroes
  small movements too; the noise floor is the host's own.
- The view turns sideways as the pad is steered, as a wheel (about its z
  axis), and up and down as its top tips (about x): the user's choice in
  play, over turning it flat (about y); the turn is then scaled to the
  Switch's (GYRO_SCALE).

A pad switched off and on is another SDL2 instance, found again the same
way. The calls are declared by hand after OpenOrbis's PS4 headers
(include/orbis/Pad.h, _types/pad.h, UserService.h), and
scePadSetMotionSensorState after shadPS4's pad.h, as OpenOrbis gives it no
signature; the SDK's libScePad.so and libSceUserService.so stubs name them.
*/

#include "host.h"
#include "ps5_host.h"
#include "host_rules_ps5.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------- the console's calls */

struct user_list
{
	int32_t user_id[4];
};
int32_t sceUserServiceGetLoginUserIdList(struct user_list *list);
int32_t sceUserServiceGetUserName(int32_t user_id, char *name, size_t size);

/* Sony's ScePadData (OpenOrbis's OrbisPadData) */
struct pad_data
{
	uint32_t buttons;
	uint8_t sticks_and_triggers[6];
	uint16_t padding;
	float orientation[4];
	float acceleration[3];
	float angular_velocity[3];
	uint8_t touch[24];
	uint8_t connected;
	uint64_t timestamp;
	uint8_t extension[16];
	uint8_t connected_count;
	uint8_t reserved[15];
};
_Static_assert(sizeof(struct pad_data) == 120, "the console's pad samples are 120 bytes (probe 1)");
_Static_assert(offsetof(struct pad_data, angular_velocity) == 0x28, "the angular velocity at 0x28");
_Static_assert(offsetof(struct pad_data, timestamp) == 0x50, "the timestamp at 0x50");

int32_t scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int32_t scePadReadState(int32_t handle, struct pad_data *data);
int32_t scePadSetMotionSensorState(int32_t handle, bool enable);

#define GYRO_PADS 8
/* the turn handed to the guest for the pad's: the Switch's, so that each
input.gyro_sensitivity_* turns the view as far as it does there. The
Switch's sensor gives turns a second (1.0 is 360 degrees a second, as yuzu
converts degrees a second to it), which host_sdl2.c takes as radians and
scales by 1.5 (its GYRO_SCALE); the DualSense's gives radians a second. At
1 the first build turned the view as far as the pad, which the user found
as sensitive as the Switch's 6 (port/ps5/PS5.md, phase 7, step 3). */
#define GYRO_SCALE (1.5f / 6.2831853f)

static pthread_mutex_t gyro_lock = PTHREAD_MUTEX_INITIALIZER;

static struct gyro_pad
{
	SDL_JoystickID instance;
	int32_t user;
	int32_t handle;
	struct ps5_gyro_state state;
	int used;
} gyro_pads[GYRO_PADS];

/* this minute's figures, for the pad: line (ps5_pad_gyro_minute) */
static struct
{
	unsigned samples;
	unsigned reads;
	float largest;
	float total;
} minute;

/* the signed-in user whose name the joystick has; -1 for none */
static int32_t user_named(const char *name)
{
	struct user_list list;
	int index;

	memset(&list, 0xff, sizeof(list));
	if (!name || sceUserServiceGetLoginUserIdList(&list) != 0)
		return -1;
	for (index = 0; index < 4; index++)
	{
		char user_name[32] = "";

		if (list.user_id[index] == -1)
			continue;
		if (sceUserServiceGetUserName(list.user_id[index], user_name, sizeof(user_name)) == 0 &&
			!strcmp(user_name, name))
			return list.user_id[index];
	}
	return -1;
}

/* the pad's entry, found or made (its user, its handle, the sensor on);
NULL if it has none to read. Under gyro_lock. */
static struct gyro_pad *gyro_pad_of(SDL_GameController *controller)
{
	SDL_JoystickID instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
	const char *name = SDL_GameControllerName(controller);
	struct gyro_pad *pad = NULL;
	int index;
	int32_t result;

	for (index = 0; index < GYRO_PADS; index++)
	{
		if (gyro_pads[index].used && gyro_pads[index].instance == instance)
			return gyro_pads[index].handle >= 0 ? &gyro_pads[index] : NULL;
	}
	for (index = 0; index < GYRO_PADS && !pad; index++)
	{
		if (!gyro_pads[index].used)
			pad = &gyro_pads[index];
	}
	if (!pad)
	{
		/* (the oldest is forgotten: instances are not reused) */
		memmove(&gyro_pads[0], &gyro_pads[1], sizeof(gyro_pads[0]) * (GYRO_PADS - 1));
		pad = &gyro_pads[GYRO_PADS - 1];
	}
	memset(pad, 0, sizeof(*pad));
	pad->used = 1;
	pad->instance = instance;
	pad->handle = -1;
	pad->user = user_named(name);
	if (pad->user < 0)
	{
		host_logf(HOST_LOG_WARN, "gyro: pad %d (\"%s\") is no signed-in user's: no gyroscope", (int)instance,
			name ? name : "");
		return NULL;
	}
	pad->handle = scePadGetHandle(pad->user, 0, 0);
	if (pad->handle < 0)
	{
		host_logf(HOST_LOG_WARN, "gyro: pad %d (\"%s\", user %d): scePadGetHandle 0x%08x: no gyroscope", (int)instance,
			name ? name : "", pad->user, (unsigned)pad->handle);
		return NULL;
	}
	result = scePadSetMotionSensorState(pad->handle, true);
	host_logf(HOST_LOG_INFO, "gyro: pad %d (\"%s\") is user %d's, handle 0x%08x; the motion sensor on: 0x%08x",
		(int)instance, name ? name : "", pad->user, (unsigned)pad->handle, (unsigned)result);
	return pad;
}

void host_sdl_gamepad_gyro(uint32_t gamepad, float *yaw, float *pitch)
{
	struct pad_data state;
	struct ps5_gyro_sample sample;
	SDL_GameController *controller;
	struct gyro_pad *pad;
	int32_t result;
	int fresh;
	float turn;

	if (!yaw || !pitch)
		return;
	*yaw = 0.0f;
	*pitch = 0.0f;
	controller = host_sdl_handle_get(gamepad, _handle_gamepad);
	if (!controller)
		return;
	pthread_mutex_lock(&gyro_lock);
	pad = gyro_pad_of(controller);
	if (!pad)
	{
		pthread_mutex_unlock(&gyro_lock);
		return;
	}
	result = scePadReadState(pad->handle, &state);
	if (result < 0)
	{
		/* (a pad gone: found again, by its new instance, when it comes back) */
		host_logf(HOST_LOG_WARN, "gyro: pad %d: scePadReadState 0x%08x: forgotten", (int)pad->instance,
			(unsigned)result);
		pad->used = 0;
		pthread_mutex_unlock(&gyro_lock);
		return;
	}
	sample.timestamp_us = state.timestamp;
	sample.x = state.angular_velocity[0];
	sample.z = state.angular_velocity[2];
	fresh = sample.timestamp_us > pad->state.last_timestamp_us;
	ps5_gyro_turn(&pad->state, ps5_now_ns() / 1000ull, &sample, 1, yaw, pitch);
	minute.samples += (unsigned)fresh;
	minute.reads++;
	turn = SDL_sqrtf(*yaw * *yaw + *pitch * *pitch);
	*yaw *= GYRO_SCALE;
	*pitch *= GYRO_SCALE;
	minute.total += turn;
	if (turn > minute.largest)
		minute.largest = turn;
	pthread_mutex_unlock(&gyro_lock);
}

int ps5_pad_gyro_minute(unsigned *samples, unsigned *reads, float *largest, float *total)
{
	pthread_mutex_lock(&gyro_lock);
	*samples = minute.samples;
	*reads = minute.reads;
	*largest = minute.largest;
	*total = minute.total;
	memset(&minute, 0, sizeof(minute));
	pthread_mutex_unlock(&gyro_lock);
	return *reads != 0;
}
