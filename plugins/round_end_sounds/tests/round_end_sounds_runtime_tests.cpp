// round_end_sounds_runtime_tests — the plugin-level test of the pure-s2sdk round_end_sounds port
// (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no plgdb; hudmenu is an optional functional API for
// the player's own menu type, not a wrapper library).
//
// The plugin TU is compiled into this test (`#include "../src/plugin.cpp"`), so the real callbacks
// run; every __s2sdk_* binding slot is a recording fake and the files the plugin reads come from a
// temporary <configs> root + working directory. Covered:
//
//   1. config/phrase resolution: HEAD's DIRECT <configs>/round_end_sounds files first, then the raw
//      working-directory file, then the Metamod addons/... spelling and the shared roots; the first
//      existing file is authoritative, a malformed DIRECT config is reported instead of falling
//      through to a valid raw decoy, and a failed reload keeps the previous configuration;
//   2. the ServerLang chain <configs>/core.cfg -> s2sdk::GetServerLanguage -> "en", with the
//      phrase file's selected language and the "en" fallback;
//   3. audio: OnBuildGameSessionManifest precaches the configured files and the sounds/... tracks,
//      round_end emits the event track on every human's pawn, file tracks go through the client's
//      `play` command, and the GE_SosStartSoundEvent hook sends a GE_SosSetSoundEventParams with
//      the packed per-player/preview volume (and GE_SosStopSoundEvent when the fade ends). Each
//      message type is resolved by its real registry name; an unresolved type is a no-op and never
//      falls back to a hard-coded id (s2sdk UserMessageCreateFromId would build a null-internals
//      object); only incoming engine messages are name-checked;
//   4. PITFALLS B1/B2: no client/entity/schema call before the first map, between OnMapEnd and the
//      next OnMapStart, or in updates the host pumps through a changelevel; the first update of a
//      plugin loaded into a running map opens the gate; OnMapEnd drops the per-map state;
//   5. C2/C1: slots 0 and 63 play, a bot/SourceTV/not-in-game slot does not, event fields -1/64
//      never reach a client, controller handles 0/-1 are refused and a valid negative handle is
//      accepted, and the MVP mute zeroes both music-kit fields then restores them on round_prestart;
//   6. PITFALLS A7/C11/C12: commands are registered with the stripped chat name and the original's
//      rights (exactly caller -1; -2/64 cannot run mm_res_*), AddConsoleCommand ownership is
//      tracked (RemoveCommand for commands we created, RemoveCommandListener for pre-existing
//      ones), paired Register+Unregister bindings are required before anything is registered, all
//      six event hooks / both map listeners / the manifest callback / the user-message hook are
//      removed with the same callbacks, and NO s2sdk timer is ever created;
//   7. menu owner identity: the SteamID AND controller handle of the menu's owner are stored at
//      creation and re-checked before any deferred playback/cookie/menu action, so a reused slot
//      (new SteamID or a new controller serial) never inherits the previous player's menu;
//   8. A7 menu fail-safe: the complete menu lifecycle binding set is required before any create
//      (reserved id 0 included), MapEnd/unload cancel with Disconnect and re-check, a still-active
//      session is switched to the builtin chat/console Close (verified with GetMenuType) before
//      DestroyMenu, and an impossible cleanup is explicitly reported as A7_UNRESOLVED.
//
// No pisexcompat, no Plugify host, no engine. Run by ../test.sh
// (build/tests/round_end_sounds_runtime_tests).

#include "../src/plugin.cpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------------------------
// checks
// ---------------------------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;

void Section(const char* name) { std::printf("-- %s\n", name); }

void Check(bool condition, const std::string& what)
{
	++g_checks;
	if (!condition)
	{
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(long long actual, long long expected, const std::string& what)
{
	++g_checks;
	if (actual != expected)
	{
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), expected, actual);
	}
}

void CheckEqStr(std::string_view actual, std::string_view expected, const std::string& what)
{
	++g_checks;
	if (actual != expected)
	{
		++g_failures;
		std::printf("FAIL: %s (expected '%s', got '%s')\n", what.c_str(), std::string(expected).c_str(),
					std::string(actual).c_str());
	}
}

std::vector<std::string> g_errors;
std::vector<std::string> g_warnings;

bool AnyErrorContains(std::string_view needle)
{
	for (const std::string& message : g_errors)
		if (message.find(needle) != std::string::npos) return true;
	return false;
}

std::string ToStr(const plg::string& text) { return std::string(text.data(), text.size()); }

// ---------------------------------------------------------------------------------------------
// temp files / working directory
// ---------------------------------------------------------------------------------------------

std::filesystem::path g_root;
std::filesystem::path g_previousCwd;

std::filesystem::path ConfigsDir() { return g_root / "configs"; }
std::filesystem::path Cwd() { return g_root; }

void WriteFile(const std::filesystem::path& path, std::string_view content)
{
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	Check(file.good(), "write " + path.string());
}

void RemoveFile(const std::filesystem::path& path)
{
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

void ResetTree()
{
	std::error_code ec;
	std::filesystem::remove_all(g_root, ec);
	std::filesystem::create_directories(g_root, ec);
	std::filesystem::create_directories(ConfigsDir(), ec);
	std::filesystem::current_path(g_root, ec);
	Check(!ec, "temporary working directory is usable");
}

std::string ConfigText(std::string_view trackName, std::string_view trackSound,
					   std::string_view commands = "!res,!music", int fadeBeforeSeconds = 1)
{
	std::string text = "\"RoundEndSounds\"\n{\n";
	text += "    \"enabled\" \"1\"\n";
	text += "    \"commands\" \"" + std::string(commands) + "\"\n";
	text += "    \"random\" \"0\"\n"; // deterministic: track order 0,1,0,1...
	text += "    \"default_enabled\" \"1\"\n";
	text += "    \"default_volume\" \"0.5\"\n";
	text += "    \"message\" \"both\"\n";
	text += "    \"center_time\" \"7\"\n";
	text += "    \"mute_mvp_anthem\" \"1\"\n";
	text += "    \"fade_before_round\" \"" + std::to_string(fadeBeforeSeconds) + "\"\n";
	text += "    \"connect_hint\" \"1\"\n";
	text += "    \"debug\" \"0\"\n";
	text += "    \"precache\" \"soundevents/test.vsndevts\"\n";
	text += "    \"tracks\"\n    {\n";
	text += "        \"" + std::string(trackName) + "\" { \"sound\" \"" + std::string(trackSound) + "\" }\n";
	text += "        \"Second\" \"sounds/test/second.vsnd\"\n";
	text += "    }\n}\n";
	return text;
}

std::string PhrasesText(std::string_view hintEn, std::string_view hintRu)
{
	std::string text = "\"Phrases\"\n{\n";
	text += "    \"Prefix\" { \"en\" \"[P] \" \"ru\" \"[П] \" }\n";
	text += "    \"Connect_Hint\" { \"en\" \"" + std::string(hintEn) + "\" \"ru\" \"" + std::string(hintRu) + "\" }\n";
	text += "    \"Now_Playing\" { \"en\" \"Now {name}\" \"ru\" \"Играет {name}\" }\n";
	text += "    \"Now_Playing_Center\" { \"en\" \"CENTER {name}{picture}\" \"ru\" \"ЦЕНТР {name}{picture}\" }\n";
	text += "    \"State_Set\" { \"en\" \"Music: {state}\" \"ru\" \"Музыка: {state}\" }\n";
	text += "    \"On\" { \"en\" \"on\" \"ru\" \"вкл\" }\n";
	text += "    \"Off\" { \"en\" \"off\" \"ru\" \"выкл\" }\n";
	text += "    \"Menu_Title\" { \"en\" \"Menu\" \"ru\" \"Меню\" }\n";
	text += "    \"Menu_State\" { \"en\" \"Music: {state}\" \"ru\" \"Музыка: {state}\" }\n";
	text += "    \"Menu_Volume\" { \"en\" \"Volume: {volume}%\" \"ru\" \"Громкость: {volume}%\" }\n";
	text += "    \"Menu_Tracks\" { \"en\" \"Tracks\" \"ru\" \"Треки\" }\n";
	text += "    \"Menu_Last\" { \"en\" \"Last\" \"ru\" \"Последний\" }\n";
	text += "    \"Volume_Title\" { \"en\" \"Volume: {volume}%\" \"ru\" \"Громкость: {volume}%\" }\n";
	text += "    \"Volume_Up\" { \"en\" \"Up\" \"ru\" \"Громче\" }\n";
	text += "    \"Volume_Down\" { \"en\" \"Down\" \"ru\" \"Тише\" }\n";
	text += "    \"Tracks_Title\" { \"en\" \"Tracks\" \"ru\" \"Треки\" }\n";
	text += "    \"Preview\" { \"en\" \"Preview {name}\" \"ru\" \"Прослушивание {name}\" }\n";
	text += "    \"No_Last\" { \"en\" \"No last\" \"ru\" \"Нет последнего\" }\n";
	text += "    \"Exit\" { \"en\" \"Exit\" \"ru\" \"Выход\" }\n";
	text += "}\n";
	return text;
}

void WritePluginConfig(std::string_view text)
{
	WriteFile(ConfigsDir() / "round_end_sounds" / "round_end_sounds.ini", text);
}

void WritePluginPhrases(std::string_view text)
{
	WriteFile(ConfigsDir() / "round_end_sounds" / "translations" / "round_end_sounds.phrases.txt", text);
}

void WriteRawConfig(std::string_view text) { WriteFile(Cwd() / "round_end_sounds.ini", text); }
void WriteRawPhrases(std::string_view text) { WriteFile(Cwd() / "round_end_sounds.phrases.txt", text); }
void WriteCoreConfig(std::string_view text) { WriteFile(ConfigsDir() / "core.cfg", text); }

// ---------------------------------------------------------------------------------------------
// recording fakes
// ---------------------------------------------------------------------------------------------

struct FakeSlot
{
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	bool authorized = false;
	bool controllerPresent = false;
	bool validPointer = true;
	int32_t controllerHandle = 0;
	std::string name = "<unknown>";
	uint64_t steam = 0;
	bool servicesPresent = true;
	int32_t controllerKit = 0;
	int32_t inventoryKit = 0;
	int32_t mvps = 0;
};

struct FakeEvent
{
	std::map<std::string, int32_t> slots;
	std::map<std::string, int32_t> ints;
};

struct FakeMessage
{
	int16_t id = 0;
	std::string name;
	bool hasGuid = false;
	int32_t guid = -1;
	uint32_t hash = 0;
	uint64_t mask = 0;
	int32_t setGuid = -1;
	std::string packed;
};

struct SentMessage
{
	int16_t id = 0;
	int32_t guid = 0;
	std::string packed;
	uint64_t mask = 0;
};

struct MenuCall
{
	s2sdk::MenuId id = 0;
	int32_t slot = -1;
	std::string action;
};

struct FakeState
{
	std::array<FakeSlot, 64> slots{};
	FakeEvent event;
	std::string serverLanguage = "en";
	float gameTime = 10.0f;
	float restartTime = 0.0f;
	int64_t noMusic = 0;
	bool hasProxy = true;
	bool hasRules = true;

	// command/menu/user-message registry
	std::map<std::string, s2sdk::ConCommandCallback> commands;
	bool createCommands = true;
	std::vector<std::string> addCommandNames;
	std::vector<std::string> removeCommandNames;
	std::vector<std::string> removeListenerNames;
	std::vector<std::string> sayListeners;

	int timerCalls = 0;

	std::map<std::string, int16_t> messageIds;
	std::map<int16_t, std::string> messageNameById;
	std::map<int16_t, s2sdk::UserMessageCallback> messageHooks;
	std::vector<std::pair<int16_t, s2sdk::UserMessageCallback>> unhookCalls;
	int findCalls = 0;
	int createCalls = 0;
	int getNameCalls = 0;
	int pbCalls = 0;
	int sendCalls = 0;
	std::vector<int16_t> createdIds;
	std::vector<SentMessage> sent;
	std::vector<std::string> precached;
	std::vector<std::string> emitted;
	std::vector<float> emittedVolumes;
	std::vector<int> emittedSlots;
	std::vector<std::string> clientCommands;
	std::vector<int> clientCommandSlots;
	std::vector<std::pair<int, std::string>> printed;
	std::vector<std::pair<int, std::string>> centerHtml;

	// menus: id 0 is the reserved dummy, then real menus
	s2sdk::MenuId nextMenuId = 0;
	std::map<s2sdk::MenuId, s2sdk::MenuHandlerCallback> menuHandlers;
	std::map<s2sdk::MenuId, std::vector<std::pair<std::string, std::string>>> menuItems;
	std::map<s2sdk::MenuId, int> menuSlots;
	std::map<s2sdk::MenuId, std::string> menuTypeById;
	std::array<s2sdk::MenuId, 64> clientMenu{};
	std::vector<std::string> registeredMenuTypes{"chat", "centerhtml", "button"};
	std::vector<s2sdk::MenuId> destroyedMenus;
	std::vector<MenuCall> menuCalls;
	std::vector<std::string> menuTypes;
	std::vector<int> cancelCalls;
	std::vector<std::pair<s2sdk::MenuId, std::string>> menuTypeSwitches;
	std::string hudmenuChoice;
	bool hudmenuThrows = false;
	bool cancelFails = false;
	bool cancelCloses = true;
	bool setTypeFails = false;
	bool displayFails = false;
	int backendCloseCalls = 0;

	// map / manifest / events
	s2sdk::OnMapStartCallback mapStart = nullptr;
	s2sdk::OnMapEndCallback mapEnd = nullptr;
	bool mapListenersRegistered = false;
	int mapStartUnregister = 0;
	int mapEndUnregister = 0;
	s2sdk::OnBuildGameSessionManifestCallback manifest = nullptr;
	bool manifestRegistered = false;
	int manifestUnregister = 0;

	struct EventHook
	{
		std::string name;
		s2sdk::EventCallback callback = nullptr;
		s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	};
	std::vector<EventHook> eventHooks;
	std::vector<EventHook> unhookedEvents;
	std::vector<s2sdk::ResultType> eventResults;
	std::vector<int> eventSetInts;

	// schema
	std::vector<std::string> schemaLookups;
	std::vector<std::string> entDataWrites;
	std::string lastNetworkStateChanged;
};

FakeState g_fake;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/)
{
	if (severity == plg::Severity::Error)
	{
		g_errors.emplace_back(message);
	}
	else if (severity == plg::Severity::Warning)
	{
		g_warnings.emplace_back(message);
	}
}

