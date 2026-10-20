// round_end_sounds — Plugify C++ port of the Metamod:Source plugin cs2-round-end-sounds
// (itself a port of Stimayk/RoundEndSounds): a track per round end, played to every player who
// wants it, with his own volume, a preview menu and the "mute the MVP anthem" trick.
//
// Ported 1:1 (keys, defaults, behaviour) from the original, together with its config (100
// Alko.Track.* soundevents from workshop addon 3811020435, message mode "none").
//
// Deliberate differences (forced by the framework, semantics unchanged):
//   * engine access is s2sdk/pisexcompat; the schema writes of the original become
//     s2sdk::SetEntSchema / SetEntData with changeState (the network update the original sent
//     with NetworkStateChanged + SetStateChanged):
//       - CCSPlayerController::m_iMusicKitID is set on the controller entity directly, and
//         CCSPlayerController_InventoryServices::m_unMusicID (the field the client plays the
//         MVP anthem from; plugins/WeaponSkins writes both) on the controller's inventory component
//         - a pointer field read fresh, written with s2sdk's non-entity chainOffset -2,
//       - CCSGameRules::m_bRoundEndNoMusic is written into the CCSGameRules object itself. That
//         object is NOT embedded in the "cs_gamerules" proxy entity: the proxy is 1936 bytes and
//         CCSGameRules is a separate 70712-byte allocation chained to it through its own
//         __m_pChainEntity (offset 8 in the schema, also readable at runtime). The pointer is read
//         fresh from CCSGameRulesProxy::m_pGameRules (offset 1928) in the same tick
//         the proxy is validated, the network update goes through the rules chainer (s2sdk's
//         SafeNetworkStateChanged with chainOffset >= 0 -> ChainNetworkStateChanged), and the
//         reference's state-change call on the rules proxy is sent too (that pointer is the chain
//         anchor the client rebuilds the rules object from);
//   * ICookiesApi is replaced by the official permissions cookies (same names: RES.Enabled /
//     RES.Volume), the user is loaded with permissions::LoadUser by a once-per-second reconcile;
//   * per-player volume of the soundevent tracks uses GE_SosSetSoundEventParams through the s2sdk
//     user-message hook (src/sound_volume.cpp) - the SoundVolume::Set/Stop of the original;
//   * the utils settings-menu hook (utils_settings) is not ported (no settings-menu API in the
//     Plugify menu layer); the !res menu keeps working as before.
//
// No s2sdk call happens before a map exists, no exception escapes into s2sdk, and the plugin never
// returns Failed from OnPluginStart.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>

#include <s2sdk/s2sdk.hpp>

#include <permissions/permissions.hpp>

#include <pisex/pisex.hpp>

#include "re_core.hpp"
#include "sound_volume.hpp"

namespace {

const char* kLogTag = "[RoundEndSounds]";

constexpr const char* kConfigFile = "round_end_sounds.ini";
constexpr const char* kPhrasesFile = "round_end_sounds.phrases.txt";

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

res::Config g_Cfg;
std::map<std::string, std::string> g_mapPhrases;
std::string g_sServerLang = "ru";
std::map<uint32_t, bool> g_mapOwnEvents;

res::PlayerData g_Players[64];
float g_flPreviewVolume[64] = {};
float g_flPreviewUntil[64] = {};

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
Playing g_Playing[64];

res::TrackPicker g_Picker;
int g_iLastTrack = -1;
std::mt19937 g_Rng{std::random_device{}()};

int32_t g_iSavedKit[64] = {};          // m_iMusicKitID that was zeroed (0: nothing to restore)
int32_t g_iSavedInventoryKit[64] = {}; // ..._InventoryServices::m_unMusicID zeroed with it
bool g_bKitMuted[64] = {};             // a kit is zeroed for this slot until round_prestart
uint64_t g_iSavedKitSteam[64] = {};

uint64_t g_uLoadedSteam[64] = {};
float g_flReconcileTimer = 0.0f;
bool g_bReady = false;
int64_t g_iPluginId = 0;

// ---------------------------------------------------------------------------------------------
// Logging / small helpers
// ---------------------------------------------------------------------------------------------

void LogMessage(const std::string& sText, plg::Severity eSeverity)
{
	if (plg::Log) plg::Log(std::string(kLogTag) + " " + sText, eSeverity, plg::source_location::current());
}

void Log(const std::string& sText) { LogMessage(sText, plg::Severity::Info); }
void LogError(const std::string& sText) { LogMessage(sText, plg::Severity::Error); }
void Debug(const std::string& sText) { if (g_Cfg.bDebug) Log("[DEBUG] " + sText); }

void Defer(std::function<void()> fn)
{
	pisex::g_pUtils->NextFrame(std::move(fn));
}

std::string Phrase(const std::string& sKey)
{
	return res::Phrase(g_mapPhrases, g_sServerLang, sKey);
}

// s2sdk entity calls take handles; the pisex/s2sdk "*Index" helpers hand out 1-based indexes.
// EntIndexToEntHandle answers INVALID_EHANDLE_INDEX (-1) for a missing index and a valid handle can
// be a negative int (see res::IsValidHandle), so only the raw value decides.
int EntityHandle(int iIndex)
{
	if (iIndex <= 0 || !__s2sdk_EntIndexToEntHandle) return 0;
	const int iHandle = s2sdk::EntIndexToEntHandle(iIndex);
	return res::IsValidHandle(iHandle) ? iHandle : 0;
}

bool HasController(int iSlot)
{
	return iSlot >= 0 && iSlot < 64 && pisex::IsConnected(iSlot) && pisex::GetControllerEntity(iSlot) > 0;
}

bool IsHuman(int iSlot)
{
	return HasController(iSlot) && !pisex::IsFakeClient(iSlot);
}

int PawnOf(int iSlot)
{
	return HasController(iSlot) ? pisex::GetPawnEntity(iSlot) : 0;
}

std::string PlayerNameOf(int iSlot)
{
	const char* sz = pisex::GetPlayerName(iSlot);
	return (sz && sz[0]) ? sz : "?";
}

int GetMvpCount(int iSlot)
{
	const int iController = HasController(iSlot) ? pisex::GetControllerEntity(iSlot) : 0;
	const int iHandle = EntityHandle(iController);
	if (!res::IsValidHandle(iHandle) || !__s2sdk_GetEntSchema) return 0;
	try { return (int)s2sdk::GetEntSchema(iHandle, "CCSPlayerController", "m_iMVPs", 0); }
	catch (...) { return 0; }
}

// ---------------------------------------------------------------------------------------------
// Cookies (official permissions plugin, the Metamod cookie names)
// ---------------------------------------------------------------------------------------------

bool CookieRead(uint64_t uSteamID64, const char* szName, std::string& sOut)
{
	if (!uSteamID64 || !__permissions_GetCookie) return false;
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
	if (!uSteamID64 || !__permissions_SetCookie) return;
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
	if (iSlot < 0 || iSlot >= 64) return;
	g_Players[iSlot].bEnabled = g_Cfg.bDefaultEnabled;
	g_Players[iSlot].iVolume = g_Cfg.iDefaultVolume;
}

void LoadPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= 64) return;
	ResetPlayer(iSlot);
	const uint64_t uSteam = pisex::GetSteamID64(iSlot);
	if (!uSteam) return;
	std::string sValue;
	if (CookieRead(uSteam, "RES.Enabled", sValue)) g_Players[iSlot].bEnabled = atoi(sValue.c_str()) != 0;
	if (CookieRead(uSteam, "RES.Volume", sValue)) g_Players[iSlot].iVolume = std::clamp(atoi(sValue.c_str()), 0, 100);
}

