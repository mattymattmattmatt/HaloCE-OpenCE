/*
MANUAL_RESPAWN.C

Respawning when the player chooses to (game.manual_respawn, off by default).
With it on, in the campaign, a player who dies stays dead: the camera stays on
the body as the stock death camera has it and the world plays on, and once
the stock delay is over (main.c's death timer, about three seconds) a small
line asks for the jump button; pressing it reverts to the last checkpoint
(all the players dead) or brings a split-screen player back beside the others,
as the stock game would have done by itself.

Only a local game: multiplayer (a game engine) keeps its own respawn timer,
and network co-op its own respawns (players.c, coop_spectate.c), untouched.
*/

#include "cseries.h"
#include "data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "input/input.h"
#include "input/input_abstraction.h"
#include "interface/hud_messaging.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "objects/objects.h"
#include "units/units.h"
#include "units/bipeds.h"

#include <math.h>

#include "manual_respawn.h"
#include "network_coop.h"
#include "interface/ui_widget.h"

#include <stdio.h>
#include <string.h>

/* port_config.c */
int config_boolean(const char *name);
const char *config_string(const char *name);
unsigned long config_changes(void);
/* platform.c */
void platform_log(const char *format, ...);

/* input_abstraction.c: how many ticks the keyboard's jump key has been held */
byte input_abstraction_port_accept(short controller_index);
short input_abstraction_port_jump_button(short controller_index);
/* xinput_sdl.c: a binding's name as the keyboard labels the key */
void halo_input_shown_name(char const *binding, char *shown, unsigned int size);

/* port_config.c */
double config_real(const char *name);

/* ---------- globals */

/* debug.manual_respawn_test's press: ticks left of the jump held down */
static long manual_respawn_test_down = 0;

/* waiting for the press (main.c asked, the setting on, the delay over) */
static boolean manual_respawn_waiting_now = FALSE;
/* the last frame main.c asked: a frame it did not ask in ends the wait */
static unsigned long manual_respawn_asked_frame = 0;
static unsigned long manual_respawn_frame = 0;
/* each controller's jump, held at the last look */
static boolean manual_respawn_held[MAXIMUM_GAMEPADS];

/* ---------- private code */

static boolean manual_respawn_setting(
	void)
{
	static boolean setting = FALSE;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		setting = config_boolean("game.manual_respawn") != 0;
	}
	return setting;
}

/* the campaign on this machine alone (split screen included) */
static boolean manual_respawn_applies(
	void)
{
	return manual_respawn_setting() &&
		!game_engine_running() &&
		game_connection() == _game_connection_local &&
		!network_coop_active() &&
		!main_menu_is_active();
}

/* the controller's jump down now: the profile's jump button (A unless the
layout moves it), A itself, or the keyboard's jump key */
static boolean manual_respawn_jump_down(
	short controller_index)
{
	struct gamepad_state const *gamepad;
	short button;

	if (controller_index < 0 || controller_index >= MAXIMUM_GAMEPADS)
		return FALSE;
	if (manual_respawn_test_down > 0 && controller_index == 0)
		return TRUE;
	if (input_abstraction_port_accept(controller_index) > 0)
		return TRUE;
	gamepad = input_get_gamepad_state(controller_index);
	if (!gamepad)
		return FALSE;
	if (gamepad->buttons[_gamepad_analog_button_a] > 0)
		return TRUE;
	button = input_abstraction_port_jump_button(controller_index);
	return button >= 0 && button < NUMBER_OF_GAMEPAD_BUTTONS && gamepad->buttons[button] > 0;
}

/* a dead local player pressed (not held over from before: a fresh press) */
static boolean manual_respawn_pressed(
	boolean prime)
{
	struct data_iterator iterator;
	struct player_datum *player;
	boolean pressed = FALSE;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		short controller = player->network_player_data.controller_index;
		boolean down;

		if (player->local_player_index == NONE || controller < 0 || controller >= MAXIMUM_GAMEPADS)
			continue;
		down = manual_respawn_jump_down(controller);
		if (!prime && down && !manual_respawn_held[controller] && player->unit_index == NONE)
			pressed = TRUE;
		manual_respawn_held[controller] = down;
	}
	return pressed;
}

