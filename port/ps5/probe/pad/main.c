/*
PS5 PAD PROBE

Phase 7's first probe (port/ps5/PS5.md, "Phase 7", part A, item 1): what
gyroscope aiming will stand on. The title holds the pad through pacbrew's
SDL2, which opens it with scePadOpen for each signed-in user, names the
joystick after the user and reads it with scePadReadState, but never turns
the motion sensor on (its libSDL2.a imports no scePadSetMotionSensorState).
So the host will take SDL's handle and read the sensor itself. This asks:

1. SDL2 started as the title starts it (timer, events, game controllers)
   and every game controller opened; their names, types and instances.
2. The users: sceUserServiceGetLoginUserIdList and each one's name, the
   foreground user.
3. For each user: scePadGetHandle(user, 0, 0) while SDL2 has the pad open
   (SDL's handle, or an error), scePadOpen again (already opened?), and
   scePadGetControllerInformation and its extended form (the pad's type).
4. The sensor off: two seconds of scePadReadState, the angular velocity's
   largest magnitude (zeros if the sensor must be turned on).
5. scePadSetMotionSensorState(handle, 1); then, read every 4 ms (250 a
   second) with scePadReadState, on every pad at once:
   a. at rest, the pad on a table, 5 s: the noise (the largest magnitude
      of each axis, and its mean: the bias), the acceleration (which axis
      gravity is on, and its unit), the orientation;
   b. the same with scePadSetAngularVelocityDeadbandState(handle, 0), 4 s:
      whether the library hides noise under a dead band by default;
   c. six turns, each asked for by a notification and the kernel log: a
      quarter turn left, the pad level (the aim's yaw), back; right; the
      top tipped away (up); toward (down); steered left and right (the
      roll): each axis's angle integrated from its angular velocity
      and the timestamps over the turn (its largest excursion, with its
      sign): which axis moves, with which sign, and in what unit (a
      quarter turn is 1.57 if radians a second);
   throughout: how often the timestamp changes (the sensor's rate as
   scePadReadState sees it), the gaps between changes, the timestamp's
   unit against the time stamp counter, connected and the sample's layout.
6. scePadRead's queue for 3 s, read every 20 ms: samples a read and the
   gaps between their timestamps (the sensor's own rate).
7. The sensor off again, SDL_Quit.

The layout of the console's sample is Sony's ScePadData, after OpenOrbis's
PS4 headers (OpenOrbis-PS4-Toolchain, include/orbis/_types/pad.h,
OrbisPadData) and shadPS4's (src/core/libraries/pad/pad.h), asserted by its
size (120 bytes, as the io probe found) and by connected and the timestamp
making sense. The calls are declared by hand after the same headers
(OpenOrbis's Pad.h and UserService.h; scePadSetMotionSensorState,
scePadSetAngularVelocityDeadbandState and the controller information's
layout after shadPS4's pad.h, as OpenOrbis gives them no signature); the
SDK's libScePad.so and libSceUserService.so stubs name them.

Build: port/ps5/probe/title/build.sh port/ps5/probe/pad (build-extra.sh
       takes pacbrew's SDL2, as the SDL2 probe's)
Run:   python3 tools/ps5_deploy.py --title PPSA99106 --title-dir
       build/ps5/probe/PPSA99106 upload launch, then fetch; a person holds
       the pad and does what the notifications ask.

The report goes to the kernel log and /app0/pad-probe-<seconds>.txt (over FTP,
/data/homebrew/PPSA99106/pad-probe-<seconds>.txt).
*/

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "probe_report.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- the console's calls */

int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetForegroundUser(int32_t *user_id);
struct user_list
{
	int32_t user_id[4];
};
int32_t sceUserServiceGetLoginUserIdList(struct user_list *list);
int32_t sceUserServiceGetUserName(int32_t user_id, char *name, size_t size);

struct vector3
{
	float x, y, z;
};