void SavePlayer(int iSlot)
{
	CookieWrite(pisex::GetSteamID64(iSlot), "RES.Enabled", g_Players[iSlot].bEnabled ? "1" : "0");
	CookieWrite(pisex::GetSteamID64(iSlot), "RES.Volume", std::to_string(g_Players[iSlot].iVolume));
}

void ReconcileCookies(float flDelta)
{
	if (!__permissions_LoadUser) return;
	g_flReconcileTimer += flDelta;
	if (g_flReconcileTimer < 1.0f) return;
	g_flReconcileTimer = 0.0f;

	for (int i = 0; i < 64; i++)
	{
		if (!HasController(i) || pisex::IsFakeClient(i) || !pisex::IsAuthenticated(i))
		{
			if (g_uLoadedSteam[i])
			{
				g_uLoadedSteam[i] = 0;
				ResetPlayer(i);
				g_iSavedKit[i] = 0;
				g_iSavedInventoryKit[i] = 0;
				g_bKitMuted[i] = false;
				g_Playing[i] = Playing();
			}
			continue;
		}
		const uint64_t uSteam = pisex::GetSteamID64(i);
		if (!uSteam || g_uLoadedSteam[i] == uSteam) continue;
		const char* szName = pisex::GetPlayerName(i);
		if (permissions::LoadUser(g_iPluginId, uSteam, plg::string(szName ? szName : ""), /*offline=*/false,
								  /*dontBroadcast=*/false) == permissions::Status::Success)
		{
			g_uLoadedSteam[i] = uSteam;
			LoadPlayer(i);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Config / phrases
// ---------------------------------------------------------------------------------------------

void LoadConfig()
{
	bool ok = false;
	const std::string sText = pisex::ReadFile(pisex::ConfigDir() + "/" + kConfigFile, &ok);
	if (!ok || !res::ParseConfigText(sText, g_Cfg))
	{
		LogError(std::string("failed to read ") + pisex::ConfigDir() + "/" + kConfigFile);
		return;
	}
	g_mapOwnEvents = res::BuildOwnEvents(g_Cfg.vTracks, &res::SoundEventHash);
	g_iLastTrack = -1;
	g_Picker.Reset();
}

void LoadPhrases()
{
	g_mapPhrases.clear();
	const char* szLang = pisex::g_pUtils->GetLanguage();
	g_sServerLang = res::Lower(res::Trim(szLang && szLang[0] ? szLang : "ru"));
	if (g_sServerLang.empty()) g_sServerLang = "ru";
	bool ok = false;
	const std::string sText = pisex::ReadFile(pisex::TranslationsDir() + "/" + kPhrasesFile, &ok);
	if (!ok) return;
	res::ParsePhrasesText(sText, g_sServerLang, g_mapPhrases);
}

void PrecacheAll()
{
	for (const auto& s : g_Cfg.vPrecache)
		if (!s.empty()) s2sdk::Precache(plg::string(s));
	// the sounds/... files of the tracks are precached too, without precacher.ini entries
	for (const auto& t : g_Cfg.vTracks)
		if (res::IsFilePathSound(t.sSound)) s2sdk::Precache(plg::string(t.sSound));
}

void OnBuildManifest()
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		if (__s2sdk_Precache) PrecacheAll();
	}
	catch (...)
	{
	}
}

// ---------------------------------------------------------------------------------------------
// Schema (music kit, round end music)
// ---------------------------------------------------------------------------------------------

int g_iNoMusicOffset = -1;
int g_iRulesChainOffset = -1;
int g_iRulesFieldOffset = -1;
int g_iRestartTimeOffset = -1;
bool g_bRulesWarned = false;
bool g_bMuteLogged = false;

// The second place a music kit lives: the controller's inventory component. m_pInventoryServices is
// a pointer field of CCSPlayerController; CCSPlayerController_InventoryServices::m_unMusicID is the
// uint16 plugins/WeaponSkins writes together with m_iMusicKitID, and the field the client reads
// the MVP anthem from.
int g_iInventoryServicesOffset = -1; // CCSPlayerController::m_pInventoryServices (pointer)
int g_iInventoryMusicIdOffset = -1;  // ..._InventoryServices::m_unMusicID / m_nMusicID (uint16)
int g_iInventoryChainOffset = -2;    // component: no chainer, announce via its own vtable slot 1

void ResolveRulesFields()
{
	if (g_iNoMusicOffset >= 0) return;
	g_iNoMusicOffset = pisex::SchemaOffset("CCSGameRules", "m_bRoundEndNoMusic");
	g_iRestartTimeOffset = pisex::SchemaOffset("CCSGameRules", "m_flRestartRoundTime");
	// CCSGameRulesProxy::m_pGameRules — the pointer to the (separately allocated) rules object
	g_iRulesFieldOffset = pisex::SchemaOffset("CCSGameRulesProxy", "m_pGameRules");
	// s2sdk's SafeNetworkStateChanged() reads a chain offset >= 0 as "the __m_pChainEntity sits at
	// object + offset": a 0 (missing binding) would make it read the object's vtable as a chainer,
	// so anything that is not a real offset becomes -1 (the object itself is the entity).
	// CCSGameRules::__m_pChainEntity sits at offset 8 (inherited from CGameRules); the chainer links
	// the rules object back to the cs_gamerules proxy entity the clients see.
	g_iRulesChainOffset = -1;
	if (__s2sdk_GetSchemaChainOffset)
	{
		const int iOffset = pisex::SchemaChainOffset("CCSGameRules");
		if (iOffset > 0) g_iRulesChainOffset = iOffset;
	}
}

void ResolveInventoryFields()
{
	if (g_iInventoryServicesOffset >= 0 && g_iInventoryMusicIdOffset >= 0) return;
	g_iInventoryServicesOffset = pisex::SchemaOffset("CCSPlayerController", "m_pInventoryServices");
	// Other game builds call the same field m_nMusicID (plugins/WeaponSkins keeps the same fallback).
	g_iInventoryMusicIdOffset = pisex::SchemaOffset("CCSPlayerController_InventoryServices", "m_unMusicID");
	if (g_iInventoryMusicIdOffset < 0)
		g_iInventoryMusicIdOffset = pisex::SchemaOffset("CCSPlayerController_InventoryServices", "m_nMusicID");
	// The component is not an entity and has no chainer of its own: its fields are announced through
	// the object's own NetworkStateChanged (vtable slot 1), s2sdk's "chainOffset -2 for non-entity
	// classes" — the convention plugins/WeaponSkins uses for the very same fields.
	const int iChain = pisex::SchemaChainOffset("CCSPlayerController_InventoryServices");
	g_iInventoryChainOffset = iChain > 0 ? iChain : -2;
}

// The controller's inventory component (m_pInventoryServices), read fresh from the pointer field.
// nullptr when this build has no such field, the binding is missing or the pointer is empty.
void* GetInventoryServices(int iSlot)
{
	const int iController = HasController(iSlot) ? pisex::GetControllerEntity(iSlot) : 0;
	if (!iController || !__s2sdk_GetEntData2) return nullptr;
	ResolveInventoryFields();
	if (g_iInventoryServicesOffset < 0) return nullptr;
	void* pController = pisex::EntIndexToEntPointer(iController);
	if (!pController) return nullptr;
	int64_t iServices = 0;
	try { iServices = s2sdk::GetEntData2(pController, g_iInventoryServicesOffset, (int32_t)sizeof(void*)); }
	catch (...) { iServices = 0; }
	return iServices > 0 ? reinterpret_cast<void*>(static_cast<intptr_t>(iServices)) : nullptr;
}

// CCSPlayerController_InventoryServices::m_unMusicID (uint16): 0 when unavailable.
int GetInventoryMusicKit(int iSlot)
{
	void* pServices = GetInventoryServices(iSlot);
	if (!pServices || g_iInventoryMusicIdOffset < 0 || !__s2sdk_GetEntData2) return 0;
	try { return (int)s2sdk::GetEntData2(pServices, g_iInventoryMusicIdOffset, /*size=*/2); }
	catch (...) { return 0; }
}

void SetInventoryMusicKit(int iSlot, int iKit)
{
	void* pServices = GetInventoryServices(iSlot);
	if (!pServices || g_iInventoryMusicIdOffset < 0 || !__s2sdk_SetEntData2) return;
	try
	{
		s2sdk::SetEntData2(pServices, g_iInventoryMusicIdOffset, iKit, /*size=*/2, /*changeState=*/true,
						   g_iInventoryChainOffset);
	}
	catch (...)
	{
	}
}

int GetMusicKit(int iSlot)
{
	const int iController = HasController(iSlot) ? pisex::GetControllerEntity(iSlot) : 0;
	const int iHandle = EntityHandle(iController);
	if (!res::IsValidHandle(iHandle) || !__s2sdk_GetEntSchema) return 0;
	try { return (int)s2sdk::GetEntSchema(iHandle, "CCSPlayerController", "m_iMusicKitID", 0); }
	catch (...) { return 0; }
}

void SetMusicKit(int iSlot, int iKit)
{
	const int iController = HasController(iSlot) ? pisex::GetControllerEntity(iSlot) : 0;
	const int iHandle = EntityHandle(iController);
	if (!res::IsValidHandle(iHandle) || !__s2sdk_SetEntSchema) return;
	try { s2sdk::SetEntSchema(iHandle, "CCSPlayerController", "m_iMusicKitID", iKit, /*changeState=*/true, 0); }
	catch (...) {}
}

bool MuteKitMusicEnabled()
{
	return g_Cfg.bEnabled && g_Cfg.bMuteMvpAnthem && !g_Cfg.vTracks.empty();
}

// Win/lose music: the client does not play it while CCSGameRules::m_bRoundEndNoMusic is set. The
// rules object is a separate heap allocation chained to the "cs_gamerules" proxy entity (see the
// header comment): the pointer is read fresh from the proxy's m_pGameRules field, the write goes
// into the rules object itself, and the network update goes through the rules chainer - the
// equivalent of the original's direct write plus its state-change call on the rules proxy.
void MuteRoundEndMusic()
{
	void* pProxy = __s2sdk_GetGameRulesProxy ? s2sdk::GetGameRulesProxy() : nullptr;
	if (!pProxy || !__s2sdk_SetEntData2 || !__s2sdk_EntPointerToEntHandle || !__s2sdk_IsValidEntHandle) return;
	// the owner entity is validated here, in the same tick as the pointer reads/writes below
	const int iProxyHandle = s2sdk::EntPointerToEntHandle(pProxy);
	if (!res::IsValidHandle(iProxyHandle) || !s2sdk::IsValidEntHandle(iProxyHandle)) return;

	ResolveRulesFields();
	if (g_iNoMusicOffset < 0)
	{
		if (!g_bRulesWarned)
		{
			g_bRulesWarned = true;
			LogError("CCSGameRules::m_bRoundEndNoMusic not found - only the MVP's music kit fields are muted");
		}
		return;
	}

	// The authoritative rules pointer, in this tick: the proxy's m_pGameRules field. s2sdk's cached
	// g_pGameRules is only set on entity create/delete, the fresh read also catches a re-created
	// proxy. Falls back to the cached global when the field offset could not be resolved.
	int64_t iRules = 0;
	if (g_iRulesFieldOffset >= 0 && __s2sdk_GetEntData2)
	{
		try { iRules = s2sdk::GetEntData2(pProxy, g_iRulesFieldOffset, (int32_t)sizeof(void*)); }
		catch (...) { iRules = 0; }
	}
	if (iRules <= 0 && __s2sdk_GetGameRules)
	{
		try { iRules = static_cast<int64_t>(reinterpret_cast<intptr_t>(s2sdk::GetGameRules())); }
		catch (...) { iRules = 0; }
	}
	if (iRules <= 0)
	{
		if (!g_bRulesWarned)
		{
			g_bRulesWarned = true;
			LogError("cs_gamerules proxy has no CCSGameRules instance - round end music stays unmuted");
		}
		return;
	}
	void* pRules = reinterpret_cast<void*>(static_cast<intptr_t>(iRules));

	// Write m_bRoundEndNoMusic=1 into the rules object. With a resolved chain offset the same call
	// marks the field changed through the chainer at rules + 8 (s2sdk's SafeNetworkStateChanged ->
	// ChainNetworkStateChanged -> the proxy entity's NetworkStateChanged with the chain path index);
	// without the binding the plain write plus the proxy NetworkStateChanged below still reach the
	// clients. The "2" pointer API is used on a non-entity object (the game rules) right after its
	// owner entity was validated above.
	try
	{
		if (g_iRulesChainOffset >= 0)
			s2sdk::SetEntData2(pRules, g_iNoMusicOffset, 1, /*size=*/1, /*changeState=*/true, g_iRulesChainOffset);
		else
			s2sdk::SetEntData2(pRules, g_iNoMusicOffset, 1, /*size=*/1, /*changeState=*/false, /*chainOffset=*/-2);
	}
	catch (...)
	{
		return;
	}

	// The reference's state-change call on the rules proxy: the chain anchor field is marked changed
	// as well, so the engine re-sends the chained gamerules state.
	if (__s2sdk_NetworkStateChanged)
	{
		try { s2sdk::NetworkStateChanged(iProxyHandle, "CCSGameRulesProxy", "m_pGameRules"); }
		catch (...) {}
	}

	// Proof of the write (debug, once per map): read back the byte that was just written.
	if (!g_bMuteLogged)
	{
		g_bMuteLogged = true;
		int64_t iRead = -1;
		if (__s2sdk_GetEntData2)
		{
			try { iRead = s2sdk::GetEntData2(pRules, g_iNoMusicOffset, /*size=*/1); }
			catch (...) { iRead = -1; }
		}
		Debug("round end music muted: CCSGameRules::m_bRoundEndNoMusic <- 1 (offset " + std::to_string(g_iNoMusicOffset)
			  + ", read back " + std::to_string(iRead) + "; chain mark offset " + std::to_string(g_iRulesChainOffset)
			  + ", proxy m_pGameRules + " + std::to_string(g_iRulesFieldOffset) + ")");
	}
}

// The MVP anthem: when m_iMVPs changes, the client plays the anthem of the MVP's kit. The kit is
// read from the controller field, or from the inventory component when that is empty - the current
// game build plays the anthem from the inventory one (both fields are what plugins/WeaponSkins writes).
// Both fields are saved and zeroed, so neither can start the anthem until the next round.
void MuteMvpKit(int iSlot)
{
	if (!HasController(iSlot)) return;
	const int iKit = GetMusicKit(iSlot);
	const int iInventoryKit = GetInventoryMusicKit(iSlot);
	if (!res::KitHasMusic(res::KitEffective(iKit, iInventoryKit)))
	{
		Debug("MVP slot " + std::to_string(iSlot) + ": no music kit to mute (kit " + std::to_string(iKit)
			  + ", inventory " + std::to_string(iInventoryKit) + ")");
		return;
	}
	if (!g_bKitMuted[iSlot])
	{
		g_bKitMuted[iSlot] = true;
		g_iSavedKit[iSlot] = res::KitHasMusic(iKit) ? iKit : 0;
		g_iSavedInventoryKit[iSlot] = res::KitHasMusic(iInventoryKit) ? iInventoryKit : 0;
		g_iSavedKitSteam[iSlot] = pisex::GetSteamID64(iSlot);
	}
	// Zero every field that holds a kit; 0 and -1 are left alone, they mean "no kit" (the
	// reference skipped them too).
	if (res::KitHasMusic(iKit)) SetMusicKit(iSlot, 0);
	if (res::KitHasMusic(iInventoryKit)) SetInventoryMusicKit(iSlot, 0);
	Debug("MVP slot " + std::to_string(iSlot) + ": music kit " + std::to_string(iKit) + " / inventory "
		  + std::to_string(iInventoryKit) + " -> 0 until the next round");
}

void RestoreKits()
{
	for (int i = 0; i < 64; i++)
	{
		if (!g_bKitMuted[i]) continue;
		const int32_t iKit = g_iSavedKit[i];
		const int32_t iInventoryKit = g_iSavedInventoryKit[i];
		g_bKitMuted[i] = false;
		g_iSavedKit[i] = 0;
		g_iSavedInventoryKit[i] = 0;
		if (!HasController(i)) continue;
		if (pisex::GetSteamID64(i) != g_iSavedKitSteam[i]) continue;
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
	// a sounds/... path is played by the client itself: the volume is the player's own
	if (res::IsFilePathSound(t.sSound))
	{
		if (__s2sdk_ClientCommand) s2sdk::ClientCommand(iSlot, plg::string("play " + t.sSound));
		return;
	}
	if (iVolume <= 0 || !__s2sdk_EmitSound) return;
	// a soundevent is played on the listener himself, so it is equally loud anywhere on the map
	const int iHandle = EntityHandle(PawnOf(iSlot));
	if (!res::IsValidHandle(iHandle)) return;
	// EmitSound does not apply the volume of a soundevent; the user-message hook does it
	s2sdk::EmitSound(iHandle, plg::string(t.sSound), 100, iVolume / 100.0f, 0.0f);
}

void AnnounceTo(int iSlot, const res::Track& t)
{
	const bool bChat = g_Cfg.sMessage == "chat" || g_Cfg.sMessage == "both";
	const bool bCenter = g_Cfg.sMessage == "center" || g_Cfg.sMessage == "both";
	if (bChat)
		pisex::g_pUtils->PrintToChat(iSlot, "%s", res::Replace(Phrase("Now_Playing"), "{name}", t.sName).c_str());
	if (bCenter)
	{
		const std::string sPicture = t.sPicture.empty() ? "" : "<br><img src='" + t.sPicture + "'/>";
		const std::string sHtml = res::Replace(res::Replace(Phrase("Now_Playing_Center"), "{name}", t.sName), "{picture}", sPicture);
		pisex::g_pUtils->PrintToCenterHtml(iSlot, g_Cfg.iCenterTime, "%s", sHtml.c_str());
	}
}

void OnRoundEnd()
{
	if (!g_Cfg.bEnabled) return;
	const int iTrack = g_Picker.Pick((int)g_Cfg.vTracks.size(), g_Cfg.bRandom, (uint32_t)g_Rng());
	if (iTrack < 0) return;
	g_iLastTrack = iTrack;
	g_Picker.iLast = iTrack;
	const res::Track& t = g_Cfg.vTracks[iTrack];

	int nPlayed = 0;
	for (int i = 0; i < 64; i++)
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
	if (g_Cfg.flFadeBeforeRound <= 0.0f) return;
	pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
	void* pRules = __s2sdk_GetGameRules ? s2sdk::GetGameRules() : nullptr;
	if (!pGlobals || !pRules) return;
	ResolveRulesFields();
	if (g_iRestartTimeOffset < 0) return;

	float flRestart = 0.0f;
	try { flRestart = (float)s2sdk::GetEntDataFloat2(pRules, g_iRestartTimeOffset, 4); }
	catch (...) { return; }
	if (flRestart <= 0.0f) return;

	const float flNow = pGlobals->curtime;
	if (flNow < flRestart - g_Cfg.flFadeBeforeRound) return;

	for (int i = 0; i < 64; i++)
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
// Menus
// ---------------------------------------------------------------------------------------------

void ShowMainMenu(int iSlot);

void SetEnabled(int iSlot, bool bOn)
{
	g_Players[iSlot].bEnabled = bOn;
	SavePlayer(iSlot);
	pisex::g_pUtils->PrintToChat(iSlot, "%s", res::Replace(Phrase("State_Set"), "{state}", Phrase(bOn ? "On" : "Off")).c_str());
}

void ShowVolumeMenu(int iSlot)
{
	const int iVolume = g_Players[iSlot].iVolume;
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, res::Replace(Phrase("Volume_Title"), "{volume}", std::to_string(iVolume)).c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "up", Phrase("Volume_Up").c_str(), iVolume >= 100 ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "down", Phrase("Volume_Down").c_str(), iVolume <= 0 ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { Defer([iSlot]() { ShowMainMenu(iSlot); }); return; }
		if (res::IsService(sBack)) return;
		Defer([iSlot, sBack]() {
			int& v = g_Players[iSlot].iVolume;
			v = std::clamp(v + (sBack == "up" ? 10 : -10), 0, 100);
			SavePlayer(iSlot);
			ShowVolumeMenu(iSlot);
		});
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

void PreviewTrack(int iSlot, int iTrack)
{
	if (iTrack < 0 || iTrack >= (int)g_Cfg.vTracks.size()) return;
	const res::Track& t = g_Cfg.vTracks[iTrack];
	// previewing works with music switched off too; at volume 0 the default volume is used
	const int iVolume = res::PreviewVolume(g_Players[iSlot].iVolume, g_Cfg.iDefaultVolume);
	g_flPreviewVolume[iSlot] = iVolume / 100.0f;
	pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
	g_flPreviewUntil[iSlot] = pGlobals ? pGlobals->curtime + 1.0f : 0.0f;
	Play(iSlot, t, iVolume);
	pisex::g_pUtils->PrintToChat(iSlot, "%s", res::Replace(Phrase("Preview"), "{name}", t.sName).c_str());
}

void ShowTracksMenu(int iSlot)
{
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Tracks_Title").c_str());
	for (size_t i = 0; i < g_Cfg.vTracks.size(); i++)
		pisex::g_pMenus->AddItemMenu(hMenu, std::to_string(i).c_str(), g_Cfg.vTracks[i].sName.c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->SetBackMenu(hMenu, true);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (sBack == "back") { Defer([iSlot]() { ShowMainMenu(iSlot); }); return; }
		if (res::IsService(sBack)) return;
		const int iTrack = atoi(sBack.c_str());
		Defer([iSlot, iTrack]() { PreviewTrack(iSlot, iTrack); });
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

bool AnyVolumeTrack()
{
	return !g_mapOwnEvents.empty();
}

void ShowMainMenu(int iSlot)
{
	if (!IsHuman(iSlot)) return;
	const res::PlayerData& p = g_Players[iSlot];
	pisex::Menu hMenu;
	pisex::g_pMenus->SetTitleMenu(hMenu, Phrase("Menu_Title").c_str());
	pisex::g_pMenus->AddItemMenu(hMenu, "state", res::Replace(Phrase("Menu_State"), "{state}", Phrase(p.bEnabled ? "On" : "Off")).c_str(), pisex::ITEM_DEFAULT);
	if (AnyVolumeTrack())
		pisex::g_pMenus->AddItemMenu(hMenu, "volume", res::Replace(Phrase("Menu_Volume"), "{volume}", std::to_string(p.iVolume)).c_str(), pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "tracks", Phrase("Menu_Tracks").c_str(), g_Cfg.vTracks.empty() ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->AddItemMenu(hMenu, "last", Phrase("Menu_Last").c_str(), g_iLastTrack < 0 ? pisex::ITEM_DISABLED : pisex::ITEM_DEFAULT);
	pisex::g_pMenus->SetExitMenu(hMenu, true);
	pisex::g_pMenus->SetCallback(hMenu, [](const char* szBack, const char*, int, int iSlot) {
		const std::string sBack = szBack ? szBack : "";
		if (res::IsService(sBack)) return;
		Defer([iSlot, sBack]() {
			if (sBack == "state") { SetEnabled(iSlot, !g_Players[iSlot].bEnabled); ShowMainMenu(iSlot); }
			else if (sBack == "volume") ShowVolumeMenu(iSlot);
			else if (sBack == "tracks") ShowTracksMenu(iSlot);
			else if (sBack == "last")
			{
				if (g_iLastTrack >= 0) PreviewTrack(iSlot, g_iLastTrack);
				else pisex::g_pUtils->PrintToChat(iSlot, "%s", Phrase("No_Last").c_str());
			}
		});
	});
	pisex::g_pMenus->DisplayPlayerMenu(hMenu, iSlot, true, true);
}

// ---------------------------------------------------------------------------------------------
// Sound event volume hook (the reference's PostEventPost)
// ---------------------------------------------------------------------------------------------

s2sdk::ResultType OnSoundEventPost(void* pUserMessage)
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		int iGuid = -1;
		uint32_t uHash = 0;
		if (!sound_volume::ParseStart(pUserMessage, iGuid, uHash)) return s2sdk::ResultType::Continue;
		if (!g_mapOwnEvents.count(uHash)) return s2sdk::ResultType::Continue;

		pisex::GlobalVars* pGlobals = pisex::GetCGlobalVars();
		const uint64_t uMask = sound_volume::RecipientMask(pUserMessage);
		for (int i = 0; i < 64; i++)
		{
			if (!(uMask & (1ull << i))) continue;
			const bool bPreview = pGlobals && res::IsPreviewActive(pGlobals->curtime, g_flPreviewUntil[i]);
			const float flVolume = bPreview ? g_flPreviewVolume[i] : g_Players[i].iVolume / 100.0f;
			Debug("volume " + std::to_string(flVolume) + " -> slot " + std::to_string(i) + " (guid "
				  + std::to_string(iGuid) + ")");
			Playing& p = g_Playing[i];
			p = Playing();
			p.iGuid = iGuid;
			p.flVolume = flVolume;
			p.flStarted = pGlobals ? pGlobals->curtime : 0.0f;
			p.bPreview = bPreview;
			if (flVolume < 1.0f) sound_volume::Set(i, iGuid, flVolume);
		}
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

// round_mvp: zero the music kits of the MVP (both fields, see MuteMvpKit) before the engine sends
// the event, and drop the musickitid of the event itself (a skin changer plugin sends its own
// round_mvp with the kit of the player - the client plays the anthem from musickitid, so without a
// kit there is no anthem).
s2sdk::ResultType OnRoundMvp(const plg::string&, void* pEvent, bool)
{
	// a callback of s2sdk: nothing may unwind out of here
	if (!MuteKitMusicEnabled() || !pEvent) return s2sdk::ResultType::Continue;
	int iSlot = -1;
	int iKit = 0;
	try
	{
		const pisex::IGameEvent event(pEvent);
		iSlot = event.GetPlayerSlot("userid");
		iKit = event.GetInt("musickitid");
		Debug("round_mvp: slot " + std::to_string(iSlot) + ", musickitid " + std::to_string(iKit));
		MuteRoundEndMusic();
		if (iSlot >= 0) MuteMvpKit(iSlot);
	}
	catch (...)
	{
		return s2sdk::ResultType::Continue;
	}
	if (iKit != 0)
	{
		// a skin changer plugin hides the original round_mvp and sends its own with the kit of the
		// player; the client plays the anthem from musickitid, so without a kit there is no anthem
		try
		{
			pisex::IGameEvent(pEvent).SetInt("musickitid", 0);
		}
		catch (...)
		{
			return s2sdk::ResultType::Continue;
		}
		return s2sdk::ResultType::Changed;
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundEndPre(const plg::string&, void* pEvent, bool)
{
	// a callback of s2sdk: nothing may unwind out of here
	try
	{
		if (MuteKitMusicEnabled() && pEvent) MuteRoundEndMusic();
	}
	catch (...)
	{
	}
	return s2sdk::ResultType::Continue;
}

void HookEvents()
{
	// A new map drops whatever was playing (the engine took the soundevents with it): the fade and
	// the preview windows start over. A pure in-memory reset, no s2sdk call.
	pisex::RegisterMapStartCallback([](const char*) {
		for (int i = 0; i < 64; i++)
		{
			g_Playing[i] = Playing();
			g_flPreviewUntil[i] = -1.0f;
			g_flPreviewVolume[i] = 0.0f;
		}
		g_bMuteLogged = false; // the mute proof is logged once per map
	});
	// the order matters: the mutes have to run before the engine broadcasts the event
	s2sdk::HookEvent(plg::string("round_mvp"), &OnRoundMvp, s2sdk::HookMode::Pre);
	s2sdk::HookEvent(plg::string("round_end"), &OnRoundEndPre, s2sdk::HookMode::Pre);

	pisex::g_pUtils->HookEvent("round_end", [](const char*, pisex::IGameEvent*, bool) { OnRoundEnd(); });
	pisex::g_pUtils->HookEvent("round_prestart", [](const char*, pisex::IGameEvent*, bool) { RestoreKits(); });
	pisex::g_pUtils->HookEvent("player_connect_full", [](const char*, pisex::IGameEvent* pEvent, bool) {
		const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
		if (!g_Cfg.bEnabled || !g_Cfg.bConnectHint || iSlot < 0 || iSlot >= 64) return;
		Defer([iSlot]() {
			if (IsHuman(iSlot)) pisex::g_pUtils->PrintToChat(iSlot, "%s", Phrase("Connect_Hint").c_str());
		});
	});
	pisex::g_pUtils->HookEvent("player_disconnect", [](const char*, pisex::IGameEvent* pEvent, bool) {
		const int iSlot = pEvent ? pEvent->GetPlayerSlot("userid") : -1;
		if (iSlot >= 0 && iSlot < 64)
		{
			ResetPlayer(iSlot);
			g_iSavedKit[iSlot] = 0;
			g_iSavedInventoryKit[iSlot] = 0;
			g_bKitMuted[iSlot] = false;
			g_Playing[iSlot] = Playing();
			g_uLoadedSteam[iSlot] = 0;
		}
	});
}

// ---------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------

void RegisterCommands()
{
	pisex::g_pUtils->RegCommand({}, g_Cfg.vCommands, [](int iSlot, const char*) -> bool {
		if (iSlot >= 0 && g_Cfg.bEnabled) ShowMainMenu(iSlot);
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_res_reload"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot != -1) return true;
		LoadConfig();
		LoadPhrases();
		PrecacheAll();
		Log("config reloaded: " + std::to_string(g_Cfg.vTracks.size()) + " tracks");
		return true;
	});
	// the music kits of the players: m_iMusicKitID and the inventory component's m_unMusicID (the
	// reference printed both as well; with only the controller field visible, a kit the skin changer
	// had put into the inventory read as "kit 0")
	pisex::g_pUtils->RegCommand({"mm_res_kits"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot != -1) return true;
		for (int i = 0; i < 64; i++)
		{
			if (!HasController(i)) continue;
			Log("slot " + std::to_string(i) + ": kit " + std::to_string(GetMusicKit(i)) + " / inventory "
				+ std::to_string(GetInventoryMusicKit(i)) + " (saved " + std::to_string(g_iSavedKit[i]) + " / "
				+ std::to_string(g_iSavedInventoryKit[i]) + "), MVP " + std::to_string(GetMvpCount(i)) + " - "
				+ PlayerNameOf(i));
		}
		return true;
	});
	pisex::g_pUtils->RegCommand({"mm_res_test"}, {}, [](int iSlot, const char*) -> bool {
		if (iSlot == -1) OnRoundEnd();
		return true;
	});
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------------------------

class RoundEndSoundsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override
	{
		try
		{
			pisex::Init("round_end_sounds");
			g_iPluginId = static_cast<int64_t>(GetId());

			LoadConfig();
			LoadPhrases();
			for (int i = 0; i < 64; i++)
			{
				ResetPlayer(i);
				g_flPreviewUntil[i] = -1.0f;
			}

			if (!sound_volume::Init())
				LogError("soundevent volume control is unavailable (s2sdk user messages missing)");
			if (__s2sdk_OnBuildGameSessionManifest_Register)
				s2sdk::OnBuildGameSessionManifest_Register(&OnBuildManifest);
			if (sound_volume::StartMessageId() > 0 && __s2sdk_HookUserMessage)
			{
				if (!s2sdk::HookUserMessage((int16_t)sound_volume::StartMessageId(), &OnSoundEventPost, s2sdk::HookMode::Post))
					LogError("hooking GE_SosStartSoundEvent failed - player volumes will not apply");
			}

			HookEvents();
			RegisterCommands();

			g_bReady = true;
			Log("loaded: " + std::to_string(g_Cfg.vTracks.size()) + " tracks, precache: "
				+ std::to_string(g_Cfg.vPrecache.size()) + " files");
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginStart failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginStart failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override
	{
		try
		{
			pisex::Poll();
			if (!g_bReady) return {};
			ReconcileCookies((float)dt.count() / 1000.0f);
			FadeFrame();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("update failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("update failed with an unknown exception");
		}
		return {};
	}

	plg::PluginResult OnPluginEnd() override
	{
		try
		{
			g_bReady = false;
			if (sound_volume::StartMessageId() > 0 && __s2sdk_UnhookUserMessage)
				s2sdk::UnhookUserMessage((int16_t)sound_volume::StartMessageId(), &OnSoundEventPost, s2sdk::HookMode::Post);
			if (__s2sdk_UnhookEvent)
			{
				s2sdk::UnhookEvent(plg::string("round_mvp"), &OnRoundMvp, s2sdk::HookMode::Pre);
				s2sdk::UnhookEvent(plg::string("round_end"), &OnRoundEndPre, s2sdk::HookMode::Pre);
			}
			if (__s2sdk_OnBuildGameSessionManifest_Unregister)
				s2sdk::OnBuildGameSessionManifest_Unregister(&OnBuildManifest);
			pisex::Shutdown();
		}
		catch (const std::exception& ex)
		{
			LogError(std::string("OnPluginEnd failed: ") + ex.what());
		}
		catch (...)
		{
			LogError("OnPluginEnd failed with an unknown exception");
		}
		return {};
	}
};

RoundEndSoundsPlugin g_roundEndSoundsPlugin;

PLUGIFY_PLUGIN(PLUGIN_API, &g_roundEndSoundsPlugin)
