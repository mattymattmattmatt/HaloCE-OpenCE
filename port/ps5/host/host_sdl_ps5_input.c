/*
HOST_SDL_PS5_INPUT.C

SDL's start-up, the pad and its events for the game on the PS5, over
pacbrew's SDL2 (port/ps5/PS5.md, phase 3). The game calls SDL3's functions
(port/android/guest/runtime/guest_sdl.c); host_sdl_ps5.c answers the rest
of them with SDL3's headers, and this file the ones SDL2 answers, with
SDL2's: the two cannot share a file (ps5_host.h). The Switch's host_sdl2.c
is the model.

- SDL2 is started for its timer, events, game controllers and audio only.
  Its video is never started: its PS5 driver calls libSceKeyboard, which is
  not loaded into a title, through a null import (phase 0 B). The window,
  the context and the displays stay host_sdl_ps5.c's stand-ins.
- The game's gamepads are SDL2's game controllers, by instance id, less
  the console's own "PS5 Remote Control", which SDL2 lists beside the pad
  as a controller of unknown type. Axes are the same numbers in both
  versions, and so are buttons 0 to 20 (the touch pad is 20 in both);
  types are mapped. The pad's mapping has no back button (phase 0 B):
  the touch pad's click is the game's Back.
- Events: a controller added or removed, and quit. Button and axis events
  are dropped (the game reads the pad's state each frame) but counted, and
  every minute in which the pad was used a line says what was pressed.
- The system's overlay (the PS button's menu, the Quick Menu) does not
  stop or tell a title (phase 0 B): sceSystemServiceGetStatus's flags are
  read every few milliseconds, and a change is the window's focus lost or
  gained, as a desktop window's would be. The overlay a title starts under
  (the system's launch screen) is told apart (up at the first look, or
  before the game's first frame) and is
  nothing more. A later one in a campaign level (the loaded
  map's header says solo) opens the campaign's pause screen the way the
  Start button does: the first pad reads Start pressed for 100 ms, which
  needs no change to the game (port/ps5/PS5.md, phase 6). In a
  multiplayer game or the menus it is nothing (a match cannot pause); on
  return the player resumes from the pause screen themselves.
- SDL2 reports neither the pad's removal nor its return when it is
  switched off and on from the Quick Menu (phase 4): once a second, when
  no event has said so, the joysticks SDL2 lists are compared with the
  last listing, and a pad gone or come is logged and its event made, so
  the game lists its pads again.
*/

#include "host.h"
#include "ps5_host.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(SDL_CONTROLLER_BUTTON_TOUCHPAD == PS5_SDL3_GAMEPAD_BUTTON_TOUCHPAD,
	"SDL2's buttons 0 to 20 are SDL3's");
_Static_assert(SDL_CONTROLLER_BUTTON_MAX == PS5_SDL3_GAMEPAD_BUTTON_TOUCHPAD + 1, "SDL2 has no button after the touch pad");
_Static_assert(SDL_CONTROLLER_AXIS_MAX == PS5_SDL3_GAMEPAD_AXIS_COUNT, "SDL2's axes are SDL3's");

/* the console's remote control, which SDL2 lists as a game controller of
unknown type (phase 0 B) */
#define REMOTE_CONTROL_NAME "PS5 Remote Control"

/* the pad's figures, every minute in which it was used */
#define PAD_LINE_NS 60000000000ull
/* how often the overlay's flags are read */
#define OVERLAY_CHECK_NS 5000000ull
/* how often the pads SDL2 lists are compared with the last listing */
#define PAD_POLL_NS 1000000000ull
/* how long the overlay's Start press lasts: a few of the game's frames, one press */
#define START_PRESS_NS 100000000ull
/* SDL2's (and SDL3's) Start, the DualSense's options button */
#define START_BUTTON SDL_CONTROLLER_BUTTON_START

/* ---------- start-up */

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static int sdl_started;

static void log_audio_devices(void)
{
	int count, index, recording;

	for (recording = 0; recording < 2; recording++)
	{
		count = SDL_GetNumAudioDevices(recording);
		host_logf(HOST_LOG_INFO, "SDL: %d %s device(s)%s", count, recording ? "recording" : "playback",
			recording ? " (SDL2 records nothing on the console: voice chat's microphone is libSceAudioIn's, "
			"host_microphone_ps5.c)" : "");
		for (index = 0; index < count; index++)
		{
			const char *name = SDL_GetAudioDeviceName(index, recording);

			host_logf(HOST_LOG_INFO, "SDL:   %s device %d: %s", recording ? "recording" : "playback", index,
				name ? name : "(no name)");
		}
	}
}

