// round_end_sounds — Plugify C++ port of the Metamod:Source plugin cs2-round-end-sounds
// (itself a port of Stimayk/RoundEndSounds): a track per round end, played to every player who
// wants it, with his own volume, a preview menu and the "mute the MVP anthem" trick.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard library and no other shared
// wrapper. hudmenu is an optional functional dependency only for the player's own menu type
// (hudmenu::GetClientMenuType, exactly like pisexcompat); without it the default HUD/button menu
// type is used. Every safety rule the old wrapper enforced lives in this plugin:
//
//   * B1/B2 map gate (src/map_gate.hpp): the OnMapStart/OnMapEnd listeners are registered in
//     OnPluginStart and the gate opens on OnMapStart - or on the first OnPluginUpdate when the
//     plugin was loaded/reloaded into a running map, which never sees an OnMapStart - and drops
//     on OnMapEnd. Every client, entity and schema call asks Ready() first, so none can reach
//     the torn-down client list of a changelevel. OnPluginEnd removes the listeners and closes
//     the gate (PITFALLS A7).
//   * C2 client checks: 0 <= slot < 64, IsClientConnected && IsClientInGame && !IsClientSourceTV;
//     the controller is PlayerSlotToEntPointer() validated with IsValidEntPointer() plus
//     PlayerSlotToEntHandle() validated with IsValidEntHandle(). The plugin refuses the 0/-1
//     handle values itself, because a negative handle (serial into bit 31) may be perfectly
//     valid (src/ent_handle.hpp). Every s2sdk binding slot is checked for null as well, since the
//     generated wrappers call theirs unchecked.
//   * C8 schema offsets are resolved only while the gate is open, only successes are cached, and
//     the cache is dropped on OnMapEnd/OnPluginEnd. The non-entity CCSGameRules and
//     CCSPlayerController_InventoryServices objects are used through the pointer API only in the
//     same tick as the entity that owns them was validated (C3/C6).
//   * C11/C12: the original's chat names (`!res`, `!music` by default) are registered on the
//     stripped command name - s2sdk strips the configured chat trigger before its command lookup,
//     so `!res` and `/res` both reach `res`; a configured trigger-less name additionally gets a
//     say/say_team listener, exactly like pisexcompat did. The engine console commands
//     mm_res_reload / mm_res_kits / mm_res_test are client-executable but ignore any caller >= 0
//     (the original's `iSlot != -1` guard). AddConsoleCommand ownership is tracked: a command the
//     plugin created is removed with RemoveCommand, a pre-existing one only loses its listener
//     (RemoveCommand would unregister callbacks that are not ours).
//   * C13: the six game-event hooks and the map/manifest listeners are registered once and removed
//     symmetrically in OnPluginEnd. Every event reads player-typed fields with
//     GetEventPlayerSlot and numeric fields with GetEventInt. Every Pre-hook answers Continue, so
//     this plugin never frees or cancels an event for the engine or for another plugin; the
//     round_mvp anthem mute is the in-place musickitid write (HEAD's Changed was inert in s2sdk:
//     OnFireEvent derives Changed only from the event's dont_broadcast field).
//   * B4/C9: the plugin registers NO s2sdk timer at all. s2sdk's TimerSystem::RunFrame holds a
//     non-recursive mutex across the callback; gameplay work (cookie reconcile, fade, deferred
//     menu actions, connect hints) is driven by OnPluginUpdate instead - a plain host callback
//     with no timer lock held. Menus are destroyed one frame later, outside s2sdk's menu-manager
//     handler (PITFALLS C10).
//
// Behaviour kept 1:1 with HEAD (the pisexcompat port), keys, defaults and side effects included:
//   * configs/round_end_sounds/round_end_sounds.ini (KeyValues; every key of the original) and
//     translations/round_end_sounds.phrases.txt are resolved through HEAD's DIRECT plugin paths
//     first (the production files), then the raw cwd and the Metamod addons/... spellings, then the
//     shared configs roots; the first existing file is authoritative. A malformed direct file is
//     reported and the previous configuration/phrase fallback stays, never a lower-priority decoy.
//   * phrase language: <configs>/core.cfg "ServerLang" -> s2sdk::GetServerLanguage -> "en",
//     normalized exactly like pisexcompat; the built-in ru/en table fills a missing phrase.
//   * per-player cookies stay the official permissions plugin's names RES.Enabled / RES.Volume,
//     loaded by the same once-per-second reconcile loop (no OnClientAuthenticated dependence).
//   * soundevents are emitted on the player's own pawn (EmitSound), file tracks through the
//     client's `play` command, and the per-player volume of the GE_SosStartSoundEvent is applied
//     with GE_SosSetSoundEventParams built through the s2sdk user-message API
//     (src/sound_volume.cpp). The ids are resolved lazily on the first live map and verified
//     against the hooked message's name, because resolving them in OnPluginStart happens before
//     the engine registers its network messages (the live "Could not find user message" logs).
//   * the win/lose music is muted through CCSGameRules::m_bRoundEndNoMusic (the rules object is a
//     separate allocation chained to the cs_gamerules proxy, so the proxy's m_pGameRules field is
//     read fresh, the byte is written through the rules chainer and the proxy field is marked
//     changed); the MVP anthem is muted by zeroing CCSPlayerController::m_iMusicKitID and the
//     inventory component's m_unMusicID, and restored on the next round_prestart only while the
//     field still reads 0 (WeaponSkins may have set its own kit meanwhile).
//   * OnPluginStart never returns Failed (PITFALLS A1); every callback is wrapped so nothing
//     unwinds into s2sdk.

#include "ent_handle.hpp"
#include "kv1.hpp"
#include "map_gate.hpp"
#include "re_core.hpp"
#include "sound_volume.hpp"
#include "text.hpp"

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <hudmenu/hudmenu.hpp>
#include <permissions/permissions.hpp>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* kLogTag = "[RoundEndSounds]";
// MAXPLAYERS of the original: slots 0..63 are players.
constexpr int kMaxSlots = 64;
// <GetConfigsDir()>/round_end_sounds/... (PLUGIN_STANDARD §3).
constexpr const char* kPluginDir = "round_end_sounds";
constexpr const char* kTranslationsDir = "translations";
constexpr const char* kConfigFile = "round_end_sounds.ini";
constexpr const char* kPhrasesFile = "round_end_sounds.phrases.txt";
// The shared Metamod core config pisexcompat read "ServerLang" from.
constexpr const char* kCoreConfigFile = "core.cfg";
// Pisex's menu timeout (pisexcompat's default); the s2sdk menu closes itself after it.
constexpr int kMenuTimeoutSeconds = 60;
// s2sdk's chat/centerhtml backends show five items per page, like cs2-menus.
constexpr int kMenuItemsPerPage = 5;
// A deferred action belongs to a player, not to the slot: capture the steamid when queued and
// re-check it when it runs (a changelevel or a reconnect can hand the slot to someone else).
struct PendingHint
{
	int iSlot = -1;
	uint64_t uSteam = 0;
	int32_t iControllerHandle = -1;
};

struct PendingMenuAction
{
	s2sdk::MenuId id = 0;
	int iSlot = -1;
	uint64_t uSteam = 0;
	int32_t iControllerHandle = -1;
	std::string sAction;
};

// The track that is playing for a player right now (the reference's Playing).
struct Playing
{
	int iGuid = -1;
	float flVolume = 1.0f;
	float flStarted = 0.0f;
	bool bPreview = false;
	float flFadeFrom = -1.0f;
	float flNextStep = 0.0f;
};

// ---------------------------------------------------------------------------------------------
// State (all of it is reset by OnPluginStart / OnPluginEnd, so `plg reload` is clean)
// ---------------------------------------------------------------------------------------------

res::Config g_Cfg;
std::map<std::string, std::string> g_mapPhrases;
std::string g_sServerLang = "en";
std::map<uint32_t, bool> g_mapOwnEvents;

std::array<res::PlayerData, kMaxSlots> g_Players{};
std::array<float, kMaxSlots> g_flPreviewVolume{};
std::array<float, kMaxSlots> g_flPreviewUntil{};
std::array<Playing, kMaxSlots> g_Playing{};

res::TrackPicker g_Picker;
int g_iLastTrack = -1;
std::mt19937 g_Rng{std::random_device{}()};

std::array<int32_t, kMaxSlots> g_iSavedKit{};          // m_iMusicKitID that was zeroed (0: nothing to restore)
std::array<int32_t, kMaxSlots> g_iSavedInventoryKit{}; // ..._InventoryServices::m_unMusicID zeroed with it
std::array<bool, kMaxSlots> g_bKitMuted{};             // a kit is zeroed for this slot until round_prestart
std::array<uint64_t, kMaxSlots> g_iSavedKitSteam{};

std::array<uint64_t, kMaxSlots> g_uLoadedSteam{};
std::array<int32_t, kMaxSlots> g_iLoadedController{};
float g_flReconcileTimer = 0.0f;
bool g_bReady = false;
bool g_bUnloading = false;
int64_t g_iPluginId = 0;

res::MapGate g_mapGate;

// Deferred work queues (processed from OnPluginUpdate, never from an engine/timer/menu callback).
std::deque<PendingHint> g_pendingHints;
std::deque<PendingMenuAction> g_pendingMenuActions;

// Flags shared by the map listeners, the schema layer and the teardown paths.
bool g_bRulesWarned = false;
bool g_bMuteLogged = false;
bool g_bResolveTriedThisMap = false;
bool g_bWrongSoundMessageLogged = false;
bool g_bSoundUnresolvedWarned = false;
bool g_bSoundAvailable = false;
int g_iHookedStartId = -1;
int g_iFailedHookId = -1;

// Menu state (s2sdk menu API directly; handlers only queue work, see OnMenuHandler).
struct MenuEntry
{
	std::string sAction;
	std::string sText;
	bool bDisabled = false;
};

struct MenuRecord
{
	int iSlot = -1;
	uint64_t uSteam = 0;          // owner identity when the menu was created
	int32_t iControllerHandle = -1; // owner's controller CEntityHandle (serial blocks slot reuse)
	std::vector<std::string> vActions;
};

std::map<s2sdk::MenuId, MenuRecord> g_menus;
std::deque<s2sdk::MenuId> g_menuDestroy;
bool g_bReservedMenuZero = false;
bool g_menuIdZeroChecked = false;
bool g_bMenuBindingsWarned = false;
bool g_bMenuTypeWarned = false;


// ---------------------------------------------------------------------------------------------
// Logging / small helpers
// ---------------------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity)
{
	if (plg::Log) plg::Log(std::string(message), severity, plg::source_location::current());
}

void LogInfo(std::string_view message) { LogMessage(message, plg::Severity::Info); }
void LogWarn(std::string_view message) { LogMessage(message, plg::Severity::Warning); }
void LogError(std::string_view message) { LogMessage(message, plg::Severity::Error); }

void Debug(const std::string& message)
{
	if (g_Cfg.bDebug) LogMessage(std::string(kLogTag) + " [DEBUG] " + message, plg::Severity::Info);
}

std::string ToStd(const plg::string& text)
{
	return std::string(text.data(), text.size());
}

// ---------------------------------------------------------------------------------------------
// Paths (the old pisexcompat candidate order, kept locally)
// ---------------------------------------------------------------------------------------------

std::string JoinPath(std::string_view a, std::string_view b)
{
	if (a.empty()) return std::string(b);
	if (b.empty()) return std::string(a);
	std::string out(a);
	if (out.back() != '/') out.push_back('/');
	if (b.front() == '/') b.remove_prefix(1);
	out.append(b);
	return out;
}

std::string ConfigsRoot()
{
	if (plg::GetConfigsDir == nullptr) return {};
	return ToStd(plg::GetConfigsDir());
}

std::string PluginConfigDir() { return JoinPath(ConfigsRoot(), kPluginDir); }
std::string PluginTranslationsDir() { return JoinPath(PluginConfigDir(), kTranslationsDir); }

bool FileExists(const std::string& path)
{
	if (path.empty()) return false;
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::string ReadFile(const std::string& path, bool& ok)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		ok = false;
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	ok = true;
	return buffer.str();
}

