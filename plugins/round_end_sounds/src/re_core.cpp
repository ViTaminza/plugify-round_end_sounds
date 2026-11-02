// round_end_sounds — engine-free core (see re_core.hpp).
//
// The HEAD port parsed these files through pisex::KeyValues. This port owns the reader
// (src/kv1.hpp, the same grammar and the same atoi/strtof value semantics) and keeps every rule of
// the HEAD core unchanged: config defaults, track order, the picker, the phrase lookup, the
// preview/fade arithmetic, the packed volume parameter and the two-field music kit mute.

#include "re_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <optional>

#include "kv1.hpp"

namespace res {

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
	size_t pos = 0;
	while (!from.empty() && (pos = s.find(from, pos)) != std::string::npos)
	{
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

std::vector<std::string> SplitList(const std::string& s)
{
	std::vector<std::string> v;
	size_t start = 0;
	while (start <= s.size())
	{
		size_t end = s.find(',', start);
		std::string part = Trim(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
		if (!part.empty()) v.push_back(part);
		if (end == std::string::npos) break;
		start = end + 1;
	}
	return v;
}

bool ParseConfigText(std::string_view text, Config& cfg)
{
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) return false;

	cfg = Config();
	const KvNode& kv = *root;
	cfg.bEnabled = kv.GetInt("enabled", 1) != 0;
	auto vCommands = SplitList(std::string(kv.GetString("commands", "!res")));
	if (!vCommands.empty()) cfg.vCommands = vCommands;
	cfg.bRandom = kv.GetInt("random", 1) != 0;
	cfg.bDefaultEnabled = kv.GetInt("default_enabled", 1) != 0;
	cfg.iDefaultVolume = std::clamp((int)lroundf(kv.GetFloat("default_volume", 0.5f) * 100.0f), 0, 100);
	cfg.sMessage = Lower(std::string(kv.GetString("message", "chat")));
	cfg.iCenterTime = std::max(1, kv.GetInt("center_time", 7));
	cfg.bMuteMvpAnthem = kv.GetInt("mute_mvp_anthem", 1) != 0;
	cfg.flFadeBeforeRound = std::max(0.0f, kv.GetFloat("fade_before_round", 1.0f));
	cfg.bUtilsSettings = kv.GetInt("utils_settings", 1) != 0;
	cfg.bConnectHint = kv.GetInt("connect_hint", 1) != 0;
	cfg.bDebug = kv.GetInt("debug", 0) != 0;
	cfg.vPrecache = SplitList(std::string(kv.GetString("precache", "")));

	if (const KvNode* pTracks = kv.Find("tracks"))
	{
		for (const KvNode& pTrack : pTracks->children)
		{
			Track t;
			t.sName = pTrack.name;
			if (!pTrack.children.empty())
			{
				t.sSound = std::string(pTrack.GetString("sound", ""));
				t.sPicture = std::string(pTrack.GetString("picture", ""));
			}
			else if (pTrack.hasValue)
			{
				t.sSound = pTrack.value; // short form: "Name" "sound"
			}
			if (!t.sSound.empty()) cfg.vTracks.push_back(std::move(t));
		}
	}
	return true;
}

uint32_t SoundEventHash(std::string_view name)
{
	std::string s = Lower(std::string(name));
	const uint32_t m = 0x5bd1e995;
	uint32_t len = (uint32_t)s.size();
	uint32_t h = 0x53524332u ^ len;
	const unsigned char* p = (const unsigned char*)s.data();
	while (len >= 4)
	{
		uint32_t k;
		memcpy(&k, p, 4);
		k *= m; k ^= k >> 24; k *= m;
		h *= m; h ^= k;
		p += 4; len -= 4;
	}
	switch (len)
	{
		case 3: h ^= (uint32_t)p[2] << 16; [[fallthrough]];
		case 2: h ^= (uint32_t)p[1] << 8; [[fallthrough]];
		case 1: h ^= p[0]; h *= m;
		default: break;
	}
	h ^= h >> 13; h *= m; h ^= h >> 15;
	return h;
}

bool IsFilePathSound(const std::string& sSound)
{
	return sSound.size() >= 7 && strncasecmp(sSound.c_str(), "sounds/", 7) == 0;
}

std::map<uint32_t, bool> BuildOwnEvents(const std::vector<Track>& vTracks, uint32_t (*pfnHash)(std::string_view))
{
	std::map<uint32_t, bool> map;
	for (const Track& t : vTracks)
		if (!IsFilePathSound(t.sSound)) map[pfnHash(t.sSound)] = true;
	return map;
}

std::string PackVolumeParam(float flVolume)
{
	// hash("volume", 0xB5D9F700) | type 4 (float) | size 4 | 0 | value
	const uint32_t uVolumeHash = [] {
		static const std::string s = "volume";
		const uint32_t m = 0x5bd1e995;
		uint32_t len = (uint32_t)s.size();
		uint32_t h = 0xB5D9F700u ^ len;
		const unsigned char* p = (const unsigned char*)s.data();
		while (len >= 4)
		{
			uint32_t k;
			memcpy(&k, p, 4);
			k *= m; k ^= k >> 24; k *= m;
			h *= m; h ^= k;
			p += 4; len -= 4;
		}
		switch (len)
		{
			case 3: h ^= (uint32_t)p[2] << 16; [[fallthrough]];
			case 2: h ^= (uint32_t)p[1] << 8; [[fallthrough]];
			case 1: h ^= p[0]; h *= m;
			default: break;
		}
		h ^= h >> 13; h *= m; h ^= h >> 15;
		return h;
	}();

	std::string sPacked;
	sPacked.append((const char*)&uVolumeHash, 4);
	sPacked.push_back((char)4);
	sPacked.push_back((char)4);
	sPacked.push_back((char)0);
	sPacked.append((const char*)&flVolume, 4);
	return sPacked;
}

int TrackPicker::Pick(int nTracks, bool bRandom, uint32_t uRandom)
{
	if (nTracks <= 0) return -1;
	if (!bRandom)
	{
		if (iNext >= nTracks) iNext = 0;
		return iNext++;
	}
	// random without repeats: a new round of the bag does not start with the track just played
	vBag.erase(std::remove_if(vBag.begin(), vBag.end(), [nTracks](int i) { return i >= nTracks; }), vBag.end());
	if (vBag.empty())
	{
		for (int i = 0; i < nTracks; i++)
			if (i != iLast || nTracks == 1) vBag.push_back(i);
	}
	const size_t k = (size_t)(uRandom % (uint32_t)vBag.size());
	const int iTrack = vBag[k];
	vBag.erase(vBag.begin() + (long)k);
	return iTrack;
}

void TrackPicker::Reset()
{
	iLast = -1;
	iNext = 0;
	vBag.clear();
}

const std::vector<DefaultPhrase>& DefaultPhrases()
{
	static const std::vector<DefaultPhrase> v = {
		{"Prefix", "{GREEN}[♫]{DEFAULT}", "{GREEN}[♫]{DEFAULT}"},
		{"Now_Playing", "{PREFIX} Сейчас играет: {GREEN}{name}", "{PREFIX} Now playing: {GREEN}{name}"},
		{"Now_Playing_Center", "<font color='lime'>Сейчас играет:</font> <font color='white'>{name}</font>{picture}",
		 "<font color='lime'>Now playing:</font> <font color='white'>{name}</font>{picture}"},
		{"Connect_Hint", "{PREFIX} Введите {GREEN}!res{DEFAULT}, чтобы настроить музыку в конце раунда",
		 "{PREFIX} Type {GREEN}!res{DEFAULT} to set up round end music"},
		{"Menu_Title", "Музыка в конце раунда", "Round end music"},
		{"Menu_State", "Музыка: {state}", "Music: {state}"},
		{"Menu_Volume", "Громкость: {volume}%", "Volume: {volume}%"},
		{"Menu_Tracks", "Прослушать трек", "Listen to a track"},
		{"Menu_Last", "Повторить последний трек", "Replay last track"},
		{"Volume_Title", "Громкость: {volume}%", "Volume: {volume}%"},
		{"Volume_Up", "Громче (+10%)", "Louder (+10%)"},
		{"Volume_Down", "Тише (−10%)", "Quieter (−10%)"},
		{"Tracks_Title", "Прослушать трек", "Listen to a track"},
		{"Preview", "{PREFIX} Прослушивание: {GREEN}{name}", "{PREFIX} Playing: {GREEN}{name}"},
		{"No_Last", "{PREFIX} На этой карте музыка ещё не играла", "{PREFIX} No music has played on this map yet"},
		{"State_Set", "{PREFIX} Музыка в конце раунда: {GREEN}{state}", "{PREFIX} Round end music: {GREEN}{state}"},
		{"On", "вкл", "on"},
		{"Off", "выкл", "off"},
		{"Settings_Item", "Музыка в конце раунда", "Round end music"},
		// The s2sdk menu has an exit button of its own; the sub-menus need an explicit "back"
		// item (s2sdk offers either the exit or the exit-back button, not both), so its label is
		// built in here. A production phrases file may override it under the same key.
		{"Back", "Назад", "Back"},
	};
	return v;
}

bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases)
{
	mapPhrases.clear();
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) return false;
	for (const KvNode& key : root->children)
	{
		if (key.children.empty()) continue; // a plain value, not a phrase block
		std::string sText;
		if (const KvNode* localized = key.Find(sServerLang); localized && localized->hasValue)
			sText = localized->value;
		if (sText.empty())
			if (const KvNode* english = key.Find("en"); english && english->hasValue)
				sText = english->value;
		if (!sText.empty()) mapPhrases[key.name] = std::move(sText);
	}
	return true;
}