/* Sony's ScePadData (OpenOrbis's OrbisPadData) */
struct pad_data
{
	uint32_t buttons;
	uint8_t left_x, left_y, right_x, right_y;
	uint8_t left_trigger, right_trigger;
	uint16_t padding;
	float orientation[4];
	struct vector3 acceleration;
	struct vector3 angular_velocity;
	struct
	{
		uint8_t fingers;
		uint8_t reserved[3];
		uint32_t reserved2;
		struct
		{
			uint16_t x, y;
			uint8_t id, reserved[3];
		} touch[2];
	} touch;
	uint8_t connected;
	uint64_t timestamp;
	uint8_t extension[16];
	uint8_t connected_count;
	uint8_t reserved[2];
	uint8_t unique_data_length;
	uint8_t unique_data[12];
};
_Static_assert(sizeof(struct pad_data) == 120, "the console's pad samples are 120 bytes");
_Static_assert(offsetof(struct pad_data, orientation) == 0x0c, "the orientation at 0x0c");
_Static_assert(offsetof(struct pad_data, angular_velocity) == 0x28, "the angular velocity at 0x28");
_Static_assert(offsetof(struct pad_data, connected) == 0x4c, "connected at 0x4c");
_Static_assert(offsetof(struct pad_data, timestamp) == 0x50, "the timestamp at 0x50");

/* shadPS4's OrbisPadControllerInformation */
struct pad_information
{
	float touch_pixel_density;
	uint16_t touch_width, touch_height;
	uint8_t dead_zone_left, dead_zone_right;
	uint8_t connection_type;
	uint8_t connected_count;
	uint8_t connected;
	int32_t device_class;
	uint8_t reserved[8];
};
_Static_assert(sizeof(struct pad_information) == 28, "the controller information is 28 bytes");

int32_t scePadInit(void);
int32_t scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int32_t scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int32_t scePadReadState(int32_t handle, struct pad_data *data);
int32_t scePadRead(int32_t handle, struct pad_data *data, int32_t count);
int32_t scePadGetControllerInformation(int32_t handle, struct pad_information *information);
int32_t scePadGetExtControllerInformation(int32_t handle, void *information);
int32_t scePadSetMotionSensorState(int32_t handle, bool enable);
int32_t scePadSetAngularVelocityDeadbandState(int32_t handle, bool enable);

int32_t sceKernelUsleep(uint32_t microseconds);

#define MAX_PADS 4
#define READ_US 4000
#define QUEUE_CAPACITY 64

static struct pad
{
	int32_t user;
	char name[32];
	int32_t handle;
	/* this phase's figures, reset by phase_start */
	uint64_t reads, changes, errors, disconnected;
	uint64_t last_timestamp, first_timestamp, first_us, last_change_us;
	uint64_t gap_min, gap_max, gap_total;
	double peak[3], sum[3], angle[3], angle_low[3], angle_high[3];
	uint64_t samples;
	struct pad_data last;
	int have_last;
} pads[MAX_PADS];
static int pad_count;

static const char *const axis_names[3] = { "x", "y", "z" };

/* ---------- 1. SDL2, as the title starts it */

static void start_sdl(void)
{
	int count, index;

	heading("1. SDL2, as the title starts it");
	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0)
	{
		say("SDL_Init: %s\n", SDL_GetError());
		return;
	}
	count = SDL_NumJoysticks();
	say("SDL_NumJoysticks: %d\n", count);
	for (index = 0; index < count; index++)
	{
		SDL_GameController *controller = SDL_IsGameController(index) ? SDL_GameControllerOpen(index) : NULL;

		say("joystick %d: \"%s\", instance %d, type %d, %s\n", index, SDL_JoystickNameForIndex(index),
			(int)SDL_JoystickGetDeviceInstanceID(index), (int)SDL_GameControllerTypeForIndex(index),
			controller ? "opened as a game controller" : "not opened");
		if (controller)
			say("  sensors SDL2 says it has: gyro %s, accelerometer %s\n",
				SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) ? "yes" : "no",
				SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) ? "yes" : "no");
	}
	SDL_GameControllerUpdate();
}

/* ---------- 2. and 3. the users and their pads */