/* (the game's flags are SDL3's; whatever they ask, SDL2 is started for these
four subsystems only, once) */
int host_sdl_init(uint32_t flags)
{
	/* never SDL_INIT_VIDEO: pacbrew's PS5 video driver starts its keyboard
	through libSceKeyboard, which is not loaded into a title, and the title
	crashes on a null import (port/ps5/PS5.md, phase 0 B). Nothing here needs
	a window: the pad, sound, events and timers work without it */
	const Uint32 subsystems = SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO;
	SDL_version linked;
	uint64_t start;
	int index;

	pthread_mutex_lock(&init_lock);
	if (sdl_started)
	{
		pthread_mutex_unlock(&init_lock);
		return 1;
	}
	SDL_SetMainReady();
	start = ps5_now_ns();
	if (SDL_Init(subsystems) != 0)
	{
		host_logf(HOST_LOG_ERROR, "SDL: SDL_Init(timer, events, game controller, audio) for the game's 0x%x: %s",
			flags, SDL_GetError());
		pthread_mutex_unlock(&init_lock);
		return 0;
	}
	sdl_started = 1;
	SDL_GetVersion(&linked);
	host_logf(HOST_LOG_INFO, "SDL: SDL_Init(timer, events, game controller, audio) for the game's 0x%x in %llu us: "
		"SDL %d.%d.%d (%s), audio driver %s, video driver %s (SDL2's video is never started here)", flags,
		(unsigned long long)((ps5_now_ns() - start) / 1000), linked.major, linked.minor, linked.patch,
		SDL_GetRevision(), SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none",
		SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
	for (index = 0; index < SDL_GetNumAudioDrivers(); index++)
		host_logf(HOST_LOG_INFO, "SDL: audio driver %d: %s", index, SDL_GetAudioDriver(index));
	log_audio_devices();
	pthread_mutex_unlock(&init_lock);
	return 1;
}

/* the device's period is the audio file's (SDL2 takes it in the spec, not
from a hint); the rest are SDL2's, whose names agree with SDL3's for the
hints the game sets */
int host_sdl_set_hint(const char *name, const char *value)
{
	if (!name || !value)
		return 0;
	if (!strcmp(name, PS5_SDL3_HINT_AUDIO_DEVICE_SAMPLE_FRAMES))
	{
		if (atoi(value) > 0)
			ps5_audio_set_period(atoi(value));
		return 1;
	}
	if (SDL_SetHint(name, value) != SDL_TRUE)
	{
		host_logf(HOST_LOG_WARN, "SDL: SDL2 refused the hint %s=%s", name, value);
		return 0;
	}
	host_logf(HOST_LOG_INFO, "SDL: the hint %s=%s", name, value);
	return 1;
}

void host_sdl_get_error(char *buffer, uint32_t size)
{
	if (size)
		SDL_strlcpy(buffer, SDL_GetError(), size);
}

/* ---------- gamepads */

static const char *controller_type_name(SDL_GameControllerType type)
{
	switch (type)
	{
	case SDL_CONTROLLER_TYPE_UNKNOWN: return "unknown";
	case SDL_CONTROLLER_TYPE_XBOX360: return "Xbox 360";
	case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox One";
	case SDL_CONTROLLER_TYPE_PS3: return "PS3";
	case SDL_CONTROLLER_TYPE_PS4: return "PS4";
	case SDL_CONTROLLER_TYPE_PS5: return "PS5";
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch Pro";
	case SDL_CONTROLLER_TYPE_VIRTUAL: return "virtual";
	default: return "other";
	}
}

/* SDL2's type to SDL3's, as the Switch's host_sdl3_gamepad_type maps them
(a type SDL3 does not have is unknown) */
static int sdl3_gamepad_type(SDL_GameControllerType type)
{
	switch (type)
	{
	case SDL_CONTROLLER_TYPE_XBOX360: return PS5_SDL3_GAMEPAD_TYPE_XBOX360;
	case SDL_CONTROLLER_TYPE_XBOXONE: return PS5_SDL3_GAMEPAD_TYPE_XBOXONE;
	case SDL_CONTROLLER_TYPE_PS3: return PS5_SDL3_GAMEPAD_TYPE_PS3;
	case SDL_CONTROLLER_TYPE_PS4: return PS5_SDL3_GAMEPAD_TYPE_PS4;
	case SDL_CONTROLLER_TYPE_PS5: return PS5_SDL3_GAMEPAD_TYPE_PS5;
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: return PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT;
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: return PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT;
	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PS5_SDL3_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR;
	default: return PS5_SDL3_GAMEPAD_TYPE_UNKNOWN;
	}
}

/* The type of the joystick at a device index, asked of SDL2 once for each
instance and remembered: pacbrew's SDL 2.30.12 builds a mapping string in
SDL_GameControllerTypeForIndex and never frees it, about 285 bytes a call,
and the pads are listed many times a frame (port/ps5/PS5.md, phase 6: the
host's heap grew 16 to 32 MB a minute in a match with a second player). A
joystick's type does not change while it is connected. */
#define KNOWN_TYPES 32

static SDL_GameControllerType device_type(int index)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static struct
	{
		SDL_JoystickID id;
		SDL_GameControllerType type;
	} known[KNOWN_TYPES];
	static int known_types, next;
	SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(index);
	SDL_GameControllerType type;
	int slot;

	pthread_mutex_lock(&lock);
	for (slot = 0; slot < known_types; slot++)
	{
		if (known[slot].id == id)
		{
			type = known[slot].type;
			pthread_mutex_unlock(&lock);
			return type;
		}
	}
	type = SDL_GameControllerTypeForIndex(index);
	/* (instance ids are not reused; the oldest is forgotten when full) */
	slot = known_types < KNOWN_TYPES ? known_types++ : next++ % KNOWN_TYPES;
	known[slot].id = id;
	known[slot].type = type;
	pthread_mutex_unlock(&lock);
	return type;
}

