/*
HOST_SDL_PS5_AUDIO.C

The game's sound on the PS5, through pacbrew's SDL2's audio device
(port/ps5/PS5.md, phase 3), with SDL2's headers: host_sdl_ps5.c answers the
game's other SDL3 calls with SDL3's (ps5_host.h says why they are apart).

The game's stream is a callback stream (port/linux/src/dsound_sdl.c: 48 kHz,
float, stereo, a callback that mixes what the device asks for), which SDL2
answers with a device of the same format whose period is the one the game
set through SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES (1024 frames). SDL2 calls
its callback on its own audio thread, which has no guest stack and so
cannot run the game's mixer: as on the Switch (host_sdl2.c) and Android
(host_sdl.c), each callback is handed to a thread of the stream's own that
has one (audio_thread), and waits for it. What the game puts into its
stream meanwhile collects in the binding's buffer, which the callback then
copies into SDL2's.

The proofs of sound where nobody listens, every minute: SDL2's callbacks
counted, the gaps between them (min, median, max; one over twice the period
is late), and the hand-off's cost, from SDL2's callback to the game's mixer
returning (min, median, max).

A stream the game opens without a callback to play is answered as in phase
2: what it puts is counted and played nowhere, at a device's rate. One to
record is voice chat's microphone (phase 7): one recording device is
listed, and its stream reads host_microphone_ps5.c's 48 kHz mono floats.
*/

#include "host.h"
#include "ps5_host.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(AUDIO_F32SYS == PS5_SDL3_AUDIO_F32, "SDL3's SDL_AUDIO_F32 is SDL2's AUDIO_F32SYS");

#define STATISTICS_NS 60000000000ull
/* the gaps and hand-offs of a minute, at 256 frames a callback and more */
#define SAMPLES 16384

/* the period the game asks for (SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES) */
static int audio_period_frames = 1024;

void ps5_audio_set_period(int frames)
{
	if (frames > 0 && frames <= 32768)
		audio_period_frames = frames;
}

/* the cadence and the hand-off of one minute (SDL2's audio thread's alone) */
struct audio_minute
{
	uint64_t start;
	uint32_t gaps[SAMPLES], handoffs[SAMPLES];
	int gap_count, handoff_count;
	unsigned late;
};

struct audio_binding
{
	uint32_t handle;
	uint32_t callback;
	uint32_t userdata;
	SDL_AudioDeviceID device;
	SDL_AudioSpec obtained;
	uint64_t period_ns;
	pthread_mutex_t lock;
	pthread_cond_t requested;
	pthread_cond_t done;
	int pending;
	/* the stream is destroyed: the thread frees the binding and ends */
	int quit;
	int additional;
	unsigned char *buffer;
	int buffer_length;
	int buffer_size;
	uint64_t last_callback;
	struct audio_minute minute;
	/* a stream without a callback: the bytes put, and its clock */
	uint64_t put_bytes, resumed_ns;
	int resumed;
	/* voice chat's microphone (host_microphone_ps5.c) */
	int microphone;
};

/* every stream's, for the end's report */
static struct
{
	uint64_t callbacks, late, bytes;
	uint64_t gap_min_ns, gap_max_ns, handoff_max_ns;
	uint64_t pushed_bytes;
} totals = { .gap_min_ns = UINT64_MAX };

static int compare_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return x < y ? -1 : x > y;
}

/* min, median and max of values, in place */
static void spread(uint32_t *values, int count, uint32_t *low, uint32_t *median, uint32_t *high)
{
	*low = *median = *high = 0;
	if (count < 1)
		return;
	qsort(values, (size_t)count, sizeof(values[0]), compare_u32);
	*low = values[0];
	*median = values[count / 2];
	*high = values[count - 1];
}

static void minute_line(struct audio_binding *binding, uint64_t now)
{
	struct audio_minute *minute = &binding->minute;
	uint32_t gap_low, gap_median, gap_high, low, median, high;
	double seconds = (now - minute->start) / 1e9;

	spread(minute->gaps, minute->gap_count, &gap_low, &gap_median, &gap_high);
	spread(minute->handoffs, minute->handoff_count, &low, &median, &high);
	host_logf(HOST_LOG_INFO, "audio: %d callbacks in %.1f s (%.1f expected), gaps min %u us, median %u us, max %u us, "
		"%u late; the hand-off min %u us, median %u us, max %u us", minute->handoff_count, seconds,
		seconds * binding->obtained.freq / binding->obtained.samples, gap_low, gap_median, gap_high, minute->late, low,
		median, high);
	minute->start = now;
	minute->gap_count = minute->handoff_count = 0;
	minute->late = 0;
}

