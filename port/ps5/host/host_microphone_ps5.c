/*
HOST_MICROPHONE_PS5.C

Voice chat's microphone on the PS5 (port/ps5/PS5.md, phase 7, step 4):
port/linux/src/voice_audio.c opens a recording stream of 48 kHz mono floats
and reads it without a callback; host_sdl_ps5_audio.c sends that stream
here. pacbrew's SDL2 cannot record on the console, so it is libSceAudioIn's
input, the signed-in player's: the DualSense's own microphone, or a headset
in its jack. The Switch's host_microphone.c is the model.

What probe 2 found shapes this (port/ps5/probe/results/audioin-probe-run3.txt):

- A title's sceKernelDlsym finds nothing in a system module, so the calls
  are imported through a stub of the port's own (port/ps5/stubs,
  tools/ps5_build.py), and the module is loaded first with
  sceSysmoduleLoadModuleInternal (ps5_microphone_start, at the host's
  start); the imports are then bound.
- sceAudioInOpen takes the voice chat type at 48 kHz, 16-bit mono, 256
  samples a call: voice chat's rate, so nothing is resampled.
  sceAudioInInput blocks until the next 256 samples, 5.33 ms apart, on a
  thread of this file's.
- While the pad's mute button is lit every buffer is zeros, and a headset
  plugged in gives a second or two of zeros as the input changes over;
  silence is never zeros. Buffers of zeros are dropped
  (host_rules_ps5.c's ps5_microphone_samples), so voice chat then hears
  nothing, as with no microphone. The others go into a second's ring the
  guest reads, the oldest dropped when it is full (a voice must not play
  late).

The calls are declared by hand after shadPS4's
src/core/libraries/audio/audioin.h (OpenOrbis's AudioIn.h names them
without their signatures), the module's loading after OpenOrbis's
Sysmodule.h.
*/

#include "host.h"
#include "ps5_host.h"
#include "host_rules_ps5.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* ---------- the console's calls */

int32_t sceSysmoduleLoadModuleInternal(uint32_t id);
int32_t sceUserServiceGetForegroundUser(int32_t *user_id);
int32_t sceAudioInInit(void);
int32_t sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t length, uint32_t rate, uint32_t format);
int32_t sceAudioInInput(int32_t handle, void *buffer);
int32_t sceAudioInClose(int32_t handle);

/* OpenOrbis's ORBIS_SYSMODULE_INTERNAL_AUDIOIN */
#define SYSMODULE_AUDIO_IN 0x80000002u
/* shadPS4's OrbisAudioInType::VoiceChat and OrbisAudioInParamFormat::S16Mono */
#define AUDIO_IN_VOICE_CHAT 0
#define AUDIO_IN_S16_MONO 0

#define MICROPHONE_RATE 48000
#define MICROPHONE_SAMPLES 256
/* a second of mono samples kept for the guest */
#define MICROPHONE_RING_SAMPLES MICROPHONE_RATE
#define MINUTE_NS 60000000000ull

static struct
{
	/* the module loaded and its calls bound (ps5_microphone_start) */
	int ready;
	int open;
	volatile int stopping;
	int32_t port;
	pthread_t thread;
	pthread_mutex_t lock;
	/* the mono samples (under lock) */
	float ring[MICROPHONE_RING_SAMPLES];
	int read, count;
	/* whether the input last gave sound, for saying when it changes */
	int hearing;
	/* this minute's figures: buffers read, those of zeros, the input's errors */
	uint64_t minute_start;
	unsigned buffers, zero_buffers, errors, dropped_samples;
} microphone = { .lock = PTHREAD_MUTEX_INITIALIZER, .port = -1 };

void ps5_microphone_start(void)
{
	int32_t loaded = sceSysmoduleLoadModuleInternal(SYSMODULE_AUDIO_IN);
	int32_t initialized = loaded == 0 ? sceAudioInInit() : -1;

	microphone.ready = loaded == 0 && initialized == 0;
	host_logf(microphone.ready ? HOST_LOG_INFO : HOST_LOG_WARN,
		"microphone: libSceAudioIn: sceSysmoduleLoadModuleInternal(0x%08x) 0x%08x, sceAudioInInit 0x%08x%s",
		SYSMODULE_AUDIO_IN, (unsigned)loaded, (unsigned)initialized,
		microphone.ready ? "" : ": no microphone, voice chat only listens");
}

static void minute_line(uint64_t now)
{
	double seconds = (now - microphone.minute_start) / 1e9;

	host_logf(HOST_LOG_INFO, "audio: the microphone: %.0f buffers a second in %.0f s, %.1f s of them zeros (muted, or a "
		"headset changing over), %u samples dropped (the ring full), %u errors", microphone.buffers / seconds, seconds,
		microphone.zero_buffers * (double)MICROPHONE_SAMPLES / MICROPHONE_RATE, microphone.dropped_samples,
		microphone.errors);
	microphone.minute_start = now;
	microphone.buffers = microphone.zero_buffers = microphone.errors = microphone.dropped_samples = 0;
}