/* whether the joystick at this device index is one of the game's: a game
controller, and not the console's remote control */
static int is_game_pad(int index)
{
	const char *name;

	if (!SDL_IsGameController(index))
		return 0;
	name = SDL_GameControllerNameForIndex(index);
	return !(device_type(index) == SDL_CONTROLLER_TYPE_UNKNOWN && name && !strcmp(name, REMOTE_CONTROL_NAME));
}

/* the joysticks SDL2 lists, each time the list changes */
static void log_listing(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static SDL_JoystickID listed[16];
	static int listed_count = -1;
	SDL_JoystickID ids[16];
	int count = SDL_NumJoysticks(), index;

	if (count > 16)
		count = 16;
	for (index = 0; index < count; index++)
		ids[index] = SDL_JoystickGetDeviceInstanceID(index);
	pthread_mutex_lock(&lock);
	if (count == listed_count && !memcmp(ids, listed, (size_t)count * sizeof(ids[0])))
	{
		pthread_mutex_unlock(&lock);
		return;
	}
	listed_count = count;
	memcpy(listed, ids, (size_t)count * sizeof(ids[0]));
	pthread_mutex_unlock(&lock);
	host_logf(HOST_LOG_INFO, "gamepads: SDL2 lists %d joystick(s)", count);
	for (index = 0; index < count; index++)
	{
		char guid[40];
		const char *name = SDL_JoystickNameForIndex(index);

		SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
		host_logf(HOST_LOG_INFO, "gamepads:   joystick %d: \"%s\", instance %d, %s, type %d (%s), GUID %s%s", index,
			name ? name : "", (int)ids[index], SDL_IsGameController(index) ? "a game controller" : "not a game controller",
			(int)device_type(index), controller_type_name(device_type(index)), guid,
			is_game_pad(index) ? "" : SDL_IsGameController(index) ? ": left out (the console's remote control)" :
			": left out");
	}
}

int host_sdl_get_gamepads(uint32_t *ids, int capacity)
{
	int count = SDL_NumJoysticks(), index, found = 0;

	log_listing();
	for (index = 0; index < count && found < capacity; index++)
	{
		if (is_game_pad(index))
			ids[found++] = (uint32_t)SDL_JoystickGetDeviceInstanceID(index);
	}
	return found;
}

static int device_index_of(SDL_JoystickID id)
{
	int count = SDL_NumJoysticks(), index;

	for (index = 0; index < count; index++)
	{
		if (SDL_JoystickGetDeviceInstanceID(index) == id)
			return index;
	}
	return -1;
}

