/*
PS5 IO PROBE

Asks whether a homebrew title can use the controller and the speakers the way
the game's PS5 host would, through the calls PS5 RetroArch's input and audio
drivers make on the console (its src/input_ps5.cpp and src/audio_ps5.cpp,
which follow ProsperoLight's; the payload SDK has no headers for them, so the
declarations below are theirs):

1. The user service: who started the title, and who is signed in.
2. The pad: scePadInit, then scePadOpen for the user who started the title
   (scePadGetHandle if the process already holds it). For 15 seconds every
   change of buttons, triggers, sticks and connection is reported, with how
   many samples each read returned. A person presses buttons while it runs.
3. Rumble: the compatible vibration mode and half strength on both motors
   for a second (the game's rumble is two motor levels, as the Xbox's).
4. Audio: sceAudioOutOpen on the main output at 48 kHz, the rate the game
   mixes at (port/linux/src/dsound_sdl.c), as 16-bit stereo and as float
   stereo, the game's own format; two seconds of a tone each (440 Hz, then
   660 Hz), timing how long each output call blocks.

Build: port/ps5/probe/title/build.sh port/ps5/probe/io
Run:   copy build/ps5/probe/PPSA99101 to /data/homebrew/PPSA99101 over FTP,
       start "Halo PS5 io probe" from the home screen, and press buttons on
       the controller when the notification asks.

The report goes to the kernel log and /app0/io-probe.txt (over FTP,
/data/homebrew/PPSA99101/io-probe.txt).
*/

#include "probe_report.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetInitialUser(int32_t *user_id);
struct user_list
{
	int32_t user_id[4];
};
int32_t sceUserServiceGetLoginUserIdList(struct user_list *list);

int32_t scePadInit(void);
int32_t scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void *params);
int32_t scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int32_t scePadRead(int32_t handle, void *samples, int32_t capacity);
int32_t scePadClose(int32_t handle);
struct vibration
{
	uint8_t large_motor;
	uint8_t small_motor;
};
int32_t scePadSetVibration(int32_t handle, const struct vibration *param);
int32_t scePadSetVibrationMode(int32_t handle, int32_t mode);

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(int32_t user_id, int32_t type, int32_t index, uint32_t length, uint32_t rate,
	uint32_t format);
int32_t sceAudioOutOutput(int32_t handle, const void *data);
int32_t sceAudioOutClose(int32_t handle);

int32_t sceKernelUsleep(uint32_t microseconds);

/* one sample, as the console writes it (PS5 RetroArch's PadSample) */
struct pad_sample
{
	uint32_t buttons;
	uint8_t left_x, left_y, right_x, right_y;
	uint8_t left_trigger, right_trigger;
	uint8_t reserved_to_touch[42];
	uint8_t touch_count;
	uint8_t reserved_touch[7];
	struct
	{
		uint16_t x, y;
		uint8_t id, reserved[3];
	} touches[2];
	int32_t connected;
	uint64_t timestamp_us;
	uint8_t extension[16];
	uint8_t connected_count;
	uint8_t remaining[15];
};
_Static_assert(sizeof(struct pad_sample) == 120, "the console's pad samples are 120 bytes");
_Static_assert(offsetof(struct pad_sample, connected) == 0x4c, "connected sits at 0x4c");

#define SAMPLE_CAPACITY 64
#define PAD_SECONDS 15
#define STICK_STEP 32
#define SYSTEM_USER 0xff
#define AUDIO_RATE 48000
#define AUDIO_GRAIN 256
#define AUDIO_S16_STEREO 1
#define AUDIO_FLOAT_STEREO 4

static const struct
{
	uint32_t bit;
	const char *name;
} button_names[] = {
	{ 0x000002, "L3" }, { 0x000004, "R3" }, { 0x000008, "options" }, { 0x000010, "up" },
	{ 0x000020, "right" }, { 0x000040, "down" }, { 0x000080, "left" }, { 0x000100, "L2" },
	{ 0x000200, "R2" }, { 0x000400, "L1" }, { 0x000800, "R1" }, { 0x001000, "triangle" },
	{ 0x002000, "circle" }, { 0x004000, "cross" }, { 0x008000, "square" }, { 0x100000, "touch pad" },
	{ 0x80000000u, "intercepted by the system" },
};

static int32_t initial_user = -1;