std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey)
{
	std::string s;
	auto it = mapPhrases.find(sKey);
	if (it != mapPhrases.end()) s = it->second;
	else
	{
		s = sKey;
		for (const DefaultPhrase& d : DefaultPhrases())
			if (sKey == d.szKey) { s = sServerLang == "ru" ? d.szRu : d.szEn; break; }
	}
	if (sKey != "Prefix") s = Replace(s, "{PREFIX}", Phrase(mapPhrases, sServerLang, "Prefix"));
	return s;
}

std::string NormalizeLanguage(std::string_view name)
{
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name)
		lower.push_back((char)std::tolower((unsigned char)c));
	// Source language names -> the codes used by the phrase files (pisexcompat's table).
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(name);
}

bool IsService(const std::string& sBack)
{
	return sBack == "back" || sBack == "next" || sBack == "exit";
}

bool IsPreviewActive(float flNow, float flPreviewUntil)
{
	return flNow <= flPreviewUntil;
}

int PreviewVolume(int iVolume, int iDefaultVolume)
{
	if (iVolume > 0) return iVolume;
	return std::max(10, iDefaultVolume);
}

float FadeFactor(float flNow, float flFadeFrom, float flFadeBefore)
{
	if (flFadeBefore <= 0.0f) return 1.0f;
	const float k = 1.0f - (flNow - flFadeFrom) / flFadeBefore;
	return k > 0.0f ? k : 0.0f;
}

bool KitHasMusic(int iKit)
{
	return iKit != 0 && iKit != -1;
}

int KitEffective(int iControllerKit, int iInventoryKit)
{
	return KitHasMusic(iControllerKit) ? iControllerKit : iInventoryKit;
}

int KitRestoreValue(int iSaved, int iCurrent)
{
	return iSaved != 0 && iCurrent == 0 ? iSaved : 0;
}

} // namespace res