uint32_t host_sdl_open_gamepad(uint32_t id)
{
	int index = device_index_of((SDL_JoystickID)id);
	SDL_GameController *gamepad;

	if (index < 0 || !is_game_pad(index))
		return 0;
	gamepad = SDL_GameControllerFromInstanceID((SDL_JoystickID)id);
	if (!gamepad)
	{
		gamepad = SDL_GameControllerOpen(index);
		if (gamepad)
			host_logf(HOST_LOG_INFO, "gamepad %u: \"%s\" opened, type %d (%s; SDL3's %d), %04x:%04x, rumble %s", (unsigned)id,
				SDL_GameControllerName(gamepad), (int)SDL_GameControllerGetType(gamepad),
				controller_type_name(SDL_GameControllerGetType(gamepad)),
				sdl3_gamepad_type(SDL_GameControllerGetType(gamepad)), SDL_GameControllerGetVendor(gamepad),
				SDL_GameControllerGetProduct(gamepad), SDL_GameControllerHasRumble(gamepad) ? "yes" : "no");
		else
			host_logf(HOST_LOG_WARN, "gamepad %u: cannot open it: %s", (unsigned)id, SDL_GetError());
	}
	return host_sdl_handle_new(_handle_gamepad, gamepad);
}

/* SDL3 opens nothing here, but a pad SDL2 lists that nobody has opened yet
(the one there at the start, which sends no "added") is opened now, as the
Switch's does; one removed is still open until the game closes it */
uint32_t host_sdl_gamepad_from_id(uint32_t id)
{
	SDL_GameController *gamepad = SDL_GameControllerFromInstanceID((SDL_JoystickID)id);

	if (!gamepad)
		return host_sdl_open_gamepad(id);
	return host_sdl_handle_new(_handle_gamepad, gamepad);
}

void host_sdl_close_gamepad(uint32_t gamepad)
{
	SDL_GameController *object = host_sdl_handle_get(gamepad, _handle_gamepad);

	if (!object)
		return;
	host_sdl_handle_free(gamepad);
	host_logf(HOST_LOG_INFO, "gamepad: \"%s\" closed", SDL_GameControllerName(object));
	SDL_GameControllerClose(object);
}

/* (-32768 to 32767, as it is: the game applies its own dead zone) */
int host_sdl_gamepad_axis(uint32_t gamepad, int axis)
{
	SDL_GameController *object = host_sdl_handle_get(gamepad, _handle_gamepad);

	if (!object || axis < 0 || axis >= PS5_SDL3_GAMEPAD_AXIS_COUNT)
		return 0;
	return SDL_GameControllerGetAxis(object, (SDL_GameControllerAxis)axis);
}

/* the overlay's Start press (overlay_check): the pad and until when */
static SDL_JoystickID start_pad = -1;
static uint64_t start_until;

/* (the pad's mapping on the console has no back, guide or misc button
(phase 0 B): they read 0, but for back, which is the touch pad's click -
the game reads Back, and never the touch pad) */
int host_sdl_gamepad_button(uint32_t gamepad, int button)
{
	SDL_GameController *object = host_sdl_handle_get(gamepad, _handle_gamepad);

	if (!object || button < 0 || button > PS5_SDL3_GAMEPAD_BUTTON_TOUCHPAD)
		return 0;
	if (button == SDL_CONTROLLER_BUTTON_BACK)
		return SDL_GameControllerGetButton(object, SDL_CONTROLLER_BUTTON_BACK) ||
			SDL_GameControllerGetButton(object, SDL_CONTROLLER_BUTTON_TOUCHPAD);
	if (button == START_BUTTON && start_pad >= 0 && ps5_now_ns() < __atomic_load_n(&start_until, __ATOMIC_ACQUIRE) &&
		SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(object)) == start_pad)
		return 1;
	return SDL_GameControllerGetButton(object, (SDL_GameControllerButton)button);
}

int host_sdl_gamepad_type(uint32_t gamepad)
{
	SDL_GameController *object = host_sdl_handle_get(gamepad, _handle_gamepad);

	return object ? sdl3_gamepad_type(SDL_GameControllerGetType(object)) : PS5_SDL3_GAMEPAD_TYPE_UNKNOWN;
}