static void find_pads(void)
{
	struct user_list list;
	int32_t result, foreground = -1;
	int index;

	heading("2. the users");
	result = sceUserServiceInitialize(NULL);
	say("sceUserServiceInitialize: 0x%08x (SDL2 has started it already)\n", (unsigned)result);
	result = sceUserServiceGetForegroundUser(&foreground);
	say("sceUserServiceGetForegroundUser: 0x%08x, user %d (0x%x)\n", (unsigned)result, foreground,
		(unsigned)foreground);
	memset(&list, 0xff, sizeof(list));
	result = sceUserServiceGetLoginUserIdList(&list);
	say("sceUserServiceGetLoginUserIdList: 0x%08x\n", (unsigned)result);
	for (index = 0; index < 4; index++)
	{
		char name[32] = "";

		if (list.user_id[index] == -1)
			continue;
		result = sceUserServiceGetUserName(list.user_id[index], name, sizeof(name));
		say("  user %d (0x%x): \"%s\" (0x%08x)\n", list.user_id[index], (unsigned)list.user_id[index], name,
			(unsigned)result);
		if (pad_count < MAX_PADS)
		{
			pads[pad_count].user = list.user_id[index];
			snprintf(pads[pad_count].name, sizeof(pads[pad_count].name), "%s", name);
			pad_count++;
		}
	}

	heading("3. each user's pad, while SDL2 holds it");
	for (index = 0; index < pad_count; index++)
	{
		struct pad *pad = &pads[index];
		struct pad_information information;
		unsigned char extended[256];
		int32_t opened;
		int byte;

		pad->handle = scePadGetHandle(pad->user, 0, 0);
		say("\"%s\": scePadGetHandle(user %d, standard port, 0): 0x%08x\n", pad->name, pad->user,
			(unsigned)pad->handle);
		opened = scePadOpen(pad->user, 0, 0, NULL);
		say("\"%s\": scePadOpen again: 0x%08x%s\n", pad->name, (unsigned)opened,
			(unsigned)opened == 0x80920004u ? " (already opened: SDL2's)" : "");
		if (pad->handle < 0 && opened >= 0)
			pad->handle = opened;
		if (pad->handle < 0)
			continue;
		memset(&information, 0, sizeof(information));
		result = scePadGetControllerInformation(pad->handle, &information);
		say("\"%s\": scePadGetControllerInformation: 0x%08x: touch pad %ux%u at %.1f a mm, dead zones %u %u, "
			"connection type %u, %u connected, connected %u, device class %d\n",
			pad->name, (unsigned)result, information.touch_width, information.touch_height,
			(double)information.touch_pixel_density, information.dead_zone_left, information.dead_zone_right,
			information.connection_type, information.connected_count, information.connected,
			information.device_class);
		memset(extended, 0, sizeof(extended));
		result = scePadGetExtControllerInformation(pad->handle, extended);
		say("\"%s\": scePadGetExtControllerInformation: 0x%08x, bytes 28 to 47 (pad types, capability):", pad->name,
			(unsigned)result);
		for (byte = 28; byte < 48; byte++)
			say(" %02x", extended[byte]);
		say("\n");
	}
}

/* ---------- the reading */

static void phase_start(void)
{
	int index;

	for (index = 0; index < pad_count; index++)
	{
		struct pad *pad = &pads[index];
		int32_t handle = pad->handle;
		int32_t user = pad->user;
		char name[32];

		memcpy(name, pad->name, sizeof(name));
		memset(pad, 0, sizeof(*pad));
		pad->handle = handle;
		pad->user = user;
		memcpy(pad->name, name, sizeof(name));
		pad->gap_min = UINT64_MAX;
	}
}

static double component(const struct vector3 *vector, int axis)
{
	return axis == 0 ? vector->x : axis == 1 ? vector->y : vector->z;
}

