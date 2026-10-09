# round_end_sounds (Plugify)

Music at the end of the round, ported from the Metamod:Source plugin cs2-round-end-sounds (itself a port of Stimayk/RoundEndSounds). Plays one track per round end (random without repeats or in order) to every player who wants it, at his own volume, with a preview menu ('!res'), and mutes the win/lose music and the MVP anthem of the players' music kits while the track is on. Reads configs/round_end_sounds/round_end_sounds.ini (the live server's fleet config: 100 Alko.Track.* soundevents of workshop addon 3811020435) and configs/round_end_sounds/translations/round_end_sounds.phrases.txt; per-player switch and volume live in the official permissions cookies under the Metamod names (RES.Enabled / RES.Volume). Per-player volume of the soundevents is applied with GE_SosSetSoundEventParams. No exported methods.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, permissions (optional), hudmenu (optional)

## Installation
Download `round_end_sounds-1.0.1.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/round_end_sounds`.

## License
GPL-3.0, see LICENSE.