/* the game asks every frame, for 100 ms; SDL2 sends the pad only a change */
static uint64_t rumble_requests, rumble_on, rumble_longest_ns;

int host_sdl_rumble_gamepad(uint32_t gamepad, uint32_t low, uint32_t high, uint32_t milliseconds)
{
	SDL_GameController *object = host_sdl_handle_get(gamepad, _handle_gamepad);
	uint64_t start, took;
	int result;

	if (!object)
		return 0;
	start = ps5_now_ns();
	result = SDL_GameControllerRumble(object, (Uint16)low, (Uint16)high, milliseconds);
	took = ps5_now_ns() - start;
	__atomic_add_fetch(&rumble_requests, 1, __ATOMIC_RELAXED);
	if (low || high)
		__atomic_add_fetch(&rumble_on, 1, __ATOMIC_RELAXED);
	if (took > __atomic_load_n(&rumble_longest_ns, __ATOMIC_RELAXED))
		__atomic_store_n(&rumble_longest_ns, took, __ATOMIC_RELAXED);
	if (result != 0)
	{
		static int logged;

		if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
			host_logf(HOST_LOG_WARN, "gamepad: SDL_GameControllerRumble: %s", SDL_GetError());
	}
	return result == 0;
}

/* ---------- what the pad did, every minute it was used (the poll's thread) */

static const char *const button_names[SDL_CONTROLLER_BUTTON_MAX] =
{
	"cross", "circle", "square", "triangle", "back", "guide", "options", "L3", "R3", "L1", "R1",
	"up", "down", "left", "right", "mic", "paddle 1", "paddle 2", "paddle 3", "paddle 4", "touch pad",
};
static const char *const axis_names[SDL_CONTROLLER_AXIS_MAX] =
{
	"left x", "left y", "right x", "right y", "L2", "R2",
};

static struct
{
	uint64_t line_start;
	unsigned presses[SDL_CONTROLLER_BUTTON_MAX];
	int axis_low[SDL_CONTROLLER_AXIS_MAX], axis_high[SDL_CONTROLLER_AXIS_MAX];
	uint64_t rumble_on_before;
	int used;
} pad_minute;

/* the line, once a minute; or now, for the end's part minute (last) */
static void pad_line(uint64_t now, int last)
{
	char line[768];
	size_t used = 0;
	uint64_t on = __atomic_load_n(&rumble_on, __ATOMIC_RELAXED);
	unsigned gyro_samples, gyro_reads;
	float gyro_largest, gyro_total;
	int index, gyro;

	if (!pad_minute.line_start)
	{
		pad_minute.line_start = now;
		return;
	}
	if (now - pad_minute.line_start < PAD_LINE_NS && !last)
		return;
	gyro = ps5_pad_gyro_minute(&gyro_samples, &gyro_reads, &gyro_largest, &gyro_total);
	if (pad_minute.used || on != pad_minute.rumble_on_before || gyro)
	{
		used += (size_t)snprintf(line + used, sizeof(line) - used, "pad: in %.0f s, presses:",
			(now - pad_minute.line_start) / 1e9);
		for (index = 0; index < SDL_CONTROLLER_BUTTON_MAX && used < sizeof(line); index++)
		{
			if (pad_minute.presses[index])
				used += (size_t)snprintf(line + used, sizeof(line) - used, " %s %u", button_names[index],
					pad_minute.presses[index]);
		}
		for (index = 0; index < SDL_CONTROLLER_AXIS_MAX && used < sizeof(line); index++)
		{
			if (pad_minute.axis_low[index] || pad_minute.axis_high[index])
				used += (size_t)snprintf(line + used, sizeof(line) - used, "%s %s %d to %d",
					index ? "," : "; axes", axis_names[index], pad_minute.axis_low[index], pad_minute.axis_high[index]);
		}
		if (used < sizeof(line))
			used += (size_t)snprintf(line + used, sizeof(line) - used, "; rumble asked with the motors on %llu times",
				(unsigned long long)(on - pad_minute.rumble_on_before));
		/* (the gyroscope's, when Gyro Aim read it: host_pad_ps5.c) */
		if (gyro && used < sizeof(line))
			snprintf(line + used, sizeof(line) - used, "; gyro: %.0f samples a second in %u reads, the largest turn "
				"of a read %.2f degrees, %.0f degrees in all", gyro_samples / ((now - pad_minute.line_start) / 1e9),
				gyro_reads, gyro_largest * 57.29578f, gyro_total * 57.29578f);
		host_log(HOST_LOG_INFO, line);
	}
	memset(&pad_minute, 0, sizeof(pad_minute));
	pad_minute.line_start = now;
	pad_minute.rumble_on_before = on;
}