plg::string FakeGetConfigsDir() { return plg::string(ConfigsDir().string()); }

// pointer identities
uintptr_t ControllerPtr(int slot) { return 0x100000 + static_cast<uintptr_t>(slot) * 0x100; }
uintptr_t PawnPtr(int slot) { return 0x200000 + static_cast<uintptr_t>(slot) * 0x100; }
uintptr_t ServicesPtr(int slot) { return 0x300000 + static_cast<uintptr_t>(slot) * 0x100; }
constexpr uintptr_t kProxyPtr = 0x400000;
constexpr uintptr_t kRulesPtr = 0x410000;

int SlotFromPointer(void* pointer)
{
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	for (int slot = 0; slot < 64; ++slot)
	{
		if (value == ControllerPtr(slot) || value == PawnPtr(slot) || value == ServicesPtr(slot)) return slot;
	}
	return -1;
}

int SlotFromServicesPointer(void* pointer)
{
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	for (int slot = 0; slot < 64; ++slot)
		if (value == ServicesPtr(slot)) return slot;
	return -1;
}

int SlotFromControllerHandle(int32_t handle)
{
	for (int slot = 0; slot < 64; ++slot)
		if (g_fake.slots[slot].controllerHandle == handle) return slot;
	return -1;
}

int SlotFromPawnHandle(int32_t handle)
{
	if (handle >= 3000 && handle < 3000 + 64) return handle - 3000;
	return -1;
}

bool FakeIsClientConnected(int32_t slot)
{
	return slot >= 0 && slot < 64 && g_fake.slots[slot].connected;
}
bool FakeIsClientInGame(int32_t slot)
{
	return slot >= 0 && slot < 64 && g_fake.slots[slot].inGame;
}
bool FakeIsClientSourceTV(int32_t slot)
{
	return slot >= 0 && slot < 64 && g_fake.slots[slot].sourceTV;
}
bool FakeIsFakeClient(int32_t slot)
{
	return slot >= 0 && slot < 64 && g_fake.slots[slot].fake;
}
bool FakeIsClientAuthorized(int32_t slot)
{
	return slot >= 0 && slot < 64 && g_fake.slots[slot].authorized;
}
plg::string FakeGetClientName(int32_t slot)
{
	if (slot < 0 || slot >= 64) return plg::string("<unknown>");
	return plg::string(g_fake.slots[slot].name);
}
uint64_t FakeGetClientSteamID64(int32_t slot)
{
	if (slot < 0 || slot >= 64) return 0;
	return g_fake.slots[slot].steam;
}

void* FakePlayerSlotToEntPointer(int32_t slot)
{
	if (slot < 0 || slot >= 64 || !g_fake.slots[slot].controllerPresent) return nullptr;
	return reinterpret_cast<void*>(ControllerPtr(slot));
}
int32_t FakePlayerSlotToEntHandle(int32_t slot)
{
	if (slot < 0 || slot >= 64 || !g_fake.slots[slot].controllerPresent) return -1;
	return g_fake.slots[slot].controllerHandle;
}
int32_t FakeEntPointerToEntIndex(void* pointer)
{
	const int slot = SlotFromPointer(pointer);
	if (slot < 0) return 0;
	if (reinterpret_cast<uintptr_t>(pointer) == ControllerPtr(slot)) return slot + 1;
	return 100 + slot;
}
int32_t FakeEntPointerToEntHandle(void* pointer)
{
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	if (value == kProxyPtr) return 9000;
	const int slot = SlotFromPointer(pointer);
	if (slot < 0) return -1;
	if (value == ControllerPtr(slot)) return g_fake.slots[slot].controllerHandle;
	return 3000 + slot; // pawn
}
bool FakeIsValidEntPointer(void* pointer) { return pointer != nullptr; }
bool FakeIsValidEntHandle(int32_t handle) { return handle != 0 && handle != -1; }
void* FakeGetClientPawn(int32_t slot)
{
	if (slot < 0 || slot >= 64 || !g_fake.slots[slot].controllerPresent) return nullptr;
	return reinterpret_cast<void*>(PawnPtr(slot));
}

int32_t FakeGetSchemaOffset(const plg::string& cls, const plg::string& field)
{
	const std::string key = ToStr(cls) + "::" + ToStr(field);
	g_fake.schemaLookups.push_back(key);
	if (key == "CCSGameRules::m_bRoundEndNoMusic") return 100;
	if (key == "CCSGameRules::m_flRestartRoundTime") return 104;
	if (key == "CCSGameRulesProxy::m_pGameRules") return 200;
	if (key == "CCSPlayerController::m_iMusicKitID") return 300;
	if (key == "CCSPlayerController::m_pInventoryServices") return 304;
	if (key == "CCSPlayerController::m_iMVPs") return 308;
	if (key == "CCSPlayerController_InventoryServices::m_unMusicID") return 400;
	return -1;
}
int32_t FakeGetSchemaChainOffset(const plg::string& cls)
{
	const std::string name = ToStr(cls);
	if (name == "CCSGameRules") return 8;
	if (name == "CCSPlayerController_InventoryServices") return 8;
	return -1;
}
int64_t FakeGetEntData(int32_t entityHandle, int32_t offset, int32_t /*size*/)
{
	const int slot = SlotFromControllerHandle(entityHandle);
	if (slot < 0) return 0;
	if (offset == 300) return g_fake.slots[slot].controllerKit;
	if (offset == 304) return static_cast<int64_t>(static_cast<intptr_t>(ServicesPtr(slot)));
	if (offset == 308) return g_fake.slots[slot].mvps;
	return 0;
}
void FakeSetEntData(int32_t entityHandle, int32_t offset, int64_t value, int32_t /*size*/, bool /*changeState*/,
					int32_t chainOffset)
{
	g_fake.entDataWrites.push_back("handle" + std::to_string(entityHandle) + ":" + std::to_string(offset) + "=" +
								   std::to_string(value) + " chain " + std::to_string(chainOffset));
	const int slot = SlotFromControllerHandle(entityHandle);
	if (slot >= 0 && offset == 300) g_fake.slots[slot].controllerKit = static_cast<int32_t>(value);
}
int64_t FakeGetEntData2(void* entity, int32_t offset, int32_t /*size*/)
{
	const uintptr_t value = reinterpret_cast<uintptr_t>(entity);
	if (value == kProxyPtr && offset == 200) return static_cast<int64_t>(kRulesPtr);
	if (value == kRulesPtr && offset == 100) return g_fake.noMusic;
	if (value == kRulesPtr && offset == 200) return static_cast<int64_t>(kRulesPtr);
	const int slot = SlotFromServicesPointer(entity);
	if (slot >= 0 && offset == 400) return g_fake.slots[slot].inventoryKit;
	return 0;
}
void FakeSetEntData2(void* entity, int32_t offset, int64_t value, int32_t /*size*/, bool changeState,
					 int32_t chainOffset)
{
	const uintptr_t pointer = reinterpret_cast<uintptr_t>(entity);
	g_fake.entDataWrites.push_back("ptr" + std::to_string(pointer) + ":" + std::to_string(offset) + "=" +
								   std::to_string(value) + (changeState ? " state" : " plain") + " chain " +
								   std::to_string(chainOffset));
	if (pointer == kRulesPtr && offset == 100) g_fake.noMusic = value;
	const int slot = SlotFromServicesPointer(entity);
	if (slot >= 0 && offset == 400) g_fake.slots[slot].inventoryKit = static_cast<int32_t>(value);
}
double FakeGetEntDataFloat2(void* entity, int32_t offset, int32_t /*size*/)
{
	if (reinterpret_cast<uintptr_t>(entity) == kRulesPtr && offset == 104) return g_fake.restartTime;
	return 0.0;
}
void* FakeGetGameRulesProxy() { return g_fake.hasProxy ? reinterpret_cast<void*>(kProxyPtr) : nullptr; }
void* FakeGetGameRules() { return g_fake.hasRules ? reinterpret_cast<void*>(kRulesPtr) : nullptr; }
void FakeNetworkStateChanged(int32_t handle, const plg::string& cls, const plg::string& field)
{
	g_fake.lastNetworkStateChanged = std::to_string(handle) + ":" + ToStr(cls) + "::" + ToStr(field);
}

