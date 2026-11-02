# round_end_sounds — music at the end of the round (pure-s2sdk Plugify port)

Port of the Metamod:Source plugin `cs2-round-end-sounds` (itself a port of
Stimayk/RoundEndSounds). One track per round end (random without repeats, or in order) for every
player who wants it, at his own volume, with a preview menu, plus the "mute the MVP anthem" trick
for players whose music kit would otherwise play over the track.

This is the **standalone s2sdk port** (`docs/PLUGIN_STANDARD.md` §7): no `pisexcompat`, no `plgdb`
(the plugin uses no SQL) and no other shared wrapper library. `hudmenu` is an optional functional
dependency only for the player's own menu type (`GetClientMenuType`); without it the default
HUD/button mapping is used. Every guard the
old wrapper provided lives in the plugin itself (`src/map_gate.hpp`, `src/ent_handle.hpp`, the
client/controller checks in `src/plugin.cpp`, the lazy user-message resolution in
`src/sound_volume.cpp`), and the whole plugin is driven by `OnPluginUpdate` — it registers **no**
s2sdk timer (PITFALLS B4/C9).

| | |
|---|---|
| original | `cs2-round-end-sounds` (844 + 665 LOC of C++) |
| config | `configs/round_end_sounds/round_end_sounds.ini` — the original's KeyValues file (93 `Alko.Track.*` soundevents); the direct plugin path is authoritative, raw/addons are fallbacks |
| phrases | `configs/round_end_sounds/translations/round_end_sounds.phrases.txt` |
| language | `<configs>/core.cfg` `ServerLang` → s2sdk `ServerLanguage` → `en` |
| commands | chat `!res`, `!music` (config `commands`; any `!`/`/` trigger or a bare name works); console `mm_res_reload`, `mm_res_kits`, `mm_res_test` (server console only) |
| cookies | official permissions plugin, names of the original: `RES.Enabled`, `RES.Volume` |
| workshop addons | **3811020435** "Quake 7 Sounds" (`Alko.Track.*` in `soundevents/soundevents_alko.vsndevts`). Add it to `csgo/cfg/plugify/multi_addon_manager.cfg` (`s2_extra_addons`). |
| tests | `round_end_sounds_tests` (config/phrases/picker/colour tags/map gate/handles) and `round_end_sounds_runtime_tests` (the real callbacks against recording `__s2sdk_*` fakes: audio, precache, direct-first config/phrase resolution + ServerLang chain, start/end map, slots/handles, menu owner identity and the A7 menu fail-safe, commands and reload teardown) |

The default config/phrases (exact copies of the production files) are bundled in
`configs/round_end_sounds/` and `package.sh` ships them; the deployment script only creates missing
files, so a live server's files are never overwritten.

## File resolution

Both files are looked up in this order; the **first existing file is authoritative** (a malformed
file is reported and never silently replaced by a lower-priority copy):

1. the **direct HEAD paths** (authoritative): `<GetConfigsDir()>/round_end_sounds/round_end_sounds.ini`
   and `<GetConfigsDir()>/round_end_sounds/translations/round_end_sounds.phrases.txt` (a flat plugin
   copy is the second candidate);
2. the raw working-directory name (`round_end_sounds.ini` / `round_end_sounds.phrases.txt`);
3. the Metamod spelling (`addons/configs/round_end_sounds.ini` /
   `addons/translations/round_end_sounds.phrases.txt`);
4. the shared `<GetConfigsDir()>/translations/` and `<GetConfigsDir()>/` roots.

A malformed config keeps the previous configuration on reload (fail soft); a missing/malformed
phrases file falls back to the built-in ru/en table. `core.cfg` is read from
`<GetConfigsDir()>/core.cfg` exactly like pisexcompat did.

## What is ported

* the whole config (tracks incl. the short `"Name" "sound"` form, `picture`, `random`,
  `default_enabled`, `default_volume`, `message` chat/center/both/none, `center_time`,
  `connect_hint`, `fade_before_round`, `precache`);