/* the stream's thread, which has a guest stack: the game's callback, once
for each of SDL2's */
static void *audio_thread(void *context)
{
	struct audio_binding *binding = context;

	pthread_mutex_lock(&binding->lock);
	for (;;)
	{
		int additional;

		while (!binding->pending && !binding->quit)
			pthread_cond_wait(&binding->requested, &binding->lock);
		if (binding->quit)
			break;
		additional = binding->additional;
		pthread_mutex_unlock(&binding->lock);
		host_call_guest(binding->callback, binding->userdata, binding->handle, (uint32_t)additional,
			(uint32_t)additional);
		pthread_mutex_lock(&binding->lock);
		binding->pending = 0;
		pthread_cond_signal(&binding->done);
	}
	pthread_mutex_unlock(&binding->lock);
	pthread_cond_destroy(&binding->done);
	pthread_cond_destroy(&binding->requested);
	pthread_mutex_destroy(&binding->lock);
	free(binding->buffer);
	free(binding);
	return NULL;
}

/* appends to the binding's buffer; the caller holds its lock */
static int audio_keep(struct audio_binding *binding, const void *data, int length)
{
	if (length < 0)
		return 0;
	if (binding->buffer_length + length > binding->buffer_size)
	{
		int size = (binding->buffer_length + length) * 2;
		unsigned char *buffer = realloc(binding->buffer, (size_t)size);

		if (!buffer)
			return 0;
		binding->buffer = buffer;
		binding->buffer_size = size;
	}
	memcpy(binding->buffer + binding->buffer_length, data, (size_t)length);
	binding->buffer_length += length;
	return 1;
}

/* SDL2's audio thread */
static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
	struct audio_binding *binding = userdata;
	struct audio_minute *minute = &binding->minute;
	uint64_t start = ps5_now_ns(), took;
	int copied;

	if (binding->last_callback)
	{
		uint64_t gap = start - binding->last_callback;

		if (minute->gap_count < SAMPLES)
			minute->gaps[minute->gap_count++] = (uint32_t)(gap / 1000);
		if (gap > 2 * binding->period_ns)
		{
			minute->late++;
			totals.late++;
		}
		if (gap < totals.gap_min_ns)
			totals.gap_min_ns = gap;
		if (gap > totals.gap_max_ns)
			totals.gap_max_ns = gap;
	}
	else
	{
		minute->start = start;
		host_logf(HOST_LOG_INFO, "audio: the first callback at %.3f s, for %d bytes", ps5_seconds(), length);
	}
	binding->last_callback = start;

	pthread_mutex_lock(&binding->lock);
	if (binding->buffer_length < length)
	{
		binding->additional = length - binding->buffer_length;
		binding->pending = 1;
		pthread_cond_signal(&binding->requested);
		while (binding->pending)
			pthread_cond_wait(&binding->done, &binding->lock);
	}
	took = ps5_now_ns() - start;
	copied = binding->buffer_length < length ? binding->buffer_length : length;
	memcpy(stream, binding->buffer, (size_t)copied);
	if (copied < length)
		memset(stream + copied, 0, (size_t)(length - copied));
	binding->buffer_length -= copied;
	if (binding->buffer_length)
		memmove(binding->buffer, binding->buffer + copied, (size_t)binding->buffer_length);
	pthread_mutex_unlock(&binding->lock);

	if (minute->handoff_count < SAMPLES)
		minute->handoffs[minute->handoff_count++] = (uint32_t)(took / 1000);
	if (took > totals.handoff_max_ns)
		totals.handoff_max_ns = took;
	totals.callbacks++;
	totals.bytes += (uint64_t)length;
	if (start - minute->start >= STATISTICS_NS)
		minute_line(binding, start);
}

/* ---------- the game's calls */

/* the recording device: the signed-in player's microphone, the pad's or a
headset in it (one id of its own, above SDL2's playback devices) */
#define MICROPHONE_DEVICE 0x100u
#define MICROPHONE_NAME "DualSense microphone"

/* the playback devices are SDL2's, numbered from 1 (0 is no device); the
recording one is the microphone */
int host_sdl_get_audio_devices(int recording, uint32_t *ids, int maximum)
{
	int count, index;

	if (recording)
	{
		if (maximum < 1)
			return 0;
		ids[0] = MICROPHONE_DEVICE;
		return 1;
	}
	count = SDL_GetNumAudioDevices(0);
	for (index = 0; index < count && index < maximum; index++)
		ids[index] = (uint32_t)index + 1;
	return index;
}

int host_sdl_get_audio_device_name(uint32_t device, char *name, int size)
{
	const char *found;

	if (size >= 1 && device == MICROPHONE_DEVICE)
	{
		SDL_strlcpy(name, MICROPHONE_NAME, (size_t)size);
		return 1;
	}
	if (size < 1 || device < 1 || (int)device > SDL_GetNumAudioDevices(0))
		return 0;
	found = SDL_GetAudioDeviceName((int)device - 1, 0);
	if (!found)
		return 0;
	SDL_strlcpy(name, found, (size_t)size);
	return 1;
}