static void probe_users(void)
{
	struct user_list list;
	int32_t result;
	int index;

	heading("users");
	result = sceUserServiceInitialize(NULL);
	say("sceUserServiceInitialize: 0x%08x\n", (unsigned)result);
	result = sceUserServiceGetInitialUser(&initial_user);
	say("sceUserServiceGetInitialUser: 0x%08x, user %d (0x%x)\n", (unsigned)result, initial_user,
		(unsigned)initial_user);
	memset(&list, 0xff, sizeof(list));
	result = sceUserServiceGetLoginUserIdList(&list);
	say("sceUserServiceGetLoginUserIdList: 0x%08x:", (unsigned)result);
	for (index = 0; index < 4; index++)
		say(" %d", list.user_id[index]);
	say("\n");
	if (initial_user < 0)
		initial_user = list.user_id[0];
}

static void describe_buttons(uint32_t changed, uint32_t now, char *text, size_t size)
{
	size_t used = 0;
	size_t index;

	text[0] = 0;
	for (index = 0; index < sizeof(button_names) / sizeof(button_names[0]); index++)
	{
		if (!(changed & button_names[index].bit) || used >= size)
			continue;
		used += snprintf(text + used, size - used, " %s%s",
			now & button_names[index].bit ? "+" : "-", button_names[index].name);
	}
}

static int stick_zone(uint8_t value)
{
	return value / STICK_STEP;
}

static void probe_pad(void)
{
	static struct pad_sample samples[SAMPLE_CAPACITY];
	struct pad_sample last;
	struct vibration vibration;
	int32_t handle;
	int32_t result;
	uint64_t start;
	uint64_t reads = 0, empty = 0, sample_total = 0, largest = 0, errors = 0;
	int have_last = 0;

	heading("pad");
	result = scePadInit();
	say("scePadInit: 0x%08x\n", (unsigned)result);
	handle = scePadOpen(initial_user, 0, 0, NULL);
	say("scePadOpen(user %d, standard port, 0): 0x%08x\n", initial_user, (unsigned)handle);
	if (handle < 0)
	{
		handle = scePadGetHandle(initial_user, 0, 0);
		say("scePadGetHandle(user %d): 0x%08x\n", initial_user, (unsigned)handle);
	}
	if (handle < 0)
	{
		say("no pad handle: the pad cannot be read\n");
		return;
	}

	notify("Halo PS5 io probe: press buttons, triggers and sticks for %d seconds", PAD_SECONDS);
	say("reading for %d s: press buttons, the triggers and both sticks\n", PAD_SECONDS);
	start = now_us();
	memset(&last, 0, sizeof(last));
	while (now_us() - start < PAD_SECONDS * 1000000ULL)
	{
		int32_t count = scePadRead(handle, samples, SAMPLE_CAPACITY);
		int32_t index;

		reads++;
		if (count < 0)
		{
			if (errors++ < 5)
				say("scePadRead: 0x%08x\n", (unsigned)count);
			sceKernelUsleep(16000);
			continue;
		}
		if (count == 0)
			empty++;
		sample_total += (uint64_t)count;
		if ((uint64_t)count > largest)
			largest = (uint64_t)count;
		for (index = 0; index < count; index++)
		{
			const struct pad_sample *sample = &samples[index];
			double seconds = (now_us() - start) / 1e6;

			if (!have_last || sample->connected != last.connected)
				say("%6.2f s: connected %d (%u controllers)\n", seconds, sample->connected,
					(unsigned)sample->connected_count);
			if (!have_last || sample->buttons != last.buttons)
			{
				char text[512];

				describe_buttons(have_last ? sample->buttons ^ last.buttons : sample->buttons,
					sample->buttons, text, sizeof(text));
				say("%6.2f s: buttons 0x%08x%s\n", seconds, (unsigned)sample->buttons, text);
			}
			if (!have_last || stick_zone(sample->left_x) != stick_zone(last.left_x) ||
				stick_zone(sample->left_y) != stick_zone(last.left_y) ||
				stick_zone(sample->right_x) != stick_zone(last.right_x) ||
				stick_zone(sample->right_y) != stick_zone(last.right_y) ||
				stick_zone(sample->left_trigger) != stick_zone(last.left_trigger) ||
				stick_zone(sample->right_trigger) != stick_zone(last.right_trigger))
			{
				say("%6.2f s: left %3u,%3u right %3u,%3u triggers %3u %3u, touches %u, time %llu us\n", seconds,
					sample->left_x, sample->left_y, sample->right_x, sample->right_y, sample->left_trigger,
					sample->right_trigger, sample->touch_count, (unsigned long long)sample->timestamp_us);
			}
			last = *sample;
			have_last = 1;
		}
		sceKernelUsleep(16000);
	}
	say("%llu reads, %llu empty, %llu samples, at most %llu in one read, %llu errors\n",
		(unsigned long long)reads, (unsigned long long)empty, (unsigned long long)sample_total,
		(unsigned long long)largest, (unsigned long long)errors);

	heading("rumble");
	notify("Halo PS5 io probe: the controller should rumble for a second");
	result = scePadSetVibrationMode(handle, 2);
	say("scePadSetVibrationMode(compatible): 0x%08x\n", (unsigned)result);
	vibration.large_motor = 128;
	vibration.small_motor = 128;
	result = scePadSetVibration(handle, &vibration);
	say("scePadSetVibration(128, 128): 0x%08x\n", (unsigned)result);
	sceKernelUsleep(1000000);
	vibration.large_motor = 0;
	vibration.small_motor = 0;
	result = scePadSetVibration(handle, &vibration);
	say("scePadSetVibration(0, 0): 0x%08x\n", (unsigned)result);

	result = scePadClose(handle);
	say("scePadClose: 0x%08x\n", (unsigned)result);
}