/* one read of every pad; integrates the angular velocity over the
timestamps (the unit found by the run: microseconds, as the io probe's) */
static void read_pads(void)
{
	int index, axis;

	for (index = 0; index < pad_count; index++)
	{
		struct pad *pad = &pads[index];
		struct pad_data data;
		uint64_t now = now_us();
		int32_t result;

		if (pad->handle < 0)
			continue;
		memset(&data, 0, sizeof(data));
		result = scePadReadState(pad->handle, &data);
		pad->reads++;
		if (result < 0)
		{
			if (pad->errors++ < 3)
				say("\"%s\": scePadReadState: 0x%08x\n", pad->name, (unsigned)result);
			continue;
		}
		if (!data.connected)
			pad->disconnected++;
		if (!pad->first_timestamp)
		{
			pad->first_timestamp = data.timestamp;
			pad->first_us = now;
		}
		if (pad->have_last && data.timestamp != pad->last.timestamp)
		{
			uint64_t gap = data.timestamp - pad->last.timestamp;
			double seconds = gap / 1e6;

			pad->changes++;
			if (gap < pad->gap_min)
				pad->gap_min = gap;
			if (gap > pad->gap_max)
				pad->gap_max = gap;
			pad->gap_total += gap;
			for (axis = 0; axis < 3; axis++)
			{
				double velocity = component(&data.angular_velocity, axis);

				if (gap < 100000)
					pad->angle[axis] += velocity * seconds;
				if (pad->angle[axis] < pad->angle_low[axis])
					pad->angle_low[axis] = pad->angle[axis];
				if (pad->angle[axis] > pad->angle_high[axis])
					pad->angle_high[axis] = pad->angle[axis];
			}
		}
		if (!pad->have_last || data.timestamp != pad->last.timestamp)
		{
			pad->samples++;
			for (axis = 0; axis < 3; axis++)
			{
				double velocity = component(&data.angular_velocity, axis);

				if (fabs(velocity) > pad->peak[axis])
					pad->peak[axis] = fabs(velocity);
				pad->sum[axis] += velocity;
			}
		}
		pad->last_timestamp = data.timestamp;
		pad->last_change_us = now;
		pad->last = data;
		pad->have_last = 1;
	}
}

static void read_for(unsigned milliseconds)
{
	uint64_t start = now_us();

	while (now_us() - start < milliseconds * 1000ull)
	{
		SDL_Event event;

		/* (SDL2 reads the pads too, as in the title) */
		while (SDL_PollEvent(&event))
		{
			if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED)
				say("SDL2: device event 0x%x, which %d\n", event.type, event.cdevice.which);
		}
		read_pads();
		sceKernelUsleep(READ_US);
	}
}

static void say_rate(const struct pad *pad)
{
	double elapsed_us = (double)(pad->last_change_us - pad->first_us);
	double stamp_us = (double)(pad->last_timestamp - pad->first_timestamp);

	say("  \"%s\": %llu reads, %llu new samples (%.1f a second), the timestamp's gaps min %llu, mean %.0f, max "
		"%llu; the timestamps advanced %.0f against the counter's %.0f us (%.4f); %llu reads not connected, "
		"%llu errors\n",
		pad->name, (unsigned long long)pad->reads, (unsigned long long)pad->changes,
		elapsed_us > 0 ? pad->changes / (elapsed_us / 1e6) : 0.0,
		(unsigned long long)(pad->gap_min == UINT64_MAX ? 0 : pad->gap_min),
		pad->changes ? (double)pad->gap_total / pad->changes : 0.0, (unsigned long long)pad->gap_max, stamp_us,
		elapsed_us, elapsed_us > 0 ? stamp_us / elapsed_us : 0.0, (unsigned long long)pad->disconnected,
		(unsigned long long)pad->errors);
}

