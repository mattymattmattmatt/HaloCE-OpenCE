/*
PS5_HOST.H

What the PS5's own host files (port/ps5/host; port/ps5/PS5.md) share
beyond the Android host's host.h: the log, the data folder, the thread id,
the futexes, and the reports each file writes when the game ends.
*/

#ifndef PS5_HOST_H
#define PS5_HOST_H

#include <stdint.h>

/* the game's data, settings, saves and log (a folder the player fills over
FTP), and the title's own read-only folder */
#define PS5_DATA_FOLDER "/data/halo"
#define PS5_TITLE_FOLDER "/app0"

/* one line to the log file and the kernel log (host_main.c), with no
newline: the log adds it. Safe from any thread and from signal handlers:
each line is queued with one atomic add, without locks or stdio, and a
thread of the log's writes it out (ps5_log_sync writes it at once). */
void ps5_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
void ps5_log_line(const char *line);
/* what is queued written out now, and with from_now_on every later line at
once too (the end, a crash report) */
void ps5_log_sync(int from_now_on);
/* the lines written since the last call, those that took over 2 ms, and the
longest write to the kernel log and to the file, in ms (the heartbeat's) */
void ps5_log_costs(unsigned *lines, unsigned *slow, double *klog_ms, double *file_ms);

/* a notification on the screen (host_main.c; sceKernelSendNotificationRequest),
logged too */
void ps5_notify(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* the monotonic clock in nanoseconds, and the time since the host started
in seconds, for the log */
uint64_t ps5_now_ns(void);
double ps5_seconds(void);

/* the calling thread's id: the console's (pthread_getthreadid_np, which
libkernel exports to a title), distinct per thread; the guest's gettid
(host_syscall.c) and the debug file's (host_debug_ps5.c) */
int ps5_thread_id(void);

/* the guest's futex calls (host_futex.c): Linux's operations on a 32-bit
word below 4 GB, over FreeBSD's _umtx_op; the result as the guest's
system call returns it (0, a count, or -errno in Linux's numbers) */
long host_futex(uint32_t address, int operation, uint32_t value, uint64_t timeout_address, uint64_t address2,
	uint32_t value3);

/* the reports the game's end writes, each file its own (host_main.c calls
them, after "the game exited" or a fatal line, before "the host ends") */
void ps5_syscall_report(void);   /* host_syscall.c: the calls answered -ENOSYS, by number */
void ps5_memory_report(void);    /* host_memory.c: __wrap_mmap's calls, the pools, direct memory */
void ps5_gl_report(void);        /* host_gl_none.c: the GL names the Vulkan image called */
void ps5_sdl_report(void);       /* host_sdl_ps5.c: frames, the 2D renderer's presents, audio */

/* the host's heap (the platform layer's, in direct memory, which never
gives memory back) and direct memory's largest free block, in MB, for the
heartbeat (host_memory.c) */
void ps5_memory_figures(double *heap_mb, double *heap_peak_mb, double *direct_free_mb);

/* the callers whose live heap grew most since the last call, logged
(host_alloc_track_ps5.c; nothing unless built with PS5_ALLOC_TRACK) */
void ps5_alloc_report(void);
/* from here on, blocks are tracked (host_main.c, as main starts) */
void ps5_alloc_start(void);

/* the network (host_net_ps5.c): a name's IPv4 address through the
console's resolver (network byte order), or 0, for posix_net.c's
posix_resolve_ipv4; and the step 1 smoke test of port/ps5/PS5.md, phase 5
(HALO_PS5_NET_TEST), which host_main.c runs before the game's thread, as
it runs ps5_net_start (brokers.txt beside config.toml, the addresses and
the DTLS certificate logged) */
uint32_t ps5_net_resolve_ipv4(const char *name);

/* the title's updater (host_update_ps5.c): an install a closed session left
halfway finished, or a download's files removed, before the guest loads
(the host may end here, for the new build to start); and the check for a
newer release, on a thread of its own, once the game's thread runs */
void ps5_update_recover(void);
void ps5_update_start(void);
void ps5_net_start(void);
void ps5_net_test(void);

/* config.toml's debug.gpu_stats, read by host_main.c: the GL names are
reported every 10 s, not only at the end (host_gl_none.c) */
extern int host_gl_statistics;

/* HALO_PS5_SCREENSHOT's seconds, read by host_main.c: the presented picture
written to a file that often (host_vk_screenshot_ps5.c); 0 for none */
extern int ps5_screenshot_seconds;

/* the stand-in window's size, which the game draws at (host_sdl_ps5.c):
config.toml's display.resolution_limit (or HALO_RESOLUTION_LIMIT) against
the display's mode, set by host_main.c before the game starts and again when
the file changes (ps5_display_check), on the game's thread */
extern int ps5_display_width, ps5_display_height;

/* once a second, from the game's event poll (host_sdl_ps5.c): config.toml
looked at, and the maximum resolution read again if it changed
(host_main.c) */
void ps5_display_check(uint64_t now);

/* VideoOut's display mode (host_vk_driver_ps5.c): its size and refresh rate
in mHz; 0 before the renderer's instance, or without one */
int ps5_vk_display_mode(int *width, int *height, unsigned *millihertz);

/* the frames the game has swapped so far (host_sdl_ps5.c): 0 before its
first, which tells the system's start-up overlay from a later one
(host_sdl_ps5_input.c) */
uint64_t ps5_frames_swapped(void);

/* ---------- SDL: the game's SDL3 calls over pacbrew's SDL2

host_sdl_ps5.c answers the game's calls with SDL3's headers (the guest's
values and layouts), host_sdl_ps5_input.c (start-up, the pad, its events,
the system's overlay) and host_sdl_ps5_audio.c (the sound) with SDL2's:
the two versions' headers declare the same names differently, so they
cannot share a file. These are the SDL3 values the SDL2 files need, as
literals; host_sdl_ps5.c checks each against SDL3's headers. */

/* SDL3's gamepad buttons 0 (south) to 20 (the touch pad) are SDL2's 0 (a)
to 20 (touch pad) one for one; SDL3's 21 on (misc 2 to 6) are not SDL2's */
#define PS5_SDL3_GAMEPAD_BUTTON_TOUCHPAD 20
/* SDL3's six gamepad axes (left x, left y, right x, right y, the left and
right triggers) are SDL2's 0 to 5 */
#define PS5_SDL3_GAMEPAD_AXIS_COUNT 6
/* SDL3's gamepad types (SDL_GamepadType); SDL2's numbers differ */
enum ps5_sdl3_gamepad_type
{
	PS5_SDL3_GAMEPAD_TYPE_UNKNOWN = 0,
	PS5_SDL3_GAMEPAD_TYPE_STANDARD = 1,
	PS5_SDL3_GAMEPAD_TYPE_XBOX360 = 2,
	PS5_SDL3_GAMEPAD_TYPE_XBOXONE = 3,
	PS5_SDL3_GAMEPAD_TYPE_PS3 = 4,
	PS5_SDL3_GAMEPAD_TYPE_PS4 = 5,
	PS5_SDL3_GAMEPAD_TYPE_PS5 = 6,
	PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO = 7,
	PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT = 8,
	PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT = 9,
	PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR = 10,
};
/* SDL3's SDL_AUDIO_F32 (native-endian 32-bit float), SDL2's AUDIO_F32SYS */
#define PS5_SDL3_AUDIO_F32 0x8120
#define PS5_SDL3_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xffffffffu
#define PS5_SDL3_AUDIO_DEVICE_DEFAULT_RECORDING 0xfffffffeu
/* SDL3's SDL_AudioSpec, as the guest passes it */
struct ps5_sdl3_audio_spec
{
	int32_t format;
	int32_t channels;
	int32_t freq;
};
/* SDL3's SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, which SDL2 does not have */
#define PS5_SDL3_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "SDL_AUDIO_DEVICE_SAMPLE_FRAMES"

/* the events host_sdl_ps5_input.c has for the game (host_sdl_poll_event
writes them as SDL3's) */
enum ps5_event_kind
{
	PS5_EVENT_NONE,
	PS5_EVENT_QUIT,
	PS5_EVENT_GAMEPAD_ADDED,
	PS5_EVENT_GAMEPAD_REMOVED,
	PS5_EVENT_FOCUS_LOST,
	PS5_EVENT_FOCUS_GAINED,
};

/* the next event from SDL2 or the system's overlay, PS5_EVENT_NONE when
there is none; *which is a gamepad's instance id (host_sdl_ps5_input.c) */
int ps5_input_poll(uint32_t *which);
/* the period the game asks its audio device for, in frames
(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES; host_sdl_ps5_audio.c) */
void ps5_audio_set_period(int frames);
/* their parts of ps5_sdl_report, when the game ends */
void ps5_input_report(void);
void ps5_audio_report(void);
/* the gyroscope's figures since the last ask (host_pad_ps5.c), for the
pad: line: the samples read, the reads, the largest turn of a read and the
turn in all, in radians; 0 if it was not read */
int ps5_pad_gyro_minute(unsigned *samples, unsigned *reads, float *largest, float *total);

/* voice chat's microphone (host_microphone_ps5.c): libSceAudioIn loaded at
the host's start; opened for voice chat's recording stream
(host_sdl_ps5_audio.c), whose 48 kHz mono floats it then holds */
void ps5_microphone_start(void);
int ps5_microphone_open(void);
int ps5_microphone_available(void);
int ps5_microphone_read(void *data, int length);
void ps5_microphone_close(void);

#endif