/* ---------- public code */

boolean manual_respawn_hold(
	void)
{
	boolean starting;

	manual_respawn_frame++;
	if (!manual_respawn_applies())
	{
		manual_respawn_waiting_now = FALSE;
		return FALSE;
	}
	/* (a wait main.c stopped asking about, a revert or a new map since,
	starts again with what is held now, so a button held through the
	death does not count) */
	starting = !manual_respawn_waiting_now || manual_respawn_asked_frame + 1 < manual_respawn_frame;
	manual_respawn_asked_frame = manual_respawn_frame;
	if (starting)
	{
		manual_respawn_waiting_now = TRUE;
		manual_respawn_pressed(TRUE);
		platform_log("manual respawn: waiting for the jump button (tick %ld)", (long)game_time_get());
		return TRUE;
	}
	/* (not through the pause menu) */
	if (game_time_get_paused())
	{
		manual_respawn_pressed(TRUE);
		return TRUE;
	}
	if (manual_respawn_pressed(FALSE))
	{
		manual_respawn_waiting_now = FALSE;
		platform_log("manual respawn: pressed (tick %ld)", (long)game_time_get());
		return FALSE;
	}
	return TRUE;
}

boolean manual_respawn_applies_now(
	void)
{
	return manual_respawn_applies();
}

boolean manual_respawn_waiting(
	void)
{
	return manual_respawn_waiting_now && manual_respawn_applies();
}