// HEAD read the config directly as <ConfigsDir>/round_end_sounds/round_end_sounds.ini (a plain
// pisex::ReadFile) and the phrases as <ConfigsDir>/round_end_sounds/translations/. Those DIRECT
// plugin paths are the production files and stay first: a decoy in the server working directory
// must never replace the configured file. Only then come the compatibility fallbacks (raw cwd, the
// Metamod addons/ spellings and the shared configs roots). First existing wins and is never
// replaced by a lower-priority copy (a malformed first file is reported, not silently shadowed).
std::vector<std::string> ConfigCandidates()
{
	std::vector<std::string> candidates;
	candidates.push_back(JoinPath(PluginConfigDir(), kConfigFile));      // HEAD's direct path
	candidates.push_back(JoinPath(PluginTranslationsDir(), kConfigFile)); // plugin-local fallback
	candidates.emplace_back(kConfigFile);                                 // raw cwd
	candidates.emplace_back(std::string("addons/configs/") + kConfigFile); // Metamod spelling
	candidates.push_back(JoinPath(ConfigsRoot(), kConfigFile));           // shared flat
	return candidates;
}

std::vector<std::string> PhrasesCandidates()
{
	std::vector<std::string> candidates;
	candidates.push_back(JoinPath(PluginTranslationsDir(), kPhrasesFile)); // HEAD's direct path
	candidates.push_back(JoinPath(PluginConfigDir(), kPhrasesFile));       // plugin-local fallback
	candidates.emplace_back(kPhrasesFile);                                 // raw cwd
	candidates.emplace_back(std::string("addons/translations/") + kPhrasesFile); // Metamod spelling
	candidates.push_back(JoinPath(ConfigsRoot(), std::string(kTranslationsDir) + "/" + kPhrasesFile));
	candidates.push_back(JoinPath(ConfigsRoot(), kPhrasesFile));
	return candidates;
}

std::string FirstExistingOr(const std::vector<std::string>& candidates, std::size_t fallback)
{
	for (const std::string& candidate : candidates)
		if (!candidate.empty() && FileExists(candidate)) return candidate;
	return fallback < candidates.size() ? candidates[fallback] : std::string();
}

// The Metamod core setting pisexcompat read first: <configs>/core.cfg "ServerLang" ("ru",
// "russian", ...). "" when the file is missing, malformed or has no ServerLang.
std::string CoreConfigLanguage()
{
	// pisexcompat read this one directly under <GetConfigsDir()> and did not fall back to the cwd;
	// without a Plugify configs root there is no core.cfg to honour.
	if (plg::GetConfigsDir == nullptr) return {};
	const std::string configsRoot = ConfigsRoot();
	if (configsRoot.empty()) return {};
	const std::string path = JoinPath(configsRoot, kCoreConfigFile);
	if (!FileExists(path)) return {};
	const std::optional<res::KvNode> root = res::LoadKv1File(path);
	if (!root) return {};
	return std::string(root->GetString("ServerLang", ""));
}

// s2sdk's ServerLanguage (envs/plugins/s2sdk/settings.jsonc); a plain config read, safe at start.
std::string ServerLanguage()
{
	if (__s2sdk_GetServerLanguage == nullptr) return {};
	try
	{
		return ToStd(s2sdk::GetServerLanguage());
	}
	catch (...)
	{
		return {};
	}
}

// <configs>/core.cfg "ServerLang" first, then s2sdk's ServerLanguage, then "en" - the chain
// pisexcompat's PhrasesLanguage used (and therefore the language the HEAD port resolved phrases in).
std::string PhrasesLanguage()
{
	if (const std::string core = CoreConfigLanguage(); !core.empty())
		return res::NormalizeLanguage(core);
	if (const std::string server = ServerLanguage(); !server.empty())
		return res::NormalizeLanguage(server);
	return "en";
}

// ---------------------------------------------------------------------------------------------
// Config / phrases
// ---------------------------------------------------------------------------------------------

bool LoadConfig()
{
	const std::vector<std::string> candidates = ConfigCandidates();
	const std::string path = FirstExistingOr(candidates, 0);
	bool ok = false;
	const std::string text = ReadFile(path, ok);
	if (!ok)
	{
		LogError(std::string(kLogTag) + " failed to read " + path + " - keeping the current configuration");
		return false;
	}
	res::Config parsed;
	if (!res::ParseConfigText(text, parsed))
	{
		LogError(std::string(kLogTag) + " failed to parse " + path + " - keeping the current configuration");
		return false;
	}
	g_Cfg = std::move(parsed);
	g_mapOwnEvents = res::BuildOwnEvents(g_Cfg.vTracks, &res::SoundEventHash);
	g_iLastTrack = -1;
	g_Picker.Reset();
	return true;
}

void LoadPhrases()
{
	g_mapPhrases.clear();
	g_sServerLang = PhrasesLanguage();
	if (g_sServerLang.empty()) g_sServerLang = "en";
	const std::string path = FirstExistingOr(PhrasesCandidates(), 0);
	bool ok = false;
	const std::string text = ReadFile(path, ok);
	if (!ok)
	{
		LogWarn(std::string(kLogTag) + " failed to read " + path + " - using the built-in phrases");
		return;
	}
	if (!res::ParsePhrasesText(text, g_sServerLang, g_mapPhrases))
	{
		g_mapPhrases.clear();
		LogWarn(std::string(kLogTag) + " failed to parse " + path + " - using the built-in phrases");
	}
}

std::string Phrase(const std::string& sKey)
{
	return res::Phrase(g_mapPhrases, g_sServerLang, sKey);
}

void PrecacheAll()
{
	if (__s2sdk_Precache == nullptr) return;
	for (const std::string& s : g_Cfg.vPrecache)
		if (!s.empty()) s2sdk::Precache(plg::string(s));
	// the sounds/... files of the tracks are precached too, without precacher.ini entries
	for (const res::Track& t : g_Cfg.vTracks)
		if (res::IsFilePathSound(t.sSound)) s2sdk::Precache(plg::string(t.sSound));
}

void OnBuildManifest()
{
	// A callback of s2sdk: nothing may unwind out of here.
	try
	{
		PrecacheAll();
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " precache failed: " + ex.what());
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " precache failed with an unknown exception");
	}
}

// ---------------------------------------------------------------------------------------------
// s2sdk map listeners (PITFALLS B1/B2)
// ---------------------------------------------------------------------------------------------

void ResetMapScopedState()
{
	for (int i = 0; i < kMaxSlots; ++i)
	{
		g_Playing[i] = Playing();
		g_flPreviewUntil[i] = -1.0f;
		g_flPreviewVolume[i] = 0.0f;
	}
	g_pendingHints.clear();
	g_pendingMenuActions.clear();
}

void QueueMenuDestroy(s2sdk::MenuId id);
void ResetSchemaOffsets();
bool MenuSessionBindingsAvailable();
bool EndMenuSessionsForSafeDestroy(s2sdk::MenuId id);
bool DestroyMenuNow(s2sdk::MenuId id);

void CloseMenusForMapEnd()
{
	// Map end: no client/entity call may happen from here. CancelClientMenu()/GetClientMenu()/
	// SetMenuType() only touch the menu manager; Cancel(Disconnect) makes s2sdk skip the backend's
	// Close, so a displayed menu can be freed immediately instead of surviving the changelevel.
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(g_menus.size() + g_menuDestroy.size());
	for (const auto& [id, record] : g_menus)
	{
		(void)record;
		if (id != 0) ids.push_back(id);
	}
	ids.insert(ids.end(), g_menuDestroy.begin(), g_menuDestroy.end());
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	g_menuDestroy.clear();
	g_pendingMenuActions.clear();

	const bool bSessions = MenuSessionBindingsAvailable();
	std::vector<s2sdk::MenuId> retry;
	for (const s2sdk::MenuId id : ids)
	{
		if (id == 0) continue;
		if (!bSessions)
		{
			// Without the session bindings the destroy waits for a live map.
			retry.push_back(id);
			continue;
		}
		if (!EndMenuSessionsForSafeDestroy(id))
		{
			// A7_UNRESOLVED (already logged): keep the menu untouched for now. It is retried on the
			// next live map (where DestroyMenu's backend Close is safe again) and by OnPluginEnd;
			// the queue is only drained while the map gate is open.
			retry.push_back(id);
			continue;
		}
		if (!DestroyMenuNow(id))
		{
			retry.push_back(id);
			continue;
		}
		g_menus.erase(id);
	}
	// The Cancel/End callbacks the destroys delivered may have queued the same ids again; keep
	// only the unresolved retries.
	g_menuDestroy.assign(retry.begin(), retry.end());
}

void OnMapStartListener()
{
	// A callback of s2sdk: nothing may unwind out of here. Only state is touched (no client,
	// entity or schema call; B1).
	try
	{
		g_mapGate.OnMapStart();
		ResetMapScopedState();
		g_bMuteLogged = false;
		g_bResolveTriedThisMap = false;
		g_iFailedHookId = -1; // a hook that failed on the previous map is retried here once
	}
	catch (...)
	{
	}
}

void OnMapEndListener()
{
	// PITFALLS B2: only state changes here - no client, entity or schema call.
	try
	{
		g_mapGate.OnMapEnd();
		ResetMapScopedState();
		CloseMenusForMapEnd();
		ResetSchemaOffsets(); // re-resolve on the next map; state only, no engine call here
		g_bResolveTriedThisMap = false;
	}
	catch (...)
	{
	}
}

void UnregisterMapListenersBestEffort()
{
	if (__s2sdk_OnMapStart_Unregister != nullptr)
	{
		try { s2sdk::OnMapStart_Unregister(&OnMapStartListener); }
		catch (...) {}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr)
	{
		try { s2sdk::OnMapEnd_Unregister(&OnMapEndListener); }
		catch (...) {}
	}
}

void RegisterMapListeners()
{
	if (g_mapGate.ListenersRegistered()) return;
	// The paired Register+Unregister set is checked before anything is registered: a listener that
	// cannot be unregistered would point into this library after unload (PITFALLS A7).
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr ||
		__s2sdk_OnMapStart_Unregister == nullptr || __s2sdk_OnMapEnd_Unregister == nullptr)
	{
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError(std::string(kLogTag) + " the s2sdk map listener pair is unavailable - the plugin stays inactive");
		return;
	}
	try
	{
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	}
	catch (const std::exception& ex)
	{
		// A half-registered pair would point into this library after unload (PITFALLS A7).
		UnregisterMapListenersBestEffort();
		LogError(std::string(kLogTag) + " registering the map listeners failed: " + ex.what());
		return;
	}
	catch (...)
	{
		UnregisterMapListenersBestEffort();
		LogError(std::string(kLogTag) + " registering the map listeners failed");
		return;
	}
	g_mapGate.SetListenersRegistered(true);
}

void UnregisterMapListeners()
{
	if (!g_mapGate.ListenersRegistered()) return;
	g_mapGate.SetListenersRegistered(false);
	UnregisterMapListenersBestEffort();
}

// ---------------------------------------------------------------------------------------------
// Client / controller / handle gates (PITFALLS C2, C1)
// ---------------------------------------------------------------------------------------------

bool SlotInRange(int iSlot) { return iSlot >= 0 && iSlot < kMaxSlots; }

bool ClientConnected(int iSlot)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot) || __s2sdk_IsClientConnected == nullptr) return false;
	try { return s2sdk::IsClientConnected(iSlot); }
	catch (...) { return false; }
}

bool ClientInGame(int iSlot)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot)) return false;
	if (__s2sdk_IsClientInGame == nullptr) return false;
	try
	{
		if (!s2sdk::IsClientInGame(iSlot)) return false;
		if (__s2sdk_IsClientSourceTV != nullptr && s2sdk::IsClientSourceTV(iSlot)) return false;
	}
	catch (...)
	{
		return false;
	}
	return true;
}

bool HasController(int iSlot)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot)) return false;
	if (!ClientConnected(iSlot) || !ClientInGame(iSlot)) return false;
	if (__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_EntPointerToEntIndex == nullptr) return false;
	try
	{
		void* pController = s2sdk::PlayerSlotToEntPointer(iSlot);
		if (pController == nullptr) return false;
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(pController)) return false;
		if (s2sdk::EntPointerToEntIndex(pController) <= 0) return false;
		if (__s2sdk_PlayerSlotToEntHandle != nullptr && __s2sdk_IsValidEntHandle != nullptr)
		{
			const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
			// 0 and -1 are not controller handles; other negative values may be valid.
			if (!res::IsValidEntityHandleValue(iHandle) || !s2sdk::IsValidEntHandle(iHandle)) return false;
		}
	}
	catch (...)
	{
		return false;
	}
	return true;
}

bool IsFakeClientSlot(int iSlot)
{
	if (!ClientConnected(iSlot) || __s2sdk_IsFakeClient == nullptr) return true;
	try { return s2sdk::IsFakeClient(iSlot); }
	catch (...) { return true; }
}

