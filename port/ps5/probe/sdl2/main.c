/*
PS5 SDL2 PROBE

The guest calls SDL3 (its host_sdl_* imports); the Switch answers them with
SDL2 underneath (port/switch/host/host_sdl2.c). This asks whether the PS5 can
do the same with pacbrew's SDL2 (ps5-payload-dev's port, pacbrew v0.40.2,
SDL 2.30.12) inside a title built with PS5_Vulkan's toolchain, for the pad,
audio and events, with no video:

1. SDL_GetVersion, SDL_GetRevision.
2. SDL_InitSubSystem one flag at a time: TIMER, EVENTS, GAMECONTROLLER
   (which brings JOYSTICK), AUDIO; SDL_GetError on a failure. Not VIDEO.
3. The pad: the joysticks, which are game controllers, their names, GUIDs
   and mappings; SDL_GameControllerOpen(0); 15 seconds of SDL_PollEvent with
   every button by name and every axis by name and value (when it moves to
   another tenth of its range), while a person presses everything; then
   SDL_GameControllerRumble for a second (did the pad shake? the person
   says).
4. Audio: the drivers; a device at 48 kHz, float, stereo, 256 samples, with
   a callback writing a 440 Hz tone: the spec SDL gave back, two seconds of
   sound, the callback's count and the gaps between calls; then a device
   with no callback fed by SDL_QueueAudio for two seconds, the queue's size
   sampled.
5. Timing: SDL_GetTicks64 and SDL_GetPerformanceCounter against the time
   stamp counter over five seconds.
6. Last: SDL_InitSubSystem(SDL_INIT_VIDEO), its driver, whether
   SDL_CreateWindow(..., 0) gives a window, and SDL_GetNumVideoDisplays.
   The host does not want SDL's video; this says what leaving it out costs.
7. SDL_Quit.

Each step is guarded: its name goes to /app0/sdl2-skip.txt before it runs
and the file is deleted after, so a run that crashes in a step skips that
step the next time it is started, and says so.

Build: port/ps5/probe/title/build.sh port/ps5/probe/sdl2 (build-extra.sh
       takes pacbrew's flags)
Run:   port/ps5/probe/title/deploy.sh 192.168.2.157 PPSA99104, start
       "Halo PS5 SDL2 probe" from the home screen, press every button, both
       sticks and both triggers when the notification asks, and say whether
       the pad shook.

The report goes to the kernel log and /app0/sdl2-probe.txt (over FTP,
/data/homebrew/PPSA99104/sdl2-probe.txt).
*/

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "probe_report.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SKIP_FILE "/app0/sdl2-skip.txt"
#define PAD_SECONDS 15
#define AUDIO_RATE 48000
#define AUDIO_SAMPLES 256
#define CALLBACK_LIMIT 2048

/* ---------- the crash guard */

static char skipped[64];

static void guard_load(void)
{
	FILE *file = fopen(SKIP_FILE, "r");

	if (!file)
		return;
	if (fgets(skipped, sizeof(skipped), file))
		skipped[strcspn(skipped, "\r\n")] = 0;
	fclose(file);
	unlink(SKIP_FILE);
	if (skipped[0])
		say("the last run did not finish step \"%s\": it is skipped this time\n", skipped);
}

/* 1 if the step is to run (its name is then on file until guard_end) */
static int guard_begin(const char *step)
{
	FILE *file;

	if (!strcmp(step, skipped))
	{
		say("skipped: the last run crashed in this step (see the kernel log of that run)\n");
		return 0;
	}
	file = fopen(SKIP_FILE, "w");
	if (file)
	{
		fprintf(file, "%s\n", step);
		fclose(file);
		chmod(SKIP_FILE, 0666);
	}
	return 1;
}

static void guard_end(void)
{
	unlink(SKIP_FILE);
}

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* ---------- 1. the version */

