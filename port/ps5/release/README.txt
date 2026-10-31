Halo CE Gore for the PS5 (PPSA99501)

The gore build: Halo: Combat Evolved with the gore mod (blood, gibs and
dismemberment), campaign allies and bots, on top of the universal port's PS5
title. It is its own title, PPSA99501 ("Halo CE Gore"), so it installs beside
the universal port's (PPSA99500) and neither replaces the other.

What the console needs: system software 13.60 or lower, jailbroken (the
Relapse exploit, or another kernel exploit, then kstuff); ShadowMountPlus
1.7beta5 or later, which puts homebrew in /data/homebrew on the home screen;
and an FTP server such as ftpsrv. You also need your own copy of the game's
Xbox disc image (.iso or .xiso). None of the game's data is in this zip.

Installing, over FTP from your computer:
1. Copy the PPSA99501 folder from this zip into /data/homebrew.
2. If the universal port is already set up (/data/halo with its maps), that
   is all: this title reads its maps from /data/halo/maps.
   Otherwise make the folder /data/halo and copy the disc image into it, as
   for the universal port, or make /data/halo-gore and copy it there.
3. Start Halo CE Gore from the home screen.

Its own folder is /data/halo-gore (made on the first start if it is not
there): settings (config.toml, which you can edit over FTP), saves, the log
(halo.log), screenshots and the shader caches. The universal port's
settings and saves in /data/halo are not touched. If /data/halo-gore has a
maps folder of its own, or game.maps.path is set in its config.toml, those
maps are used instead of /data/halo/maps. With no maps anywhere, the first
start takes them out of a disc image in /data/halo-gore (about 1.8 GB,
under a minute).

Voice chat talks while the V key is held: to talk with the pad, put
push_to_talk = "Gamepad LB" under [controls] in config.toml.

Updates: the game looks for a newer Halo CE Gore PS5 release (this
repository's ps5-gore-b<number> releases only, never the universal port's)
a few seconds after it starts, and installs it while you play; when a
notification says so, close the game and start it again. To turn it off,
put this in /data/halo-gore/config.toml:
    [update]
    auto = false
To update by hand, copy a newer release's PPSA99501 folder over the old one.

The source: https://github.com/mattymattmattmatt/HaloCE-OpenCE (branch ps5).
The licences are in COPYING and licenses/; NOTICE.txt says where the source is.
