/*
HUD_TOGGLE.C
The HUD/reticle visibility cycle, on the gamepad Back (Select) button.

Successive presses step through:
	0	both the HUD and the reticle shown (the stock game)
	1	the HUD hidden, the reticle still shown
	2	both hidden
and back to 0. A press is the button going down (held counts once), and
only in play: not in a menu (the game does not tick there, and Back is the
menus' own), not while paused, not in a cinematic, and not while dead or
on a scripted/neutral camera, where hud_draw_screen draws no interface
either. In split screen the first local player's button steps the one
state, which every local view follows.

While the player is zoomed with a zoom-capable weapon (the pistol, the
sniper rifle, the rocket launcher, a vehicle's gun), the reticle is never
hidden, whatever the state: hud_toggle_reticle_visible is true, and the
weapon interface also draws its zoomed elements (the sniper scope and its
range numbers, which follow the zoom in the weapon HUD tags). Unzooming
returns to the state's own behaviour. The rest of the HUD (health, shield,
motion sensor, ammunition panels, grenades, nav points, damage direction)
follows the state zoomed or not, as hud_draw_screen (hud.c) and
hud_render_weapon_interface (hud_weapon.c) ask here.

Settings: display.hud_toggle (the button at all; false is the stock game),
display.hud_toggle_start (the state a map starts in, 0 to 2).

Note Back is not wholly unused in the stock game: held, it shows the
multiplayer scoreboard (game_engine.c), and Tab is its keyboard twin. A
tap here steps the cycle and the scoreboard only fades for as long as the
button is down, as it did; in single player there is no scoreboard.
*/

#include "cseries.h"
#include "hud_toggle.h"
#include "camera/director.h"
#include "cseries/errors.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/players.h"
#include "input/input.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"

int config_boolean(const char *name);
long config_integer(const char *name);

/* ---------- state */

/* -1 until hud_toggle_reset (or the first update) reads the start state */
static long hud_toggle_state = -1;
static boolean hud_toggle_back_was_down;

static long hud_toggle_start_state(void)
{
	long state = config_integer("display.hud_toggle_start");

	if (state < 0)
		state = 0;
	if (state > 2)
		state = 2;
	return state;
}

static void hud_toggle_log(void)
{
	switch (hud_toggle_state)
	{
	case 1:
		error(_error_silent, "[HUD] hidden, reticle shown");
		break;
	case 2:
		error(_error_silent, "[HUD] hidden, reticle hidden (shown while zoomed)");
		break;
	default:
		error(_error_silent, "[HUD] shown, reticle shown");
		break;
	}
}

/* ---------- public code */

void hud_toggle_reset(void)
{
	hud_toggle_state = hud_toggle_start_state();
	hud_toggle_back_was_down = FALSE;
	return;
}

void hud_toggle_update(void)
{
	short local_player_index;
	long player_index;
	struct player_datum *player;
	const struct gamepad_state *gamepad;
	boolean down;
	boolean pressed;
	director_perspective perspective;

	if (hud_toggle_state < 0)
		hud_toggle_reset();

	local_player_index = local_player_get_next(NONE);
	if (local_player_index == NONE || !input_has_gamepad(local_player_index))
		return;
	gamepad = input_get_gamepad_state(local_player_index);
	if (!gamepad)
		return;

	/* edge, not level: the button's own tick count (input_xbox.c) is 1 on
	its first tick down, but polls and ticks need not line up, so the
	previous poll is kept too */
	down = gamepad->buttons[_gamepad_binary_button_back] > 0;
	pressed = down && !hud_toggle_back_was_down;
	hud_toggle_back_was_down = down;
	if (!pressed || !config_boolean("display.hud_toggle"))
		return;

	/* in play only (see the head comment) */
	if (cinematic_in_progress() || game_time_get_paused())
		return;
	if (global_scenario_get() && global_scenario_get()->type == _scenario_type_main_menu)
		return;
	player_index = local_player_get_player_index(local_player_index);
	if (player_index == NONE)
		return;
	player = player_get(player_index);
	if (!player || player->unit_index == NONE)
		return;
	perspective = director_get_perspective(local_player_index);
	if (perspective == _director_perspective_neutral ||
		perspective == _director_perspective_scripted)
		return;

	hud_toggle_state = (hud_toggle_state + 1) % 3;
	hud_toggle_log();
	return;
}

boolean hud_toggle_hud_visible(void)
{
	if (hud_toggle_state < 0)
		hud_toggle_reset();
	return !config_boolean("display.hud_toggle") || hud_toggle_state == 0;
}

boolean hud_toggle_reticle_visible(
	short local_player_index)
{
	if (hud_toggle_state < 0)
		hud_toggle_reset();
	if (!config_boolean("display.hud_toggle") || hud_toggle_state != 2)
		return TRUE;
	/* zoomed: the reticle (and the scope drawn with it) is never hidden */
	return player_control_get_zoom_level(local_player_index) != NONE;
}