static void step_version(void)
{
	SDL_version compiled, linked;

	heading("1. the version");
	if (!guard_begin("version"))
		return;
	SDL_VERSION(&compiled);
	SDL_GetVersion(&linked);
	say("compiled against SDL %u.%u.%u, linked %u.%u.%u, revision %s\n", compiled.major, compiled.minor,
		compiled.patch, linked.major, linked.minor, linked.patch, SDL_GetRevision());
	say("platform \"%s\"\n", SDL_GetPlatform());
	guard_end();
}

/* ---------- 2. the subsystems */

static int audio_ready, controller_ready;

static void step_subsystems(void)
{
	static const struct
	{
		Uint32 flag;
		const char *name;
	} flags[] = {
		{ SDL_INIT_TIMER, "SDL_INIT_TIMER" },
		{ SDL_INIT_EVENTS, "SDL_INIT_EVENTS" },
		{ SDL_INIT_GAMECONTROLLER, "SDL_INIT_GAMECONTROLLER" },
		{ SDL_INIT_AUDIO, "SDL_INIT_AUDIO" },
	};
	size_t index;

	heading("2. the subsystems, one at a time");
	SDL_SetMainReady();
	for (index = 0; index < sizeof(flags) / sizeof(flags[0]); index++)
	{
		uint64_t start;
		int result;

		if (!guard_begin(flags[index].name))
			continue;
		start = now_us();
		result = SDL_InitSubSystem(flags[index].flag);
		say("SDL_InitSubSystem(%s): %d in %llu us%s%s\n", flags[index].name, result,
			(unsigned long long)(now_us() - start), result ? ", " : "", result ? SDL_GetError() : "");
		if (result == 0 && flags[index].flag == SDL_INIT_AUDIO)
			audio_ready = 1;
		if (result == 0 && flags[index].flag == SDL_INIT_GAMECONTROLLER)
			controller_ready = 1;
		guard_end();
	}
	say("SDL_WasInit: 0x%08x\n", (unsigned)SDL_WasInit(0));
}

/* ---------- 3. the pad */

static void step_pad(void)
{
	SDL_GameController *controller;
	int count, index;
	int last_axis[SDL_CONTROLLER_AXIS_MAX];
	uint64_t start;
	unsigned events = 0, buttons = 0, motions = 0;
	int result;

	heading("3. the pad");
	if (!controller_ready)
	{
		say("skipped: SDL_INIT_GAMECONTROLLER did not start\n");
		return;
	}
	if (!guard_begin("pad"))
		return;
	count = SDL_NumJoysticks();
	say("SDL_NumJoysticks: %d%s%s\n", count, count < 0 ? ", " : "", count < 0 ? SDL_GetError() : "");
	for (index = 0; index < count; index++)
	{
		char guid[64];
		char *mapping;

		SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
		mapping = SDL_GameControllerMappingForDeviceIndex(index);
		say("joystick %d: \"%s\", game controller %s, GUID %s, type %d\n", index, SDL_JoystickNameForIndex(index),
			SDL_IsGameController(index) ? "yes" : "no", guid, (int)SDL_GameControllerTypeForIndex(index));
		say("  mapping: %s\n", mapping ? mapping : "(none)");
		SDL_free(mapping);
	}
	controller = SDL_GameControllerOpen(0);
	say("SDL_GameControllerOpen(0): %s%s\n", controller ? "opened" : "NULL, ", controller ? "" : SDL_GetError());
	if (!controller)
	{
		guard_end();
		return;
	}
	say("its name \"%s\", player index %d, rumble %s\n", SDL_GameControllerName(controller),
		SDL_GameControllerGetPlayerIndex(controller), SDL_GameControllerHasRumble(controller) ? "yes" : "no");

	for (index = 0; index < SDL_CONTROLLER_AXIS_MAX; index++)
		last_axis[index] = 100;
	notify("Halo PS5 SDL2 probe: press every button, both sticks and both triggers for %d seconds", PAD_SECONDS);
	say("reading events for %d s: press every button, both sticks and both triggers\n", PAD_SECONDS);
	start = now_us();
	while (now_us() - start < PAD_SECONDS * 1000000ULL)
	{
		SDL_Event event;

		while (SDL_PollEvent(&event))
		{
			double seconds = (now_us() - start) / 1e6;

			events++;
			switch (event.type)
			{
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				buttons++;
				say("%6.2f s: %s %s\n", seconds,
					SDL_GameControllerGetStringForButton((SDL_GameControllerButton)event.cbutton.button),
					event.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
				break;
			case SDL_CONTROLLERAXISMOTION:
			{
				/* a tenth of the range: -32768..32767 */
				int tenth = (event.caxis.value + 32768) * 10 / 65536;

				motions++;
				if (event.caxis.axis < SDL_CONTROLLER_AXIS_MAX && tenth != last_axis[event.caxis.axis])
				{
					last_axis[event.caxis.axis] = tenth;
					say("%6.2f s: %s %d\n", seconds,
						SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)event.caxis.axis),
						event.caxis.value);
				}
				break;
			}
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
			case SDL_JOYDEVICEADDED:
			case SDL_JOYDEVICEREMOVED:
				say("%6.2f s: device event 0x%x, which %d\n", seconds, event.type, event.cdevice.which);
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
			case SDL_JOYAXISMOTION:
			case SDL_JOYHATMOTION:
				break;
			default:
				say("%6.2f s: event 0x%x\n", seconds, event.type);
				break;
			}
		}
		SDL_Delay(5);
	}
	say("%u events: %u button changes, %u axis motions\n", events, buttons, motions);

	notify("Halo PS5 SDL2 probe: the pad should shake for a second now");
	result = SDL_GameControllerRumble(controller, 0x8000, 0x8000, 1000);
	say("SDL_GameControllerRumble(0x8000, 0x8000, 1000 ms): %d%s%s (did the pad shake? see the run's notes)\n",
		result, result ? ", " : "", result ? SDL_GetError() : "");
	SDL_Delay(1500);
	SDL_GameControllerClose(controller);
	guard_end();
}