static const char *format_name(SDL_AudioFormat format)
{
	return format == AUDIO_F32SYS ? "float" : format == AUDIO_S16SYS ? "16-bit" : "another format";
}

uint32_t host_sdl_open_audio_stream(uint32_t device, const void *spec, uint32_t callback, uint32_t userdata)
{
	const struct ps5_sdl3_audio_spec *guest_spec = spec;
	const char *name = NULL;
	struct audio_binding *binding;
	SDL_AudioSpec wanted;

	/* voice chat's microphone, by the default's id or its own: 48 kHz mono
	floats, as voice_audio.c asks */
	if (device == PS5_SDL3_AUDIO_DEVICE_DEFAULT_RECORDING || device == MICROPHONE_DEVICE)
	{
		if (guest_spec->format != PS5_SDL3_AUDIO_F32 || guest_spec->channels != 1 || guest_spec->freq != 48000)
		{
			host_logf(HOST_LOG_ERROR, "microphone: asked for format 0x%x, %d channel(s) at %d Hz; it gives 48000 Hz "
				"mono floats", (unsigned)guest_spec->format, (int)guest_spec->channels, (int)guest_spec->freq);
			return 0;
		}
		binding = calloc(1, sizeof(*binding));
		if (!binding)
			return 0;
		binding->microphone = 1;
		if (!ps5_microphone_open())
		{
			free(binding);
			return 0;
		}
		binding->handle = host_sdl_handle_new(_handle_audio, binding);
		if (!binding->handle)
		{
			ps5_microphone_close();
			free(binding);
			return 0;
		}
		return binding->handle;
	}
	binding = calloc(1, sizeof(*binding));
	if (!binding)
		return 0;
	binding->callback = callback;
	binding->userdata = userdata;
	if (!callback)
	{
		/* (phase 2's answers: counted, played nowhere) */
		binding->obtained.freq = guest_spec->freq;
		binding->obtained.channels = (Uint8)guest_spec->channels;
		binding->obtained.format = (SDL_AudioFormat)guest_spec->format;
		binding->handle = host_sdl_handle_new(_handle_audio, binding);
		if (!binding->handle)
		{
			free(binding);
			return 0;
		}
		host_logf(HOST_LOG_WARN, "audio: stream %u without a callback (format 0x%x, %d channels, %d Hz): counted, "
			"not played", binding->handle, (unsigned)guest_spec->format, (int)guest_spec->channels, (int)guest_spec->freq);
		return binding->handle;
	}
	pthread_mutex_init(&binding->lock, NULL);
	pthread_cond_init(&binding->requested, NULL);
	pthread_cond_init(&binding->done, NULL);
	SDL_zero(wanted);
	/* SDL3's audio format values are SDL2's */
	wanted.format = (SDL_AudioFormat)guest_spec->format;
	wanted.channels = (Uint8)guest_spec->channels;
	wanted.freq = guest_spec->freq;
	wanted.samples = (Uint16)audio_period_frames;
	wanted.callback = audio_callback;
	wanted.userdata = binding;
	if (device != PS5_SDL3_AUDIO_DEVICE_DEFAULT_PLAYBACK && device >= 1 && (int)device <= SDL_GetNumAudioDevices(0))
		name = SDL_GetAudioDeviceName((int)device - 1, 0);
	/* (SDL2 converts what it is given to what the device takes) */
	binding->device = SDL_OpenAudioDevice(name, 0, &wanted, &binding->obtained, 0);
	if (!binding->device)
	{
		host_logf(HOST_LOG_ERROR, "audio: SDL_OpenAudioDevice(%s, %d Hz, format 0x%x, %d channels, %d frames): %s; the "
			"game is silent", name ? name : "the default", wanted.freq, (unsigned)wanted.format, wanted.channels,
			wanted.samples, SDL_GetError());
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		free(binding);
		return 0;
	}
	binding->period_ns = (uint64_t)binding->obtained.samples * 1000000000ull /
		(uint64_t)(binding->obtained.freq > 0 ? binding->obtained.freq : 48000);
	/* the device starts paused, so no callback can run before this */
	binding->handle = host_sdl_handle_new(_handle_audio, binding);
	if (!binding->handle)
	{
		SDL_CloseAudioDevice(binding->device);
		pthread_cond_destroy(&binding->done);
		pthread_cond_destroy(&binding->requested);
		pthread_mutex_destroy(&binding->lock);
		free(binding);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "audio: stream %u on SDL2's device %u (%s): asked %d Hz, %s (0x%x), %d channels, %d "
		"frames; obtained %d Hz, %s (0x%x), %d channels, %d frames (%.2f ms), %u bytes a callback, silence %d",
		binding->handle, (unsigned)binding->device, name ? name : "the default", wanted.freq, format_name(wanted.format),
		(unsigned)wanted.format, wanted.channels, wanted.samples, binding->obtained.freq,
		format_name(binding->obtained.format), (unsigned)binding->obtained.format, binding->obtained.channels,
		binding->obtained.samples, binding->period_ns / 1e6, (unsigned)binding->obtained.size, binding->obtained.silence);
	if (host_native_thread_create(audio_thread, binding, 256 * 1024) != 0)
		host_fatal("cannot start the audio thread");
	return binding->handle;
}