void FakePrecache(const plg::string& resource) { g_fake.precached.push_back(ToStr(resource)); }
void FakeEmitSound(int32_t handle, const plg::string& sound, int32_t /*pitch*/, float volume, float /*delay*/)
{
	g_fake.emitted.push_back(ToStr(sound));
	g_fake.emittedVolumes.push_back(volume);
	g_fake.emittedSlots.push_back(SlotFromPawnHandle(handle));
}
void FakeClientCommand(int32_t slot, const plg::string& command)
{
	g_fake.clientCommandSlots.push_back(slot);
	g_fake.clientCommands.push_back(ToStr(command));
}
void FakePrintToChat(int32_t slot, const plg::string& message)
{
	g_fake.printed.emplace_back(slot, ToStr(message));
}
void FakePrintCentreHtml(int32_t slot, const plg::string& message, int32_t /*duration*/)
{
	g_fake.centerHtml.emplace_back(slot, ToStr(message));
}
float FakeGetGameTime() { return g_fake.gameTime; }
plg::string FakeGetServerLanguage() { return plg::string(g_fake.serverLanguage); }

// events
s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode)
{
	g_fake.eventHooks.push_back({ToStr(name), callback, mode});
	return s2sdk::EventHookError::Okay;
}
s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode)
{
	g_fake.unhookedEvents.push_back({ToStr(name), callback, mode});
	g_fake.eventHooks.erase(std::remove_if(g_fake.eventHooks.begin(), g_fake.eventHooks.end(),
										  [&](const FakeState::EventHook& hook) {
											  return hook.name == ToStr(name) && hook.callback == callback &&
													 hook.mode == mode;
										  }),
							g_fake.eventHooks.end());
	return s2sdk::EventHookError::Okay;
}
int32_t FakeGetEventPlayerSlot(void* /*event*/, const plg::string& key)
{
	const auto it = g_fake.event.slots.find(ToStr(key));
	return it != g_fake.event.slots.end() ? it->second : -1;
}
int32_t FakeGetEventInt(void* /*event*/, const plg::string& key)
{
	const auto it = g_fake.event.ints.find(ToStr(key));
	return it != g_fake.event.ints.end() ? it->second : 0;
}
void FakeSetEventInt(void* /*event*/, const plg::string& key, int32_t value)
{
	(void)key;
	g_fake.eventSetInts.push_back(value);
}

// map / manifest
void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback)
{
	g_fake.mapStart = callback;
	g_fake.mapListenersRegistered = true;
}
void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback)
{
	g_fake.mapEnd = callback;
	g_fake.mapListenersRegistered = true;
}
void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/)
{
	g_fake.mapStart = nullptr;
	++g_fake.mapStartUnregister;
}
void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/)
{
	g_fake.mapEnd = nullptr;
	++g_fake.mapEndUnregister;
}
void FakeManifestRegister(s2sdk::OnBuildGameSessionManifestCallback callback)
{
	g_fake.manifest = callback;
	g_fake.manifestRegistered = true;
}
void FakeManifestUnregister(s2sdk::OnBuildGameSessionManifestCallback /*callback*/)
{
	g_fake.manifest = nullptr;
	g_fake.manifestRegistered = false;
	++g_fake.manifestUnregister;
}

// commands
bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/, s2sdk::ConVarFlag /*flags*/,
						   s2sdk::ConCommandCallback callback, s2sdk::HookMode /*mode*/)
{
	const std::string command = ToStr(name);
	g_fake.addCommandNames.push_back(command);
	g_fake.commands[command] = callback;
	return g_fake.createCommands;
}
bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/)
{
	g_fake.removeCommandNames.push_back(ToStr(name));
	g_fake.commands.erase(ToStr(name));
	return true;
}
bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/, s2sdk::HookMode /*mode*/)
{
	g_fake.sayListeners.push_back(ToStr(name));
	return true;
}
bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/, s2sdk::HookMode /*mode*/)
{
	g_fake.removeListenerNames.push_back(ToStr(name));
	return true;
}

// timers: the plugin must never touch this API
s2sdk::TimerId FakeCreateTimer(double /*delay*/, s2sdk::TimerCallback /*callback*/, s2sdk::TimerFlag /*flags*/,
							   const plg::vector<plg::any>& /*userData*/)
{
	++g_fake.timerCalls;
	return 1;
}

// user messages
int16_t FakeUserMessageFindMessageIdByName(const plg::string& name)
{
	++g_fake.findCalls;
	const auto it = g_fake.messageIds.find(ToStr(name));
	return it != g_fake.messageIds.end() ? it->second : 0;
}
bool FakeHookUserMessage(int16_t id, s2sdk::UserMessageCallback callback, s2sdk::HookMode /*mode*/)
{
	g_fake.messageHooks[id] = callback;
	return true;
}
bool FakeUnhookUserMessage(int16_t id, s2sdk::UserMessageCallback callback, s2sdk::HookMode /*mode*/)
{
	g_fake.unhookCalls.emplace_back(id, callback);
	g_fake.messageHooks.erase(id);
	return true;
}
void* FakeUserMessageCreateFromId(int16_t id)
{
	++g_fake.createCalls;
	g_fake.createdIds.push_back(id);
	auto* message = new FakeMessage();
	message->id = id;
	const auto it = g_fake.messageNameById.find(id);
	message->name = it != g_fake.messageNameById.end() ? it->second : "CMsgUnknown";
	return message;
}
void FakeUserMessageDestroy(void* message) { delete static_cast<FakeMessage*>(message); }
void FakeUserMessageSend(void* raw)
{
	++g_fake.sendCalls;
	auto* message = static_cast<FakeMessage*>(raw);
	g_fake.sent.push_back(SentMessage{message->id, message->setGuid, message->packed, message->mask});
}
plg::string FakeUserMessageGetMessageName(void* raw)
{
	++g_fake.getNameCalls;
	return plg::string(static_cast<FakeMessage*>(raw)->name);
}
int16_t FakeUserMessageGetMessageID(void* raw) { return static_cast<FakeMessage*>(raw)->id; }
bool FakeUserMessageHasField(void* raw, const plg::string& field)
{
	return ToStr(field) == "soundevent_guid" ? static_cast<FakeMessage*>(raw)->hasGuid : false;
}
uint64_t FakeUserMessageGetRecipientMask(void* raw) { return static_cast<FakeMessage*>(raw)->mask; }
void FakeUserMessageSetRecipientMask(void* raw, uint64_t mask) { static_cast<FakeMessage*>(raw)->mask = mask; }
int32_t FakePbReadInt32(void* raw, const plg::string& field, int32_t /*index*/)
{
	++g_fake.pbCalls;
	return ToStr(field) == "soundevent_guid" ? static_cast<FakeMessage*>(raw)->guid : 0;
}
uint32_t FakePbReadUInt32(void* raw, const plg::string& field, int32_t /*index*/)
{
	++g_fake.pbCalls;
	return ToStr(field) == "soundevent_hash" ? static_cast<FakeMessage*>(raw)->hash : 0;
}
bool FakePbSetInt32(void* raw, const plg::string& field, int32_t value)
{
	++g_fake.pbCalls;
	if (ToStr(field) == "soundevent_guid") static_cast<FakeMessage*>(raw)->setGuid = value;
	return true;
}
bool FakePbSetString(void* raw, const plg::string& field, const plg::string& value)
{
	++g_fake.pbCalls;
	if (ToStr(field) == "packed_params") static_cast<FakeMessage*>(raw)->packed = ToStr(value);
	return true;
}

// menus
s2sdk::MenuId FakeCreateMenu(const plg::string& /*title*/, s2sdk::MenuHandlerCallback handler,
							 const plg::string& type)
{
	const s2sdk::MenuId id = g_fake.nextMenuId++;
	g_fake.menuHandlers[id] = handler;
	g_fake.menuTypes.push_back(ToStr(type));
	g_fake.menuTypeById[id] = ToStr(type);
	return id;
}
int32_t FakeAddMenuItem(s2sdk::MenuId id, const plg::string& info, const plg::string& display,
						s2sdk::MenuItemStyle /*style*/)
{
	auto& items = g_fake.menuItems[id];
	items.emplace_back(ToStr(info), ToStr(display));
	return static_cast<int32_t>(items.size()) - 1;
}
bool FakeSetMenuPagination(s2sdk::MenuId /*id*/, int32_t /*itemsPerPage*/) { return true; }
bool FakeSetMenuExitButton(s2sdk::MenuId /*id*/, bool /*enabled*/) { return true; }
bool FakeSetMenuExitBackButton(s2sdk::MenuId /*id*/, bool /*enabled*/) { return true; }
bool FakeSetMenuCloseOnSelect(s2sdk::MenuId /*id*/, bool /*enabled*/) { return true; }
bool FakeDisplayMenu(s2sdk::MenuId id, int32_t slot, int32_t /*time*/)
{
	if (g_fake.displayFails) return false;
	g_fake.menuSlots[id] = slot;
	g_fake.clientMenu[slot] = id;
	return true;
}
bool FakeDestroyMenu(s2sdk::MenuId id)
{
	// A custom backend Close reaches the client: simulate the live-client backend close unless the
	// menu was switched to the no-op builtin chat/console type first.
	for (int slot = 0; slot < 64; ++slot)
	{
		if (g_fake.clientMenu[slot] != id) continue;
		const std::string type = g_fake.menuTypeById[id];
		if (type != "chat" && type != "console") ++g_fake.backendCloseCalls;
		g_fake.clientMenu[slot] = 0;
	}
	g_fake.destroyedMenus.push_back(id);
	g_fake.menuHandlers.erase(id);
	g_fake.menuItems.erase(id);
	g_fake.menuSlots.erase(id);
	g_fake.menuTypeById.erase(id);
	return true;
}
bool FakeCancelClientMenu(int32_t slot, s2sdk::MenuCancelReason reason)
{
	g_fake.cancelCalls.push_back(slot);
	if (g_fake.cancelFails)
	{
		if (g_fake.cancelCloses) g_fake.clientMenu[slot] = 0;
		return false;
	}
	for (const auto& [id, menuSlot] : g_fake.menuSlots)
	{
		if (menuSlot != slot) continue;
		const auto handler = g_fake.menuHandlers.find(id);
		if (handler != g_fake.menuHandlers.end())
		{
			handler->second(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(reason));
			handler->second(id, s2sdk::MenuAction::End, slot, 0);
		}
		break;
	}
	g_fake.clientMenu[slot] = 0;
	return true;
}
s2sdk::MenuId FakeGetClientMenu(int32_t slot)
{
	if (slot < 0 || slot >= 64) return 0;
	return g_fake.clientMenu[slot];
}
bool FakeSetMenuType(s2sdk::MenuId id, const plg::string& type)
{
	if (g_fake.setTypeFails || g_fake.menuHandlers.count(id) == 0) return false;
	const std::string name = ToStr(type);
	g_fake.menuTypeById[id] = name;
	g_fake.menuTypeSwitches.emplace_back(id, name);
	return true;
}
plg::string FakeGetMenuType(s2sdk::MenuId id)
{
	const auto it = g_fake.menuTypeById.find(id);
	return plg::string(it != g_fake.menuTypeById.end() ? it->second : std::string());
}
bool FakeIsValidMenu(s2sdk::MenuId id) { return g_fake.menuHandlers.count(id) != 0; }
bool FakeIsMenuTypeRegistered(const plg::string& name)
{
	const std::string type = ToStr(name);
	return std::find(g_fake.registeredMenuTypes.begin(), g_fake.registeredMenuTypes.end(), type) !=
		   g_fake.registeredMenuTypes.end();
}
plg::string FakeHudmenuGetClientMenuType(int32_t /*slot*/)
{
	if (g_fake.hudmenuThrows) throw std::runtime_error("fake hudmenu failure");
	return plg::string(g_fake.hudmenuChoice);
}