* the menus (`!res`: state, volume, the track list with preview, "repeat the last track"), the
  preview volume window (1 s at the player's volume, the default when his is 0);
* the end-of-round fade towards `CCSGameRules::m_flRestartRoundTime`, using the per-player volume
  channel (`GE_SosSetSoundEventParams`) and `GE_SosStopSoundEvent` for the end;
* muting the win/lose music and the MVP anthem: `CCSGameRules::m_bRoundEndNoMusic` is written
  through the `cs_gamerules` proxy (the rules object is a separate allocation chained to it, so the
  `m_pGameRules` field is read fresh, the byte is written through the rules chainer and the proxy
  field is marked changed), the MVP's `CCSPlayerController::m_iMusicKitID` and the inventory
  component's `m_unMusicID` are zeroed and restored on `round_prestart`, and the `round_mvp`
  event's `musickitid` is zeroed in place (the Pre-hook returns Continue; no event is ever
  superseded for other plugins);
* the per-player volume of a started soundevent through the s2sdk user-message API. Each message
  type is resolved by its real registry name (`SosStartSoundEvent`, `SosSetSoundEventParams`,
  `SosStopSoundEvent`) on a live map; HEAD resolved them in `OnPluginStart`, before the engine
  registers its network messages. There is **no numeric fallback**: an unresolved type makes that
  functionality a no-op, because s2sdk's `UserMessageCreateFromId` returns a non-null object with
  null internals for an unknown id. The incoming engine message's name is verified; created
  objects are never introspected.

## Menu backend

`s2sdk::CreateMenu` is used directly. The player's own choice is read through the optional
`hudmenu::GetClientMenuType` (`hud`, `wasd`, `button`, `centerhtml`, `chat`); `hud`/`wasd` are used
only while their backends are registered, and an absent/stale answer falls back to the Pisex
HUD_LAYOUT mapping (`hud` when registered, otherwise the built-in `button`). Without the hudmenu
plugin the fallback mapping is used. Menus are destroyed one frame after they close, outside s2sdk's
menu-manager handler (PITFALLS C10); the map-end/unload path uses the A7 fail-safe above.

## Deliberate differences from HEAD

1. **No pisexcompat/plgdb.** hudmenu is only an optional API for the player's menu type. Cookies
   still go through the official permissions plugin with the same names (the user is loaded by the
   same once-per-second reconcile).
2. **The utils settings-menu toggle** (`utils_settings`) is not ported (it needed the Pisex
   settings-menu API); `!res` keeps working as before.
3. **A malformed config keeps the previous configuration on reload** instead of resetting the
   in-memory defaults (fail soft; a first start still uses the defaults when the file is missing).
4. **Menu shape**: the main menu keeps its four items; the volume/track sub-menus use the s2sdk exit
   button plus an explicit `Back` item (s2sdk offers either "exit" or "exit-back", not both like
   Pisex). Phrase keys and texts are unchanged; `Back` is a built-in phrase that a production
   phrases file may override.
5. `mm_res_kits` prints kit/saved-kit/MVP count/name, like HEAD (the original also printed the
   inventory services' `m_unMusicID`; HEAD already did not).
6. **Menu lifecycle fail-safe**: all of CreateMenu/AddMenuItem/DisplayMenu/DestroyMenu/
   GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType must exist before any menu (including
   the reserved id 0); the map-end/unload path cancels with the Disconnect reason, re-checks the
   session, and if it still refuses to end switches the menu to the builtin `chat`/`console`
   type with a no-op Close (verified with GetMenuType) before DestroyMenu. If even that is
   impossible it logs `A7_UNRESOLVED` and leaves the callback alone rather than running a custom
   backend Close over the dying client layer.

## Not ported

* nothing else — every key of the original's config is honoured.

## Needs an in-game check

* the schema writes above (`m_bRoundEndNoMusic` on the rules object, `m_iMusicKitID` /
  `m_unMusicID` on the controller/inventory component) are the only places where this port touches
  the engine's network state directly; verify that the win/lose music and the MVP anthem stay
  silent while a track plays (listen with a music kit equipped) and that the kit returns on the
  next round (`mm_res_kits`).
* the `hud` menu backend is used when hudmenu is installed (or the player's own choice); without
  it the built-in button/center-html menu needs the s2sdk digit commands (`css_1`…), which the
  menu itself prints.