/* (from the game's callback, on the stream's thread, while SDL2's callback
waits for it without holding the lock) */
int host_sdl_put_audio_stream_data(uint32_t handle, const void *data, int length)
{
	struct audio_binding *binding = host_sdl_handle_get(handle, _handle_audio);
	int result;

	if (!binding || length < 0)
		return 0;
	if (!binding->callback)
	{
		binding->put_bytes += (uint64_t)length;
		__atomic_add_fetch(&totals.pushed_bytes, (uint64_t)length, __ATOMIC_RELAXED);
		return 1;
	}
	pthread_mutex_lock(&binding->lock);
	result = audio_keep(binding, data, length);
	pthread_mutex_unlock(&binding->lock);
	return result;
}

int host_sdl_resume_audio_stream_device(uint32_t handle)
{
	struct audio_binding *binding = host_sdl_handle_get(handle, _handle_audio);

	if (!binding)
		return 0;
	/* (the microphone captures from its opening) */
	if (binding->microphone)
		return 1;
	if (!binding->callback)
	{
		if (!binding->resumed)
			binding->resumed_ns = ps5_now_ns();
		binding->resumed = 1;
		return 1;
	}
	SDL_PauseAudioDevice(binding->device, 0);
	return 1;
}

/* voice chat's microphone, the only stream the game reads */
int host_sdl_get_audio_stream_data(uint32_t handle, void *data, int length)
{
	struct audio_binding *binding = host_sdl_handle_get(handle, _handle_audio);

	if (!binding || !binding->microphone)
		return 0;
	return ps5_microphone_read(data, length);
}

/* (a stream without a callback: the bytes put less what a device at its
rate would have played since it was resumed) */
int host_sdl_get_audio_stream_available(uint32_t handle)
{
	struct audio_binding *binding = host_sdl_handle_get(handle, _handle_audio);
	uint64_t played = 0;
	uint32_t frame;

	if (!binding)
		return -1;
	if (binding->microphone)
		return ps5_microphone_available();
	if (binding->callback)
		return 0;
	frame = (uint32_t)SDL_AUDIO_BITSIZE(binding->obtained.format) / 8 *
		(uint32_t)(binding->obtained.channels ? binding->obtained.channels : 1);
	if (binding->resumed && binding->obtained.freq > 0)
		played = (ps5_now_ns() - binding->resumed_ns) * (uint64_t)binding->obtained.freq / 1000000000ull * frame;
	return binding->put_bytes > played ? (int)(binding->put_bytes - played) : 0;
}

/* pause (a callback that is running finishes first), close the device, and
tell the stream's thread to end; it frees the binding */
void host_sdl_destroy_audio_stream(uint32_t handle)
{
	struct audio_binding *binding = host_sdl_handle_get(handle, _handle_audio);

	if (!binding)
		return;
	host_sdl_handle_free(handle);
	if (binding->microphone)
		ps5_microphone_close();
	if (!binding->callback)
	{
		free(binding);
		return;
	}
	SDL_PauseAudioDevice(binding->device, 1);
	SDL_CloseAudioDevice(binding->device);
	host_logf(HOST_LOG_INFO, "audio: stream %u closed", handle);
	pthread_mutex_lock(&binding->lock);
	binding->quit = 1;
	pthread_cond_signal(&binding->requested);
	pthread_mutex_unlock(&binding->lock);
}

void ps5_audio_report(void)
{
	host_logf(HOST_LOG_INFO, "audio: %llu callbacks of SDL2's for %.1f MB of the game's sound, %llu late; gaps min %.2f ms, "
		"max %.2f ms; the hand-off's max %.2f ms; %.1f MB put into streams without a callback",
		(unsigned long long)totals.callbacks, totals.bytes / (1024.0 * 1024.0), (unsigned long long)totals.late,
		totals.callbacks > 1 ? totals.gap_min_ns / 1e6 : 0.0, totals.gap_max_ns / 1e6, totals.handoff_max_ns / 1e6,
		totals.pushed_bytes / (1024.0 * 1024.0));
}
