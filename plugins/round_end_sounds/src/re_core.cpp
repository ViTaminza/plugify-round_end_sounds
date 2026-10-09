// round_end_sounds — engine-free core (see re_core.hpp).
#include "re_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include <pisex/keyvalues.hpp>

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
	cfg = Config();
	pisex::KeyValues kv("RoundEndSounds");
	if (!kv.LoadFromBuffer(text)) return false;

	cfg.bEnabled = kv.GetInt("enabled", 1) != 0;
	auto vCommands = SplitList(kv.GetString("commands", "!res,!music"));
	if (!vCommands.empty()) cfg.vCommands = vCommands;
	cfg.bRandom = kv.GetInt("random", 1) != 0;
	cfg.bDefaultEnabled = kv.GetInt("default_enabled", 1) != 0;
	cfg.iDefaultVolume = std::clamp((int)lroundf(kv.GetFloat("default_volume", 0.5f) * 100.0f), 0, 100);
	cfg.sMessage = Lower(kv.GetString("message", "chat"));
	cfg.iCenterTime = std::max(1, kv.GetInt("center_time", 7));
	cfg.bMuteMvpAnthem = kv.GetInt("mute_mvp_anthem", 1) != 0;
	cfg.flFadeBeforeRound = std::max(0.0f, kv.GetFloat("fade_before_round", 1.0f));
	cfg.bUtilsSettings = kv.GetInt("utils_settings", 1) != 0;
	cfg.bConnectHint = kv.GetInt("connect_hint", 1) != 0;
	cfg.bDebug = kv.GetInt("debug", 0) != 0;
	cfg.vPrecache = SplitList(kv.GetString("precache", ""));

	if (pisex::KeyValues* pTracks = kv.FindKey("tracks"))
	{
		for (pisex::KeyValues* pTrack = pTracks->GetFirstSubKey(); pTrack; pTrack = pTrack->GetNextKey())
		{
			Track t;
			t.sName = pTrack->GetName();
			if (pTrack->GetFirstSubKey())
			{
				t.sSound = pTrack->GetString("sound", "");
				t.sPicture = pTrack->GetString("picture", "");
			}
			else
				t.sSound = pTrack->GetString(nullptr, ""); // short form: "Name" "sound"
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
	};
	return v;
}

bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases)
{
	mapPhrases.clear();
	pisex::KeyValues kv("Phrases");
	if (!kv.LoadFromBuffer(text)) return false;
	for (pisex::KeyValues* pKey = kv.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		const char* sz = pKey->GetString(sServerLang.c_str(), "");
		if (!sz[0]) sz = pKey->GetString("en", "");
		if (sz[0]) mapPhrases[pKey->GetName()] = sz;
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
