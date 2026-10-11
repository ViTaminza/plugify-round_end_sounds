# round_end_sounds — music at the end of the round (Plugify port)

Port of the Metamod:Source plugin `cs2-round-end-sounds` (itself a port of
Stimayk/RoundEndSounds). One track per round end (random without repeats, or in order) for every
player who wants it, at his own volume, with a preview menu, plus the "mute the MVP anthem" trick
for players whose music kit would otherwise play over the track.

| | |
|---|---|
| original | `cs2-round-end-sounds` (844 + 665 LOC of C++) |
| config | `configs/round_end_sounds/round_end_sounds.ini` — the original's configuration file (93 `Alko.Track.*` soundevents) |
| phrases | `configs/round_end_sounds/translations/round_end_sounds.phrases.txt` |
| commands | chat `!res`, `!music`; console `mm_res_reload`, `mm_res_kits`, `mm_res_test` |
| cookies | official permissions plugin, names of the original: `RES.Enabled`, `RES.Volume` |
| workshop addons | **3811020435** "Quake 7 Sounds" (`Alko.Track.*` in `soundevents/soundevents_alko.vsndevts`). Add it to `csgo/cfg/plugify/multi_addon_manager.cfg` (`s2_extra_addons`). |
| tests | `test/re_core_tests.cpp` (build/tests/round_end_sounds_tests): config + phrase parsing, the picker, the preview/fade arithmetic, the packed volume parameter |

## What is ported

* the whole config (tracks incl. the short `"Name" "sound"` form, `picture`, `random`,
  `default_enabled`, `default_volume`, `message` chat/center/both/none, `center_time`,
  `connect_hint`, `fade_before_round`, `precache`);
* the menus (`!res`: state, volume, the track list with preview, "repeat the last track"), the
  preview volume window (1 s at the player's volume, the default when his is 0);
* the end-of-round fade towards `CCSGameRules::m_flRestartRoundTime`, using the per-player volume
  channel (`GE_SosSetSoundEventParams`) and `GE_SosStopSoundEvent` for the end;
* muting the win/lose music and the MVP anthem: `CCSGameRules::m_bRoundEndNoMusic` is written
  through the `cs_gamerules` proxy (the rules object is embedded in it, so the write uses the
  `(rules - proxy)` offset with the `__m_pChainEntity` offset of `CCSGameRules` — the equivalent of
  the original's state-change call on the rules proxy), the MVP's
  `CCSPlayerController::m_iMusicKitID` is zeroed and restored on `round_prestart`, and the
  `round_mvp` event's `musickitid` is zeroed (the event hook returns `Changed`, like the original's
  `EventHookResult::Changed`).

## Deliberate differences

1. **Cookies** through the official permissions plugin (same names, loaded by a once-per-second
   `permissions::LoadUser` reconcile).
2. **The utils settings-menu toggle** (`utils_settings`) is not ported (no settings-menu API in the
   Plugify menu layer); `!res` keeps working as before.
3. `mm_res_kits` prints kit/saved-kit/MVP count/name. The original also printed the inventory
   services' `m_unMusicID`; that component is not read (no behaviour depends on it).

## Not ported

* nothing else — every key of the original's config is honoured.

## Needs an in-game check

* the two schema writes above (`m_bRoundEndNoMusic` on the embedded rules object, `m_iMusicKitID`
  on the controller) are the only places where this port touches the engine's network state
  directly; verify that the win/lose music and the MVP anthem stay silent while a track plays
  (listen with a music kit equipped) and that the kit returns on the next round (`mm_res_kits`).