static void say_rest(const char *what)
{
	int index, axis;

	for (index = 0; index < pad_count; index++)
	{
		const struct pad *pad = &pads[index];

		if (pad->handle < 0)
			continue;
		say_rate(pad);
		say("  \"%s\" %s: angular velocity's largest magnitude", pad->name, what);
		for (axis = 0; axis < 3; axis++)
			say(" %s %.5f", axis_names[axis], pad->peak[axis]);
		say("; mean");
		for (axis = 0; axis < 3; axis++)
			say(" %s %+.5f", axis_names[axis], pad->samples ? pad->sum[axis] / pad->samples : 0.0);
		say("\n  \"%s\" last sample: acceleration %+.4f %+.4f %+.4f, orientation %+.4f %+.4f %+.4f %+.4f, "
			"buttons 0x%08x, connected %u, count %u\n",
			pad->name, (double)pad->last.acceleration.x, (double)pad->last.acceleration.y,
			(double)pad->last.acceleration.z, (double)pad->last.orientation[0], (double)pad->last.orientation[1],
			(double)pad->last.orientation[2], (double)pad->last.orientation[3], (unsigned)pad->last.buttons,
			pad->last.connected, pad->last.connected_count);
	}
}

static void say_turn(const char *turn)
{
	int index, axis;

	for (index = 0; index < pad_count; index++)
	{
		const struct pad *pad = &pads[index];
		int moved = 0;

		if (pad->handle < 0)
			continue;
		say("  \"%s\" %s: each axis's angle, its excursion low to high and where it ended:", pad->name, turn);
		for (axis = 0; axis < 3; axis++)
		{
			say(" %s %+.3f to %+.3f (end %+.3f)", axis_names[axis], pad->angle_low[axis], pad->angle_high[axis],
				pad->angle[axis]);
			if (fmax(-pad->angle_low[axis], pad->angle_high[axis]) >
				fmax(-pad->angle_low[moved], pad->angle_high[moved]))
				moved = axis;
		}
		say("; the axis that moved most: %s, %s\n", axis_names[moved],
			-pad->angle_low[moved] > pad->angle_high[moved] ? "negative" : "positive");
		say("  \"%s\" peak angular velocity: x %.3f y %.3f z %.3f\n", pad->name, pad->peak[0], pad->peak[1],
			pad->peak[2]);
	}
}

/* a notification, the kernel log's line, and a pause to read it */
static void ask(const char *text, unsigned settle_ms)
{
	notify("Halo PS5 pad probe: %s", text);
	say("--- asked: %s (at %.2f s)\n", text, now_us() / 1e6);
	read_for(settle_ms);
}

static void step_sensor_off(void)
{
	heading("4. the sensor not yet turned on: 2 s of scePadReadState");
	phase_start();
	read_for(2000);
	say_rest("with the sensor off");
}

static void step_sensor(void)
{
	static const struct
	{
		const char *ask;
		const char *name;
	} turns[] = {
		/* (run 2 asked "like turning a steering wheel" and got a roll: the
		aim's turn is asked as the pad pointed at a wall, and the roll on its
		own) */
		{ "AIM LEFT: keep the pad level and point it at the LEFT wall (a quarter turn), hold", "aim left" },
		{ "AIM RIGHT: keep the pad level and point it at the RIGHT wall, hold", "aim right" },
		{ "tip the TOP of the pad AWAY from you a quarter turn (look up), hold", "up" },
		{ "tip the TOP of the pad TOWARD you a quarter turn (look down), hold", "down" },
		{ "STEER LEFT: turn the pad like a steering wheel to the left, hold", "steer left" },
		{ "STEER RIGHT: turn the pad like a steering wheel to the right, hold", "steer right" },
	};
	size_t turn;
	int index;

	heading("5. the sensor on");
	for (index = 0; index < pad_count; index++)
	{
		if (pads[index].handle < 0)
			continue;
		say("\"%s\": scePadSetMotionSensorState(handle, 1): 0x%08x\n", pads[index].name,
			(unsigned)scePadSetMotionSensorState(pads[index].handle, 1));
	}

	heading("5a. at rest, the dead band as the library has it");
	ask("put the pad DOWN on a table and do not touch it", 4000);
	phase_start();
	read_for(5000);
	say_rest("at rest");

	heading("5b. at rest, the dead band off");
	for (index = 0; index < pad_count; index++)
	{
		if (pads[index].handle < 0)
			continue;
		say("\"%s\": scePadSetAngularVelocityDeadbandState(handle, 0): 0x%08x\n", pads[index].name,
			(unsigned)scePadSetAngularVelocityDeadbandState(pads[index].handle, 0));
	}
	phase_start();
	read_for(4000);
	say_rest("at rest, the dead band off");
	for (index = 0; index < pad_count; index++)
	{
		if (pads[index].handle < 0)
			continue;
		say("\"%s\": scePadSetAngularVelocityDeadbandState(handle, 1): 0x%08x\n", pads[index].name,
			(unsigned)scePadSetAngularVelocityDeadbandState(pads[index].handle, 1));
	}

	heading("5c. the turns (the dead band on, as the library starts)");
	ask("pick the pad up and hold it as you play, facing the TV", 4000);
	for (turn = 0; turn < sizeof(turns) / sizeof(turns[0]); turn++)
	{
		ask(turns[turn].ask, 0);
		phase_start();
		read_for(3500);
		say_turn(turns[turn].name);
		ask("back to facing the TV", 2500);
	}
}