bool IsHuman(int iSlot) { return HasController(iSlot) && !IsFakeClientSlot(iSlot); }

uint64_t ClientSteamId64(int iSlot)
{
	if (!ClientConnected(iSlot) || __s2sdk_GetClientSteamID64 == nullptr) return 0;
	try { return s2sdk::GetClientSteamID64(iSlot); }
	catch (...) { return 0; }
}

std::string ClientName(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_GetClientName == nullptr) return {};
	try
	{
		std::string name = ToStd(s2sdk::GetClientName(iSlot));
		// s2sdk answers "<unknown>" when it cannot resolve the client; callers treat that as empty.
		if (name == "<unknown>") name.clear();
		return name;
	}
	catch (...)
	{
		return {};
	}
}

// The controller's CEntityHandle, validated again right before a handle-based schema call.
int32_t ControllerHandle(int iSlot)
{
	if (!HasController(iSlot) || __s2sdk_PlayerSlotToEntHandle == nullptr) return -1;
	try
	{
		const int32_t iHandle = s2sdk::PlayerSlotToEntHandle(iSlot);
		if (!res::IsValidEntityHandleValue(iHandle)) return -1;
		if (__s2sdk_IsValidEntHandle != nullptr && !s2sdk::IsValidEntHandle(iHandle)) return -1;
		return iHandle;
	}
	catch (...)
	{
		return -1;
	}
}

bool ClientAuthorized(int iSlot)
{
	if (!ClientConnected(iSlot) || __s2sdk_IsClientAuthorized == nullptr) return false;
	try { return s2sdk::IsClientAuthorized(iSlot); }
	catch (...) { return false; }
}

bool PrintChatLine(int iSlot, const std::string& message)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot) || __s2sdk_PrintToChat == nullptr) return false;
	try
	{
		// The phrase's {TAG} words become the cs2-menus control bytes, like pisex::PrintToChat.
		s2sdk::PrintToChat(iSlot, plg::string(res::Colorize(message)));
		return true;
	}
	catch (...)
	{
		return false;
	}
}

bool PrintCentreHtml(int iSlot, const std::string& html, int iDuration)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot) || __s2sdk_PrintCentreHtml == nullptr) return false;
	try
	{
		s2sdk::PrintCentreHtml(iSlot, plg::string(html), iDuration);
		return true;
	}
	catch (...)
	{
		return false;
	}
}

float GameTime()
{
	if (!g_mapGate.Ready() || __s2sdk_GetGameTime == nullptr) return 0.0f;
	try { return s2sdk::GetGameTime(); }
	catch (...) { return 0.0f; }
}

// ---------------------------------------------------------------------------------------------
// Cookies (official permissions plugin, the Metamod cookie names)
// ---------------------------------------------------------------------------------------------

bool CookieRead(uint64_t uSteamID64, const char* szName, std::string& sOut)
{
	if (!uSteamID64 || __permissions_GetCookie == nullptr) return false;
	try
	{
		plg::any value;
		if (permissions::GetCookie(uSteamID64, plg::string(szName), value) != permissions::Status::Success) return false;
		if (plg::holds_alternative<plg::string>(value))
		{
			const plg::string& s = plg::get<plg::string>(value);
			sOut.assign(s.data(), s.size());
			return true;
		}
		if (plg::holds_alternative<int32_t>(value)) { sOut = std::to_string(plg::get<int32_t>(value)); return true; }
		if (plg::holds_alternative<int64_t>(value)) { sOut = std::to_string(plg::get<int64_t>(value)); return true; }
		if (plg::holds_alternative<bool>(value)) { sOut = plg::get<bool>(value) ? "1" : "0"; return true; }
		return false;
	}
	catch (...)
	{
		return false;
	}
}

void CookieWrite(uint64_t uSteamID64, const char* szName, const std::string& sValue)
{
	if (!uSteamID64 || __permissions_SetCookie == nullptr) return;
	try
	{
		permissions::SetCookie(g_iPluginId, uSteamID64, plg::string(szName), plg::any(plg::string(sValue)),
							   /*dontBroadcast=*/false);
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Player state
// ---------------------------------------------------------------------------------------------

void ResetPlayer(int iSlot)
{
	if (!SlotInRange(iSlot)) return;
	g_Players[iSlot].bEnabled = g_Cfg.bDefaultEnabled;
	g_Players[iSlot].iVolume = g_Cfg.iDefaultVolume;
}

void LoadPlayer(int iSlot)
{
	if (!SlotInRange(iSlot)) return;
	ResetPlayer(iSlot);
	const uint64_t uSteam = ClientSteamId64(iSlot);
	if (!uSteam) return;
	std::string sValue;
	if (CookieRead(uSteam, "RES.Enabled", sValue)) g_Players[iSlot].bEnabled = atoi(sValue.c_str()) != 0;
	if (CookieRead(uSteam, "RES.Volume", sValue)) g_Players[iSlot].iVolume = std::clamp(atoi(sValue.c_str()), 0, 100);
}

void SavePlayer(int iSlot)
{
	if (!SlotInRange(iSlot)) return;
	const uint64_t uSteam = ClientSteamId64(iSlot);
	CookieWrite(uSteam, "RES.Enabled", g_Players[iSlot].bEnabled ? "1" : "0");
	CookieWrite(uSteam, "RES.Volume", std::to_string(g_Players[iSlot].iVolume));
}

void ReconcileCookies(float flDelta)
{
	if (__permissions_LoadUser == nullptr) return;
	g_flReconcileTimer += flDelta;
	if (g_flReconcileTimer < 1.0f) return;
	g_flReconcileTimer = 0.0f;

	for (int i = 0; i < kMaxSlots; ++i)
	{
		if (!HasController(i) || IsFakeClientSlot(i) || !ClientAuthorized(i))
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				g_iLoadedController[i] = 0;
				ResetPlayer(i);
				g_iSavedKit[i] = 0;
				g_iSavedInventoryKit[i] = 0;
				g_bKitMuted[i] = false;
				g_Playing[i] = Playing();
				g_pendingMenuActions.erase(
					std::remove_if(g_pendingMenuActions.begin(), g_pendingMenuActions.end(),
								   [i](const PendingMenuAction& action) { return action.iSlot == i; }),
					g_pendingMenuActions.end());
			}
			continue;
		}
		const uint64_t uSteam = ClientSteamId64(i);
		const int32_t iControllerHandle = ControllerHandle(i);
		if (!uSteam || !res::IsValidHandle(iControllerHandle)) continue;
		// Same player on a re-used slot after a reconnect has a new controller serial: reload the
		// cookies for it instead of trusting the previous bearer of the SteamID.
		if (g_uLoadedSteam[i] == uSteam && g_iLoadedController[i] == iControllerHandle) continue;
		const std::string name = ClientName(i);
		try
		{
			if (permissions::LoadUser(g_iPluginId, uSteam, plg::string(name), /*offline=*/false,
									  /*dontBroadcast=*/false) == permissions::Status::Success)
			{
				g_uLoadedSteam[i] = uSteam;
				g_iLoadedController[i] = iControllerHandle;
				LoadPlayer(i);
			}
		}
		catch (...)
		{
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Schema offsets (PITFALLS C8: only while the gate is open, only successes are cached)
// ---------------------------------------------------------------------------------------------

struct FieldOffset
{
	int iValue = -1;
	bool bResolved = false;
};

struct SchemaOffsets
{
	FieldOffset noMusic;     // CCSGameRules::m_bRoundEndNoMusic (bool, 1)
	FieldOffset restart;     // CCSGameRules::m_flRestartRoundTime (float, 4)
	FieldOffset rulesField;  // CCSGameRulesProxy::m_pGameRules (ptr, 8)
	FieldOffset musicKit;    // CCSPlayerController::m_iMusicKitID (int32, 4)
	FieldOffset invServices; // CCSPlayerController::m_pInventoryServices (ptr, 8)
	FieldOffset invMusic;    // ..._InventoryServices::m_unMusicID (uint16, 2)
	FieldOffset mvps;        // CCSPlayerController::m_iMVPs (int32, 4)
	FieldOffset rulesChain;  // CCSGameRules::__m_pChainEntity
	FieldOffset invChain;    // ..._InventoryServices::__m_pChainEntity
	FieldOffset ctrlChain;   // CCSPlayerController::__m_pChainEntity (none on this build)
};

SchemaOffsets g_off;

int OffsetFor(FieldOffset& field, const char* szClass, const char* szField)
{
	if (field.iValue >= 0) return field.iValue;
	if (field.bResolved) return -1; // not in this build; do not spam the lookup every tick
	if (!g_mapGate.Ready() || __s2sdk_GetSchemaOffset == nullptr) return -1;
	field.bResolved = true;
	try
	{
		const int32_t offset = s2sdk::GetSchemaOffset(plg::string(szClass), plg::string(szField));
		if (offset >= 0) field.iValue = offset;
	}
	catch (...)
	{
	}
	return field.iValue;
}

// The chain offset of a class: s2sdk's SafeNetworkStateChanged treats >= 0 as "the chainer sits
// at object + offset", -1 as the entity itself and -2 as an inline component's own
// NetworkStateChanged. 0 is never a real chainer offset (it would read the object's vtable), so a
// non-positive answer becomes `iNone`.
int ChainOffsetFor(FieldOffset& field, const char* szClass, int iNone)
{
	if (field.bResolved) return field.iValue;
	if (!g_mapGate.Ready() || __s2sdk_GetSchemaChainOffset == nullptr) return iNone;
	field.bResolved = true;
	field.iValue = iNone;
	try
	{
		const int32_t offset = s2sdk::GetSchemaChainOffset(plg::string(szClass));
		if (offset > 0) field.iValue = offset;
	}
	catch (...)
	{
	}
	return field.iValue;
}

void ResetSchemaOffsets()
{
	g_off = SchemaOffsets{};
	g_off.invChain.iValue = -2;
	g_bRulesWarned = false;
}

void ResolveRulesFields()
{
	if (!g_mapGate.Ready()) return;
	OffsetFor(g_off.noMusic, "CCSGameRules", "m_bRoundEndNoMusic");
	OffsetFor(g_off.restart, "CCSGameRules", "m_flRestartRoundTime");
	OffsetFor(g_off.rulesField, "CCSGameRulesProxy", "m_pGameRules");
	ChainOffsetFor(g_off.rulesChain, "CCSGameRules", -1);
}

void ResolvePlayerFields()
{
	if (!g_mapGate.Ready()) return;
	OffsetFor(g_off.musicKit, "CCSPlayerController", "m_iMusicKitID");
	OffsetFor(g_off.invServices, "CCSPlayerController", "m_pInventoryServices");
	// Builds differ: this one names the field m_unMusicID, the C# port's fallback is m_nMusicID.
	if (!g_off.invMusic.bResolved)
	{
		g_off.invMusic.bResolved = true;
		int32_t iOffset = -1;
		if (__s2sdk_GetSchemaOffset != nullptr)
		{
			try
			{
				iOffset = s2sdk::GetSchemaOffset(plg::string("CCSPlayerController_InventoryServices"),
												 plg::string("m_unMusicID"));
				if (iOffset < 0)
					iOffset = s2sdk::GetSchemaOffset(plg::string("CCSPlayerController_InventoryServices"),
													 plg::string("m_nMusicID"));
			}
			catch (...)
			{
				iOffset = -1;
			}
		}
		g_off.invMusic.iValue = iOffset;
	}
	OffsetFor(g_off.mvps, "CCSPlayerController", "m_iMVPs");
	ChainOffsetFor(g_off.invChain, "CCSPlayerController_InventoryServices", -2);
	ChainOffsetFor(g_off.ctrlChain, "CCSPlayerController", -1);
}

// ---------------------------------------------------------------------------------------------
// The two music kit fields
// ---------------------------------------------------------------------------------------------

int GetMusicKit(int iSlot)
{
	ResolvePlayerFields();
	if (g_off.musicKit.iValue < 0 || __s2sdk_GetEntData == nullptr) return 0;
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!res::IsValidHandle(iHandle)) return 0;
	try { return (int)s2sdk::GetEntData(iHandle, g_off.musicKit.iValue, /*size=*/4); }
	catch (...) { return 0; }
}

void SetMusicKit(int iSlot, int iKit)
{
	ResolvePlayerFields();
	if (g_off.musicKit.iValue < 0 || __s2sdk_SetEntData == nullptr) return;
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!res::IsValidHandle(iHandle)) return;
	try
	{
		s2sdk::SetEntData(iHandle, g_off.musicKit.iValue, iKit, /*size=*/4, /*changeState=*/true,
						  g_off.ctrlChain.iValue);
	}
	catch (...)
	{
	}
}

// The controller's inventory component (m_pInventoryServices), read fresh from the pointer field in
// the same tick as the controller was validated; never cached across ticks (PITFALLS C3).
void* GetInventoryServices(int iSlot)
{
	ResolvePlayerFields();
	if (g_off.invServices.iValue < 0 || __s2sdk_GetEntData == nullptr) return nullptr;
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!res::IsValidHandle(iHandle)) return nullptr;
	try
	{
		const int64_t iServices = s2sdk::GetEntData(iHandle, g_off.invServices.iValue, (int32_t)sizeof(void*));
		return iServices > 0 ? reinterpret_cast<void*>(static_cast<intptr_t>(iServices)) : nullptr;
	}
	catch (...)
	{
		return nullptr;
	}
}