void InstallFakes()
{
	g_fake = FakeState{};
	g_errors.clear();
	g_warnings.clear();

	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;

	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetSchemaChainOffset = &FakeGetSchemaChainOffset;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_SetEntData = &FakeSetEntData;
	__s2sdk_GetEntData2 = &FakeGetEntData2;
	__s2sdk_SetEntData2 = &FakeSetEntData2;
	__s2sdk_GetEntDataFloat2 = &FakeGetEntDataFloat2;
	__s2sdk_GetGameRulesProxy = &FakeGetGameRulesProxy;
	__s2sdk_GetGameRules = &FakeGetGameRules;
	__s2sdk_NetworkStateChanged = &FakeNetworkStateChanged;

	__s2sdk_Precache = &FakePrecache;
	__s2sdk_EmitSound = &FakeEmitSound;
	__s2sdk_ClientCommand = &FakeClientCommand;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintCentreHtml = &FakePrintCentreHtml;
	__s2sdk_GetGameTime = &FakeGetGameTime;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_SetEventInt = &FakeSetEventInt;

	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_OnBuildGameSessionManifest_Register = &FakeManifestRegister;
	__s2sdk_OnBuildGameSessionManifest_Unregister = &FakeManifestUnregister;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
	__s2sdk_CreateTimer = &FakeCreateTimer;

	__s2sdk_UserMessageFindMessageIdByName = &FakeUserMessageFindMessageIdByName;
	__s2sdk_HookUserMessage = &FakeHookUserMessage;
	__s2sdk_UnhookUserMessage = &FakeUnhookUserMessage;
	__s2sdk_UserMessageCreateFromId = &FakeUserMessageCreateFromId;
	__s2sdk_UserMessageDestroy = &FakeUserMessageDestroy;
	__s2sdk_UserMessageSend = &FakeUserMessageSend;
	__s2sdk_UserMessageGetMessageName = &FakeUserMessageGetMessageName;
	__s2sdk_UserMessageGetMessageID = &FakeUserMessageGetMessageID;
	__s2sdk_UserMessageHasField = &FakeUserMessageHasField;
	__s2sdk_UserMessageGetRecipientMask = &FakeUserMessageGetRecipientMask;
	__s2sdk_UserMessageSetRecipientMask = &FakeUserMessageSetRecipientMask;
	__s2sdk_PbReadInt32 = &FakePbReadInt32;
	__s2sdk_PbReadUInt32 = &FakePbReadUInt32;
	__s2sdk_PbSetInt32 = &FakePbSetInt32;
	__s2sdk_PbSetString = &FakePbSetString;

	__s2sdk_CreateMenu = &FakeCreateMenu;
	__s2sdk_AddMenuItem = &FakeAddMenuItem;
	__s2sdk_SetMenuPagination = &FakeSetMenuPagination;
	__s2sdk_SetMenuExitButton = &FakeSetMenuExitButton;
	__s2sdk_SetMenuExitBackButton = &FakeSetMenuExitBackButton;
	__s2sdk_SetMenuCloseOnSelect = &FakeSetMenuCloseOnSelect;
	__s2sdk_DisplayMenu = &FakeDisplayMenu;
	__s2sdk_DestroyMenu = &FakeDestroyMenu;
	__s2sdk_CancelClientMenu = &FakeCancelClientMenu;
	__s2sdk_GetClientMenu = &FakeGetClientMenu;
	__s2sdk_SetMenuType = &FakeSetMenuType;
	__s2sdk_GetMenuType = &FakeGetMenuType;
	__s2sdk_IsValidMenu = &FakeIsValidMenu;
	__s2sdk_IsMenuTypeRegistered = &FakeIsMenuTypeRegistered;
	__hudmenu_GetClientMenuType = &FakeHudmenuGetClientMenuType;

	// The s2sdk sound-event ids the plugin tries to resolve lazily.
	g_fake.messageIds["SosStartSoundEvent"] = 208;
	g_fake.messageIds["SosSetSoundEventParams"] = 210;
	g_fake.messageIds["SosStopSoundEvent"] = 209;
	g_fake.messageNameById[208] = "CMsgSosStartSoundEvent";
	g_fake.messageNameById[209] = "CMsgSosStopSoundEvent";
	g_fake.messageNameById[210] = "CMsgSosSetSoundEventParams";

	// No permissions plugin: ReconcileCookies is a no-op and the player defaults stay in use.
	__permissions_LoadUser = nullptr;
	__permissions_GetCookie = nullptr;
	__permissions_SetCookie = nullptr;
}

// ---------------------------------------------------------------------------------------------
// driving the plugin
// ---------------------------------------------------------------------------------------------

bool g_pluginStarted = false;

void AddPlayer(int slot, const std::string& name, uint64_t steam = 1000 + 1)
{
	g_fake.slots[slot].connected = true;
	g_fake.slots[slot].inGame = true;
	g_fake.slots[slot].fake = false;
	g_fake.slots[slot].authorized = true;
	g_fake.slots[slot].controllerPresent = true;
	g_fake.slots[slot].controllerHandle = 1000 + slot;
	g_fake.slots[slot].name = name;
	g_fake.slots[slot].steam = steam;
}

void AddBot(int slot) { AddPlayer(slot, "BOT", 0); g_fake.slots[slot].fake = true; }

void Start()
{
	Check(!g_pluginStarted, "the plugin is not started twice without OnPluginEnd");
	const plg::PluginResult result = g_roundEndSoundsPlugin.OnPluginStart();
	g_pluginStarted = true;
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart answers Ok (never Failed, PITFALLS A1)");
}

void Update(int milliseconds = 100)
{
	g_roundEndSoundsPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}

void Stop()
{
	if (!g_pluginStarted) return;
	g_roundEndSoundsPlugin.OnPluginEnd();
	g_pluginStarted = false;
}

void FireMapStart()
{
	if (g_fake.mapStart != nullptr) g_fake.mapStart();
}

void FireMapEnd()
{
	if (g_fake.mapEnd != nullptr) g_fake.mapEnd();
}

void FireManifest()
{
	if (g_fake.manifest != nullptr) g_fake.manifest();
}

void FireEvent(const std::string& name)
{
	// Copy: a callback could in theory register/unregister (none of ours does).
	const std::vector<FakeState::EventHook> hooks = g_fake.eventHooks;
	for (const FakeState::EventHook& hook : hooks)
		if (hook.name == name && hook.mode == s2sdk::HookMode::Pre && hook.callback != nullptr)
			g_fake.eventResults.push_back(hook.callback(plg::string(name), &g_fake.event, false));
}

bool AllEventResultsContinue()
{
	for (const s2sdk::ResultType result : g_fake.eventResults)
		if (result != s2sdk::ResultType::Continue) return false;
	return true;
}

void FireSoundStart(int32_t guid, uint32_t hash, uint64_t mask, const std::string& name = "CMsgSosStartSoundEvent")
{
	const auto it = g_fake.messageHooks.find(208);
	if (it == g_fake.messageHooks.end()) return;
	FakeMessage message;
	message.id = 208;
	message.name = name;
	message.hasGuid = true;
	message.guid = guid;
	message.hash = hash;
	message.mask = mask;
	it->second(&message);
}

// Mimics s2sdk's SelectMenuItem: close the display first, then Select + End callbacks.
void SelectMenu(s2sdk::MenuId id, int slot, int index)
{
	auto it = g_fake.menuHandlers.find(id);
	Check(it != g_fake.menuHandlers.end(), "the menu has a handler");
	if (it == g_fake.menuHandlers.end()) return;
	if (slot >= 0 && slot < 64 && g_fake.clientMenu[slot] == id) g_fake.clientMenu[slot] = 0;
	it->second(id, s2sdk::MenuAction::Select, slot, index);
	it->second(id, s2sdk::MenuAction::End, slot, 0);
}

// Mimics the exit-back button: close the display, then Cancel(ExitBack) + End.
void ExitBackMenu(s2sdk::MenuId id, int slot)
{
	auto it = g_fake.menuHandlers.find(id);
	Check(it != g_fake.menuHandlers.end(), "the menu has a handler");
	if (it == g_fake.menuHandlers.end()) return;
	if (slot >= 0 && slot < 64 && g_fake.clientMenu[slot] == id) g_fake.clientMenu[slot] = 0;
	it->second(id, s2sdk::MenuAction::Cancel, slot, static_cast<int32_t>(s2sdk::MenuCancelReason::ExitBack));
	it->second(id, s2sdk::MenuAction::End, slot, 0);
}

bool IsCommandRegistered(const std::string& name) { return g_fake.commands.count(name) != 0; }

void CallCommand(const std::string& name, int32_t caller, s2sdk::ConCommandContext context)
{
	const auto it = g_fake.commands.find(name);
	Check(it != g_fake.commands.end(), "command '" + name + "' is registered");
	if (it == g_fake.commands.end() || it->second == nullptr) return;
	plg::vector<plg::string> args;
	args.push_back(plg::string(name));
	it->second(caller, context, args);
}

bool HasPrinted(int slot, std::string_view needle)
{
	for (const auto& [printedSlot, message] : g_fake.printed)
		if (printedSlot == slot && message.find(needle) != std::string::npos) return true;
	return false;
}

int CountEmitted(const std::string& sound)
{
	return static_cast<int>(std::count(g_fake.emitted.begin(), g_fake.emitted.end(), sound));
}

const SentMessage* LastSent()
{
	return g_fake.sent.empty() ? nullptr : &g_fake.sent.back();
}