/* ---------- 4. audio */

static volatile unsigned callback_count;
static uint64_t callback_times[CALLBACK_LIMIT];
static double tone_phase;

static void tone(float *samples, int frames, int channels)
{
	int frame, channel;

	for (frame = 0; frame < frames; frame++)
	{
		float value = (float)(0.25 * sin(tone_phase));

		tone_phase += 2.0 * M_PI * 440.0 / AUDIO_RATE;
		if (tone_phase > 2.0 * M_PI)
			tone_phase -= 2.0 * M_PI;
		for (channel = 0; channel < channels; channel++)
			samples[frame * channels + channel] = value;
	}
}

static int callback_channels = 2;

static void audio_callback(void *user, Uint8 *stream, int length)
{
	(void)user;
	if (callback_count < CALLBACK_LIMIT)
		callback_times[callback_count] = now_us();
	callback_count++;
	tone((float *)stream, length / (int)(sizeof(float) * callback_channels), callback_channels);
}

static void say_spec(const char *what, const SDL_AudioSpec *spec)
{
	say("%s: %d Hz, format 0x%04x, %u channels, %u samples, buffer %u bytes, silence %u\n", what, spec->freq,
		(unsigned)spec->format, (unsigned)spec->channels, (unsigned)spec->samples, (unsigned)spec->size,
		(unsigned)spec->silence);
}