int GetInventoryMusicKit(int iSlot)
{
	void* pServices = GetInventoryServices(iSlot);
	if (!pServices || g_off.invMusic.iValue < 0 || __s2sdk_GetEntData2 == nullptr) return 0;
	try { return (int)s2sdk::GetEntData2(pServices, g_off.invMusic.iValue, /*size=*/2); }
	catch (...) { return 0; }
}

void SetInventoryMusicKit(int iSlot, int iKit)
{
	void* pServices = GetInventoryServices(iSlot);
	if (!pServices || g_off.invMusic.iValue < 0 || __s2sdk_SetEntData2 == nullptr) return;
	try
	{
		s2sdk::SetEntData2(pServices, g_off.invMusic.iValue, iKit, /*size=*/2, /*changeState=*/true,
						   g_off.invChain.iValue);
	}
	catch (...)
	{
	}
}

int GetMvpCount(int iSlot)
{
	ResolvePlayerFields();
	if (g_off.mvps.iValue < 0 || __s2sdk_GetEntData == nullptr) return 0;
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!res::IsValidHandle(iHandle)) return 0;
	try { return (int)s2sdk::GetEntData(iHandle, g_off.mvps.iValue, /*size=*/4); }
	catch (...) { return 0; }
}

bool MuteKitMusicEnabled()
{
	return g_Cfg.bEnabled && g_Cfg.bMuteMvpAnthem && !g_Cfg.vTracks.empty();
}

// Win/lose music: the client does not play it while CCSGameRules::m_bRoundEndNoMusic is set. The
// rules object is a separate heap allocation chained to the "cs_gamerules" proxy entity: the
// pointer is read fresh from the proxy's m_pGameRules field, the write goes into the rules object
// itself, and the network update goes through the rules chainer - the HEAD port's exact behaviour.
void MuteRoundEndMusic()
{
	if (!g_mapGate.Ready()) return;
	void* pProxy = __s2sdk_GetGameRulesProxy != nullptr ? s2sdk::GetGameRulesProxy() : nullptr;
	if (pProxy == nullptr || __s2sdk_SetEntData2 == nullptr || __s2sdk_EntPointerToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr)
		return;
	// the owner entity is validated here, in the same tick as the pointer reads/writes below
	const int32_t iProxyHandle = s2sdk::EntPointerToEntHandle(pProxy);
	if (!res::IsValidEntityHandleValue(iProxyHandle) || !s2sdk::IsValidEntHandle(iProxyHandle)) return;

	ResolveRulesFields();
	if (g_off.noMusic.iValue < 0)
	{
		if (!g_bRulesWarned)
		{
			g_bRulesWarned = true;
			LogError(std::string(kLogTag) + " CCSGameRules::m_bRoundEndNoMusic not found - only the MVP's music kit "
										   "fields are muted");
		}
		return;
	}

	// The authoritative rules pointer, in this tick: the proxy's m_pGameRules field. s2sdk's cached
	// g_pGameRules is only set on entity create/delete; the fresh read also catches a re-created
	// proxy. Falls back to the cached global when the field offset could not be resolved.
	int64_t iRules = 0;
	if (g_off.rulesField.iValue >= 0 && __s2sdk_GetEntData2 != nullptr)
	{
		try { iRules = s2sdk::GetEntData2(pProxy, g_off.rulesField.iValue, (int32_t)sizeof(void*)); }
		catch (...) { iRules = 0; }
	}
	if (iRules <= 0 && __s2sdk_GetGameRules != nullptr)
	{
		try { iRules = static_cast<int64_t>(reinterpret_cast<intptr_t>(s2sdk::GetGameRules())); }
		catch (...) { iRules = 0; }
	}
	if (iRules <= 0)
	{
		if (!g_bRulesWarned)
		{
			g_bRulesWarned = true;
			LogError(std::string(kLogTag) + " cs_gamerules proxy has no CCSGameRules instance - round end music stays "
										   "unmuted");
		}
		return;
	}
	void* pRules = reinterpret_cast<void*>(static_cast<intptr_t>(iRules));

	// Write m_bRoundEndNoMusic=1 into the rules object. With a resolved chain offset the same call
	// marks the field changed through the chainer at rules + offset (s2sdk's SafeNetworkStateChanged
	// -> ChainNetworkStateChanged -> the proxy entity's NetworkStateChanged with the chain path
	// index); without it the plain write plus the proxy NetworkStateChanged below still reach the
	// clients.
	try
	{
		if (g_off.rulesChain.iValue >= 0)
			s2sdk::SetEntData2(pRules, g_off.noMusic.iValue, 1, /*size=*/1, /*changeState=*/true,
							   g_off.rulesChain.iValue);
		else
			s2sdk::SetEntData2(pRules, g_off.noMusic.iValue, 1, /*size=*/1, /*changeState=*/false,
							   /*chainOffset=*/-2);
	}
	catch (...)
	{
		return;
	}

	// The reference's state-change call on the rules proxy: the chain anchor field is marked changed
	// as well, so the engine re-sends the chained gamerules state.
	if (__s2sdk_NetworkStateChanged != nullptr)
	{
		try { s2sdk::NetworkStateChanged(iProxyHandle, "CCSGameRulesProxy", "m_pGameRules"); }
		catch (...) {}
	}

	// Proof of the write (debug, once per map): read back the byte that was just written.
	if (!g_bMuteLogged)
	{
		g_bMuteLogged = true;
		int64_t iRead = -1;
		if (__s2sdk_GetEntData2 != nullptr)
		{
			try { iRead = s2sdk::GetEntData2(pRules, g_off.noMusic.iValue, /*size=*/1); }
			catch (...) { iRead = -1; }
		}
		Debug("round end music muted: CCSGameRules::m_bRoundEndNoMusic <- 1 (offset " +
			  std::to_string(g_off.noMusic.iValue) + ", read back " + std::to_string(iRead) + "; chain mark offset " +
			  std::to_string(g_off.rulesChain.iValue) + ", proxy m_pGameRules + " +
			  std::to_string(g_off.rulesField.iValue) + ")");
	}
}

// The MVP anthem: when m_iMVPs changes, the client plays the anthem of the MVP's kit. The kit is
// read from the controller field, or from the inventory component when that is empty - the current
// game build plays the anthem from the inventory one (both fields are what plugins/WeaponSkins
// writes). Both fields are saved and zeroed, so neither can start the anthem until the next round.
void MuteMvpKit(int iSlot)
{
	if (!HasController(iSlot)) return;
	const int iKit = GetMusicKit(iSlot);
	const int iInventoryKit = GetInventoryMusicKit(iSlot);
	if (!res::KitHasMusic(res::KitEffective(iKit, iInventoryKit)))
	{
		Debug("MVP slot " + std::to_string(iSlot) + ": no music kit to mute (kit " + std::to_string(iKit) +
			  ", inventory " + std::to_string(iInventoryKit) + ")");
		return;
	}
	if (!g_bKitMuted[iSlot])
	{
		g_bKitMuted[iSlot] = true;
		g_iSavedKit[iSlot] = res::KitHasMusic(iKit) ? iKit : 0;
		g_iSavedInventoryKit[iSlot] = res::KitHasMusic(iInventoryKit) ? iInventoryKit : 0;
		g_iSavedKitSteam[iSlot] = ClientSteamId64(iSlot);
	}
	// Zero every field that holds a kit; 0 and -1 are left alone, they mean "no kit" (the
	// reference skipped them too).
	if (res::KitHasMusic(iKit)) SetMusicKit(iSlot, 0);
	if (res::KitHasMusic(iInventoryKit)) SetInventoryMusicKit(iSlot, 0);
	Debug("MVP slot " + std::to_string(iSlot) + ": music kit " + std::to_string(iKit) + " / inventory " +
		  std::to_string(iInventoryKit) + " -> 0 until the next round");
}

void RestoreKits()
{
	for (int i = 0; i < kMaxSlots; ++i)
	{
		if (!g_bKitMuted[i]) continue;
		const int32_t iKit = g_iSavedKit[i];
		const int32_t iInventoryKit = g_iSavedInventoryKit[i];
		g_bKitMuted[i] = false;
		g_iSavedKit[i] = 0;
		g_iSavedInventoryKit[i] = 0;
		if (!HasController(i)) continue;
		if (ClientSteamId64(i) != g_iSavedKitSteam[i]) continue;
		// the reference's rule, per field: write it back only while it still reads 0 - a kit the
		// skin changer has set meanwhile stays its own
		const int iRestoreKit = res::KitRestoreValue(iKit, GetMusicKit(i));
		if (iRestoreKit != 0) SetMusicKit(i, iRestoreKit);
		const int iRestoreInventoryKit = res::KitRestoreValue(iInventoryKit, GetInventoryMusicKit(i));
		if (iRestoreInventoryKit != 0) SetInventoryMusicKit(i, iRestoreInventoryKit);
	}
}

// ---------------------------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------------------------

void Play(int iSlot, const res::Track& t, int iVolume)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot)) return;
	// a sounds/... path is played by the client itself: the volume is the player's own
	if (res::IsFilePathSound(t.sSound))
	{
		if (__s2sdk_ClientCommand != nullptr) s2sdk::ClientCommand(iSlot, plg::string("play " + t.sSound));
		return;
	}
	if (iVolume <= 0 || __s2sdk_EmitSound == nullptr || __s2sdk_GetClientPawn == nullptr ||
		__s2sdk_EntPointerToEntHandle == nullptr)
		return;
	// a soundevent is played on the listener himself, so it is equally loud anywhere on the map
	void* pPawn = nullptr;
	try { pPawn = s2sdk::GetClientPawn(iSlot); }
	catch (...) { return; }
	if (pPawn == nullptr) return;
	int32_t iHandle = 0;
	try { iHandle = s2sdk::EntPointerToEntHandle(pPawn); }
	catch (...) { return; }
	if (!res::IsValidHandle(iHandle)) return;
	if (__s2sdk_IsValidEntHandle != nullptr)
	{
		try
		{
			if (!s2sdk::IsValidEntHandle(iHandle)) return;
		}
		catch (...)
		{
			return;
		}
	}
	// EmitSound does not apply the volume of a soundevent; the user-message hook does it
	try { s2sdk::EmitSound(iHandle, plg::string(t.sSound), 100, iVolume / 100.0f, 0.0f); }
	catch (...) {}
}

void AnnounceTo(int iSlot, const res::Track& t)
{
	const bool bChat = g_Cfg.sMessage == "chat" || g_Cfg.sMessage == "both";
	const bool bCenter = g_Cfg.sMessage == "center" || g_Cfg.sMessage == "both";
	if (bChat)
		PrintChatLine(iSlot, res::Replace(Phrase("Now_Playing"), "{name}", t.sName));
	if (bCenter)
	{
		const std::string sPicture = t.sPicture.empty() ? "" : "<br><img src='" + t.sPicture + "'/>";
		const std::string sHtml =
			res::Replace(res::Replace(Phrase("Now_Playing_Center"), "{name}", t.sName), "{picture}", sPicture);
		PrintCentreHtml(iSlot, sHtml, g_Cfg.iCenterTime);
	}
}

void OnRoundEnd()
{
	if (!g_mapGate.Ready() || !g_Cfg.bEnabled) return;
	const int iTrack = g_Picker.Pick((int)g_Cfg.vTracks.size(), g_Cfg.bRandom, (uint32_t)g_Rng());
	if (iTrack < 0) return;
	g_iLastTrack = iTrack;
	g_Picker.iLast = iTrack;
	const res::Track& t = g_Cfg.vTracks[iTrack];

	int nPlayed = 0;
	for (int i = 0; i < kMaxSlots; ++i)
	{
		if (!IsHuman(i) || !g_Players[i].bEnabled) continue;
		Play(i, t, g_Players[i].iVolume);
		AnnounceTo(i, t);
		nPlayed++;
	}
	Debug("round end: \"" + t.sName + "\" (" + t.sSound + "), players: " + std::to_string(nPlayed));
}