static void pad_count(const SDL_Event *event)
{
	if (event->type == SDL_CONTROLLERBUTTONDOWN && event->cbutton.button < SDL_CONTROLLER_BUTTON_MAX)
	{
		pad_minute.presses[event->cbutton.button]++;
		pad_minute.used = 1;
	}
	else if (event->type == SDL_CONTROLLERAXISMOTION && event->caxis.axis < SDL_CONTROLLER_AXIS_MAX)
	{
		if (event->caxis.value < pad_minute.axis_low[event->caxis.axis])
			pad_minute.axis_low[event->caxis.axis] = event->caxis.value;
		if (event->caxis.value > pad_minute.axis_high[event->caxis.axis])
			pad_minute.axis_high[event->caxis.axis] = event->caxis.value;
		pad_minute.used = 1;
	}
}

/* ---------- the system's overlay */

/* OpenOrbis's PS4 declaration, as the host probe declared it: the call
fills a structure that starts with the count of events waiting (an int32),
then flags a byte each, isSystemUiOverlaid first. Its full size is not
known with confidence, so it is given a buffer far larger than it can be,
and only those first bytes are read. */
int32_t sceSystemServiceGetStatus(void *status);

/* what the pad reads while the menu is up, once (phase 0 B: the system
takes the pad, and scePad marks it intercepted with no buttons) */
static void log_pad_while_overlaid(void)
{
	int count = SDL_NumJoysticks(), index;

	for (index = 0; index < count; index++)
	{
		SDL_GameController *gamepad = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(index));
		unsigned buttons = 0;
		int button;

		if (!gamepad)
			continue;
		for (button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++)
		{
			if (SDL_GameControllerGetButton(gamepad, (SDL_GameControllerButton)button))
				buttons |= 1u << button;
		}
		host_logf(HOST_LOG_INFO, "overlay: \"%s\" reads buttons 0x%x, axes %d %d %d %d %d %d while the menu is up",
			SDL_GameControllerName(gamepad), buttons, SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTX),
			SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_LEFTY),
			SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTX),
			SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_RIGHTY),
			SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERLEFT),
			SDL_GameControllerGetAxis(gamepad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
	}
}

/* the loaded map, from the guest's cache_file_globals (host_loader.c finds
it): { boolean tags_loaded; byte pad[3]; struct cache_file_header header;
... }, and the header's name at 0x20 and its type at 0x60 (a short: 0 solo,
1 multiplayer, 2 the main menu, scenario_definitions.h's scenario types;
cache_files.c names the field reserved60). The type, or -1 with no map. */
static int loaded_map(char *name, size_t size)
{
	const unsigned char *globals = (const unsigned char *)(uintptr_t)host_image.cache_file_globals;
	uint16_t type;

	name[0] = 0;
	if (!globals || !globals[0])
		return -1;
	snprintf(name, size, "%.31s", (const char *)globals + 4 + 0x20);
	memcpy(&type, globals + 4 + 0x60, sizeof(type));
	return type;
}

/* the first of the game's pads SDL2 lists, the one Start is pressed on; -1 for none */
static SDL_JoystickID first_game_pad(void)
{
	int count = SDL_NumJoysticks(), index;

	for (index = 0; index < count; index++)
	{
		if (is_game_pad(index) && SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(index)))
			return SDL_JoystickGetDeviceInstanceID(index);
	}
	return -1;
}