/* ---------- 6. the queue */

static void step_queue(void)
{
	static struct pad_data queue[QUEUE_CAPACITY];
	int index;

	heading("6. scePadRead's queue, every 20 ms for 3 s");
	for (index = 0; index < pad_count; index++)
	{
		const struct pad *pad = &pads[index];
		uint64_t start = now_us(), reads = 0, samples = 0, most = 0, last = 0, gaps = 0, gap_total = 0;
		uint64_t gap_min = UINT64_MAX, gap_max = 0;
		int errors = 0;

		if (pad->handle < 0)
			continue;
		while (now_us() - start < 3000000ull)
		{
			int32_t count = scePadRead(pad->handle, queue, QUEUE_CAPACITY);
			int32_t sample;

			reads++;
			if (count < 0)
			{
				if (errors++ < 3)
					say("\"%s\": scePadRead: 0x%08x\n", pad->name, (unsigned)count);
			}
			for (sample = 0; sample < count; sample++)
			{
				if (last && queue[sample].timestamp > last)
				{
					uint64_t gap = queue[sample].timestamp - last;

					gaps++;
					gap_total += gap;
					if (gap < gap_min)
						gap_min = gap;
					if (gap > gap_max)
						gap_max = gap;
				}
				last = queue[sample].timestamp;
			}
			if (count > 0)
			{
				samples += (uint64_t)count;
				if ((uint64_t)count > most)
					most = (uint64_t)count;
			}
			sceKernelUsleep(20000);
		}
		say("\"%s\": %llu reads, %llu samples (%.1f a second), at most %llu in a read; gaps between their "
			"timestamps min %llu, mean %.0f, max %llu\n",
			pad->name, (unsigned long long)reads, (unsigned long long)samples, samples / 3.0,
			(unsigned long long)most, (unsigned long long)(gap_min == UINT64_MAX ? 0 : gap_min),
			gaps ? (double)gap_total / gaps : 0.0, (unsigned long long)gap_max);
	}
}

int main(void)
{
	int index;

	char report_name[64];

	/* (named for the start, so a later run never writes over an earlier one's) */
	snprintf(report_name, sizeof(report_name), "pad-probe-%llu.txt", (unsigned long long)(now_us() / 1000000));
	report_open(report_name);
	notify("Halo PS5 pad probe starts: hold the pad, the notifications say what to do");
	say("ScePadData is %zu bytes; angular velocity at 0x%zx, timestamp at 0x%zx\n", sizeof(struct pad_data),
		offsetof(struct pad_data, angular_velocity), offsetof(struct pad_data, timestamp));
	start_sdl();
	find_pads();
	if (!pad_count)
		say("no signed-in user: nothing to read\n");
	step_sensor_off();
	step_sensor();
	step_queue();
	heading("7. the end");
	for (index = 0; index < pad_count; index++)
	{
		if (pads[index].handle >= 0)
			say("\"%s\": scePadSetMotionSensorState(handle, 0): 0x%08x\n", pads[index].name,
				(unsigned)scePadSetMotionSensorState(pads[index].handle, 0));
	}
	SDL_Quit();
	say("SDL_Quit returned\n");
	heading("done");
	notify("Halo PS5 pad probe is done");
	report_close();
	return 0;
}