static void step_audio_callback(void)
{
	SDL_AudioSpec want, got;
	SDL_AudioDeviceID device;
	uint64_t gaps[CALLBACK_LIMIT];
	unsigned count, index;

	if (!guard_begin("audio, callback"))
		return;
	memset(&want, 0, sizeof(want));
	want.freq = AUDIO_RATE;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = AUDIO_SAMPLES;
	want.callback = audio_callback;
	memset(&got, 0, sizeof(got));
	device = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
	say("SDL_OpenAudioDevice(NULL, 48000 Hz, AUDIO_F32SYS, 2, 256, a callback): %u%s%s\n", (unsigned)device,
		device ? "" : ", ", device ? "" : SDL_GetError());
	if (!device)
	{
		guard_end();
		return;
	}
	say_spec("the spec SDL gave back", &got);
	callback_channels = got.channels;
	notify("Halo PS5 SDL2 probe: a 440 Hz tone for two seconds, twice");
	SDL_PauseAudioDevice(device, 0);
	SDL_Delay(2000);
	SDL_PauseAudioDevice(device, 1);
	SDL_CloseAudioDevice(device);
	count = callback_count < CALLBACK_LIMIT ? callback_count : CALLBACK_LIMIT;
	for (index = 1; index < count; index++)
		gaps[index - 1] = callback_times[index] - callback_times[index - 1];
	if (count > 2)
	{
		qsort(gaps, count - 1, sizeof(gaps[0]), compare_u64);
		say("the callback: %u calls in 2 s (%.0f expected at %u samples), gaps between calls min %llu us, median "
			"%llu us, max %llu us\n",
			callback_count, 2.0 * AUDIO_RATE / got.samples, (unsigned)got.samples, (unsigned long long)gaps[0],
			(unsigned long long)gaps[(count - 1) / 2], (unsigned long long)gaps[count - 2]);
	}
	else
		say("the callback: %u calls in 2 s\n", callback_count);
	guard_end();
}

static void step_audio_queue(void)
{
	SDL_AudioSpec want, got;
	SDL_AudioDeviceID device;
	static float grain[AUDIO_SAMPLES * 2 * 4];
	uint64_t start;
	Uint32 smallest = 0xffffffffu, largest = 0, samples = 0;
	unsigned long long queued_bytes = 0;
	int failures = 0;

	if (!guard_begin("audio, queue"))
		return;
	memset(&want, 0, sizeof(want));
	want.freq = AUDIO_RATE;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = AUDIO_SAMPLES;
	memset(&got, 0, sizeof(got));
	device = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
	say("SDL_OpenAudioDevice(NULL, 48000 Hz, AUDIO_F32SYS, 2, 256, no callback): %u%s%s\n", (unsigned)device,
		device ? "" : ", ", device ? "" : SDL_GetError());
	if (!device)
	{
		guard_end();
		return;
	}
	say_spec("the spec SDL gave back", &got);
	SDL_PauseAudioDevice(device, 0);
	start = now_us();
	/* keep about 4 grains queued, sampling the queue every few ms */
	while (now_us() - start < 2000000ULL)
	{
		Uint32 queued = SDL_GetQueuedAudioSize(device);

		if (now_us() - start > 100000ULL)
		{
			if (queued < smallest)
				smallest = queued;
			if (queued > largest)
				largest = queued;
			samples++;
		}
		if (queued < sizeof(grain))
		{
			tone(grain, AUDIO_SAMPLES * 4, 2);
			if (SDL_QueueAudio(device, grain, sizeof(grain)) != 0 && failures++ < 5)
				say("SDL_QueueAudio: %s\n", SDL_GetError());
			queued_bytes += sizeof(grain);
		}
		SDL_Delay(2);
	}
	say("SDL_QueueAudio: %.3f s of sound queued in 2 s; SDL_GetQueuedAudioSize sampled %u times after the first "
		"0.1 s: min %u bytes, max %u bytes (%u bytes = %.1f ms); %d failures\n",
		queued_bytes / (8.0 * AUDIO_RATE), (unsigned)samples, (unsigned)smallest, (unsigned)largest,
		(unsigned)sizeof(grain), sizeof(grain) / (8.0 * AUDIO_RATE) * 1000.0, failures);
	SDL_PauseAudioDevice(device, 1);
	SDL_CloseAudioDevice(device);
	guard_end();
}

