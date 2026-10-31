/*
HOST_RULES_PS5.H

The PS5 host's rules that need no console (port/ps5/PS5.md, phase 7): the
maximum resolution against the display's, the gyroscope's turn from the
DualSense's samples, the microphone's samples, and the updater's reading of
a release (phase 8), compiled into the title and,
with port/ps5/tests/host_rules_ps5_test.c, on the PC
(tools/test_ps5_host.py).
*/

#ifndef HOST_RULES_PS5_H
#define HOST_RULES_PS5_H

/* the fewest and most lines display.resolution_limit may ask for */
#define PS5_LIMIT_LINES_LOWEST 480
#define PS5_LIMIT_LINES_DEFAULT 2160

/* display.resolution_limit's text: its lines, or 0 if it is not a whole
number from PS5_LIMIT_LINES_LOWEST to 4320 */
int ps5_limit_lines(const char *text);

/* the size the game draws at for a limit of `lines` on a display of
display_width x display_height: the smaller of the limit and the display's
height, in the display's shape (an even width). 1 if the display's height
is the smaller (the display decides), else 0. */
int ps5_limit_rule(int lines, int display_width, int display_height, int *width, int *height);

/* the gyroscope (port/ps5/probe/results/pad-probe-run2.txt, -run3.txt): an
angular velocity under this, radians a second, is the sensor's noise at rest
(at most 0.02 there), and is no turn */
#define PS5_GYRO_NOISE 0.02f
/* samples further apart than this (microseconds), or a read this long after
the last, are not a turn: the sensor was not being read (the Switch's
limit) */
#define PS5_GYRO_LONGEST_GAP_US 100000ull

/* one sample of the DualSense's: its timestamp (microseconds) and its
angular velocity about the pad's x (right) and z (toward the player),
radians a second */
struct ps5_gyro_sample
{
	unsigned long long timestamp_us;
	float x, z;
};

/* a pad's place in its samples */
struct ps5_gyro_state
{
	unsigned long long last_timestamp_us;
	unsigned long long last_read_us;
	int had_sample;
};

/* the turn in `count` samples (oldest first; the host gives the pad's
state, one a call), read at now_us, in the sensor's radians: *yaw the
steering's, +z (the guest's sign: positive to the left, as steering left
turns the pad), *pitch +x's, positive looking up. A first read, or one PS5_GYRO_LONGEST_GAP_US after
the last, only notes where the samples are and turns nothing, so aiming
turned on does not apply what was queued. */
void ps5_gyro_turn(struct ps5_gyro_state *state, unsigned long long now_us, const struct ps5_gyro_sample *samples,
	int count, float *yaw, float *pitch);

/* the microphone (port/ps5/probe/results/audioin-probe-run3.txt): a buffer
of the input's 16-bit mono samples as voice chat's floats (-1 to 1) in out,
and how many; 0, and nothing written, for a buffer all zeros, which the
input gives while the pad's mute button is lit or a headset is changing
over (speech or silence never is: silence reads a peak of 22) */
int ps5_microphone_samples(const short *samples, int count, float *out);

/* the updater (host_update_ps5.c; port/ps5/PS5.md, phase 8) */

/* the build number in GitHub's answer for the latest release (its
"tag_name": "build-<number>"), or 0 */
long ps5_update_release_build(const char *json);

/* a zip's local file header (30 bytes), as the release's stored archive
has it */
#define PS5_ZIP_LOCAL_HEADER 30
struct ps5_zip_entry
{
	unsigned long crc, compressed, size;
	unsigned method, flags, name_length, extra_length;
};

/* 1 if header is a local file header, its fields in *entry; 0 if it is the
central directory's start (the files have ended); -1 for anything else */
int ps5_zip_local_header(const unsigned char *header, struct ps5_zip_entry *entry);

/* 1 if the entry can be taken as it is: stored, its sizes before its data
(no data descriptor), not encrypted */
int ps5_zip_entry_stored(const struct ps5_zip_entry *entry);

/* the CRC-32 of data after crc (0 to start), as a zip's */
unsigned long ps5_crc32(unsigned long crc, const void *data, unsigned long size);

/* where an archive's entry goes in the title's folder: for a name under
folder ("PPSA99501/"), the rest of it in out (empty for the folder
itself) and 1, a folder's ending in '/'; 0 for a name outside it (the
release's README.txt, its licences), which the title does not take; -1
for a name the title refuses (a part "..", "." or empty, a backslash, or
too long for out) */
int ps5_update_entry_path(const char *name, const char *folder, char *out, unsigned long size);

/* strcasestr, for posix_update.c, which reads a chunked answer's header
with it (tools/ps5_build.py gives it this name): only libScePosixForWebKit
exports it, and a title's import of it is null (phase 8: a run crashed
there when api.github.com answered chunked) */
char *ps5_update_strcasestr(const char *text, const char *wanted);

#endif
