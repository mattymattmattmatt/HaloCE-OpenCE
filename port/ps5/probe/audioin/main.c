/*
PS5 AUDIO INPUT PROBE

Phase 7's second probe (port/ps5/PS5.md, "Phase 7", part A, item 2): what
voice chat's microphone will stand on. Voice chat asks for a recording
stream of 48 kHz mono floats in frames of 960 (port/linux/src/voice_audio.c);
pacbrew's SDL2 has no recording on the console (its libSDL2.a imports
nothing of libSceAudioIn), and the SDK has no stub for libSceAudioIn. So:

1. The users: the foreground user, the signed-in ones.
2. The module: sceKernelLoadStartModule of libSceAudioIn.sprx at the
   system's library folders, and sceSysmoduleLoadModuleInternal of its
   number (OpenOrbis's ORBIS_SYSMODULE_INTERNAL_AUDIOIN, 0x80000002) if
   none loads; the functions by sceKernelDlsym.
3. sceAudioInOpen for the foreground user: type voice chat (0), then
   general (1); 48 kHz, then 16 kHz; 256 samples a call, then 128, 512 and
   1024; 16-bit mono. Every result, each port closed after it.
4. Ten seconds of sceAudioInInput on the first port accepted at 48 kHz
   (else 16 kHz), 256 samples: the cadence of its returns (the gaps between
   them, their spread), and each second's peak and how many buffers were
   all zeros, while the person speaks (5 s) and is silent (5 s); and
   sceAudioInGetSilentState each second.
5. The mute button: five seconds while the pad's microphone is muted (its
   button lit) and the person speaks, then five unmuted.
6. A headset in the pad's jack, if the person has one: five seconds while
   they speak into it.
7. A second open: for the same user while the first port is open, and for
   a second signed-in user if there is one.

The calls are declared by hand after shadPS4's
src/core/libraries/audio/audioin.h (OpenOrbis's AudioIn.h names them
without their signatures): sceAudioInOpen(user, type, index, length,
rate, format), sceAudioInInput(handle, buffer), sceAudioInClose(handle),
sceAudioInGetSilentState(handle); the module's loading after OpenOrbis's
libkernel.h and Sysmodule.h.

Build: port/ps5/probe/title/build.sh port/ps5/probe/audioin
Run:   python3 tools/ps5_deploy.py --title PPSA99107 --title-dir
       build/ps5/probe/PPSA99107 upload launch, then fetch; a person
       speaks, is silent, mutes the pad and plugs a headset in as the
       notifications ask.

The report goes to the kernel log and /app0/audioin-probe-<seconds>.txt (over FTP,
/data/homebrew/PPSA99107/audioin-probe-<seconds>.txt).
*/

#include "probe_report.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int32_t sceUserServiceInitialize(const void *params);
int32_t sceUserServiceGetForegroundUser(int32_t *user_id);
struct user_list
{
	int32_t user_id[4];
};
int32_t sceUserServiceGetLoginUserIdList(struct user_list *list);
int32_t sceUserServiceGetUserName(int32_t user_id, char *name, size_t size);

int32_t sceKernelLoadStartModule(const char *path, size_t argument_size, const void *arguments, uint32_t flags,
	const void *options, int32_t *result);
int32_t sceKernelDlsym(int32_t module, const char *name, void **address);
int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
int32_t sceKernelUsleep(uint32_t microseconds);

/* OpenOrbis's ORBIS_SYSMODULE_INTERNAL_AUDIOIN */
#define SYSMODULE_AUDIO_IN 0x80000002u

typedef int32_t (*audio_in_init_t)(void);
typedef int32_t (*audio_in_open_t)(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t rate,
	uint32_t format);
typedef int32_t (*audio_in_input_t)(int32_t handle, void *buffer);
typedef int32_t (*audio_in_close_t)(int32_t handle);
typedef int32_t (*audio_in_silent_state_t)(int32_t handle);

static audio_in_init_t audio_in_init;
static audio_in_open_t audio_in_open;
static audio_in_input_t audio_in_input;
static audio_in_close_t audio_in_close;
static audio_in_silent_state_t audio_in_silent_state;

/* shadPS4's OrbisAudioInType and OrbisAudioInParamFormat */
#define TYPE_VOICE_CHAT 0
#define TYPE_GENERAL 1
#define FORMAT_S16_MONO 0

