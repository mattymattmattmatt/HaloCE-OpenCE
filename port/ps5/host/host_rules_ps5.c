/*
HOST_RULES_PS5.C

The PS5 host's rules that need no console (host_rules_ps5.h). Nothing here
calls the system, so port/ps5/tests/host_rules_ps5_test.c runs it on the PC.

The maximum resolution (port/ps5/PS5.md, phase 7, step 2) is the Switch's
display.resolution_limit with the PS5's values: the most lines the game
draws at, no more than the display shows. The display is VideoOut's mode,
which the console's own output setting decides (Screen and Video); the
picture is drawn in its shape and the present scales it to the mode.

The gyroscope (step 3) is the Switch's gyro_read with the DualSense's
axes, as probe 1 found them: radians a second, the pad's x to its right, y
out of its face, z toward the player; looking up turns it about x the
positive way, steering it left (as a wheel) about z the positive way. The
view turns sideways as the pad is steered (the user's choice in play, over
turning it flat about y), and the guest's yaw is positive to the left
(input_abstraction.c: the stick's yaw is -x), so the yaw is +z; the pitch,
positive looking up, is +x. The samples' own timestamps (microseconds)
give the time each turned for.

The microphone (step 4) gives 48 kHz 16-bit mono, voice chat's rate, so its
samples are only turned to floats; a buffer all zeros (muted, or a
headset changing over) is dropped, as the Switch's host_microphone.c
drops them, so voice chat hears nothing rather than a silence it would
send.

The updater (phase 8) reads GitHub's answer for the latest release as
port/linux/src/updater.c does (the "tag_name"), and the release's PS5 zip
as port/switch/host/host_update.c reads the Switch's: stored entries, each
with its sizes and CRC in its local header, taken one after another. Only
the entries under the title's folder go to the console, and only names
that stay inside it.
*/

#include "host_rules_ps5.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* (8K's lines: more than any display the console drives) */
#define LINES_HIGHEST 4320

int ps5_limit_lines(const char *text)
{
	char *end;
	long lines;

	if (!text || !*text)
		return 0;
	lines = strtol(text, &end, 10);
	if (*end || lines < PS5_LIMIT_LINES_LOWEST || lines > LINES_HIGHEST)
		return 0;
	return (int)lines;
}

int ps5_limit_rule(int lines, int display_width, int display_height, int *width, int *height)
{
	int display_decides = display_height > 0 && display_height <= lines;

	if (display_width <= 0 || display_height <= 0)
	{
		/* (no display mode known: the limit, 16:9) */
		display_width = 16;
		display_height = 9;
		display_decides = 0;
	}
	*height = display_decides ? display_height : lines;
	*width = display_decides ? display_width :
		(int)(((long long)lines * display_width + display_height / 2) / display_height) & ~1;
	return display_decides;
}

void ps5_gyro_turn(struct ps5_gyro_state *state, unsigned long long now_us, const struct ps5_gyro_sample *samples,
	int count, float *yaw, float *pitch)
{
	unsigned long long previous = state->last_timestamp_us;
	int index, fresh = !state->had_sample || now_us - state->last_read_us > PS5_GYRO_LONGEST_GAP_US;

	*yaw = 0.0f;
	*pitch = 0.0f;
	state->last_read_us = now_us;
	if (count <= 0)
		return;
	if (fresh)
	{
		for (index = 0; index < count; index++)
		{
			if (samples[index].timestamp_us > previous || !state->had_sample)
				previous = samples[index].timestamp_us;
		}
		state->last_timestamp_us = previous;
		state->had_sample = 1;
		return;
	}
	for (index = 0; index < count; index++)
	{
		const struct ps5_gyro_sample *sample = &samples[index];
		unsigned long long gap;
		float x = sample->x, z = sample->z, seconds;

		if (sample->timestamp_us <= previous)
			continue;
		gap = sample->timestamp_us - previous;
		previous = sample->timestamp_us;
		if (gap > PS5_GYRO_LONGEST_GAP_US)
			continue;
		if (x < PS5_GYRO_NOISE && x > -PS5_GYRO_NOISE)
			x = 0.0f;
		if (z < PS5_GYRO_NOISE && z > -PS5_GYRO_NOISE)
			z = 0.0f;
		seconds = (float)gap * 1.0e-6f;
		*yaw += z * seconds;
		*pitch += x * seconds;
	}
	state->last_timestamp_us = previous;
}