/* what the overlay coming up means: nothing at the start, the pause screen
in a campaign level, nothing elsewhere */
static void overlay_up(uint64_t now, int start_up)
{
	char map[40];
	int type = loaded_map(map, sizeof(map));
	SDL_JoystickID pad;

	if (start_up)
	{
		host_logf(HOST_LOG_INFO, "overlay: the start-up overlay (up before the game's first frame): not a pause");
		return;
	}
	if (type != 0)
	{
		host_logf(HOST_LOG_INFO, "overlay: the map is %s (type %d, %s): no pause", map[0] ? map : "none", type,
			type == 1 ? "multiplayer: a match cannot pause" : type == 2 ? "the menus" : "no map");
		return;
	}
	pad = first_game_pad();
	if (pad < 0)
	{
		host_logf(HOST_LOG_INFO, "overlay: the map is %s (a campaign level), but no pad is open to press Start on", map);
		return;
	}
	start_pad = pad;
	__atomic_store_n(&start_until, now + START_PRESS_NS, __ATOMIC_RELEASE);
	host_logf(HOST_LOG_INFO, "overlay: the map is %s (a campaign level): Start pressed on pad %d for %llu ms, "
		"the pause screen", map, (int)pad, (unsigned long long)(START_PRESS_NS / 1000000));
}

/* PS5_EVENT_FOCUS_LOST or GAINED when the overlay comes or goes, else
PS5_EVENT_NONE. Overlaid is either of the first two flags: the PS button's
menu sets the first, the Quick Menu the second for as long as it is up
(phase 0 B's run 3: 01 at 19.775 s, then 00 01 until 23.780 s) */
static int overlay_check(uint64_t now)
{
	static unsigned char status[16384];
	static unsigned char last[4];
	static uint64_t checked_at, overlaid_at;
	static int overlaid, pad_logged, failure_logged, start_up, looked;
	int looked_before;
	int32_t result;
	int now_overlaid;

	if (checked_at && now - checked_at < OVERLAY_CHECK_NS)
	{
		if (overlaid && !pad_logged && now - overlaid_at > 500000000ull)
		{
			pad_logged = 1;
			log_pad_while_overlaid();
		}
		return PS5_EVENT_NONE;
	}
	checked_at = now;
	memset(status, 0, 8);
	result = sceSystemServiceGetStatus(status);
	if (result != 0)
	{
		if (!failure_logged)
			host_logf(HOST_LOG_WARN, "overlay: sceSystemServiceGetStatus: 0x%08x (the overlay is not followed)",
				(unsigned)result);
		failure_logged = 1;
		return PS5_EVENT_NONE;
	}
	looked_before = looked;
	looked = 1;
	if (memcmp(last, status + 4, sizeof(last)))
	{
		memcpy(last, status + 4, sizeof(last));
		host_logf(HOST_LOG_INFO, "overlay: the system's status flags %02x %02x %02x %02x at %.3f s", last[0], last[1],
			last[2], last[3], ps5_seconds());
	}
	now_overlaid = status[4] || status[5];
	if (now_overlaid == overlaid)
		return PS5_EVENT_NONE;
	overlaid = now_overlaid;
	if (overlaid)
	{
		overlaid_at = now;
		pad_logged = 0;
		/* (up at the first look, or before the game's first frame: the game
		draws its first frame before it first asks for events, phase 6) */
		start_up = !looked_before || ps5_frames_swapped() == 0;
	}
	host_logf(HOST_LOG_INFO, "overlay: the system's menu is %s at %.3f s: the window's focus is %s%s",
		overlaid ? "up" : "gone", ps5_seconds(), overlaid ? "lost" : "gained",
		start_up ? " (the start-up overlay)" : "");
	if (overlaid)
		overlay_up(now, start_up);
	else
		start_up = 0;
	return overlaid ? PS5_EVENT_FOCUS_LOST : PS5_EVENT_FOCUS_GAINED;
}

/* ---------- the pads' arrival and removal, by polling

The game's pads as last seen (by SDL2's events or this poll): what a new
listing is compared with. An event SDL2 sends updates it too, so a change
it reported is not made again. */

#define KNOWN_PADS 16

static SDL_JoystickID known_pads[KNOWN_PADS];
static int known_count = -1;

static int known_index(SDL_JoystickID id)
{
	int index;

	for (index = 0; index < known_count; index++)
	{
		if (known_pads[index] == id)
			return index;
	}
	return -1;
}

static void known_add(SDL_JoystickID id)
{
	if (known_count >= 0 && known_index(id) < 0 && known_count < KNOWN_PADS)
		known_pads[known_count++] = id;
}

static void known_remove(SDL_JoystickID id)
{
	int index = known_count >= 0 ? known_index(id) : -1;

	if (index >= 0)
		known_pads[index] = known_pads[--known_count];
}

