# round_end_sounds (Plugify)

Music at the end of the round, ported from the Metamod:Source plugin cs2-round-end-sounds (itself a port of Stimayk/RoundEndSounds). Plays one track per round end (random without repeats or in order) to every player who wants it, at his own volume, with a preview menu ('!res'), and mutes the win/lose music and the MVP anthem of the players' music kits while the track is on. Pure s2sdk: no pisexcompat and no wrapper libraries; hudmenu is an optional functional dependency for the player's per-player menu type. Reads configs/round_end_sounds/round_end_sounds.ini (the live server's fleet config: 93 Alko.Track.* soundevents of workshop addon 3811020435) and configs/round_end_sounds/translations/round_end_sounds.phrases.txt, direct plugin files first with raw-cwd/addons fallbacks; the phrase language is <configs>/core.cfg ServerLang -> s2sdk ServerLanguage -> en. Per-player switch and volume live in the official permissions cookies under the Metamod names (RES.Enabled / RES.Volume). Per-player volume of the soundevents is applied with GE_SosSetSoundEventParams. No exported methods.

Original plugin (not a port).

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2), permissions (>=1.3.1, optional), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `round_end_sounds-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/round_end_sounds`.

## License
GPL-3.0, see LICENSE.

