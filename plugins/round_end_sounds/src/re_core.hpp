// round_end_sounds — engine-free core of the port (config, track picking, phrases, player state).
//
// The reference is ref/src/cs2-round-end-sounds (a Metamod port of Stimayk/RoundEndSounds): music
// at the end of the round, with a per-player on/off switch and volume, a preview menu and the
// "mute the MVP anthem" trick for players whose music kit would otherwise play over the track.
//
// Everything that needs the engine (events, sound emission, schema fields) lives in src/plugin.cpp;
// this core keeps the decisions, so test/re_core_tests.cpp can drive it against the real
// production config (ref/prod-27016/addons/configs/round_end_sounds.ini, "fleet" variant with the
// Alko.Track.* soundevents of workshop addon 3811020435).

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace res {

std::string Lower(std::string s);
std::string Trim(const std::string& s);
std::string Replace(std::string s, const std::string& from, const std::string& to);
std::vector<std::string> SplitList(const std::string& s);

// ---------------------------------------------------------------- entity handles

// HANDLE validity of the raw value (no engine access): a handle is invalid only when it is
// INVALID_EHANDLE_INDEX (-1) or 0. A CEntityHandle is a uint32 view (entity index in the low 15
// bits, serial above); a few minutes after the server starts the serial passes 65535, bit 31 gets
// set and every valid handle reads as a NEGATIVE int (live: -921534371 = 0xC912805D = index 93,
// serial 102949). The sign means nothing. 0 is index 0 serial 0 - the world, never a player or a
// weapon - and is also the "none" the handle helpers here return, so it stays invalid.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

struct Track
{
	std::string sName;    // label shown to the players (the config key)
	std::string sSound;   // soundevent name (per-player volume works) or sounds/....vsnd (client play)
	std::string sPicture; // optional image URL for the center message
};

struct Config
{
	bool bEnabled = true;
	std::vector<std::string> vCommands = {"!res", "!music"};
	bool bRandom = true;
	bool bDefaultEnabled = true;
	int iDefaultVolume = 50; // percent
	std::string sMessage = "chat"; // chat | center | both | none
	int iCenterTime = 7;
	bool bMuteMvpAnthem = true;
	float flFadeBeforeRound = 1.0f; // seconds before the next round the track is faded out
	bool bUtilsSettings = true;
	bool bConnectHint = true;
	bool bDebug = false;
	std::vector<std::string> vPrecache;
	std::vector<Track> vTracks;
};

// Parses round_end_sounds.ini (KeyValues text).
bool ParseConfigText(std::string_view text, Config& cfg);

// Records that carry a soundevent (i.e. whose volume can be controlled) as hash -> true.
std::map<uint32_t, bool> BuildOwnEvents(const std::vector<Track>& vTracks, uint32_t (*pfnHash)(std::string_view));
// MurmurHash2 (lower case input, seed 0x53524332) — the engine's soundevent hash.
uint32_t SoundEventHash(std::string_view name);
bool IsFilePathSound(const std::string& sSound);
// The packed GE_SosSetSoundEventParams "volume" parameter (hash + type + size + float).
std::string PackVolumeParam(float flVolume);

// --- track picking (the reference's PickTrack: random without repeats, or in order) -----------
struct TrackPicker
{
	int iLast = -1;
	int iNext = 0;
	std::vector<int> vBag;

	// uRandom is the caller's RNG output (0 when irrelevant).
	int Pick(int nTracks, bool bRandom, uint32_t uRandom);
	void Reset();
};

// --- phrases ---------------------------------------------------------------------------------
struct DefaultPhrase
{
	const char* szKey;
	const char* szRu;
	const char* szEn;
};
const std::vector<DefaultPhrase>& DefaultPhrases();
bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases);
std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey);

// --- per-player state ------------------------------------------------------------------------
struct PlayerData
{
	bool bEnabled = true;
	int iVolume = 50;
};

// Menu service values (the reference's IsService): never treated as an item.
bool IsService(const std::string& sBack);

// Preview window: while it is open the player hears a preview at that volume instead of his own
// setting (the reference keeps flPreviewUntil = curtime + 1 s).
bool IsPreviewActive(float flNow, float flPreviewUntil);
// The volume a preview is played at: the player's own, or the default when his is 0.
int PreviewVolume(int iVolume, int iDefaultVolume);
// Fade factor of the end-of-round fade (1 -> 0 over flFadeBefore seconds), 0 when finished.
float FadeFactor(float flNow, float flFadeFrom, float flFadeBefore);

// --- music kit mute (the two schema fields) ----------------------------------------------------
// A player's music kit lives in two schema fields: CCSPlayerController::m_iMusicKitID (int32) and
// CCSPlayerController_InventoryServices::m_unMusicID (uint16, on the controller's inventory
// component). plugins/WeaponSkins writes both; the live client plays the MVP anthem from the
// inventory one, so the reference's mute of the controller field alone (ref/src/cs2-round-end-sounds,
// MuteMvpKit) stopped being enough as soon as the skin changer could hand out kits.
bool KitHasMusic(int iKit); // 0 and -1 both mean "no kit" (the reference's check)
// The kit the mute acts on: the controller field, or the inventory one when that is empty (0/-1).
int KitEffective(int iControllerKit, int iInventoryKit);
// The value a restore has to write back into one field: the saved kit while the field still reads 0
// (nobody - e.g. the skin changer - has set a kit meanwhile), else 0 meaning "nothing to write".
int KitRestoreValue(int iSaved, int iCurrent);

} // namespace res