/* once a second: a pad of the game's that SDL2 no longer lists is removed,
one it lists that was not known is added (PS5_EVENT_GAMEPAD_*, *which its
instance), one change a call */
static int pad_poll(uint64_t now, uint32_t *which)
{
	static uint64_t polled_at;
	SDL_JoystickID listed[KNOWN_PADS];
	int count, index, listed_count = 0;

	if (polled_at && now - polled_at < PAD_POLL_NS)
		return PS5_EVENT_NONE;
	polled_at = now;
	SDL_JoystickUpdate();
	count = SDL_NumJoysticks();
	for (index = 0; index < count && listed_count < KNOWN_PADS; index++)
	{
		if (is_game_pad(index))
			listed[listed_count++] = SDL_JoystickGetDeviceInstanceID(index);
	}
	if (known_count < 0)
	{
		/* (the first listing: the pads there at the start, which the game
		asks for itself) */
		memcpy(known_pads, listed, (size_t)listed_count * sizeof(listed[0]));
		known_count = listed_count;
		return PS5_EVENT_NONE;
	}
	for (index = 0; index < known_count; index++)
	{
		int other, still = 0;

		for (other = 0; other < listed_count; other++)
			still |= listed[other] == known_pads[index];
		if (!still)
		{
			*which = (uint32_t)known_pads[index];
			known_remove(known_pads[index]);
			host_logf(HOST_LOG_INFO, "gamepads: instance %u is no longer listed, and SDL2 said nothing: removed "
				"(the poll)", *which);
			log_listing();
			return PS5_EVENT_GAMEPAD_REMOVED;
		}
	}
	for (index = 0; index < listed_count; index++)
	{
		if (known_index(listed[index]) < 0)
		{
			*which = (uint32_t)listed[index];
			known_add(listed[index]);
			host_logf(HOST_LOG_INFO, "gamepads: instance %u is listed, and SDL2 said nothing: added (the poll)",
				*which);
			log_listing();
			return PS5_EVENT_GAMEPAD_ADDED;
		}
	}
	return PS5_EVENT_NONE;
}

/* ---------- events */

int ps5_input_poll(uint32_t *which)
{
	uint64_t now = ps5_now_ns();
	SDL_Event event;
	int kind;

	*which = 0;
	if (!sdl_started)
		return PS5_EVENT_NONE;
	pad_line(now, 0);
	kind = overlay_check(now);
	if (kind != PS5_EVENT_NONE)
		return kind;
	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_QUIT:
			host_logf(HOST_LOG_INFO, "SDL: quit");
			return PS5_EVENT_QUIT;
		case SDL_CONTROLLERDEVICEADDED:
			/* SDL2 names the new device by its index, SDL3 by its instance */
			if (!is_game_pad(event.cdevice.which))
				break;
			*which = (uint32_t)SDL_JoystickGetDeviceInstanceID(event.cdevice.which);
			host_logf(HOST_LOG_INFO, "SDL: gamepad added: \"%s\", instance %u",
				SDL_GameControllerNameForIndex(event.cdevice.which), *which);
			if (known_count >= 0 && known_index((SDL_JoystickID)*which) >= 0)
			{
				/* (the poll has made it already) */
				host_logf(HOST_LOG_INFO, "SDL: (the poll had added it)");
				*which = 0;
				break;
			}
			known_add((SDL_JoystickID)*which);
			return PS5_EVENT_GAMEPAD_ADDED;
		case SDL_CONTROLLERDEVICEREMOVED:
			*which = (uint32_t)event.cdevice.which;
			host_logf(HOST_LOG_INFO, "SDL: gamepad removed: instance %u", *which);
			if (known_count >= 0 && known_index((SDL_JoystickID)*which) < 0)
			{
				host_logf(HOST_LOG_INFO, "SDL: (the poll had removed it)");
				*which = 0;
				break;
			}
			known_remove((SDL_JoystickID)*which);
			return PS5_EVENT_GAMEPAD_REMOVED;
		default:
			pad_count(&event);
			break;
		}
	}
	return pad_poll(now, which);
}

void ps5_input_report(void)
{
	/* (what the pad did since the last minute's line) */
	pad_line(ps5_now_ns(), 1);
	host_logf(HOST_LOG_INFO, "pad: rumble asked %llu times (%llu with the motors on), the longest call %.2f ms",
		(unsigned long long)rumble_requests, (unsigned long long)rumble_on, rumble_longest_ns / 1e6);
}