int ps5_microphone_samples(const short *samples, int count, float *out)
{
	int index, any = 0;

	for (index = 0; index < count && !any; index++)
		any = samples[index] != 0;
	if (!any)
		return 0;
	for (index = 0; index < count; index++)
		out[index] = samples[index] / 32768.0f;
	return count;
}

/* ---------- the updater */

long ps5_update_release_build(const char *json)
{
	const char *tag = json ? strstr(json, "\"tag_name\"") : NULL;
	char *end;
	long build;

	if (!tag)
		return 0;
	tag = strchr(tag + strlen("\"tag_name\""), '"');
	if (!tag || strncmp(tag, "\"build-", 7))
		return 0;
	build = strtol(tag + 7, &end, 10);
	return *end == '"' && build > 0 ? build : 0;
}

static unsigned long little(const unsigned char *bytes, int count)
{
	unsigned long value = 0;
	int index;

	for (index = count - 1; index >= 0; index--)
		value = (value << 8) | bytes[index];
	return value;
}

int ps5_zip_local_header(const unsigned char *header, struct ps5_zip_entry *entry)
{
	unsigned long signature = little(header, 4);

	if (signature == 0x02014b50UL || signature == 0x06054b50UL)
		return 0;
	if (signature != 0x04034b50UL)
		return -1;
	entry->flags = (unsigned)little(header + 6, 2);
	entry->method = (unsigned)little(header + 8, 2);
	entry->crc = little(header + 14, 4);
	entry->compressed = little(header + 18, 4);
	entry->size = little(header + 22, 4);
	entry->name_length = (unsigned)little(header + 26, 2);
	entry->extra_length = (unsigned)little(header + 28, 2);
	return 1;
}

int ps5_zip_entry_stored(const struct ps5_zip_entry *entry)
{
	/* (bit 0 encrypted, bit 3 the sizes after the data) */
	return entry->method == 0 && !(entry->flags & 9) && entry->compressed == entry->size;
}

unsigned long ps5_crc32(unsigned long crc, const void *data, unsigned long size)
{
	static unsigned long table[256];
	const unsigned char *bytes = data;
	unsigned long index;

	if (!table[1])
	{
		unsigned long entry, bit;

		for (entry = 0; entry < 256; entry++)
		{
			unsigned long value = entry;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xedb88320UL ^ (value >> 1) : value >> 1;
			table[entry] = value;
		}
	}
	crc = ~crc & 0xffffffffUL;
	for (index = 0; index < size; index++)
		crc = table[(crc ^ bytes[index]) & 0xff] ^ (crc >> 8);
	return ~crc & 0xffffffffUL;
}

int ps5_update_entry_path(const char *name, const char *folder, char *out, unsigned long size)
{
	size_t folder_length = strlen(folder);
	const char *rest, *part;

	if (strncmp(name, folder, folder_length))
		return 0;
	rest = name + folder_length;
	if (strlen(rest) >= size || strchr(rest, '\\'))
		return -1;
	/* each part between slashes a name: not empty (but for a folder's
	final slash), not "." or ".." */
	for (part = rest; *part;)
	{
		const char *slash = strchr(part, '/');
		size_t length = slash ? (size_t)(slash - part) : strlen(part);

		if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.'))
			return -1;
		if (!slash)
			break;
		part = slash + 1;
	}
	memcpy(out, rest, strlen(rest) + 1);
	return 1;
}

char *ps5_update_strcasestr(const char *text, const char *wanted)
{
	size_t length = strlen(wanted);

	for (; *text; text++)
	{
		if (!strncasecmp(text, wanted, length))
			return (char *)text;
	}
	return length ? NULL : (char *)text;
}