// A usable plugin instance: fresh fakes, a fresh temp tree, a human slot 0 and a live map.
void StartScenario(std::string_view trackName = "Test", std::string_view trackSound = "Test.Track",
				   std::string_view commands = "!res,!music", int fadeBeforeSeconds = 1)
{
	Stop();
	InstallFakes();
	ResetTree();
	WritePluginConfig(ConfigText(trackName, trackSound, commands, fadeBeforeSeconds));
	WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
	AddPlayer(0, "Player");
	AddPlayer(63, "Other");
	Start();
	FireMapStart();
	Update();
}

} // namespace

// ---------------------------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------------------------

int main()
{
	g_root = std::filesystem::temp_directory_path() /
			 ("round_end_sounds_runtime_tests_" + std::to_string(static_cast<long long>(::getpid())));
	g_previousCwd = std::filesystem::current_path();
	std::error_code ec;
	std::filesystem::remove_all(g_root, ec);

	Section("startup: registrations, config, phrases, precache, menu id 0");
	{
		Stop();
		InstallFakes();
		ResetTree();
		WriteRawConfig(ConfigText("Raw", "Test.Raw"));
		WriteRawPhrases(PhrasesText("RAW-HINT", "RAW-ПОДСКАЗКА"));
		WritePluginConfig(ConfigText("Plugin", "Test.Plugin"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		Start();

		Check(g_fake.mapListenersRegistered, "OnPluginStart registers the map listeners");
		Check(g_fake.manifestRegistered, "OnPluginStart registers the manifest callback");
		CheckEqInt(static_cast<long long>(g_fake.eventHooks.size()), 6,
				   "six event hooks (round_mvp, round_end x2, round_prestart, connect_full, disconnect)");
		Check(IsCommandRegistered("res"), "the config's !res command is registered under the stripped name");
		Check(IsCommandRegistered("music"), "the config's !music command is registered");
		Check(IsCommandRegistered("mm_res_reload"), "mm_res_reload is registered");
		Check(IsCommandRegistered("mm_res_kits"), "mm_res_kits is registered");
		Check(IsCommandRegistered("mm_res_test"), "mm_res_test is registered");
		CheckEqInt(g_fake.timerCalls, 0, "OnPluginStart creates no s2sdk timer");
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 0,
				   "OnPluginStart creates no menu (menu id 0 is burned lazily, only when a menu is shown)");
		CheckEqInt(static_cast<long long>(g_fake.schemaLookups.size()), 0,
				   "no schema lookup before a map (PITFALLS B1)");

		// The manifest callback precaches the configured file and the sounds/... track.
		FireManifest();
		Check(std::find(g_fake.precached.begin(), g_fake.precached.end(), "soundevents/test.vsndevts") !=
				  g_fake.precached.end(),
			  "the manifest callback precaches the configured precache list");
		Check(std::find(g_fake.precached.begin(), g_fake.precached.end(), "sounds/test/second.vsnd") !=
				  g_fake.precached.end(),
			  "the manifest callback precaches a sounds/... track");

		// Before the first map nothing may be touched.
		FireEvent("round_end");
		Check(g_fake.emitted.empty(), "round_end before the first map plays nothing (B1)");
		Check(g_fake.printed.empty(), "round_end before the first map prints nothing");

		FireMapStart();
		Update();
		Check(g_fake.messageHooks.count(208) == 1, "the GE_SosStartSoundEvent hook is installed on the live map");

		FireEvent("round_end");
		CheckEqInt(CountEmitted("Test.Plugin"), 1,
				   "the direct <configs>/round_end_sounds config wins over the raw decoy");
		CheckEqInt(static_cast<long long>(g_fake.emittedSlots.size()), 1, "one EmitSound per human");
		Check(HasPrinted(0, "Now Plugin"), "the chat announcement uses the direct phrase file");
		Check(!g_fake.centerHtml.empty(), "message=both also sends the centre message");
		Check(!HasPrinted(0, "RAW-HINT"), "the raw phrases decoy is not used");

		// The direct phrases file wins over the raw decoy too.
		g_fake.printed.clear();
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "HINT-EN"), "the direct translations file is used for the connect hint");
		Check(!HasPrinted(0, "RAW-HINT"), "the raw translations decoy is not used");

		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.clientCommands.size()), 1,
				   "the second (file) track is played through the client's play command");
		CheckEqStr(g_fake.clientCommands.front(), "play sounds/test/second.vsnd", "the file track path is kept");
		Check(AllEventResultsContinue(), "every Pre-hook returns Continue and never supersedes the event");
	}

	Section("config/phrase fallbacks and the ServerLang chain");
	{
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Plugin", "Test.Plugin"));
		WritePluginPhrases(PhrasesText("CFG-HINT", "CFG-ПОДСКАЗКА"));
		AddPlayer(0, "Player");
		Start();
		FireMapStart();
		Update();
		FireEvent("round_end");
		CheckEqInt(CountEmitted("Test.Plugin"), 1, "the plugin config directory is used when no raw file exists");

		g_fake.printed.clear();
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "CFG-HINT"), "the plugin translations are used when no raw phrases file exists");

		// core.cfg ServerLang wins over s2sdk's ServerLanguage.
		WriteCoreConfig("\"Core\" { \"ServerLang\" \"russian\" }");
		g_fake.serverLanguage = "english";
		Stop();
		Start();
		FireMapStart();
		Update();
		g_fake.printed.clear();
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "CFG-ПОДСКАЗКА"), "core.cfg ServerLang=russian selects the ru phrase");

		// Without core.cfg s2sdk's ServerLanguage is used.
		RemoveFile(ConfigsDir() / "core.cfg");
		Stop();
		Start();
		FireMapStart();
		Update();
		g_fake.printed.clear();
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "CFG-HINT"), "s2sdk ServerLanguage=english selects the en phrase");

		// No ServerLanguage at all falls back to "en".
		g_fake.serverLanguage.clear();
		Stop();
		Start();
		FireMapStart();
		Update();
		g_fake.printed.clear();
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "CFG-HINT"), "no language setting falls back to en");

		// Direct files missing: the raw cwd fallback is used (both config and phrases).
		Stop();
		InstallFakes();
		ResetTree();
		WriteRawConfig(ConfigText("Raw", "Test.Raw"));
		WriteRawPhrases(PhrasesText("RAW-HINT", "RAW-ПОДСКАЗКА"));
		AddPlayer(0, "Player");
		Start();
		FireMapStart();
		Update();
		FireEvent("round_end");
		CheckEqInt(CountEmitted("Test.Raw"), 1, "the raw cwd config is the fallback when the direct file is absent");
		g_fake.printed.clear();
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "RAW-HINT"), "the raw cwd phrases file is the fallback too");

		// A malformed DIRECT config is authoritative and reported: the valid raw decoy must not be
		// used instead. A failed reload keeps the previous configuration.
		WritePluginConfig("\"RoundEndSounds\" { \"enabled\" \"1\" \"tracks\" { \"T\" { \"sound\" \"Test.Broken\" }");
		WritePluginPhrases("\"Phrases\" { \"Connect_Hint\" { \"en\" \"unterminated }");
		Stop();
		InstallFakes();
		ResetTree();
		WriteRawConfig(ConfigText("Raw", "Test.Raw"));
		WriteRawPhrases(PhrasesText("RAW-HINT", "RAW-ПОДСКАЗКА"));
		WritePluginConfig("\"RoundEndSounds\" { \"enabled\" \"1\" \"tracks\" { \"T\" { \"sound\" \"Test.Broken\" }");
		WritePluginPhrases("\"Phrases\" { \"Connect_Hint\" { \"en\" \"unterminated }");
		AddPlayer(0, "Player");
		Start();
		FireMapStart();
		Update();
		Check(AnyErrorContains("failed to parse"), "a malformed direct config is reported");
		Check(AnyErrorContains("failed to parse"), "a malformed direct phrases file is reported");
		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.emitted.size() + g_fake.clientCommands.size()), 0,
				   "a malformed direct config does not fall through to the raw decoy");
		g_fake.printed.clear();
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "Type"), "a malformed direct phrases file falls back to the built-in English phrase");
		Check(!HasPrinted(0, "RAW-HINT"), "the raw phrases decoy is not used when the direct file is malformed");

		// A failed reload (malformed direct now) keeps the previous configuration.
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Plugin", "Test.Plugin"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		Start();
		FireMapStart();
		Update();
		WritePluginConfig("\"RoundEndSounds\" { \"enabled\" \"1\" \"tracks\" { \"T\" { \"sound\" \"Test.Broken\" }");
		g_fake.emitted.clear();
		g_fake.emittedSlots.clear();
		g_fake.emittedVolumes.clear();
		g_fake.clientCommands.clear();
		CallCommand("mm_res_reload", -1, s2sdk::ConCommandContext::Console);
		Check(AnyErrorContains("failed to parse"), "a malformed direct config on reload is reported");
		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.emitted.size() + g_fake.clientCommands.size()), 1,
				   "a failed reload keeps the previous configuration (fail soft)");

		// Server console commands: exactly caller -1; -2 and out-of-range slots must not bypass.
		g_fake.precached.clear();
		CallCommand("mm_res_reload", -2, s2sdk::ConCommandContext::Console);
		CallCommand("mm_res_reload", 64, s2sdk::ConCommandContext::Console);
		Check(g_fake.precached.empty(), "-2 and slot 64 cannot run the server-console reload");
		CallCommand("mm_res_reload", -1, s2sdk::ConCommandContext::Console);
		Check(!g_fake.precached.empty(), "the real server console (-1) runs the reload");
		g_fake.emitted.clear();
		g_fake.clientCommands.clear();
		CallCommand("mm_res_test", -2, s2sdk::ConCommandContext::Console);
		CallCommand("mm_res_test", 64, s2sdk::ConCommandContext::Console);
		Check(g_fake.emitted.empty() && g_fake.clientCommands.empty(),
			  "-2/64 cannot run the server-console test playback");
		CallCommand("mm_res_test", -1, s2sdk::ConCommandContext::Console);
		Check(!g_fake.emitted.empty() || !g_fake.clientCommands.empty(),
			  "the server console can run mm_res_test");
	}

	Section("map gate (PITFALLS B1/B2) and the mid-map reload latch");
	{
		StartScenario();
		Check(g_bReady, "the plugin is running");
		Check(g_mapGate.Ready(), "the gate is open after OnMapStart");
		FireEvent("round_end");
		const size_t played = g_fake.emitted.size() + g_fake.clientCommands.size();
		Check(played > 0, "round_end plays while the map is up");

		FireMapEnd();
		Check(!g_mapGate.Ready(), "OnMapEnd closes the gate");
		Update();
		Check(!g_mapGate.Ready(), "updates pumped through a changelevel do not reopen the gate");
		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.emitted.size() + g_fake.clientCommands.size()),
				   static_cast<long long>(played), "round_end between maps plays nothing (B2)");

		FireMapStart();
		Update();
		FireEvent("round_end");
		Check(static_cast<long long>(g_fake.emitted.size() + g_fake.clientCommands.size()) >
				  static_cast<long long>(played),
			  "the next OnMapStart reopens the gate");

		// A plugin loaded (plg reload) into a running map never sees OnMapStart: the first update
		// with registered listeners opens the gate.
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		Start();
		Check(!g_mapGate.Ready(), "the gate starts shut");
		Update();
		Check(g_mapGate.Ready(), "the first update opens the gate for a mid-map reload");
		FireEvent("round_end");
		Check(!g_fake.emitted.empty(), "round_end plays after the mid-map latch");
	}

	Section("slots, controllers, handles and the MVP mute (PITFALLS C2/C1)");
	{
		StartScenario();
		AddBot(1);
		AddPlayer(2, "TV");
		g_fake.slots[2].sourceTV = true;
		AddPlayer(3, "Connecting");
		g_fake.slots[3].inGame = false;
		Update();
		g_fake.emitted.clear();
		g_fake.emittedSlots.clear();
		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.emittedSlots.size()), 2, "only the two human in-game players hear it");
		Check(std::find(g_fake.emittedSlots.begin(), g_fake.emittedSlots.end(), 0) != g_fake.emittedSlots.end(),
			  "slot 0 plays");
		Check(std::find(g_fake.emittedSlots.begin(), g_fake.emittedSlots.end(), 63) != g_fake.emittedSlots.end(),
			  "slot 63 plays");
		Check(std::find(g_fake.emittedSlots.begin(), g_fake.emittedSlots.end(), 1) == g_fake.emittedSlots.end(),
			  "a bot does not get round-end music");
		Check(std::find(g_fake.emittedSlots.begin(), g_fake.emittedSlots.end(), 2) == g_fake.emittedSlots.end(),
			  "SourceTV does not get round-end music");
		Check(std::find(g_fake.emittedSlots.begin(), g_fake.emittedSlots.end(), 3) == g_fake.emittedSlots.end(),
			  "a not-in-game client does not get round-end music");

		// Event player fields: -1 and 64 never reach a client.
		g_fake.printed.clear();
		g_fake.event.slots["userid"] = -1;
		FireEvent("player_connect_full");
		g_fake.event.slots["userid"] = 64;
		FireEvent("player_connect_full");
		Update();
		CheckEqInt(static_cast<long long>(g_fake.printed.size()), 0, "out-of-range event slots are ignored");

		// Controller handle 0 and -1 are invalid; a negative valid handle is accepted.
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		g_fake.slots[0].controllerHandle = 0;
		Update();
		CheckEqInt(static_cast<long long>(g_fake.printed.size()), 0, "controller handle 0 is refused");
		g_fake.slots[0].controllerHandle = -1;
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		CheckEqInt(static_cast<long long>(g_fake.printed.size()), 0, "controller handle -1 is refused");
		g_fake.slots[0].controllerHandle = -921534371; // 0xC912805D: a valid negative CEntityHandle
		g_fake.event.slots["userid"] = 0;
		FireEvent("player_connect_full");
		Update();
		Check(HasPrinted(0, "HINT-EN"), "a valid negative controller handle is accepted");

		// MVP mute: both music-kit fields are saved and zeroed, the event's musickitid is cleared,
		// and the next round_prestart restores the controller field.
		g_fake.slots[0].controllerHandle = 1000;
		g_fake.slots[0].controllerKit = 555;
		g_fake.slots[0].inventoryKit = 777;
		g_fake.slots[0].steam = 777001;
		g_fake.event.slots["userid"] = 0;
		g_fake.event.ints["musickitid"] = 555;
		FireEvent("round_mvp");
		CheckEqInt(g_fake.slots[0].controllerKit, 0, "round_mvp zeroes CCSPlayerController::m_iMusicKitID");
		CheckEqInt(g_fake.slots[0].inventoryKit, 0, "round_mvp zeroes the inventory m_unMusicID");
		CheckEqInt(g_fake.noMusic, 1, "round_mvp sets CCSGameRules::m_bRoundEndNoMusic");
		CheckEqInt(static_cast<long long>(g_fake.eventSetInts.size()), 1, "round_mvp clears musickitid");
		CheckEqInt(g_fake.eventSetInts.front(), 0, "musickitid becomes 0");
		Check(g_fake.lastNetworkStateChanged.find("CCSGameRulesProxy::m_pGameRules") != std::string::npos,
			  "the rules proxy field is marked changed");

		FireEvent("round_prestart");
		CheckEqInt(g_fake.slots[0].controllerKit, 555, "round_prestart restores the controller kit");
		CheckEqInt(g_fake.slots[0].inventoryKit, 777, "round_prestart restores the inventory kit");

		// A zero musickitid needs no write.
		const size_t setInts = g_fake.eventSetInts.size();
		g_fake.event.ints["musickitid"] = 0;
		g_fake.slots[0].controllerKit = 555;
		FireEvent("round_mvp");
		CheckEqInt(static_cast<long long>(g_fake.eventSetInts.size()), static_cast<long long>(setInts),
				   "musickitid 0 is left alone");
		Check(AllEventResultsContinue(), "round_mvp/round_prestart Pre-hooks return Continue (never cancel the event)");
	}

	Section("audio: per-player volume, preview and fade");
	{
		StartScenario("Test", "Test.Track", "!res,!music", 2);
		g_Players[0].iVolume = 50;
		FireEvent("round_end");
		CheckEqInt(CountEmitted("Test.Track"), 2, "the event track is emitted on the live map");
		Check(g_fake.emittedVolumes[0] > 0.49f && g_fake.emittedVolumes[0] < 0.51f,
			  "EmitSound gets the player's volume as a hint");

		const uint32_t hash = res::SoundEventHash("Test.Track");
		FireSoundStart(42, hash, 1ull << 0);
		const SentMessage* sent = LastSent();
		Check(sent != nullptr, "a GE_SosStartSoundEvent produces a params message");
		if (sent != nullptr)
		{
			CheckEqInt(sent->id, 210, "the params message id is 210");
			CheckEqInt(sent->guid, 42, "the params message carries the soundevent_guid");
			CheckEqInt(static_cast<long long>(sent->mask), 1LL << 0, "only the recipient gets the params");
			CheckEqInt(static_cast<long long>(sent->packed.size()), 11, "the packed volume parameter is 11 bytes");
			if (sent->packed.size() == 11)
			{
				float volume = 0.0f;
				std::memcpy(&volume, sent->packed.data() + 7, 4);
				Check(volume > 0.49f && volume < 0.51f, "the params volume is the player's 0.5");
			}
		}

		// Volume 0: the reference still sends a params message with volume 0.
		g_Players[0].iVolume = 0;
		FireSoundStart(43, hash, 1ull << 0);
		sent = LastSent();
		Check(sent != nullptr && sent->guid == 43, "volume 0 still sends the params message");

		// Preview window overrides the player's volume.
		g_Players[0].iVolume = 20;
		g_flPreviewUntil[0] = g_fake.gameTime + 1.0f;
		g_flPreviewVolume[0] = 0.9f;
		FireSoundStart(44, hash, 1ull << 0);
		sent = LastSent();
		Check(sent != nullptr && sent->guid == 44, "a preview start produces a params message");
		if (sent != nullptr && sent->packed.size() == 11)
		{
			float volume = 0.0f;
			std::memcpy(&volume, sent->packed.data() + 7, 4);
			Check(volume > 0.89f && volume < 0.91f, "the preview volume overrides the player's setting");
		}
		g_flPreviewUntil[0] = -1.0f;

		// Fade towards m_flRestartRoundTime: the track started before the restart fades and stops.
		g_Players[0].iVolume = 100;
		g_fake.gameTime = 98.0f;
		g_fake.restartTime = 100.0f;
		FireSoundStart(45, hash, 1ull << 0);
		g_fake.gameTime = 98.5f;
		g_fake.sent.clear();
		Update();
		Check(!g_fake.sent.empty(), "the fade sends a params message while the track plays out");
		g_fake.gameTime = 101.0f;
		g_fake.sent.clear();
		Update();
		Check(!g_fake.sent.empty() && g_fake.sent.back().id == 209, "the finished fade sends GE_SosStopSoundEvent");

		// A message with an unexpected name is ignored (the lazy-id verification).
		g_fake.sent.clear();
		FireSoundStart(46, hash, 1ull << 0, "CMsgSomethingElse");
		Check(g_fake.sent.empty(), "a message whose name is not SosStartSoundEvent is ignored");
		Check(AnyErrorContains("expected GE_SosStartSoundEvent"), "the wrong message name is reported once");

		// A hash that is not one of our tracks is ignored.
		g_fake.sent.clear();
		FireSoundStart(47, 0xDEADBEEF, 1ull << 0);
		Check(g_fake.sent.empty(), "an unrelated soundevent is not touched");
	}

	Section("audio registry fail-soft (no numeric fallback)");
	{
		// The registry knows NONE of the three types: no hook, and no Create/GetName/Pb/Send at
		// all. Playback itself still works (default soundevent volume).
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		AddPlayer(63, "Other");
		g_fake.messageIds.clear();
		Start();
		FireMapStart();
		Update();
		Check(g_fake.messageHooks.empty(), "an unresolved registry installs no user-message hook");
		CheckEqInt(g_fake.createCalls, 0, "an unresolved registry causes no UserMessageCreateFromId");
		CheckEqInt(g_fake.getNameCalls, 0, "an unresolved registry causes no GetMessageName");
		CheckEqInt(g_fake.pbCalls, 0, "an unresolved registry causes no Pb read/write");
		CheckEqInt(g_fake.sendCalls, 0, "an unresolved registry causes no UserMessageSend");
		Check(g_fake.findCalls >= 3, "the three real registry substrings were looked up");
		FireEvent("round_end");
		Check(!g_fake.emitted.empty(), "round_end still plays at the soundevent's own volume");
		Check(g_fake.sent.empty(), "no per-player volume message is built without a resolved Set type");

		// Only SosStartSoundEvent resolves: the hook works and incoming parsing is allowed, but
		// Set/Stop stay no-ops (no Create/Pb/Send for them).
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		g_fake.messageIds.clear();
		g_fake.messageIds["SosStartSoundEvent"] = 208;
		Start();
		FireMapStart();
		Update();
		Check(g_fake.messageHooks.count(208) == 1, "a resolved start type installs the hook");
		g_fake.createCalls = 0;
		g_fake.getNameCalls = 0;
		g_fake.pbCalls = 0;
		g_fake.sendCalls = 0;
		FireSoundStart(51, res::SoundEventHash("Test.Track"), 1ull << 0);
		CheckEqInt(g_fake.getNameCalls, 1, "the incoming engine message name is verified");
		CheckEqInt(g_fake.pbCalls, 2, "the incoming message fields are read");
		CheckEqInt(g_fake.createCalls, 0, "the unresolved Set type creates no params message");
		CheckEqInt(g_fake.sendCalls, 0, "the unresolved Set type sends no params message");

		// Start + Set resolve, Stop does not: params work, the fade end is a no-op (no Create).
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track", "!res,!music", 2));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		g_fake.messageIds.clear();
		g_fake.messageIds["SosStartSoundEvent"] = 208;
		g_fake.messageIds["SosSetSoundEventParams"] = 210;
		Start();
		FireMapStart();
		Update();
		Check(g_fake.messageHooks.count(208) == 1, "start+set resolved installs the hook");
		g_Players[0].iVolume = 100;
		g_fake.gameTime = 98.0f;
		g_fake.restartTime = 100.0f;
		FireSoundStart(52, res::SoundEventHash("Test.Track"), 1ull << 0);
		g_fake.gameTime = 98.5f;
		g_fake.createdIds.clear();
		g_fake.sendCalls = 0;
		Update();
		Check(!g_fake.createdIds.empty() && g_fake.createdIds.back() == 210,
			  "the resolved Set type builds the params message");
		CheckEqInt(g_fake.sendCalls, 1, "the resolved Set type sends the params message");
		g_fake.gameTime = 101.0f;
		g_fake.createdIds.clear();
		g_fake.sendCalls = 0;
		Update();
		Check(g_fake.createdIds.empty(), "the unresolved Stop type creates nothing at fade end");
		CheckEqInt(g_fake.sendCalls, 0, "the unresolved Stop type sends nothing at fade end");
	}

	Section("commands, rights and menu ownership");
	{
		StartScenario();
		Check(IsCommandRegistered("res"), "chat command res is registered");
		CheckEqInt(static_cast<long long>(g_fake.sayListeners.size()), 0,
				   "a triggered config name needs no bare say listener");
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 0, "no menu is created at startup");

		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 0,
				   "the command only queues the open (no menu from the command callback)");
		Update();
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 2,
				   "OnPluginUpdate burns id 0 and opens the main menu");
		CheckEqStr(g_fake.menuTypes.front(), "chat", "the reserved id 0 uses the no-op chat backend");
		CheckEqStr(g_fake.menuTypes.back(), "button",
				   "without a hudmenu choice the Pisex HUD_LAYOUT default maps to the built-in button");
		const s2sdk::MenuId mainMenu = g_fake.nextMenuId - 1;
		CheckEqInt(static_cast<long long>(g_fake.menuItems[mainMenu].size()), 4, "the main menu has four items");
		CheckEqStr(g_fake.menuItems[mainMenu][0].first, "state", "the first item is the on/off switch");
		CheckEqStr(g_fake.menuItems[mainMenu][1].first, "volume", "the second item is the volume");
		CheckEqStr(g_fake.menuItems[mainMenu][2].first, "tracks", "the third item is the track list");
		CheckEqStr(g_fake.menuItems[mainMenu][3].first, "last", "the fourth item is the last track");
		CheckEqInt(g_fake.menuSlots[mainMenu], 0, "the menu is displayed to the caller");
		CheckEqInt(g_fake.clientMenu[0], mainMenu, "the fake client has the menu open");

		// Selecting "state" toggles the switch and reopens the main menu, one frame later.
		SelectMenu(mainMenu, 0, 0);
		Update();
		Check(!g_Players[0].bEnabled, "the selection toggles the player's switch in OnPluginUpdate");
		Check(HasPrinted(0, "Music: off"), "the state change is announced with the phrase");
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 3, "the main menu is redisplayed");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), mainMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the old menu is destroyed outside the handler");

		// The exit-back button (0) on a sub-menu returns to the main menu.
		const s2sdk::MenuId secondMenu = g_fake.nextMenuId - 1;
		ExitBackMenu(secondMenu, 0);
		Update();
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), 4,
				   "the exit-back button returns to the main menu");

		// Per-player menu type choice through the optional hudmenu API, all five types.
		const auto reopen = [&](const char* choice) {
			g_fake.hudmenuChoice = choice != nullptr ? choice : "";
			const size_t before = g_fake.menuTypes.size();
			CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
			Update();
			Check(g_fake.menuTypes.size() == before + 1, std::string("one menu for choice '") +
					(choice != nullptr ? choice : "") + "'");
			return g_fake.menuTypes.back();
		};
		CheckEqStr(reopen("chat"), "chat", "hudmenu choice chat is used");
		CheckEqStr(reopen("centerhtml"), "centerhtml", "hudmenu choice centerhtml is used");
		CheckEqStr(reopen("button"), "button", "hudmenu choice button is used");
		CheckEqStr(reopen("hud"), "button", "hud choice falls back to button while hud is not registered");
		g_fake.registeredMenuTypes.push_back("hud");
		CheckEqStr(reopen("hud"), "hud", "hud choice is used when hudmenu registered hud");
		g_fake.registeredMenuTypes.push_back("wasd");
		CheckEqStr(reopen("wasd"), "wasd", "wasd choice is used when hudmenu registered wasd");
		g_fake.registeredMenuTypes.erase(
			std::remove(g_fake.registeredMenuTypes.begin(), g_fake.registeredMenuTypes.end(), "wasd"),
			g_fake.registeredMenuTypes.end());
		CheckEqStr(reopen("wasd"), "button", "wasd choice falls back to button while wasd is not registered");
		// An unknown/stale hudmenu answer falls through to the default hud -> button mapping.
		CheckEqStr(reopen("bogus"), "hud", "an unknown choice uses the registered hud default");

		// A malformed/absent player choice (throwing hudmenu) still opens with the default.
		g_fake.hudmenuThrows = true;
		CheckEqStr(reopen(""), "hud", "a throwing hudmenu falls back to the default hud");
		g_fake.hudmenuThrows = false;

		// Without the optional hudmenu binding the default mapping is used, not the cookie.
		__hudmenu_GetClientMenuType = nullptr;
		CheckEqStr(reopen("chat"), "hud", "without hudmenu the default hud mapping is used");
		__hudmenu_GetClientMenuType = &FakeHudmenuGetClientMenuType;

		// The chat command only ever opens a menu for a real slot: -2/64 are not bypasses.
		const size_t menusBefore = g_fake.menuTypes.size();
		CallCommand("res", -2, s2sdk::ConCommandContext::Console);
		CallCommand("res", 64, s2sdk::ConCommandContext::Chat);
		Update();
		CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), static_cast<long long>(menusBefore),
				   "-2/slot 64 cannot open the !res menu");

		// A client caller cannot run the server-only commands; the server console can.
		g_fake.addCommandNames.clear();
		CallCommand("mm_res_reload", 0, s2sdk::ConCommandContext::Chat);
		CallCommand("mm_res_reload", -2, s2sdk::ConCommandContext::Console);
		CallCommand("mm_res_reload", 64, s2sdk::ConCommandContext::Console);
		Check(g_fake.addCommandNames.empty(), "client/-2/64 callers cannot reload the config");
		CallCommand("mm_res_reload", -1, s2sdk::ConCommandContext::Console);
		CheckEqInt(g_fake.timerCalls, 0, "no timer is created for the deferred work");
	}

	Section("menu owner identity and slot reuse");
	{
		StartScenario();
		FireEvent("round_end"); // sets g_iLastTrack for the "last" menu item
		const size_t emittedBefore = g_fake.emitted.size();

		// A menu opened for the old occupant, then the slot changes hands BEFORE the handler fires:
		// the stored owner identity must reject the selection and destroy the stale menu.
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId oldMenu = g_fake.nextMenuId - 1;
		AddPlayer(0, "NewOccupant", 76561198000000002);
		g_fake.slots[0].controllerHandle = 5000;
		g_fake.printed.clear();
		SelectMenu(oldMenu, 0, 0); // "state"
		Update();
		Check(g_Players[0].bEnabled, "a stale menu selection never changes the new occupant's switch");
		Check(!HasPrinted(0, "Music: off"), "... and prints nothing for the new occupant");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), oldMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the stale menu is destroyed on the identity mismatch");
		CheckEqInt(g_fake.clientMenu[0], 0, "the stale display is gone");

		// Handler -> slot reuse BEFORE the deferred update: a playback action must be dropped.
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId playbackMenu = g_fake.nextMenuId - 1;
		SelectMenu(playbackMenu, 0, 3); // "last" -> would PreviewTrack/play
		AddPlayer(0, "AnotherOccupant", 76561198000000003);
		g_fake.slots[0].controllerHandle = 5001;
		g_fake.emitted.clear();
		Update();
		CheckEqInt(static_cast<long long>(g_fake.emitted.size()), 0,
				   "a deferred playback action never reaches the new occupant");
		CheckEqInt(static_cast<long long>(g_fake.emitted.size()), static_cast<long long>(0),
				   "no sound was emitted by the stale menu action");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), playbackMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the stale playback menu is destroyed");

		// Same SteamID, new controller serial (reconnect): the handle half of the identity rejects it.
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId reconnectMenu = g_fake.nextMenuId - 1;
		const uint64_t sameSteam = g_fake.slots[0].steam;
		SelectMenu(reconnectMenu, 0, 0);
		g_fake.slots[0].controllerHandle = 5002; // new serial only
		g_fake.printed.clear();
		Update();
		CheckEqInt(static_cast<long long>(g_fake.slots[0].steam), static_cast<long long>(sameSteam),
				   "the test kept the same SteamID");
		Check(g_Players[0].bEnabled, "the same SteamID with a new controller serial is rejected too");
		Check(!HasPrinted(0, "Music: off"), "... and no state text is printed");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), reconnectMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the reconnect-stale menu is destroyed");

		Check(emittedBefore > 0, "the scenario's own round_end played once");
	}

	Section("menu cleanup fail-safe (PITFALLS B2/A7) and partial bindings");
	{
		// Normal MapEnd: Cancel(Disconnect) skips the backend Close, the menu is freed at once.
		StartScenario();
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId mapMenu = g_fake.nextMenuId - 1;
		Check(g_fake.clientMenu[0] == mapMenu, "the menu is displayed before the map ends");
		FireMapEnd();
		CheckEqInt(g_fake.backendCloseCalls, 0, "MapEnd never runs a custom backend Close");
		CheckEqInt(g_fake.clientMenu[0], 0, "the display session ended with the Disconnect reason");
		Check(g_fake.menuHandlers.count(mapMenu) == 0, "the menu is destroyed during OnMapEnd");
		Check(!AnyErrorContains("A7_UNRESOLVED"), "the normal cleanup is fully resolved");
		Stop();
		Check(g_fake.menuHandlers.empty(), "after unload no menu callback is left registered");
		CheckEqInt(g_fake.backendCloseCalls, 0, "unload after MapEnd runs no backend Close either");

		// Cancel refuses and the session stays displayed: switch to the builtin chat type, verify,
		// then destroy - no custom backend Close over the dying client layer.
		StartScenario();
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId stuckMenu = g_fake.nextMenuId - 1;
		g_fake.cancelFails = true;
		g_fake.cancelCloses = false;
		FireMapEnd();
		Check(!g_fake.menuTypeSwitches.empty() && g_fake.menuTypeSwitches.front().first == stuckMenu &&
				  g_fake.menuTypeSwitches.front().second == "chat",
			  "a still-displayed menu is switched to the builtin chat type");
		Check(g_fake.menuHandlers.count(stuckMenu) == 0, "the switched menu is destroyed");
		CheckEqInt(g_fake.backendCloseCalls, 0, "the switched chat Close has no client side effect");
		Check(!AnyErrorContains("A7_UNRESOLVED"), "the switch fail-safe resolves the cleanup");
		Stop();
		Check(g_fake.menuHandlers.empty(), "no live callbacks after the switched cleanup");

		// Cancel refuses, the session stays displayed and SetMenuType is missing: explicit
		// A7_UNRESOLVED, the menu is left alone instead of running the custom backend Close.
		StartScenario();
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId unresolvedMenu = g_fake.nextMenuId - 1;
		g_fake.cancelFails = true;
		g_fake.cancelCloses = false;
		__s2sdk_SetMenuType = nullptr;
		FireMapEnd();
		Check(AnyErrorContains("A7_UNRESOLVED"), "the unresolved cleanup is reported explicitly");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), unresolvedMenu) ==
				  g_fake.destroyedMenus.end(),
			  "the unresolved menu is not destroyed in the map window");
		Check(g_fake.menuHandlers.count(unresolvedMenu) == 1,
			  "the unresolved menu callback is left registered (explicit, never silent)");
		CheckEqInt(g_fake.backendCloseCalls, 0, "no backend Close runs over the dying client layer");
		Stop(); // unload retry: still unresolved, still no backend Close
		CheckEqInt(g_fake.backendCloseCalls, 0, "the unload retry also runs no backend Close");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), unresolvedMenu) ==
				  g_fake.destroyedMenus.end(),
			  "the unresolved menu is left alone on unload as well");

		// Unload while the map is already down: Cancel refuses and the session stays displayed, but
		// SetMenuType/GetMenuType are available. The unload path alone must switch to the builtin
		// chat type, verify it, destroy the menu and leave no live callback / backend Close.
		StartScenario();
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId unloadStuckMenu = g_fake.nextMenuId - 1;
		g_fake.cancelFails = true;
		g_fake.cancelCloses = false;
		__s2sdk_SetMenuType = nullptr;
		FireMapEnd(); // A7_UNRESOLVED, the menu stays displayed
		Check(g_fake.menuHandlers.count(unloadStuckMenu) == 1, "the map-end cleanup left the menu as unresolved");
		__s2sdk_SetMenuType = &FakeSetMenuType;
		Stop(); // unload retry with the map down: cancel false -> builtin switch -> destroy
		Check(g_fake.menuHandlers.empty(), "the map-down unload leaves no live menu callback");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), unloadStuckMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the map-down unload destroys the switched menu");
		CheckEqInt(g_fake.backendCloseCalls, 0, "the map-down unload runs no custom backend Close");

		// A menu is active and DestroyMenu disappears before unload: the callback cannot be freed
		// safely, which must be reported as A7_UNRESOLVED and never silently dropped or followed by
		// a backend Close attempt.
		StartScenario();
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId noDestroyMenu = g_fake.nextMenuId - 1;
		__s2sdk_DestroyMenu = nullptr;
		Stop();
		Check(AnyErrorContains("A7_UNRESOLVED"), "a missing DestroyMenu at unload is reported explicitly");
		Check(g_fake.menuHandlers.count(noDestroyMenu) == 1,
			  "the callback is left registered instead of being silently dropped");
		CheckEqInt(g_fake.backendCloseCalls, 0, "no backend Close is attempted without DestroyMenu");

		// Partial lifecycle bindings: no menu is ever created (all eight are required).
		for (int missing = 0; missing < 8; ++missing)
		{
			StartScenario();
			switch (missing)
			{
			case 0: __s2sdk_CreateMenu = nullptr; break;
			case 1: __s2sdk_AddMenuItem = nullptr; break;
			case 2: __s2sdk_DisplayMenu = nullptr; break;
			case 3: __s2sdk_DestroyMenu = nullptr; break;
			case 4: __s2sdk_GetClientMenu = nullptr; break;
			case 5: __s2sdk_CancelClientMenu = nullptr; break;
			case 6: __s2sdk_SetMenuType = nullptr; break;
			default: __s2sdk_GetMenuType = nullptr; break;
			}
			const size_t before = g_fake.menuTypes.size();
			CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
			Update();
			CheckEqInt(static_cast<long long>(g_fake.menuTypes.size()), static_cast<long long>(before),
					   "a partial menu lifecycle binding set creates no menu");
			Stop();
		}
	}

	Section("reload/teardown (PITFALLS A7) and command ownership");
	{
		StartScenario();
		FireEvent("round_end");
		CheckEqInt(static_cast<long long>(g_fake.messageHooks.size()), 1, "the sound hook is installed");
		CallCommand("res", 0, s2sdk::ConCommandContext::Chat);
		Update();
		const s2sdk::MenuId unloadMenu = g_fake.nextMenuId - 1;
		Check(g_fake.menuHandlers.count(unloadMenu) == 1, "a menu is open before the unload");
		Stop();
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), unloadMenu) !=
				  g_fake.destroyedMenus.end(),
			  "the open menu is destroyed on unload");
		Check(g_fake.menuHandlers.empty(), "no menu callback survives OnPluginEnd");

		CheckEqInt(static_cast<long long>(g_fake.unhookedEvents.size()), 6, "OnPluginEnd unhooks all six events");
		for (const auto& hook : g_fake.eventHooks)
		{
			bool found = false;
			for (const auto& unhooked : g_fake.unhookedEvents)
				if (unhooked.name == hook.name && unhooked.callback == hook.callback &&
					unhooked.mode == hook.mode)
					found = true;
			Check(found, "event '" + hook.name + "' is unhooked with the same callback and mode");
		}
		CheckEqInt(g_fake.mapStartUnregister, 1, "OnPluginEnd unregisters OnMapStart");
		CheckEqInt(g_fake.mapEndUnregister, 1, "OnPluginEnd unregisters OnMapEnd");
		CheckEqInt(g_fake.manifestUnregister, 1, "OnPluginEnd unregisters the manifest callback");
		CheckEqInt(static_cast<long long>(g_fake.unhookCalls.size()), 1, "OnPluginEnd unhooks the user message");
		CheckEqInt(g_fake.unhookCalls.front().first, 208, "the user-message hook id matches the one hooked");
		Check(std::find(g_fake.destroyedMenus.begin(), g_fake.destroyedMenus.end(), 0) != g_fake.destroyedMenus.end(),
			  "the reserved menu id 0 is destroyed on unload");

		// All commands we created are removed with RemoveCommand (never the listener-only path).
		for (const char* name : {"res", "music", "mm_res_reload", "mm_res_kits", "mm_res_test"})
		{
			Check(std::find(g_fake.removeCommandNames.begin(), g_fake.removeCommandNames.end(), name) !=
					  g_fake.removeCommandNames.end(),
				  std::string("command '") + name + "' is removed with RemoveCommand");
		}
		CheckEqInt(g_fake.timerCalls, 0, "OnPluginEnd creates no timer");

		// A stale update after OnPluginEnd cannot reach a client.
		const size_t emittedAfterEnd = g_fake.emitted.size();
		Update();
		CheckEqInt(static_cast<long long>(g_fake.emitted.size()), static_cast<long long>(emittedAfterEnd),
				   "an update after OnPluginEnd is inert");
		CheckEqInt(g_fake.unhookCalls.size(), 1, "a second update does not unhook again");

		// Reload: a name that now already exists must only lose its listener, not the whole command.
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		g_fake.createCommands = false; // the names exist already (engine or another plugin)
		Start();
		CheckEqInt(static_cast<long long>(g_fake.eventHooks.size()), 6, "a reload registers the events exactly once");
		CheckEqInt(g_fake.mapStartUnregister, 0, "a reload has not unregistered anything yet");
		Stop();
		Check(g_fake.removeCommandNames.empty(), "pre-existing commands are not removed with RemoveCommand");
		Check(std::find(g_fake.removeListenerNames.begin(), g_fake.removeListenerNames.end(), "res") !=
				  g_fake.removeListenerNames.end(),
			  "the listener of a pre-existing command is removed with RemoveCommandListener");
		Check(std::find(g_fake.removeListenerNames.begin(), g_fake.removeListenerNames.end(), "mm_res_reload") !=
				  g_fake.removeListenerNames.end(),
			  "mm_res_reload's listener is removed too");
	}

	Section("paired register/unregister bindings (PITFALLS A7)");
	{
		struct Case
		{
			const char* name;
			int which;
		};
		const Case cases[] = {{"UnhookEvent", 0}, {"RemoveCommand", 1}, {"OnMapEnd_Unregister", 2},
						  {"UnhookUserMessage", 3}};
		for (const Case& testCase : cases)
		{
			Stop();
			InstallFakes();
			ResetTree();
			WritePluginConfig(ConfigText("Test", "Test.Track"));
			WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
			AddPlayer(0, "Player");
			switch (testCase.which)
			{
			case 0: __s2sdk_UnhookEvent = nullptr; break;
			case 1: __s2sdk_RemoveCommand = nullptr; break;
			case 2: __s2sdk_OnMapEnd_Unregister = nullptr; break;
			default: __s2sdk_UnhookUserMessage = nullptr; break;
			}
			Start();
			FireMapStart();
			Update();
			switch (testCase.which)
			{
			case 0:
				Check(g_fake.eventHooks.empty(), "without UnhookEvent no event hook is registered");
				break;
			case 1:
				Check(g_fake.commands.empty(), "without RemoveCommand no command is registered");
				break;
			case 2:
				Check(g_fake.mapStart == nullptr && g_fake.mapEnd == nullptr,
					  "without the unregister half no map listener is registered");
				Check(!g_mapGate.Ready(), "the gate stays closed without the map listener pair");
				break;
			default:
				Check(g_fake.messageHooks.empty(),
					  "without UnhookUserMessage the sound hook is not installed");
				break;
			}
			Stop(); // must be safe: nothing half-registered is left to remove
		}
	}

	Section("bare chat names and say listeners");
	{
		Stop();
		InstallFakes();
		ResetTree();
		WritePluginConfig(ConfigText("Test", "Test.Track", "res,!music"));
		WritePluginPhrases(PhrasesText("HINT-EN", "HINT-RU"));
		AddPlayer(0, "Player");
		Start();
		Check(std::find(g_fake.sayListeners.begin(), g_fake.sayListeners.end(), "say") != g_fake.sayListeners.end(),
			  "a trigger-less chat name installs the say listener");
		Check(std::find(g_fake.sayListeners.begin(), g_fake.sayListeners.end(), "say_team") !=
				  g_fake.sayListeners.end(),
			  "a trigger-less chat name installs the say_team listener");
		Check(IsCommandRegistered("res") && IsCommandRegistered("music"), "both names are registered as commands");
		Stop();
		Check(std::find(g_fake.removeListenerNames.begin(), g_fake.removeListenerNames.end(), "say") !=
				  g_fake.removeListenerNames.end(),
			  "the say listener is removed on unload");
	}

	Stop();
	std::filesystem::current_path(g_previousCwd, ec);
	std::filesystem::remove_all(g_root, ec);
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