static void play_tone(const char *name, uint32_t format, double frequency)
{
	static float float_grain[AUDIO_GRAIN * 2];
	static int16_t s16_grain[AUDIO_GRAIN * 2];
	const void *grain = format == AUDIO_FLOAT_STEREO ? (const void *)float_grain : (const void *)s16_grain;
	int32_t handle;
	int32_t user;
	uint64_t start, slowest = 0, frame = 0;
	int calls, errors = 0;

	/* the system's user, as PS5 RetroArch opens it, else the one who
	started the title */
	user = SYSTEM_USER;
	handle = sceAudioOutOpen(user, 0, 0, AUDIO_GRAIN, AUDIO_RATE, format);
	say("%s: sceAudioOutOpen(system user, main, 0, %d, %d, %u): 0x%08x\n", name, AUDIO_GRAIN, AUDIO_RATE,
		(unsigned)format, (unsigned)handle);
	if (handle < 0 && initial_user >= 0)
	{
		user = initial_user;
		handle = sceAudioOutOpen(user, 0, 0, AUDIO_GRAIN, AUDIO_RATE, format);
		say("%s: sceAudioOutOpen(user %d, ...): 0x%08x\n", name, user, (unsigned)handle);
	}
	if (handle < 0)
		return;

	start = now_us();
	for (calls = 0; calls < 2 * AUDIO_RATE / AUDIO_GRAIN; calls++)
	{
		uint64_t before;
		uint64_t took;
		int index;
		int32_t result;

		for (index = 0; index < AUDIO_GRAIN; index++, frame++)
		{
			double value = 0.25 * sin(2.0 * M_PI * frequency * (double)frame / AUDIO_RATE);

			float_grain[index * 2] = float_grain[index * 2 + 1] = (float)value;
			s16_grain[index * 2] = s16_grain[index * 2 + 1] = (int16_t)(value * 32767.0);
		}
		before = now_us();
		result = sceAudioOutOutput(handle, grain);
		took = now_us() - before;
		if (took > slowest && calls > 4)
			slowest = took;
		if (result < 0 && errors++ < 5)
			say("%s: sceAudioOutOutput: 0x%08x\n", name, (unsigned)result);
	}
	sceAudioOutOutput(handle, NULL);
	say("%s: %d grains of %d frames in %.3f s (%.3f s of sound), slowest call %llu us, %d errors\n", name, calls,
		AUDIO_GRAIN, (now_us() - start) / 1e6, (double)calls * AUDIO_GRAIN / AUDIO_RATE,
		(unsigned long long)slowest, errors);
	say("%s: sceAudioOutClose: 0x%08x\n", name, (unsigned)sceAudioOutClose(handle));
}

static void probe_audio(void)
{
	heading("audio");
	say("sceAudioOutInit: 0x%08x\n", (unsigned)sceAudioOutInit());
	notify("Halo PS5 io probe: you should hear a low tone, then a higher one");
	play_tone("16-bit stereo, 440 Hz", AUDIO_S16_STEREO, 440.0);
	play_tone("float stereo, 660 Hz", AUDIO_FLOAT_STEREO, 660.0);
}

int main(void)
{
	report_open("io-probe.txt");
	notify("Halo PS5 io probe starts");
	probe_users();
	probe_audio();
	probe_pad();
	heading("done");
	notify("Halo PS5 io probe is done: read io-probe.txt over FTP");
	report_close();
	return 0;
}