// The fade towards the next round (the reference's GameFrame): the tick at which the round
// restarts comes from CCSGameRules::m_flRestartRoundTime (a GameTime_t).
void FadeFrame()
{
	if (!g_mapGate.Ready() || g_Cfg.flFadeBeforeRound <= 0.0f) return;
	if (__s2sdk_GetGameRules == nullptr || __s2sdk_GetEntDataFloat2 == nullptr) return;
	void* pRules = nullptr;
	try { pRules = s2sdk::GetGameRules(); }
	catch (...) { return; }
	if (pRules == nullptr) return;
	ResolveRulesFields();
	if (g_off.restart.iValue < 0) return;

	float flRestart = 0.0f;
	try { flRestart = (float)s2sdk::GetEntDataFloat2(pRules, g_off.restart.iValue, 4); }
	catch (...) { return; }
	if (flRestart <= 0.0f) return;

	const float flNow = GameTime();
	if (flNow < flRestart - g_Cfg.flFadeBeforeRound) return;

	for (int i = 0; i < kMaxSlots; ++i)
	{
		Playing& p = g_Playing[i];
		if (p.iGuid < 0) continue;
		if (p.flFadeFrom < 0.0f)
		{
			// only the track that was started before the new round was scheduled is faded
			if (p.bPreview || flRestart <= p.flStarted) continue;
			p.flFadeFrom = flNow;
			p.flNextStep = flNow;
			Debug("slot " + std::to_string(i) + ": fading track (guid " + std::to_string(p.iGuid) + ")");
		}
		if (flNow < p.flNextStep) continue;
		const float k = res::FadeFactor(flNow, p.flFadeFrom, g_Cfg.flFadeBeforeRound);
		if (k <= 0.0f)
		{
			sound_volume::Stop(i, p.iGuid);
			p = Playing();
			continue;
		}
		sound_volume::Set(i, p.iGuid, p.flVolume * k);
		p.flNextStep = flNow + 0.1f;
	}
}

// ---------------------------------------------------------------------------------------------
// Menus (s2sdk menu API directly; deferred outside the menu-manager handler, PITFALLS C10)
// ---------------------------------------------------------------------------------------------

bool IsMenuTypeRegistered(const char* szName)
{
	if (__s2sdk_IsMenuTypeRegistered == nullptr) return false;
	try { return s2sdk::IsMenuTypeRegistered(plg::string(szName)); }
	catch (...) { return false; }
}

// The complete menu lifecycle set is required before ANY menu is created - the reserved id-0
// dummy included. A missing GetClientMenu/CancelClientMenu/SetMenuType/GetMenuType would leave the
// map-end/unload path unable to end a displayed session safely before DestroyMenu (whose backend
// Close would then touch the client layer being torn down, PITFALLS B2).
bool MenuLifecycleBindingsAvailable()
{
	return __s2sdk_CreateMenu != nullptr && __s2sdk_AddMenuItem != nullptr && __s2sdk_DisplayMenu != nullptr &&
		   __s2sdk_DestroyMenu != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_SetMenuType != nullptr &&
		   __s2sdk_GetMenuType != nullptr && __s2sdk_IsMenuTypeRegistered != nullptr;
}

// The player's own "!menu" choice, read through the optional hudmenu plugin exactly like
// pisexcompat's PlayerTypeChoice(). Empty when hudmenu is absent, the slot has no live client or
// the call throws; the default HUD mapping below is used then.
std::string PlayerMenuTypeChoice(int iSlot)
{
	if (__hudmenu_GetClientMenuType == nullptr || !g_mapGate.Ready() || !SlotInRange(iSlot)) return {};
	try { return ToStd(hudmenu::GetClientMenuType(iSlot)); }
	catch (...) { return {}; }
}

// pisexcompat's ResolveS2sdkMenuTypeName with the Pisex default MenuType::HUD_LAYOUT: the player's
// choice wins (all five restored types), hud/wasd only while their backends are registered, then
// the default hud -> wasd -> button mapping. Empty = no usable menu backend (menus disabled).
std::string ResolveMenuType(int iSlot)
{
	const std::string sChoice = PlayerMenuTypeChoice(iSlot);
	if (!sChoice.empty())
	{
		if ((sChoice == "chat" || sChoice == "centerhtml" || sChoice == "button") &&
			IsMenuTypeRegistered(sChoice.c_str()))
			return sChoice;
		if (sChoice == "hud")
			return IsMenuTypeRegistered("hud") ? sChoice : (IsMenuTypeRegistered("button") ? "button" : "");
		if (sChoice == "wasd")
			return IsMenuTypeRegistered("wasd") ? sChoice : (IsMenuTypeRegistered("button") ? "button" : "");
	}
	// Pisex's default MenuType::HUD_LAYOUT: the hud backend, else the built-in WASD "button".
	if (IsMenuTypeRegistered("hud")) return "hud";
	if (IsMenuTypeRegistered("button")) return "button";
	return {};
}

// A no-op handler for the dummy menu that owns the reserved id 0. The menu is never displayed.
void OnReservedMenuHandler(s2sdk::MenuId, s2sdk::MenuAction, int32_t, int32_t) {}

// s2sdk's client-menu state uses id 0 for "no menu", but 0 is also a valid CreateMenu() answer
// (ids start at 0). Reserve it with a dummy nobody can display (pisexcompat's BurnMenuIdZero),
// only once the full lifecycle set exists and a menu can actually be shown.
bool EnsureMenuIdZeroBurned()
{
	if (g_menuIdZeroChecked) return true;
	if (!MenuLifecycleBindingsAvailable()) return false;
	try
	{
		const s2sdk::MenuId id =
			s2sdk::CreateMenu(plg::string("round_end_sounds:reserved-id-0"), &OnReservedMenuHandler,
							  plg::string("chat"));
		if (id == 0)
			g_bReservedMenuZero = true;
		else
			s2sdk::DestroyMenu(id); // id 0 was already taken by another plugin
		g_menuIdZeroChecked = true;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

void QueueMenuDestroy(s2sdk::MenuId id)
{
	if (id == 0) return;
	if (std::find(g_menuDestroy.begin(), g_menuDestroy.end(), id) == g_menuDestroy.end())
		g_menuDestroy.push_back(id);
}

// True when a display session can be ended without the menu backend's Close (which reaches the
// client layer): CancelClientMenu(Disconnect) makes s2sdk skip it.
bool MenuSessionBindingsAvailable()
{
	return __s2sdk_GetClientMenu != nullptr && __s2sdk_CancelClientMenu != nullptr &&
		   __s2sdk_DestroyMenu != nullptr;
}

// A menu type whose backend Close has no client side effect: s2sdk's builtin chat/console close
// callbacks are empty. Used only when a session refuses to end between maps.
bool SwitchMenuToSafeBuiltinType(s2sdk::MenuId id)
{
	if (id == 0 || __s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr) return false;
	for (const char* szType : {"chat", "console"})
	{
		try
		{
			if (!s2sdk::SetMenuType(id, plg::string(szType))) continue;
			if (ToStd(s2sdk::GetMenuType(id)) == szType) return true;
		}
		catch (...)
		{
		}
	}
	return false;
}

// Ends every display session of id so DestroyMenu cannot run a custom backend Close over a dying
// client layer. Returns false (A7_UNRESOLVED, logged) when that cannot be done safely; the caller
// must then leave the menu alone.
bool EndMenuSessionsForSafeDestroy(s2sdk::MenuId id)
{
	if (id == 0) return true;
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr)
	{
		LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
				 " cannot be checked/cancelled (s2sdk session bindings missing); not destroying it");
		return false;
	}
	for (int32_t iSlot = 0; iSlot < kMaxSlots; ++iSlot)
	{
		try
		{
			if (s2sdk::GetClientMenu(iSlot) != id) continue;
			s2sdk::CancelClientMenu(iSlot, s2sdk::MenuCancelReason::Disconnect);
			// Cancel's answer is not trusted: re-check whether the session is really gone.
			if (s2sdk::GetClientMenu(iSlot) != id) continue;
		}
		catch (...)
		{
			// A throwing query is treated as "still displayed".
		}
		if (!SwitchMenuToSafeBuiltinType(id))
		{
			LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
					 " is still displayed but its backend cannot be switched to a builtin type; not destroying it");
			return false;
		}
	}
	return true;
}

bool DestroyMenuNow(s2sdk::MenuId id)
{
	if (id == 0 && !g_bReservedMenuZero) return true;
	if (__s2sdk_DestroyMenu == nullptr) return false;
	try
	{
		if (__s2sdk_IsValidMenu == nullptr || id == 0 || s2sdk::IsValidMenu(id))
		{
			s2sdk::DestroyMenu(id);
			if (id == 0) g_bReservedMenuZero = false;
			return true;
		}
		return true; // already gone
	}
	catch (...)
	{
		return false;
	}
}

// Safe destroy for the map-window / unload path: with a map up the backend Close is exactly what
// hides the UI, so it may run; between maps the sessions are ended with the Disconnect reason (and
// a builtin type switch if necessary) first. Returns false when the menu was left alive (A7).
bool DestroyOneMenuSafely(s2sdk::MenuId id)
{
	if (id == 0)
	{
		if (DestroyMenuNow(0)) return true;
		LogError("A7_UNRESOLVED: reserved menu id 0 cannot be destroyed (s2sdk DestroyMenu missing)");
		return false;
	}
	if (!g_mapGate.Ready() && !EndMenuSessionsForSafeDestroy(id)) return false;
	if (!DestroyMenuNow(id))
	{
		LogError("A7_UNRESOLVED: menu " + std::to_string(id) +
				 " cannot be destroyed (s2sdk DestroyMenu missing/failed); callback left registered");
		return false;
	}
	g_menus.erase(id);
	return true;
}

void ProcessMenuDestroys()
{
	if (g_menuDestroy.empty()) return;
	if (!g_mapGate.Ready() && !g_bUnloading) return; // map-window destroys were handled in OnMapEnd
	const std::deque<s2sdk::MenuId> ids = std::move(g_menuDestroy);
	g_menuDestroy.clear();
	for (const s2sdk::MenuId id : ids)
	{
		if (g_mapGate.Ready())
		{
			if (DestroyMenuNow(id))
				g_menus.erase(id);
			else
				QueueMenuDestroy(id);
		}
		else
		{
			DestroyOneMenuSafely(id);
		}
	}
}

// The menu handler runs under s2sdk's menu-manager lock (and for a timeout inside the timer
// system): it only copies the menu's stored owner identity and action into the deferred queue.
// No client query, callback, cookie write or menu API call happens here.
void OnMenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t /*iSlot*/, int32_t iParam)
{
	try
	{
		if (g_bUnloading) return;
		const auto it = g_menus.find(id);
		if (action == s2sdk::MenuAction::Select)
		{
			if (it == g_menus.end() || iParam < 0 || iParam >= (int)it->second.vActions.size()) return;
			// The owner slot stored at creation, not the slot the callback reports: a menu can
			// only ever act for the player it was created for.
			g_pendingMenuActions.push_back(PendingMenuAction{id, it->second.iSlot, it->second.uSteam,
															 it->second.iControllerHandle,
															 it->second.vActions[iParam]});
			return;
		}
		if (action == s2sdk::MenuAction::Cancel)
		{
			// 0 on a sub-menu is the exit-back button: return to the main menu (the explicit Back
			// item does the same). Exit/Timeout/Disconnect/Interrupted only close the menu.
			if (it != g_menus.end() &&
				iParam == static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack))
				g_pendingMenuActions.push_back(PendingMenuAction{id, it->second.iSlot, it->second.uSteam,
																 it->second.iControllerHandle, "back"});
			QueueMenuDestroy(id);
		}
		else if (action == s2sdk::MenuAction::End)
		{
			QueueMenuDestroy(id);
		}
	}
	catch (...)
	{
	}
}

bool QueueMenuAction(int iSlot, const std::string& sAction)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot)) return false;
	const uint64_t uSteam = ClientSteamId64(iSlot);
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!uSteam || !res::IsValidHandle(iHandle)) return false;
	g_pendingMenuActions.push_back(PendingMenuAction{0, iSlot, uSteam, iHandle, sAction});
	return true;
}

