/*
DESKTOP_HOST_PORT.C

The desktop and web builds run the game and the platform layer in one
process (tools/linux_build.py, tools/windows_build.py, tools/web_build.py).
The map lists' IMPORT and the Steam Workshop screen call host_* functions
that the Android, Switch and PS5 hosts implement (port/android/host).
Replay records use that host's memory too (host_replay.c, linked beside
this file).

These stand-ins keep those menus linkable here: IMPORT and Workshop do
nothing, and report an empty or idle state. Theater playback's store is
host_replay.c, not this file.
*/

#include <stdint.h>
#include <string.h>

/* ---------- MCC import (mcc_import.c, steam_screen.c, mcc_share.c) */

void host_mcc_root(unsigned int path)
{
	(void)path;
}

int host_mcc_import_start(unsigned int root, unsigned int custom_maps)
{
	(void)root;
	(void)custom_maps;
	return 0;
}

int host_mcc_import_state(void)
{
	return 0; /* MCC_IMPORT_IDLE */
}

void host_mcc_import_progress(unsigned int text, unsigned int size)
{
	if (text && size)
		((char *)(uintptr_t)text)[0] = 0;
}

int host_mcc_import_question(unsigned int title, unsigned int title_size, unsigned int destination,
	unsigned int destination_size)
{
	(void)title;
	(void)title_size;
	if (destination && destination_size)
		((char *)(uintptr_t)destination)[0] = 0;
	return 0;
}

void host_mcc_import_answer(int question, int replace)
{
	(void)question;
	(void)replace;
}

int host_mcc_import_finish(unsigned int report, unsigned int size)
{
	if (report && size)
		((char *)(uintptr_t)report)[0] = 0;
	return 0;
}

/* ---------- MCC conversion and fingerprints */

int host_mcc_convert_workers(void)
{
	return 0;
}

int host_mcc_convert_submit(unsigned int request)
{
	(void)request;
	return -1;
}

int host_mcc_convert_wait(int id, int milliseconds)
{
	(void)id;
	(void)milliseconds;
	return 0;
}

unsigned int host_mcc_convert_output_size(int id)
{
	(void)id;
	return 0;
}

void host_mcc_convert_release(int id)
{
	(void)id;
}

void host_mcc_convert_reset(void)
{
}

int host_mcc_audio_read(unsigned int slot, unsigned int offset, unsigned int size, unsigned int destination)
{
	(void)slot;
	(void)offset;
	(void)size;
	(void)destination;
	return 0;
}

int host_mcc_hash_start(unsigned int request)
{
	(void)request;
	return -1;
}

int host_mcc_hash_wait(int id, int milliseconds, unsigned int result)
{
	(void)id;
	(void)milliseconds;
	(void)result;
	return 0;
}

void host_mcc_hash_cancel(int id)
{
	(void)id;
}

/* ---------- Steam Workshop screen (steam_screen.c, mcc_share.c) */

void host_steam_open(unsigned int data_root)
{
	(void)data_root;
}

void host_steam_close(void)
{
}

int host_steam_poll(void)
{
	return 0;
}

int host_steam_count(void)
{
	return 0;
}

int host_steam_view(void)
{
	return 0;
}

int host_steam_first(void)
{
	return 0;
}

void host_steam_set_first(int first)
{
	(void)first;
}

int host_steam_chosen(void)
{
	return -1;
}

void host_steam_set_chosen(int chosen)
{
	(void)chosen;
}

void host_steam_row(int place, unsigned int name, int name_size, unsigned int state, int state_size)
{
	(void)place;
	if (name && name_size > 0)
		((char *)(uintptr_t)name)[0] = 0;
	if (state && state_size > 0)
		((char *)(uintptr_t)state)[0] = 0;
}

void host_steam_details(int place, unsigned int title, int title_size, unsigned int text, int text_size)
{
	(void)place;
	if (title && title_size > 0)
		((char *)(uintptr_t)title)[0] = 0;
	if (text && text_size > 0)
		((char *)(uintptr_t)text)[0] = 0;
}

void host_steam_choose(int place)
{
	(void)place;
}

int host_steam_back(void)
{
	return 1;
}

int host_steam_picture(unsigned int key, unsigned int pixels)
{
	(void)key;
	(void)pixels;
	return 0;
}

int host_steam_text_wanted(unsigned int initial, int initial_size, unsigned int secret)
{
	(void)initial;
	(void)initial_size;
	(void)secret;
	return 0;
}

void host_steam_text_caption(int kind, unsigned int text, int text_size)
{
	(void)kind;
	if (text && text_size > 0)
		((char *)(uintptr_t)text)[0] = 0;
}

void host_steam_text_typed(int kind, unsigned int text, int saved)
{
	(void)kind;
	(void)text;
	(void)saved;
}

int host_steam_qr(unsigned int modules, int capacity, unsigned int serial)
{
	(void)modules;
	(void)capacity;
	(void)serial;
	return 0;
}

int host_steam_shared_fetch(unsigned int data_root)
{
	(void)data_root;
	return 0;
}

int host_steam_shared_state(void)
{
	return 0;
}

int host_steam_install_start(unsigned int data_root, unsigned int id_low, unsigned int id_high)
{
	(void)data_root;
	(void)id_low;
	(void)id_high;
	return 0;
}

int host_steam_install_status(void)
{
	return 0;
}

void host_steam_install_progress(unsigned int progress)
{
	(void)progress;
}

int host_steam_take_rescan(void)
{
	return 0;
}