void manual_respawn_draw(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	long font_index = hud_get_font_index();
	struct font_header *font;
	real_argb_color color = *global_real_argb_white;
	rectangle2d bounds;
	wchar_t text[64];
	char key[64];
	short height;

	if (!manual_respawn_waiting() || player_index == NONE || player_get(player_index)->unit_index != NONE ||
		font_index == NONE || game_time_get_paused())
	{
		return;
	}
#ifdef HALO_ANDROID
	(void)key;
	usnprintf(text, NUMBEROF(text) - 1, L"TAP OR PRESS A TO RESPAWN");
#else
	{
		/* (the keyboard's first jump key, as Controls Setup shows it) */
		char const *binding = config_string("controls.jump");
		char first[64];
		char *comma;
		short index;

		snprintf(first, sizeof(first), "%s", binding ? binding : "");
		comma = strchr(first, ',');
		if (comma)
			*comma = 0;
		key[0] = 0;
		if (*first)
			halo_input_shown_name(first, key, sizeof(key));
		for (index = 0; key[index]; index++)
		{
			if (key[index] >= 'a' && key[index] <= 'z')
				key[index] -= 32;
		}
		if (*key)
		{
			/* (the key's name is ASCII or Latin-1, as the menus' fonts have) */
			wchar_t wide[32];

			for (index = 0; key[index] && index < (short)NUMBEROF(wide) - 1; index++)
				wide[index] = (wchar_t)(unsigned char)key[index];
			wide[index] = 0;
			usnprintf(text, NUMBEROF(text) - 1, L"PRESS %s OR A TO RESPAWN", wide);
		}
		else
			usnprintf(text, NUMBEROF(text) - 1, L"PRESS A TO RESPAWN");
	}
#endif
	text[NUMBEROF(text) - 1] = 0;
	font = font_definition_get(font_index);
	height = (short)(font->ascending_height + font->descending_height);
	/* (low in the player's own view, under where the HUD's messages go:
	the view's window, which a split screen divides) */
	bounds = render.camera.window_bounds;
	bounds.y1 = (short)(bounds.y1 - 18);
	bounds.y0 = (short)(bounds.y1 - height - 2);
	/* subtle: the HUD's font, dimmed */
	color.alpha = 0.55f;
	/* draw mode 2: centered */
	draw_string_set_draw_mode(font_index, NONE, 2, 0, &color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

/* ---------- the automated test (debug.manual_respawn_test)

Kills the first local player that many seconds into a map, presses jump
debug.manual_respawn_test_press seconds after the death (0: never), and logs
every two seconds whether the player lives, whether this waits, and how the
world goes on: the living AI bipeds, how far they moved, the projectiles. */
void manual_respawn_test_update(
	void)
{
	static boolean killed = FALSE;
	static boolean pressed = FALSE;
	static long death_tick = 0;
	static long last_report = -1;
	static long last_tick = 0;
	static real_point3d positions[256];
	static long position_ids[256];
	real kill_seconds = (real)config_real("debug.manual_respawn_test");
	real press_seconds = (real)config_real("debug.manual_respawn_test_press");
	long tick = game_time_get();
	long player_index;
	struct player_datum *player;

	if (manual_respawn_test_down > 0)
		manual_respawn_test_down--;
	if (kill_seconds <= 0.f || main_menu_is_active())
		return;
	player_index = local_player_get_player_index(local_player_get_next(NONE));
	if (player_index == NONE)
		return;
	player = player_get(player_index);
	if (tick < last_tick)
		platform_log("manual respawn test: the clock went back %ld -> %ld (a revert)", last_tick, tick);
	last_tick = tick;
	if (!killed && tick >= (long)(kill_seconds * TICKS_PER_SECOND) && player->unit_index != NONE)
	{
		killed = TRUE;
		death_tick = tick;
		platform_log("manual respawn test: killing the player at tick %ld (setting %s, multiplayer %d)",
			tick, manual_respawn_setting() ? "on" : "off", game_engine_running() ? 1 : 0);
		unit_kill(player->unit_index);
	}
	if (killed && !pressed && press_seconds > 0.f && tick >= death_tick + (long)(press_seconds * TICKS_PER_SECOND))
	{
		pressed = TRUE;
		manual_respawn_test_down = 10;
		platform_log("manual respawn test: pressing jump at tick %ld (%.1f s after the death)",
			tick, (tick - death_tick) / (double)TICKS_PER_SECOND);
	}
	if (killed && (last_report < 0 || tick - last_report >= 2 * TICKS_PER_SECOND || tick < last_report))
	{
		struct object_iterator iterator;
		struct biped_datum *biped;
		short living = 0, index = 0;
		real moved = 0.f;
		long projectiles = 0;

		object_iterator_new(&iterator, _object_mask_biped, 0);
		while ((biped = (struct biped_datum *)object_iterator_next(&iterator)) != NULL && index < 256)
		{
			if (biped->unit.player_index != NONE || biped->object.body_vitality <= 0.f)
				continue;
			living++;
			if (position_ids[index] == iterator.index)
			{
				real dx = biped->object.position.x - positions[index].x;
				real dy = biped->object.position.y - positions[index].y;
				real dz = biped->object.position.z - positions[index].z;
				moved += (real)sqrt(dx * dx + dy * dy + dz * dz);
			}
			position_ids[index] = iterator.index;
			positions[index] = biped->object.position;
			index++;
		}
		object_iterator_new(&iterator, _object_mask_projectile, 0);
		while (object_iterator_next(&iterator) != NULL)
			projectiles++;
		platform_log("manual respawn test: tick %ld, %.1f s after the death: player %s, waiting %d, "
			"AI bipeds alive %d moved %.1f wu, projectiles %ld, respawn timer %ld",
			tick, (tick - death_tick) / (double)TICKS_PER_SECOND,
			player->unit_index != NONE ? "ALIVE" : "dead", manual_respawn_waiting() ? 1 : 0,
			living, moved, projectiles, (long)player->respawn_timer);
		last_report = tick;
	}
}