s2sdk::MenuId CreateAndShowMenu(int iSlot, const std::string& sTitle, const std::vector<MenuEntry>& vEntries,
								bool bExitBack)
{
	if (!g_mapGate.Ready() || !IsHuman(iSlot)) return 0;
	if (!MenuLifecycleBindingsAvailable())
	{
		if (!g_bMenuBindingsWarned)
		{
			g_bMenuBindingsWarned = true;
			LogError(std::string(kLogTag) + " the s2sdk menu lifecycle bindings are incomplete "
										   "(Create/Add/Display/Destroy/GetClientMenu/CancelClientMenu/"
										   "SetMenuType/GetMenuType) - the menus are disabled");
		}
		return 0;
	}
	// Capture the owner identity once, at creation: a later slot reuse (new SteamID or a new
	// controller serial) must never be able to act on this menu's selection.
	const uint64_t uSteam = ClientSteamId64(iSlot);
	const int32_t iControllerHandle = ControllerHandle(iSlot);
	if (!uSteam || !res::IsValidHandle(iControllerHandle)) return 0;
	const std::string sMenuType = ResolveMenuType(iSlot);
	if (sMenuType.empty())
	{
		if (!g_bMenuTypeWarned)
		{
			g_bMenuTypeWarned = true;
			LogError(std::string(kLogTag) + " no usable s2sdk menu type is registered - the menus are disabled");
		}
		return 0;
	}
	if (!EnsureMenuIdZeroBurned())
	{
		LogError(std::string(kLogTag) + " could not reserve the s2sdk menu id 0 - the menus are disabled");
		return 0;
	}
	try
	{
		const s2sdk::MenuId id = s2sdk::CreateMenu(plg::string(sTitle), &OnMenuHandler, plg::string(sMenuType));
		if (id == 0)
		{
			LogError(std::string(kLogTag) + " s2sdk refused to create a menu for slot " + std::to_string(iSlot));
			return 0;
		}
		MenuRecord record;
		record.iSlot = iSlot;
		record.uSteam = uSteam;
		record.iControllerHandle = iControllerHandle;
		record.vActions.reserve(vEntries.size());
		for (const MenuEntry& entry : vEntries)
		{
			s2sdk::AddMenuItem(id, plg::string(entry.sAction), plg::string(entry.sText),
							   entry.bDisabled ? s2sdk::MenuItemStyle::Disabled : s2sdk::MenuItemStyle::Default);
			record.vActions.push_back(entry.sAction);
		}
		g_menus[id] = std::move(record);
		if (__s2sdk_SetMenuPagination != nullptr) s2sdk::SetMenuPagination(id, kMenuItemsPerPage);
		if (bExitBack)
		{
			if (__s2sdk_SetMenuExitBackButton != nullptr) s2sdk::SetMenuExitBackButton(id, true);
			if (__s2sdk_SetMenuExitButton != nullptr) s2sdk::SetMenuExitButton(id, false);
		}
		else
		{
			if (__s2sdk_SetMenuExitButton != nullptr) s2sdk::SetMenuExitButton(id, true);
			if (__s2sdk_SetMenuExitBackButton != nullptr) s2sdk::SetMenuExitBackButton(id, false);
		}
		// We let s2sdk close the display on selection and reopen the follow-up menu from
		// OnPluginUpdate (outside the menu-manager lock).
		if (__s2sdk_SetMenuCloseOnSelect != nullptr) s2sdk::SetMenuCloseOnSelect(id, true);
		if (!s2sdk::DisplayMenu(id, iSlot, kMenuTimeoutSeconds))
		{
			LogError(std::string(kLogTag) + " s2sdk could not display a menu on slot " + std::to_string(iSlot));
			g_menus.erase(id);
			DestroyMenuNow(id);
			return 0;
		}
		return id;
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " menu creation failed: " + ex.what());
		return 0;
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " menu creation failed");
		return 0;
	}
}

void ShowMainMenu(int iSlot);
void ProcessPendingMenuActions();

void SetEnabled(int iSlot, bool bOn)
{
	if (!SlotInRange(iSlot)) return;
	g_Players[iSlot].bEnabled = bOn;
	SavePlayer(iSlot);
	PrintChatLine(iSlot, res::Replace(Phrase("State_Set"), "{state}", Phrase(bOn ? "On" : "Off")));
}

bool AnyVolumeTrack() { return !g_mapOwnEvents.empty(); }

void ShowVolumeMenu(int iSlot)
{
	const int iVolume = g_Players[iSlot].iVolume;
	std::vector<MenuEntry> entries;
	entries.push_back({"up", Phrase("Volume_Up"), iVolume >= 100});
	entries.push_back({"down", Phrase("Volume_Down"), iVolume <= 0});
	entries.push_back({"exit", Phrase("Exit"), false});
	CreateAndShowMenu(iSlot, res::Replace(Phrase("Volume_Title"), "{volume}", std::to_string(iVolume)), entries, true);
}

void PreviewTrack(int iSlot, int iTrack)
{
	if (!g_mapGate.Ready() || !SlotInRange(iSlot)) return;
	if (iTrack < 0 || iTrack >= (int)g_Cfg.vTracks.size()) return;
	const res::Track& t = g_Cfg.vTracks[iTrack];
	// previewing works with music switched off too; at volume 0 the default volume is used
	const int iVolume = res::PreviewVolume(g_Players[iSlot].iVolume, g_Cfg.iDefaultVolume);
	g_flPreviewVolume[iSlot] = iVolume / 100.0f;
	g_flPreviewUntil[iSlot] = GameTime() + 1.0f;
	Play(iSlot, t, iVolume);
	PrintChatLine(iSlot, res::Replace(Phrase("Preview"), "{name}", t.sName));
}

void ShowTracksMenu(int iSlot)
{
	std::vector<MenuEntry> entries;
	entries.push_back({"exit", Phrase("Exit"), false});
	for (std::size_t i = 0; i < g_Cfg.vTracks.size(); ++i)
		entries.push_back({"track:" + std::to_string(i), g_Cfg.vTracks[i].sName, false});
	CreateAndShowMenu(iSlot, Phrase("Tracks_Title"), entries, true);
}

void ShowMainMenu(int iSlot)
{
	if (!g_mapGate.Ready() || !IsHuman(iSlot)) return;
	const res::PlayerData& p = g_Players[iSlot];
	std::vector<MenuEntry> entries;
	entries.push_back({"state", res::Replace(Phrase("Menu_State"), "{state}", Phrase(p.bEnabled ? "On" : "Off")), false});
	if (AnyVolumeTrack())
		entries.push_back({"volume", res::Replace(Phrase("Menu_Volume"), "{volume}", std::to_string(p.iVolume)), false});
	entries.push_back({"tracks", Phrase("Menu_Tracks"), g_Cfg.vTracks.empty()});
	entries.push_back({"last", Phrase("Menu_Last"), g_iLastTrack < 0});
	CreateAndShowMenu(iSlot, Phrase("Menu_Title"), entries, false);
}

void ProcessPendingMenuActions()
{
	if (g_pendingMenuActions.empty()) return;
	const std::deque<PendingMenuAction> actions = std::move(g_pendingMenuActions);
	g_pendingMenuActions.clear();
	for (const PendingMenuAction& action : actions)
	{
		if (!g_mapGate.Ready() || !SlotInRange(action.iSlot))
			continue;
		// Re-validate BOTH halves of the owner identity: a reused slot (new SteamID or the same
		// SteamID with a new controller serial) must never receive the previous player's menu
		// action, playback or cookie write.
		if (action.uSteam == 0 || ClientSteamId64(action.iSlot) != action.uSteam ||
			ControllerHandle(action.iSlot) != action.iControllerHandle)
		{
			if (action.id != 0) QueueMenuDestroy(action.id);
			continue;
		}
		const std::string& sAction = action.sAction;
		if (sAction == "main")
			ShowMainMenu(action.iSlot);
		else if (sAction == "state")
		{
			SetEnabled(action.iSlot, !g_Players[action.iSlot].bEnabled);
			ShowMainMenu(action.iSlot);
		}
		else if (sAction == "volume")
			ShowVolumeMenu(action.iSlot);
		else if (sAction == "tracks")
			ShowTracksMenu(action.iSlot);
		else if (sAction == "last")
		{
			if (g_iLastTrack >= 0)
				PreviewTrack(action.iSlot, g_iLastTrack);
			else
				PrintChatLine(action.iSlot, Phrase("No_Last"));
		}
		else if (sAction == "up" || sAction == "down")
		{
			int& v = g_Players[action.iSlot].iVolume;
			v = std::clamp(v + (sAction == "up" ? 10 : -10), 0, 100);
			SavePlayer(action.iSlot);
			ShowVolumeMenu(action.iSlot);
		}
		else if (sAction == "back")
			ShowMainMenu(action.iSlot);
		else if (sAction == "exit")
		{
			// the display is already closed; nothing else to do
		}
		else if (sAction.rfind("track:", 0) == 0)
			PreviewTrack(action.iSlot, atoi(sAction.c_str() + 6));
	}
}

void DestroyAllMenusForUnload()
{
	// The plugin is going away: every menu callback pointer into this library has to be released.
	// With a map up DestroyMenu may run the backend Close (safe: clients are valid). Between maps
	// the session is ended with the Disconnect reason and, if it still refuses to end, switched to
	// a builtin type with a no-op Close and verified before DestroyMenu. If even that is impossible
	// the menu is reported as A7_UNRESOLVED and left registered rather than running a custom
	// backend Close over the dying client layer.
	std::vector<s2sdk::MenuId> ids;
	ids.reserve(g_menus.size() + g_menuDestroy.size());
	for (const auto& [id, record] : g_menus)
	{
		(void)record;
		if (id != 0) ids.push_back(id);
	}
	ids.insert(ids.end(), g_menuDestroy.begin(), g_menuDestroy.end());
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	g_menuDestroy.clear();
	for (const s2sdk::MenuId id : ids)
		if (id != 0) DestroyOneMenuSafely(id);
	if (g_bReservedMenuZero)
	{
		if (DestroyMenuNow(0))
			g_bReservedMenuZero = false;
		else
			LogError("A7_UNRESOLVED: reserved menu id 0 cannot be destroyed (s2sdk DestroyMenu missing)");
	}
	g_menus.clear();
}


// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnCommand(int32_t iCaller, s2sdk::ConCommandContext context,
							const plg::vector<plg::string>& args);
s2sdk::ResultType OnSayCommand(int32_t iCaller, s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& args);

struct CommandRecord
{
	std::string sName;
	s2sdk::ConCommandCallback pfnCallback = nullptr;
	bool bOwned = false;
};

std::vector<CommandRecord> g_commands;
std::vector<std::string> g_chatCommandNames;
std::vector<std::string> g_bareChatNames;
bool g_bSayHooked = false;
bool g_bSayTeamHooked = false;

bool IsChatTrigger(char c) { return c == '!' || c == '/'; }

// The server console is exactly caller -1. Any other negative value (-2, INVALID) and every
// out-of-range positive slot is not a server caller and must not run a server-only command.
bool IsServerConsoleCaller(int32_t iCaller) { return iCaller == -1; }

std::string StripChatTrigger(const std::string& s)
{
	if (!s.empty() && IsChatTrigger(s.front())) return s.substr(1);
	return s;
}

bool Contains(const std::vector<std::string>& values, const std::string& value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

s2sdk::ConVarFlag CommandFlags()
{
	return static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
										  static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));
}

void RegisterOneCommand(const std::string& sName, const char* szDescription)
{
	// Register+Unregister are a pair. Both removal paths are required because whether this plugin
	// owns the name is only known after AddConsoleCommand (true = created, false = pre-existing).
	if (sName.empty() || __s2sdk_AddConsoleCommand == nullptr || __s2sdk_RemoveCommand == nullptr ||
		__s2sdk_RemoveCommandListener == nullptr)
		return;
	for (const CommandRecord& record : g_commands)
		if (record.sName == sName) return;
	try
	{
		const bool bOwned = s2sdk::AddConsoleCommand(plg::string(sName), plg::string(szDescription), CommandFlags(),
													 &OnCommand, s2sdk::HookMode::Pre);
		g_commands.push_back(CommandRecord{sName, &OnCommand, bOwned});
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " registering command '" + sName + "' failed: " + ex.what());
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " registering command '" + sName + "' failed");
	}
}