#define MAX_LENGTH 1024

static int32_t foreground = -1;
static int32_t users[4];
static int user_count;

/* ---------- 1. the users */

static void step_users(void)
{
	struct user_list list;
	int32_t result;
	int index;

	heading("1. the users");
	result = sceUserServiceInitialize(NULL);
	say("sceUserServiceInitialize: 0x%08x\n", (unsigned)result);
	result = sceUserServiceGetForegroundUser(&foreground);
	say("sceUserServiceGetForegroundUser: 0x%08x, user %d\n", (unsigned)result, foreground);
	memset(&list, 0xff, sizeof(list));
	result = sceUserServiceGetLoginUserIdList(&list);
	say("sceUserServiceGetLoginUserIdList: 0x%08x\n", (unsigned)result);
	for (index = 0; index < 4; index++)
	{
		char name[32] = "";

		if (list.user_id[index] == -1)
			continue;
		sceUserServiceGetUserName(list.user_id[index], name, sizeof(name));
		say("  user %d: \"%s\"\n", list.user_id[index], name);
		users[user_count++] = list.user_id[index];
	}
}

/* ---------- 2. the module

Run 1 (audioin-probe-run1.txt): the system paths answered 0x80020002 (not
found), the bare name loaded as handle 0x6d, and sceKernelDlsym found
neither sceAudioInInit nor sceAudioInOpen in it (0x80020003). Run 2
(audioin-probe-run2.txt): sceKernelGetModuleList lists only the title's own
two modules (eboot.bin, libc.prx), not the system's, and
sceSysmoduleLoadModuleInternal(0x80000002) answered 0. So this tries:

a. the module loaded both ways (sceSysmoduleLoadModuleInternal, then
   sceKernelLoadStartModule of the bare name, for its handle);
b. sceKernelDlsym on a system module the title has loaded for certain
   (libScePad.sprx's handle, from sceKernelLoadStartModule, and
   scePadInit), against the linked scePadInit: whether runtime lookups
   work on the console at all;
c. sceKernelDlsym of the audio input's functions in its handle;
d. failing that, the functions imported at link time through the stub
   build-extra.sh makes, the first call guarded: its name goes to
   /app0/audioin-skip.txt before it and is deleted after, so a run that
   crashes in it skips it the next time, and says so. */

int32_t scePadInit(void);
int32_t sceAudioInInit(void);
int32_t sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t rate,
	uint32_t format);
int32_t sceAudioInInput(int32_t handle, void *buffer);
int32_t sceAudioInClose(int32_t handle);
int32_t sceAudioInGetSilentState(int32_t handle);

#define SKIP_FILE "/app0/audioin-skip.txt"

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
		say("the last run did not finish \"%s\": it is skipped this time\n", skipped);
}

