# PS5 port — 5.1 surround: plan

The game mixes 5.1 on the desktop builds (`port/linux/src/dsound_sdl.c`,
`audio.channels`), and the PS5 build mixes it too when asked, but what reaches
the console's audio output is stereo. This is the plan to play the game's 5.1
through the console. It is deferred: nothing below is built yet.

Status: not started. Written after the upstream merge of build 106 (network
version 32), from the findings below.

## What is known

- **The game's side works.** `dsound_sdl.c`'s `choose_output_channels` mixes
  six channels for `audio.channels = "5.1"`, or for `"auto"` when the device
  says it has six or more (`SDL_GetAudioDeviceFormat`). The PS5's guest counts
  as a desktop for the settings (`CONFIG_PLATFORM` in `port_config.c`:
  desktop, guest and PS5), so the setting is there; its default is `"stereo"`.
  The six channels are SDL3's order: FL, FR, FC, LFE, BL, BR (SDL_audio.h:
  the last two can also be the sides).
- **The host plays it through pacbrew's SDL2, which plays stereo at most.**
  `host_sdl_ps5_audio.c` opens an SDL2 device in the game's format and lets
  SDL2 convert. pacbrew's SDL2 (v0.40.2, `SDL_AUDIO_DRIVER_PS5`,
  `SDL_ps5audio.c`) opens `sceAudioOutOpen` with the parameter for mono or
  stereo only, and sets the spec's channels to 1 or 2 whatever was asked
  (disassembled `PS5AUDIO_OpenDevice`: format 16-bit gives parameter
  `channels != 1`, float the same plus 3; then `channels = 2 - (channels ==
  1)`). Six channels asked are folded to stereo by SDL2's conversion.
- **"auto" can never choose 5.1 now.** The guest answers
  `SDL_GetAudioDeviceFormat` with "unknown" (`port/linux/host/guest_desktop.c`,
  added with the AGPL merge, when the call first had to be answered), so
  `"auto"` plays stereo.
- **The console's own audio output is in reach.** The SDK's stubs
  (`PS5_PayloadSDK/sce_stubs/libSceAudioOut.c`) have `sceAudioOutOpen`,
  `sceAudioOutOutput`, `sceAudioOutGetPortState` and
  `sceAudioOutGetSystemState`, among others. The I/O probe opened the port
  directly on the console, 16-bit and float stereo, 256-frame grains at
  48 kHz, with no errors (`probe/results/io-probe.txt`, "audio").
- **What is not known.** Whether the console takes the float 8-channel
  parameters (on the PS4, `FLOAT_8CH` is 5 and `FLOAT_8CH_STD` 7; the two
  order the back and side pairs differently), which order the PS5's `_STD`
  one has, what `sceAudioOutGetPortState` says of a 5.1 receiver, a soundbar
  and a TV, and whether the console downmixes 8 channels itself for a stereo
  output (it should, as for any game).

## Step 1 — the probe

A probe in `port/ps5/probe` (beside the I/O probe), run on the console with a
surround system connected, that:

1. opens the port (system user, main, index 0, 256 frames, 48000 Hz) with
   float 8-channel, each of the two parameters in turn, and says what
   `sceAudioOutOpen` returned;
2. plays a tone on one channel at a time, two seconds each, in the order of
   the channels in memory, and logs which index it is playing, for the person
   listening to say where each one came from (front left, front right,
   centre, LFE, the surround pairs);
3. logs `sceAudioOutGetPortState`'s fields for that port, and
   `sceAudioOutGetSystemState`'s, with the receiver, then again with the TV
   alone (stereo), so that "auto" can be decided from them.

Done when: the parameter that works is known, the speaker each of its eight
indices reaches is written down here, and the port state's field that tells
stereo from surround is found.

## Step 2 — the host plays 5.1 itself

In `host_sdl_ps5_audio.c`, a stream the game opens with six (or eight)
channels skips SDL2:

- **The port.** `sceAudioOutOpen` with the 8-channel float parameter step 1
  chose, 256-frame grains at 48 kHz, as the probe; closed with the stream.
- **The thread.** A native thread of the stream's own that loops
  `sceAudioOutOutput` (which blocks until the port takes the grain: the
  console paces it). For each grain it fills its buffer from the game's
  mixer exactly as SDL2's callback does now: the hand-off to the thread with
  a guest stack (`audio_thread`), `binding->buffer`, `audio_keep`. The
  stream's statistics (callbacks, gaps, hand-offs) stay as they are.
- **The channels.** The game's six (FL, FR, FC, LFE, BL, BR) go to the
  port's eight in the order step 1 found, BL and BR to the pair the
  speakers call the surrounds, the two left over silent.
- **Volume and pause.** `SDL_PauseAudioDevice` and the stream's other calls
  the game makes answer for this port as they do for SDL2's (the thread
  outputs silence while paused).
- Stereo and mono streams keep SDL2, as now: nothing changes for a player on
  stereo.

Done when: a 5.1 run on the console plays each of the game's channels from
its speaker (a level with sounds around the player, and the menus' music),
with no gaps in the statistics, and a stereo run sounds as it does now.

## Step 3 — "auto" decides from the console

- `SDL_GetAudioDeviceFormat` in the guest (`guest_desktop.c`) asks the host,
  which answers from step 1's field of the port state (or the system state):
  2, 6 or 8 channels.
- `audio.channels` defaults to `"auto"` on the PS5 (`port_config.c`, as the
  PS5's own entries are made), so a player with a surround system gets 5.1
  without a setting, and one with a TV gets stereo.
- `"stereo"` and `"5.1"` still force either, for a console that says
  something wrong.

Done when: with the receiver the log says "sound: 5.1 surround" at the start
with no setting, and with the TV alone "sound: stereo".

## Step 4 — the menu

- Audio Setup's channels row (`tools/port_settings.py`, the setting's
  platforms) shows on the PS5, with AUTO, STEREO and 5.1, and its change
  takes effect as it does on the desktop (the stream is opened again).
- `port/ps5/release/README.txt` says the game plays 5.1 through the
  console's audio output.

## Risks

- **The order of the channels.** The two 8-channel parameters differ in
  where the back and side pairs are; only listening tells. Step 1 settles it
  before anything depends on it.
- **The console's downmix.** If the console does not fold 8 channels for a
  stereo output, "auto" must never choose 5.1 for a TV (step 3's port state);
  `"5.1"` forced on a TV would then lose the centre and the surrounds.
- **The grain.** SDL2's device takes the game's 1024-frame period; the port
  takes 256-frame grains. The thread asks the mixer for the period and
  outputs it in four grains, so the game's cadence does not change.
- **3D audio.** The console's Tempest 3D audio (`sceAudioOut2*`) is not part
  of this: the game's 5.1 is a channel mix, played as one.