void RegisterSayListeners()
{
	if (g_bSayHooked && g_bSayTeamHooked) return;
	// The say listener pair must be removable before it is attached (PITFALLS A7).
	if (__s2sdk_AddCommandListener == nullptr || __s2sdk_RemoveCommandListener == nullptr) return;
	// Each name in its own guard: if the second registration throws, the first listener must stay
	// tracked so OnPluginEnd removes it (PITFALLS A7).
	if (!g_bSayHooked)
	{
		try { g_bSayHooked = s2sdk::AddCommandListener(plg::string("say"), &OnSayCommand, s2sdk::HookMode::Pre); }
		catch (...) { g_bSayHooked = false; }
	}
	if (!g_bSayTeamHooked)
	{
		try
		{
			g_bSayTeamHooked =
				s2sdk::AddCommandListener(plg::string("say_team"), &OnSayCommand, s2sdk::HookMode::Pre);
		}
		catch (...)
		{
			g_bSayTeamHooked = false;
		}
	}
}

void RegisterCommands()
{
	RegisterOneCommand("mm_res_reload", "round_end_sounds: reload config, phrases and precache");
	RegisterOneCommand("mm_res_kits", "round_end_sounds: print every player's music kit");
	RegisterOneCommand("mm_res_test", "round_end_sounds: play a round-end track now");
	for (const std::string& rawCommand : g_Cfg.vCommands)
	{
		const std::string trimmed = res::Trim(rawCommand);
		if (trimmed.empty()) continue;
		const std::string sName = res::Lower(StripChatTrigger(trimmed));
		if (sName.empty() || Contains(g_chatCommandNames, sName)) continue;
		g_chatCommandNames.push_back(sName);
		RegisterOneCommand(sName, "round_end_sounds: end-of-round music menu");
		if (!IsChatTrigger(trimmed.front()) && !Contains(g_bareChatNames, sName))
			g_bareChatNames.push_back(sName);
	}
	if (!g_bareChatNames.empty()) RegisterSayListeners();
}

void UnregisterCommands()
{
	for (const CommandRecord& record : g_commands)
	{
		try
		{
			if (record.bOwned)
			{
				if (__s2sdk_RemoveCommand != nullptr) s2sdk::RemoveCommand(plg::string(record.sName), record.pfnCallback);
			}
			else if (__s2sdk_RemoveCommandListener != nullptr)
			{
				// A command the plugin did not create (the name already existed): only our listener
				// may be removed - RemoveCommand would unregister callbacks that are not ours.
				s2sdk::RemoveCommandListener(plg::string(record.sName), record.pfnCallback, s2sdk::HookMode::Pre);
			}
		}
		catch (...)
		{
		}
	}
	g_commands.clear();
	g_chatCommandNames.clear();
	g_bareChatNames.clear();

	if (__s2sdk_RemoveCommandListener != nullptr)
	{
		if (g_bSayHooked)
		{
			try { s2sdk::RemoveCommandListener(plg::string("say"), &OnSayCommand, s2sdk::HookMode::Pre); }
			catch (...) {}
		}
		if (g_bSayTeamHooked)
		{
			try { s2sdk::RemoveCommandListener(plg::string("say_team"), &OnSayCommand, s2sdk::HookMode::Pre); }
			catch (...) {}
		}
	}
	g_bSayHooked = false;
	g_bSayTeamHooked = false;
}

void ReloadAll()
{
	LoadConfig();
	LoadPhrases();
	PrecacheAll();
	LogInfo(std::string(kLogTag) + " config reloaded: " + std::to_string(g_Cfg.vTracks.size()) + " tracks");
}

void PrintKitReport()
{
	for (int i = 0; i < kMaxSlots; ++i)
	{
		if (!HasController(i)) continue;
		std::string name = ClientName(i);
		if (name.empty()) name = "?";
		LogInfo(std::string(kLogTag) + " slot " + std::to_string(i) + ": kit " + std::to_string(GetMusicKit(i)) +
				" / inventory " + std::to_string(GetInventoryMusicKit(i)) + " (saved " +
				std::to_string(g_iSavedKit[i]) + " / " + std::to_string(g_iSavedInventoryKit[i]) + "), MVP " +
				std::to_string(GetMvpCount(i)) + " - " + name);
	}
}

s2sdk::ResultType OnCommand(int32_t iCaller, s2sdk::ConCommandContext /*context*/,
							const plg::vector<plg::string>& args)
{
	try
	{
		if (!g_bReady || args.empty()) return s2sdk::ResultType::Continue;
		const std::string sName = res::Lower(ToStd(args[0]));
		// Server-console commands: any caller >= 0 is ignored, exactly like the original's
		// `if (iSlot != -1) return true;` guard.
		if (sName == "mm_res_reload")
		{
			if (IsServerConsoleCaller(iCaller)) ReloadAll();
			return s2sdk::ResultType::Handled;
		}
		if (sName == "mm_res_kits")
		{
			if (IsServerConsoleCaller(iCaller)) PrintKitReport();
			return s2sdk::ResultType::Handled;
		}
		if (sName == "mm_res_test")
		{
			if (IsServerConsoleCaller(iCaller)) OnRoundEnd();
			return s2sdk::ResultType::Handled;
		}
		if (Contains(g_chatCommandNames, sName))
		{
			// Queue the open for OnPluginUpdate: the menu is then created outside the command
			// dispatch lock, and the action carries the owner identity checked before it runs.
			if (g_Cfg.bEnabled && SlotInRange((int)iCaller)) QueueMenuAction((int)iCaller, "main");
			return s2sdk::ResultType::Handled;
		}
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " command failed: " + ex.what());
		return s2sdk::ResultType::Handled;
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " command failed with an unknown exception");
		return s2sdk::ResultType::Handled;
	}
	return s2sdk::ResultType::Continue;
}

// A configured chat name without a "!"/"/" trigger: s2sdk only chains triggered chat to the
// command lookup, so the raw say/say_team line gets the same first-token dispatch pisexcompat had.
s2sdk::ResultType OnSayCommand(int32_t iCaller, s2sdk::ConCommandContext /*context*/,
							   const plg::vector<plg::string>& args)
{
	try
	{
		if (!g_bReady || iCaller < 0 || args.size() < 2) return s2sdk::ResultType::Continue;
		std::string sMessage;
		if (args.size() == 2)
			sMessage = ToStd(args[1]);
		else
		{
			for (std::size_t i = 1; i < args.size(); ++i)
			{
				if (i > 1) sMessage.push_back(' ');
				sMessage.append(args[i].data(), args[i].size());
			}
		}
		std::istringstream stream(sMessage);
		std::string sFirst;
		stream >> sFirst;
		if (sFirst.empty() || IsChatTrigger(sFirst.front())) return s2sdk::ResultType::Continue;
		sFirst = res::Lower(sFirst);
		if (!Contains(g_bareChatNames, sFirst)) return s2sdk::ResultType::Continue;
		if (g_Cfg.bEnabled && SlotInRange((int)iCaller)) QueueMenuAction((int)iCaller, "main");
		return s2sdk::ResultType::Handled;
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " say listener failed: " + ex.what());
		return s2sdk::ResultType::Handled;
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " say listener failed with an unknown exception");
		return s2sdk::ResultType::Handled;
	}
}

// ---------------------------------------------------------------------------------------------
// Deferred player actions (connect hint; a changelevel/reconnect cannot leak it to a new occupant)
// ---------------------------------------------------------------------------------------------

void QueueConnectHint(int iSlot)
{
	if (!SlotInRange(iSlot)) return;
	const uint64_t uSteam = ClientSteamId64(iSlot);
	const int32_t iHandle = ControllerHandle(iSlot);
	if (!uSteam || !res::IsValidHandle(iHandle)) return;
	g_pendingHints.push_back(PendingHint{iSlot, uSteam, iHandle});
}

void ProcessPendingHints()
{
	if (g_pendingHints.empty()) return;
	const std::deque<PendingHint> hints = std::move(g_pendingHints);
	g_pendingHints.clear();
	for (const PendingHint& hint : hints)
	{
		if (!g_mapGate.Ready() || !SlotInRange(hint.iSlot)) continue;
		// Both halves of the identity are checked: a reused slot, or the same SteamID with a new
		// controller serial, must not receive the previous player's hint.
		if (ClientSteamId64(hint.iSlot) != hint.uSteam ||
			ControllerHandle(hint.iSlot) != hint.iControllerHandle)
			continue;
		if (!IsHuman(hint.iSlot)) continue;
		PrintChatLine(hint.iSlot, Phrase("Connect_Hint"));
	}
}

// ---------------------------------------------------------------------------------------------
// Sound event volume (the reference's PostEventPost)
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnSoundEventPost(void* pUserMessage);

bool VerifySoundStartMessage(void* pUserMessage)
{
	// Only ever called with an INCOMING engine-owned message; the id was resolved by name at hook
	// time. Created objects are never passed here (their internals are not guaranteed valid).
	if (__s2sdk_UserMessageGetMessageName == nullptr) return true; // cannot verify
	std::string sName;
	try { sName = res::Lower(ToStd(s2sdk::UserMessageGetMessageName(pUserMessage))); }
	catch (...) { return true; }
	if (sName.empty() || sName.find("sosstartsoundevent") != std::string::npos) return true;
	if (!g_bWrongSoundMessageLogged)
	{
		g_bWrongSoundMessageLogged = true;
		LogError(std::string(kLogTag) + " hooked message " + std::to_string(sound_volume::StartMessageId()) + " is \"" +
				 sName + "\", expected GE_SosStartSoundEvent - per-player volume stays off");
	}
	return false;
}

s2sdk::ResultType OnSoundEventPost(void* pUserMessage)
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		if (!g_mapGate.Ready()) return s2sdk::ResultType::Continue;
		if (!VerifySoundStartMessage(pUserMessage)) return s2sdk::ResultType::Continue;
		int iGuid = -1;
		uint32_t uHash = 0;
		if (!sound_volume::ParseStart(pUserMessage, iGuid, uHash)) return s2sdk::ResultType::Continue;
		if (g_mapOwnEvents.find(uHash) == g_mapOwnEvents.end()) return s2sdk::ResultType::Continue;

		const float flNow = GameTime();
		const uint64_t uMask = sound_volume::RecipientMask(pUserMessage);
		for (int i = 0; i < kMaxSlots; ++i)
		{
			if ((uMask & (1ull << i)) == 0) continue;
			const bool bPreview = res::IsPreviewActive(flNow, g_flPreviewUntil[i]);
			const float flVolume = bPreview ? g_flPreviewVolume[i] : g_Players[i].iVolume / 100.0f;
			Debug("volume " + std::to_string(flVolume) + " -> slot " + std::to_string(i) + " (guid " +
				  std::to_string(iGuid) + ")");
			Playing& p = g_Playing[i];
			p = Playing();
			p.iGuid = iGuid;
			p.flVolume = flVolume;
			p.flStarted = flNow;
			p.bPreview = bPreview;
			if (flVolume < 1.0f) sound_volume::Set(i, iGuid, flVolume);
		}
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " sound volume hook failed: " + ex.what());
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " sound volume hook failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

void EnsureSoundHook()
{
	if (!g_bSoundAvailable || !g_mapGate.Ready()) return;
	// Each message type is resolved by its real registry name, once per live map while still
	// unresolved. There is no numeric fallback: creating a UserMessage from an id the registry
	// does not know yields null internals and the s2sdk getters/setters would dereference them.
	if ((!sound_volume::StartResolved() || !sound_volume::SetResolved() || !sound_volume::StopResolved()) &&
		!g_bResolveTriedThisMap)
	{
		g_bResolveTriedThisMap = true;
		sound_volume::ResolveIds();
		if (!g_bSoundUnresolvedWarned && (!sound_volume::StartResolved() || !sound_volume::SetResolved() ||
										  !sound_volume::StopResolved()))
		{
			g_bSoundUnresolvedWarned = true;
			std::string missing;
			if (!sound_volume::StartResolved()) missing += " SosStartSoundEvent";
			if (!sound_volume::SetResolved()) missing += " SosSetSoundEventParams";
			if (!sound_volume::StopResolved()) missing += " SosStopSoundEvent";
			LogWarn(std::string(kLogTag) + " network-message registry lookup failed for:" + missing +
					" - the corresponding sound-volume functionality stays a no-op");
		}
	}
	if (!sound_volume::StartResolved()) return; // no hook without a verified message id
	const int iWant = sound_volume::StartMessageId();
	if (iWant <= 0) return;
	if (iWant == g_iHookedStartId || iWant == g_iFailedHookId) return;
	if (g_iHookedStartId > 0 && __s2sdk_UnhookUserMessage != nullptr)
	{
		try { s2sdk::UnhookUserMessage((int16_t)g_iHookedStartId, &OnSoundEventPost, s2sdk::HookMode::Post); }
		catch (...) {}
		g_iHookedStartId = -1;
	}
	if (__s2sdk_HookUserMessage == nullptr) return;
	try
	{
		if (s2sdk::HookUserMessage((int16_t)iWant, &OnSoundEventPost, s2sdk::HookMode::Post))
		{
			g_iHookedStartId = iWant;
			g_iFailedHookId = -1;
		}
		else
		{
			g_iFailedHookId = iWant;
			LogError(std::string(kLogTag) + " hooking the GE_SosStartSoundEvent user message failed - player volumes "
										   "will not apply");
		}
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " hooking the sound-event user message failed: " + ex.what());
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " hooking the sound-event user message failed");
	}
}