static void keep(const short *samples)
{
	float values[MICROPHONE_SAMPLES];
	int count = ps5_microphone_samples(samples, MICROPHONE_SAMPLES, values), index;

	if ((count > 0) != microphone.hearing)
	{
		microphone.hearing = count > 0;
		host_logf(HOST_LOG_INFO, microphone.hearing ? "microphone: hearing the input" :
			"microphone: the input gives zeros (the pad's mute button, or a headset changing over)");
	}
	if (!count)
	{
		microphone.zero_buffers++;
		return;
	}
	pthread_mutex_lock(&microphone.lock);
	for (index = 0; index < count; index++)
	{
		if (microphone.count == MICROPHONE_RING_SAMPLES)
		{
			microphone.read = (microphone.read + 1) % MICROPHONE_RING_SAMPLES;
			microphone.count--;
			microphone.dropped_samples++;
		}
		microphone.ring[(microphone.read + microphone.count) % MICROPHONE_RING_SAMPLES] = values[index];
		microphone.count++;
	}
	pthread_mutex_unlock(&microphone.lock);
}

static void *capture_thread(void *unused)
{
	short samples[MICROPHONE_SAMPLES];

	(void)unused;
	microphone.minute_start = ps5_now_ns();
	while (!microphone.stopping)
	{
		int32_t result = sceAudioInInput(microphone.port, samples);
		uint64_t now = ps5_now_ns();

		if (result < 0)
		{
			if (!microphone.errors++)
				host_logf(HOST_LOG_WARN, "microphone: sceAudioInInput 0x%08x", (unsigned)result);
			/* (not a busy loop on an input gone) */
			usleep(5000);
		}
		else
		{
			microphone.buffers++;
			keep(samples);
		}
		if (now - microphone.minute_start >= MINUTE_NS)
			minute_line(now);
	}
	return NULL;
}

int ps5_microphone_open(void)
{
	int32_t user = -1, result;

	if (microphone.open)
	{
		host_logf(HOST_LOG_ERROR, "microphone: already open");
		return 0;
	}
	if (!microphone.ready)
	{
		host_logf(HOST_LOG_WARN, "microphone: libSceAudioIn is not loaded: voice chat only listens");
		return 0;
	}
	result = sceUserServiceGetForegroundUser(&user);
	if (result != 0)
	{
		host_logf(HOST_LOG_ERROR, "microphone: sceUserServiceGetForegroundUser 0x%08x", (unsigned)result);
		return 0;
	}
	microphone.port = sceAudioInOpen(user, AUDIO_IN_VOICE_CHAT, 0, MICROPHONE_SAMPLES, MICROPHONE_RATE,
		AUDIO_IN_S16_MONO);
	if (microphone.port < 0)
	{
		host_logf(HOST_LOG_ERROR, "microphone: sceAudioInOpen(user %d, voice chat, %d samples, %d Hz, 16-bit mono) "
			"0x%08x: voice chat only listens", user, MICROPHONE_SAMPLES, MICROPHONE_RATE, (unsigned)microphone.port);
		return 0;
	}
	pthread_mutex_lock(&microphone.lock);
	microphone.read = microphone.count = 0;
	pthread_mutex_unlock(&microphone.lock);
	microphone.hearing = -1;
	microphone.stopping = 0;
	microphone.buffers = microphone.zero_buffers = microphone.errors = microphone.dropped_samples = 0;
	if (pthread_create(&microphone.thread, NULL, capture_thread, NULL) != 0)
	{
		host_logf(HOST_LOG_ERROR, "microphone: its thread could not be made");
		sceAudioInClose(microphone.port);
		microphone.port = -1;
		return 0;
	}
	microphone.open = 1;
	host_logf(HOST_LOG_INFO, "microphone: open, user %d's (the pad's microphone or a headset in it), port 0x%08x: "
		"%d samples of 16-bit mono at %d Hz a read", user, (unsigned)microphone.port, MICROPHONE_SAMPLES,
		MICROPHONE_RATE);
	return 1;
}

/* the bytes of mono floats there are to read */
int ps5_microphone_available(void)
{
	int bytes;

	pthread_mutex_lock(&microphone.lock);
	bytes = microphone.count * (int)sizeof(float);
	pthread_mutex_unlock(&microphone.lock);
	return bytes;
}

/* up to length bytes of them (whole samples), into data; how many */
int ps5_microphone_read(void *data, int length)
{
	float *out = data;
	int wanted = length / (int)sizeof(float), taken;

	if (wanted <= 0)
		return 0;
	pthread_mutex_lock(&microphone.lock);
	for (taken = 0; taken < wanted && microphone.count; taken++)
	{
		out[taken] = microphone.ring[microphone.read];
		microphone.read = (microphone.read + 1) % MICROPHONE_RING_SAMPLES;
		microphone.count--;
	}
	pthread_mutex_unlock(&microphone.lock);
	return taken * (int)sizeof(float);
}

void ps5_microphone_close(void)
{
	if (!microphone.open)
		return;
	microphone.stopping = 1;
	pthread_join(microphone.thread, NULL);
	minute_line(ps5_now_ns());
	sceAudioInClose(microphone.port);
	microphone.port = -1;
	microphone.open = 0;
	host_logf(HOST_LOG_INFO, "microphone: closed");
}