static int guard_begin(const char *step)
{
	FILE *file;

	if (!strcmp(step, skipped))
		return 0;
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

static int step_module(void)
{
	int32_t module, pad_module, result, start_result = 0;
	void *address;

	heading("2. the module");
	guard_load();
	result = sceSysmoduleLoadModuleInternal(SYSMODULE_AUDIO_IN);
	say("a. sceSysmoduleLoadModuleInternal(0x%08x): 0x%08x\n", SYSMODULE_AUDIO_IN, (unsigned)result);
	module = sceKernelLoadStartModule("libSceAudioIn.sprx", 0, NULL, 0, NULL, &start_result);
	say("a. sceKernelLoadStartModule(\"libSceAudioIn.sprx\"): 0x%08x, its start 0x%08x\n", (unsigned)module,
		(unsigned)start_result);

	pad_module = sceKernelLoadStartModule("libScePad.sprx", 0, NULL, 0, NULL, &start_result);
	address = NULL;
	result = pad_module >= 0 ? sceKernelDlsym(pad_module, "scePadInit", &address) : -1;
	say("b. sceKernelLoadStartModule(\"libScePad.sprx\"): 0x%08x; sceKernelDlsym(\"scePadInit\"): 0x%08x, %p "
		"(the linked scePadInit is %p)\n",
		(unsigned)pad_module, (unsigned)result, address, (void *)scePadInit);

	if (module >= 0)
	{
		void *open = NULL, *input = NULL, *close = NULL, *init = NULL, *silent = NULL;

		sceKernelDlsym(module, "sceAudioInInit", &init);
		sceKernelDlsym(module, "sceAudioInGetSilentState", &silent);
		say("c. sceKernelDlsym in 0x%x: sceAudioInOpen 0x%08x %p, sceAudioInInput 0x%08x %p, sceAudioInClose 0x%08x %p, "
			"sceAudioInInit %p, sceAudioInGetSilentState %p\n",
			(unsigned)module, (unsigned)sceKernelDlsym(module, "sceAudioInOpen", &open), open,
			(unsigned)sceKernelDlsym(module, "sceAudioInInput", &input), input,
			(unsigned)sceKernelDlsym(module, "sceAudioInClose", &close), close, init, silent);
		if (open && input && close)
		{
			say("c. the functions found by name: used\n");
			audio_in_open = (audio_in_open_t)open;
			audio_in_input = (audio_in_input_t)input;
			audio_in_close = (audio_in_close_t)close;
			audio_in_init = (audio_in_init_t)init;
			audio_in_silent_state = (audio_in_silent_state_t)silent;
			if (audio_in_init)
				say("sceAudioInInit: 0x%08x\n", (unsigned)audio_in_init());
			return 1;
		}
	}

	if (!guard_begin("the linked sceAudioInInit"))
	{
		say("d. skipped: the last run crashed in the linked sceAudioInInit (the import is not bound)\n");
		return 0;
	}
	say("d. the linked imports: sceAudioInInit (if this is the last line, it crashed)\n");
	result = sceAudioInInit();
	guard_end();
	say("d. sceAudioInInit: 0x%08x: the linked imports are used\n", (unsigned)result);
	audio_in_open = sceAudioInOpen;
	audio_in_input = sceAudioInInput;
	audio_in_close = sceAudioInClose;
	audio_in_silent_state = sceAudioInGetSilentState;
	return 1;
}

/* ---------- 3. the opens */

static int32_t open_port(int32_t user, uint32_t type, uint32_t rate, uint32_t length, int keep)
{
	int32_t handle = audio_in_open(user, type, 0, length, rate, FORMAT_S16_MONO);

	say("sceAudioInOpen(user %d, type %u, index 0, %u samples, %u Hz, 16-bit mono): 0x%08x\n", user, type, length,
		rate, (unsigned)handle);
	if (handle >= 0 && !keep)
		say("  sceAudioInClose: 0x%08x\n", (unsigned)audio_in_close(handle));
	return handle;
}

static uint32_t accepted_type = 0xffffffffu, accepted_rate;

static void step_opens(void)
{
	static const uint32_t types[] = { TYPE_VOICE_CHAT, TYPE_GENERAL };
	static const uint32_t rates[] = { 48000, 16000 };
	static const uint32_t lengths[] = { 256, 128, 512, 1024 };
	size_t type, rate, length;

	heading("3. the opens, for the foreground user");
	for (type = 0; type < sizeof(types) / sizeof(types[0]); type++)
	{
		for (rate = 0; rate < sizeof(rates) / sizeof(rates[0]); rate++)
		{
			for (length = 0; length < sizeof(lengths) / sizeof(lengths[0]); length++)
			{
				int32_t handle = open_port(foreground, types[type], rates[rate], lengths[length], 0);

				/* the first accepted, preferring 48 kHz, 256 samples */
				if (handle >= 0 && lengths[length] == 256 &&
					(accepted_type == 0xffffffffu || (accepted_rate != 48000 && rates[rate] == 48000)))
				{
					accepted_type = types[type];
					accepted_rate = rates[rate];
				}
			}
		}
	}
	if (accepted_type == 0xffffffffu)
		say("no open with 256 samples was accepted\n");
	else
		say("captured with: type %u, %u Hz, 256 samples\n", accepted_type, accepted_rate);
}

/* ---------- 4. to 6. the capture */

static int compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* `seconds` seconds of input, a line a second, and the cadence at the end */
static void capture(int32_t handle, uint32_t rate, const char *what, int seconds)
{
	static int16_t buffer[MAX_LENGTH];
	static uint64_t gaps[4096];
	uint64_t start = now_us(), second_start = start, last = 0;
	unsigned calls = 0, second_calls = 0, zero_buffers = 0, errors = 0, gap_count = 0;
	int peak = 0, second = 0;

	(void)rate;
	say("--- %s (at %.2f s)\n", what, start / 1e6);
	while (second < seconds)
	{
		int32_t result = audio_in_input(handle, buffer);
		uint64_t now = now_us();
		int index, any = 0;

		calls++;
		second_calls++;
		if (last && gap_count < sizeof(gaps) / sizeof(gaps[0]))
			gaps[gap_count++] = now - last;
		last = now;
		if (result < 0)
		{
			if (errors++ < 5)
				say("sceAudioInInput: 0x%08x\n", (unsigned)result);
			sceKernelUsleep(5000);
		}
		else
		{
			for (index = 0; index < 256; index++)
			{
				int value = abs(buffer[index]);

				any |= value;
				if (value > peak)
					peak = value;
			}
			if (!any)
				zero_buffers++;
		}
		if (now - second_start >= 1000000ull)
		{
			say("  second %d: %u returns (the last %d samples), peak %d (%.1f%%), %u buffers all zeros, silent "
				"state 0x%08x\n",
				second + 1, second_calls, result, peak, peak * 100.0 / 32768.0, zero_buffers,
				audio_in_silent_state ? (unsigned)audio_in_silent_state(handle) : 0u);
			second_start = now;
			second_calls = 0;
			zero_buffers = 0;
			peak = 0;
			second++;
		}
	}
	if (gap_count > 2)
	{
		qsort(gaps, gap_count, sizeof(gaps[0]), compare_u64);
		say("  %u returns; the gaps between them: min %llu us, median %llu us, 90th %llu us, max %llu us; %u errors\n",
			calls, (unsigned long long)gaps[0], (unsigned long long)gaps[gap_count / 2],
			(unsigned long long)gaps[gap_count * 9 / 10], (unsigned long long)gaps[gap_count - 1], errors);
	}
}

static void ask(const char *text)
{
	notify("Halo PS5 audio input probe: %s", text);
	say("--- asked: %s\n", text);
	sceKernelUsleep(3000000);
}

static void step_capture(void)
{
	int32_t handle, second;
	int index;

	heading("4. the capture, the pad's microphone");
	if (accepted_type == 0xffffffffu)
	{
		say("skipped: no open was accepted\n");
		return;
	}
	handle = open_port(foreground, accepted_type, accepted_rate, 256, 1);
	if (handle < 0)
		return;
	ask("SPEAK into the pad (count out loud) for 5 seconds");
	capture(handle, accepted_rate, "speaking", 5);
	ask("now be SILENT for 5 seconds");
	capture(handle, accepted_rate, "silent", 5);

	heading("5. the mute button");
	ask("press the pad's MUTE button (it lights up), then speak for 5 seconds");
	capture(handle, accepted_rate, "muted, speaking", 5);
	ask("press MUTE again (unlit), speak for 5 seconds");
	capture(handle, accepted_rate, "unmuted, speaking", 5);

	heading("6. a headset in the pad");
	ask("if you have a headset, plug it into the pad and speak into it for 5 seconds (else just speak)");
	capture(handle, accepted_rate, "a headset (if any), speaking", 5);

	heading("7. a second open");
	second = open_port(foreground, accepted_type, accepted_rate, 256, 0);
	say("the same user again while the first port is open: 0x%08x\n", (unsigned)second);
	for (index = 0; index < user_count; index++)
	{
		if (users[index] != foreground)
		{
			second = open_port(users[index], accepted_type, accepted_rate, 256, 0);
			say("a second user (%d) while the first port is open: 0x%08x\n", users[index], (unsigned)second);
		}
	}
	if (user_count < 2)
		say("one user signed in: no second user to try\n");
	say("sceAudioInClose: 0x%08x\n", (unsigned)audio_in_close(handle));
}

int main(void)
{
	char report_name[64];

	/* (named for the start, so a later run never writes over an earlier one's) */
	snprintf(report_name, sizeof(report_name), "audioin-probe-%llu.txt", (unsigned long long)(now_us() / 1000000));
	report_open(report_name);
	notify("Halo PS5 audio input probe starts: the notifications say what to do");
	step_users();
	if (!step_module())
		say("the module or its functions cannot be had: nothing more to try\n");
	else
	{
		step_opens();
		step_capture();
	}
	heading("done");
	notify("Halo PS5 audio input probe is done");
	report_close();
	return 0;
}