// ---------------------------------------------------------------------------------------------
// Game events (PITFALLS C12/C13)
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnRoundMvpEvent(const plg::string& /*sName*/, void* pEvent, bool /*bDontBroadcast*/)
{
	if (!MuteKitMusicEnabled() || pEvent == nullptr) return s2sdk::ResultType::Continue;
	int iSlot = -1;
	int iKit = 0;
	try
	{
		if (__s2sdk_GetEventPlayerSlot == nullptr || __s2sdk_GetEventInt == nullptr)
			return s2sdk::ResultType::Continue;
		// "userid" is a player_controller_and_pawn field: GetEventPlayerSlot is the slot accessor;
		// "musickitid" is a numeric event field: GetEventInt.
		iSlot = (int)s2sdk::GetEventPlayerSlot(pEvent, "userid");
		iKit = (int)s2sdk::GetEventInt(pEvent, "musickitid");
		Debug("round_mvp: slot " + std::to_string(iSlot) + ", musickitid " + std::to_string(iKit));
		MuteRoundEndMusic();
		if (SlotInRange(iSlot)) MuteMvpKit(iSlot);
	}
	catch (const std::exception& ex)
	{
		LogError(std::string(kLogTag) + " round_mvp failed: " + ex.what());
		return s2sdk::ResultType::Continue;
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " round_mvp failed");
		return s2sdk::ResultType::Continue;
	}
	if (iKit != 0)
	{
		// A skin changer plugin hides the original round_mvp and sends its own with the kit of the
		// player; the client plays the anthem from musickitid, so without a kit there is no anthem.
		// The mute is the in-place field write. HEAD returned Changed here (the original's
		// EventHookResult::Changed), but s2sdk derives Changed from the event's dont_broadcast
		// field, not from the callback's return, so it would be inert - and a Pre-hook must never
		// supersede the event for the other plugins. Continue on every path.
		if (__s2sdk_SetEventInt != nullptr)
		{
			try { s2sdk::SetEventInt(pEvent, "musickitid", 0); }
			catch (...) { return s2sdk::ResultType::Continue; }
		}
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundEndPreEvent(const plg::string& /*sName*/, void* /*pEvent*/, bool /*bDontBroadcast*/)
{
	try
	{
		if (MuteKitMusicEnabled() && g_mapGate.Ready()) MuteRoundEndMusic();
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundEndEvent(const plg::string& /*sName*/, void* /*pEvent*/, bool /*bDontBroadcast*/)
{
	try { OnRoundEnd(); }
	catch (const std::exception& ex) { LogError(std::string(kLogTag) + " round_end failed: " + ex.what()); }
	catch (...) { LogError(std::string(kLogTag) + " round_end failed"); }
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundPrestartEvent(const plg::string& /*sName*/, void* /*pEvent*/, bool /*bDontBroadcast*/)
{
	try { RestoreKits(); }
	catch (const std::exception& ex) { LogError(std::string(kLogTag) + " round_prestart failed: " + ex.what()); }
	catch (...) { LogError(std::string(kLogTag) + " round_prestart failed"); }
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerConnectFullEvent(const plg::string& /*sName*/, void* pEvent, bool /*bDontBroadcast*/)
{
	try
	{
		if (!g_Cfg.bEnabled || !g_Cfg.bConnectHint || pEvent == nullptr) return s2sdk::ResultType::Continue;
		if (__s2sdk_GetEventPlayerSlot == nullptr) return s2sdk::ResultType::Continue;
		const int iSlot = (int)s2sdk::GetEventPlayerSlot(pEvent, "userid");
		if (!SlotInRange(iSlot)) return s2sdk::ResultType::Continue;
		QueueConnectHint(iSlot);
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*sName*/, void* pEvent, bool /*bDontBroadcast*/)
{
	// No client call may happen here: only the per-slot state is dropped (PITFALLS B2/C2).
	try
	{
		if (pEvent == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) return s2sdk::ResultType::Continue;
		const int iSlot = (int)s2sdk::GetEventPlayerSlot(pEvent, "userid");
		if (!SlotInRange(iSlot)) return s2sdk::ResultType::Continue;
		ResetPlayer(iSlot);
		g_iSavedKit[iSlot] = 0;
		g_iSavedInventoryKit[iSlot] = 0;
		g_bKitMuted[iSlot] = false;
		g_Playing[iSlot] = Playing();
		g_uLoadedSteam[iSlot] = 0;
		g_iLoadedController[iSlot] = 0;
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

struct EventRegistration
{
	const char* szName;
	s2sdk::EventCallback pfnCallback;
	bool bHooked = false;
};

EventRegistration g_events[] = {
	{"round_mvp", &OnRoundMvpEvent, false},
	{"round_end", &OnRoundEndPreEvent, false},
	{"round_end", &OnRoundEndEvent, false},
	{"round_prestart", &OnRoundPrestartEvent, false},
	{"player_connect_full", &OnPlayerConnectFullEvent, false},
	{"player_disconnect", &OnPlayerDisconnectEvent, false},
};

void RegisterEventHooks()
{
	for (EventRegistration& entry : g_events) entry.bHooked = false;
	// Hook and unhook are only useful as a pair: a hook that cannot be removed would point into
	// this library after unload (PITFALLS A7).
	if (__s2sdk_HookEvent == nullptr || __s2sdk_UnhookEvent == nullptr)
	{
		LogError(std::string(kLogTag) + " the s2sdk event hook pair is unavailable - no event is hooked");
		return;
	}
	for (EventRegistration& entry : g_events)
	{
		try
		{
			if (s2sdk::HookEvent(plg::string(entry.szName), entry.pfnCallback, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay)
				entry.bHooked = true;
			else
				LogError(std::string(kLogTag) + " hooking '" + entry.szName + "' was rejected");
		}
		catch (const std::exception& ex)
		{
			LogError(std::string(kLogTag) + " hooking '" + entry.szName + "' failed: " + ex.what());
		}
		catch (...)
		{
			LogError(std::string(kLogTag) + " hooking '" + entry.szName + "' failed");
		}
	}
}

void UnregisterEventHooks()
{
	for (EventRegistration& entry : g_events)
	{
		if (!entry.bHooked) continue;
		entry.bHooked = false;
		if (__s2sdk_UnhookEvent == nullptr) continue;
		try { s2sdk::UnhookEvent(plg::string(entry.szName), entry.pfnCallback, s2sdk::HookMode::Pre); }
		catch (...) {}
	}
}

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

bool g_bManifestRegistered = false;

void RegisterManifest()
{
	if (__s2sdk_OnBuildGameSessionManifest_Register == nullptr ||
		__s2sdk_OnBuildGameSessionManifest_Unregister == nullptr)
		return; // the pair is required, otherwise the callback cannot be removed on unload (A7)
	try
	{
		s2sdk::OnBuildGameSessionManifest_Register(&OnBuildManifest);
		g_bManifestRegistered = true;
	}
	catch (...)
	{
		LogError(std::string(kLogTag) + " registering the manifest callback failed");
	}
}

void UnregisterManifest()
{
	if (!g_bManifestRegistered) return;
	g_bManifestRegistered = false;
	if (__s2sdk_OnBuildGameSessionManifest_Unregister == nullptr) return;
	try { s2sdk::OnBuildGameSessionManifest_Unregister(&OnBuildManifest); }
	catch (...) {}
}

void ResetAllState()
{
	g_Cfg = res::Config();
	g_mapPhrases.clear();
	g_sServerLang = "en";
	g_mapOwnEvents.clear();
	for (int i = 0; i < kMaxSlots; ++i)
	{
		g_Players[i] = res::PlayerData();
		g_Playing[i] = Playing();
		g_flPreviewUntil[i] = -1.0f;
		g_flPreviewVolume[i] = 0.0f;
		g_iSavedKit[i] = 0;
		g_iSavedInventoryKit[i] = 0;
		g_bKitMuted[i] = false;
		g_iSavedKitSteam[i] = 0;
		g_uLoadedSteam[i] = 0;
		g_iLoadedController[i] = 0;
	}
	g_Picker.Reset();
	g_iLastTrack = -1;
	g_flReconcileTimer = 0.0f;
	g_bReady = false;
	g_bUnloading = false;
	g_iPluginId = 0;
	g_pendingHints.clear();
	g_pendingMenuActions.clear();
	g_menus.clear();
	g_menuDestroy.clear();
	g_commands.clear();
	g_chatCommandNames.clear();
	g_bareChatNames.clear();
	g_bSayHooked = false;
	g_bSayTeamHooked = false;
	g_bReservedMenuZero = false;
	g_menuIdZeroChecked = false;
	g_bMenuBindingsWarned = false;
	g_bMenuTypeWarned = false;
	g_bSoundAvailable = false;
	g_iHookedStartId = -1;
	g_iFailedHookId = -1;
	g_bResolveTriedThisMap = false;
	g_bWrongSoundMessageLogged = false;
	g_bSoundUnresolvedWarned = false;
	g_bMuteLogged = false;
	g_bRulesWarned = false;
	g_bManifestRegistered = false;
	ResetSchemaOffsets();
	sound_volume::Reset();
	g_mapGate.Reset();
}

} // namespace

class RoundEndSoundsPlugin final : public plg::Plugin
{
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			ResetAllState();
			// The host fills plg::plugin::GetId; a test/standalone process leaves it null. The id
			// is only used for the permissions cookies, so 0 is a harmless fallback there.
			g_iPluginId = plg::plugin::GetId != nullptr ? static_cast<int64_t>(GetId()) : 0;

			// Only file/config work here - no client, entity or schema call before a map (B1).
			LoadConfig();
			LoadPhrases();
			for (int i = 0; i < kMaxSlots; ++i) ResetPlayer(i);

			g_bSoundAvailable = sound_volume::Init();
			if (!g_bSoundAvailable)
				LogError(std::string(kLogTag) + " soundevent volume control is unavailable (s2sdk user messages missing)");
			RegisterManifest();
			RegisterMapListeners();
			RegisterEventHooks();
			RegisterCommands();

			g_bReady = true;
			LogInfo(std::string(kLogTag) + " loaded: " + std::to_string(g_Cfg.vTracks.size()) + " tracks, precache: " +
					std::to_string(g_Cfg.vPrecache.size()) + " files");
		}
		catch (const std::exception& ex)
		{
			LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
		}
		catch (...)
		{
			LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
		}
		return {}; // never Failed: that stops every Plugify plugin (PITFALLS A1)
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		try
		{
			g_mapGate.OnPluginUpdate();
			if (!g_bReady) return {};
			if (g_mapGate.Ready())
			{
				EnsureSoundHook();
				ReconcileCookies((float)dt.count() / 1000.0f);
				FadeFrame();
				ProcessPendingHints();
				ProcessPendingMenuActions();
			}
			ProcessMenuDestroys();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string(kLogTag) + " update failed: " + ex.what());
		}
		catch (...)
		{
			LogError(std::string(kLogTag) + " update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		try
		{
			g_bReady = false;
			g_bUnloading = true;
			if (g_iHookedStartId > 0 && __s2sdk_UnhookUserMessage != nullptr)
			{
				try { s2sdk::UnhookUserMessage((int16_t)g_iHookedStartId, &OnSoundEventPost, s2sdk::HookMode::Post); }
				catch (...) {}
			}
			g_iHookedStartId = -1;
			UnregisterEventHooks();
			UnregisterManifest();
			UnregisterMapListeners();
			UnregisterCommands();
			DestroyAllMenusForUnload();
			sound_volume::Reset();
			ResetSchemaOffsets();
			g_pendingHints.clear();
			g_pendingMenuActions.clear();
			g_bSoundAvailable = false;
			g_bReady = false;
			g_bUnloading = false;
			g_mapGate.Close();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
		}
		catch (...)
		{
			LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

namespace {

RoundEndSoundsPlugin g_roundEndSoundsPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_roundEndSoundsPlugin)