static void step_audio(void)
{
	int count, index;

	heading("4. audio");
	if (!audio_ready)
	{
		say("skipped: SDL_INIT_AUDIO did not start\n");
		return;
	}
	count = SDL_GetNumAudioDrivers();
	say("SDL_GetNumAudioDrivers: %d:", count);
	for (index = 0; index < count; index++)
		say(" %s", SDL_GetAudioDriver(index));
	say("\nSDL_GetCurrentAudioDriver: %s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)");
	say("SDL_GetNumAudioDevices(output): %d\n", SDL_GetNumAudioDevices(0));
	step_audio_callback();
	step_audio_queue();
}

/* ---------- 5. timing */

static void step_timing(void)
{
	uint64_t tsc_start, tsc_end;
	Uint64 ticks_start, ticks_end, counter_start, counter_end, frequency;
	double tsc_seconds, ticks_seconds, counter_seconds;

	heading("5. timing");
	if (!guard_begin("timing"))
		return;
	frequency = SDL_GetPerformanceFrequency();
	tsc_start = now_us();
	ticks_start = SDL_GetTicks64();
	counter_start = SDL_GetPerformanceCounter();
	SDL_Delay(5000);
	tsc_end = now_us();
	ticks_end = SDL_GetTicks64();
	counter_end = SDL_GetPerformanceCounter();
	tsc_seconds = (tsc_end - tsc_start) / 1e6;
	ticks_seconds = (ticks_end - ticks_start) / 1e3;
	counter_seconds = frequency ? (double)(counter_end - counter_start) / (double)frequency : 0.0;
	say("SDL_GetPerformanceFrequency: %llu Hz\n", (unsigned long long)frequency);
	say("over SDL_Delay(5000): the time stamp counter %.6f s, SDL_GetTicks64 %.3f s (%+.3f ms), "
		"SDL_GetPerformanceCounter %.6f s (%+.3f ms)\n",
		tsc_seconds, ticks_seconds, (ticks_seconds - tsc_seconds) * 1e3, counter_seconds,
		(counter_seconds - tsc_seconds) * 1e3);
	guard_end();
}

/* ---------- 6. video, last */

static void step_video(void)
{
	SDL_Window *window;
	uint64_t start;
	int result;

	heading("6. video (last; the host does not want it)");
	if (!guard_begin("video"))
		return;
	start = now_us();
	result = SDL_InitSubSystem(SDL_INIT_VIDEO);
	say("SDL_InitSubSystem(SDL_INIT_VIDEO): %d in %llu us%s%s\n", result, (unsigned long long)(now_us() - start),
		result ? ", " : "", result ? SDL_GetError() : "");
	if (result == 0)
	{
		SDL_DisplayMode mode;
		int displays = SDL_GetNumVideoDisplays();

		say("SDL_GetCurrentVideoDriver: %s\n", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "(none)");
		say("SDL_GetNumVideoDisplays: %d\n", displays);
		if (displays > 0 && SDL_GetCurrentDisplayMode(0, &mode) == 0)
			say("display 0: %dx%d at %d Hz, format 0x%08x\n", mode.w, mode.h, mode.refresh_rate,
				(unsigned)mode.format);
		window = SDL_CreateWindow("Halo PS5 SDL2 probe", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1920, 1080,
			0);
		say("SDL_CreateWindow(1920x1080, 0): %s%s\n", window ? "a window" : "NULL, ", window ? "" : SDL_GetError());
		if (window)
		{
			int width = 0, height = 0;

			SDL_GetWindowSize(window, &width, &height);
			say("its size %dx%d\n", width, height);
			SDL_DestroyWindow(window);
		}
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}
	guard_end();
}

int main(void)
{
	report_open("sdl2-probe.txt");
	notify("Halo PS5 SDL2 probe starts");
	guard_load();
	step_version();
	step_subsystems();
	step_pad();
	step_audio();
	step_timing();
	step_video();
	heading("7. SDL_Quit");
	if (guard_begin("quit"))
	{
		SDL_Quit();
		say("SDL_Quit returned\n");
		guard_end();
	}
	heading("done");
	notify("Halo PS5 SDL2 probe is done: read sdl2-probe.txt over FTP");
	report_close();
	return 0;
}
