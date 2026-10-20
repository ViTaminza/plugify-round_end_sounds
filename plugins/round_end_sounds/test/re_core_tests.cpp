// Tests for plugins/round_end_sounds/src/re_core.hpp (the engine-free core of the ported
// cs2-round-end-sounds).
//
//   * config parsing against a real round_end_sounds.ini of the original (93 Alko.Track.*
//     soundevents of workshop addon 3811020435),
//   * the track picker (random without repeats / in order), the phrase table, the preview and
//     fade arithmetic, the packed GE_SosSetSoundEventParams volume parameter and the two-field
//     music kit mute rules (m_iMusicKitID / m_unMusicID).

#include <plg/plugin.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "re_core.hpp"

// The plugin gets these from PLUGIFY_PLUGIN; pisexcompat references them, the test leaves them null.
namespace plg {
GetBaseDirFn GetBaseDir{nullptr};
GetExtensionsDirFn GetExtensionsDir{nullptr};
GetConfigsDirFn GetConfigsDir{nullptr};
GetDataDirFn GetDataDir{nullptr};
GetLogsDirFn GetLogsDir{nullptr};
GetCacheDirFn GetCacheDir{nullptr};
IsLoadedFn IsLoaded{nullptr};
LogFn Log{nullptr};
BeginZoneFn BeginZone{nullptr};
EndZoneFn EndZone{nullptr};
} // namespace plg

#ifndef RES_TEST_DATA_DIR
#define RES_TEST_DATA_DIR "."
#endif

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name)
{
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),             \
						g_section.data(), __FILE__, __LINE__, #expr);                              \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_ = (actual);                                                           \
		const std::string e_ = (expected);                                                         \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_.c_str(), e_.c_str());                                                   \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                            \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__, __LINE__,  \
						a_, e_);                                                                   \
		}                                                                                          \
	} while (false)

std::string ReadTestFile(const std::string& sRelative)
{
	std::ifstream file(std::string(RES_TEST_DATA_DIR) + "/" + sRelative, std::ios::binary);
	if (!file) {
		std::printf("FATAL: cannot read %s\n", sRelative.c_str());
		++g_failures;
		return {};
	}
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

void TestConfig()
{
	Section("config: the production round_end_sounds.ini");
	res::Config cfg;
	CHECK(res::ParseConfigText(ReadTestFile("configs/round_end_sounds.ini"), cfg));

	CHECK(cfg.bEnabled);
	CHECK_EQ_INT(cfg.vCommands.size(), 2);
	CHECK_EQ_STR(cfg.vCommands[0], "!res");
	CHECK_EQ_STR(cfg.vCommands[1], "!music");
	CHECK(cfg.bRandom);
	CHECK(cfg.bDefaultEnabled);
	CHECK_EQ_INT(cfg.iDefaultVolume, 30); // default_volume 0.3
	CHECK_EQ_STR(cfg.sMessage, "none");
	CHECK_EQ_INT(cfg.iCenterTime, 7);
	CHECK(cfg.bMuteMvpAnthem);
	CHECK(cfg.bConnectHint);
	CHECK(cfg.bUtilsSettings);
	CHECK(!cfg.bDebug);
	CHECK(cfg.flFadeBeforeRound > 0.99f && cfg.flFadeBeforeRound < 1.01f);
	CHECK_EQ_INT(cfg.vPrecache.size(), 1);
	CHECK_EQ_STR(cfg.vPrecache[0], "soundevents/soundevents_alko.vsndevts");

	Section("config: tracks");
	CHECK_EQ_INT(cfg.vTracks.size(), 93);
	CHECK_EQ_STR(cfg.vTracks[0].sName, "Алко-трек #10");
	CHECK_EQ_STR(cfg.vTracks[0].sSound, "Alko.Track.10");
	CHECK_EQ_STR(cfg.vTracks[92].sSound, "Alko.Track.200");
	for (const res::Track& t : cfg.vTracks)
	{
		CHECK(!t.sSound.empty());
		CHECK(!res::IsFilePathSound(t.sSound));
	}
	CHECK_EQ_INT(res::BuildOwnEvents(cfg.vTracks, &res::SoundEventHash).size(), 93);

	Section("config: in-memory variants (short form, picture, sounds/ path)");
	res::Config cfg2;
	CHECK(res::ParseConfigText(R"(
"RoundEndSounds"
{
	"enabled" "0"
	"message" "CENTER"
	"default_volume" "1.0"
	"center_time" "0"
	"fade_before_round" "0"
	"precache" "soundevents/a.vsndevts, sounds/b.vsnd"
	"tracks"
	{
		"Short" "sounds/music/track.vsnd"
		"Long" { "sound" "Alko.Track.5" "picture" "https://example.org/c.gif" }
		"NoSound" { "picture" "https://example.org/c.gif" }
	}
}
)", cfg2));
	CHECK(!cfg2.bEnabled);
	CHECK_EQ_STR(cfg2.sMessage, "center"); // lowered
	CHECK_EQ_INT(cfg2.iDefaultVolume, 100);
	CHECK_EQ_INT(cfg2.iCenterTime, 1); // clamped to >= 1
	CHECK(cfg2.flFadeBeforeRound == 0.0f);
	CHECK_EQ_INT(cfg2.vPrecache.size(), 2);
	CHECK_EQ_INT(cfg2.vTracks.size(), 2); // the record without a sound is dropped
	CHECK_EQ_STR(cfg2.vTracks[0].sName, "Short");
	CHECK_EQ_STR(cfg2.vTracks[0].sSound, "sounds/music/track.vsnd");
	CHECK(res::IsFilePathSound(cfg2.vTracks[0].sSound));
	CHECK_EQ_STR(cfg2.vTracks[1].sPicture, "https://example.org/c.gif");
	CHECK_EQ_INT(res::BuildOwnEvents(cfg2.vTracks, &res::SoundEventHash).size(), 1);
}

void TestPicker()
{
	Section("track picker");
	// in order: 0,1,2,0,...
	res::TrackPicker seq;
	CHECK_EQ_INT(seq.Pick(3, false, 0), 0);
	CHECK_EQ_INT(seq.Pick(3, false, 0), 1);
	CHECK_EQ_INT(seq.Pick(3, false, 0), 2);
	CHECK_EQ_INT(seq.Pick(3, false, 0), 0);

	// random without repeats: the first bag holds the whole list, so the first four picks are the
	// four tracks, each exactly once (the caller records the played track in iLast, as the plugin
	// does)
	res::TrackPicker picker;
	std::vector<int> vSeen(4, 0);
	int iFirstCycleLast = -1;
	for (int i = 0; i < 4; i++)
	{
		const int iTrack = picker.Pick(4, true, (uint32_t)(i * 7 + 3));
		CHECK(iTrack >= 0 && iTrack < 4);
		vSeen[iTrack]++;
		iFirstCycleLast = iTrack;
		picker.iLast = iTrack;
	}
	for (int i = 0; i < 4; i++) CHECK_EQ_INT(vSeen[i], 1);

	// the next bag excludes the track just played (the reference's "a new cycle does not start
	// with the same track"), so the following n-1 picks are distinct and none of them is that one
	std::vector<int> vSecond;
	for (int i = 0; i < 3; i++)
	{
		const int iTrack = picker.Pick(4, true, (uint32_t)(i * 5 + 1));
		vSecond.push_back(iTrack);
		picker.iLast = iTrack;
	}
	for (int iTrack : vSecond) CHECK(iTrack != iFirstCycleLast);
	CHECK(vSecond[0] != vSecond[1] && vSecond[1] != vSecond[2] && vSecond[0] != vSecond[2]);

	// a single track always repeats
	res::TrackPicker one;
	one.iLast = 0;
	CHECK_EQ_INT(one.Pick(1, true, 12345), 0);
	CHECK_EQ_INT(one.Pick(1, true, 12345), 0);
	CHECK_EQ_INT(one.Pick(0, true, 0), -1);
	CHECK_EQ_INT(one.Pick(0, false, 0), -1);
}

void TestPhrases()
{
	Section("phrases: the production file");
	std::map<std::string, std::string> ru;
	std::map<std::string, std::string> en;
	CHECK(res::ParsePhrasesText(ReadTestFile("translations/round_end_sounds.phrases.txt"), "ru", ru));
	CHECK(res::ParsePhrasesText(ReadTestFile("translations/round_end_sounds.phrases.txt"), "en", en));
	CHECK(!ru.empty());

	const std::string sPlaying = res::Phrase(ru, "ru", "Now_Playing");
	CHECK(sPlaying.find("{name}") != std::string::npos);
	CHECK(sPlaying.find("{PREFIX}") == std::string::npos);
	CHECK(res::Phrase(ru, "ru", "Menu_Title").size() > 3);
	// unknown key -> the key itself; missing store -> the built-in table
	CHECK_EQ_STR(res::Phrase(ru, "ru", "Nope"), "Nope");
	std::map<std::string, std::string> empty;
	CHECK_EQ_STR(res::Phrase(empty, "ru", "On"), "вкл");
	CHECK_EQ_STR(res::Phrase(empty, "en", "On"), "on");
	CHECK_EQ_STR(res::Phrase(empty, "en", "Off"), "off");

	Section("menu service / preview / fade helpers");
	CHECK(res::IsService("back"));
	CHECK(res::IsService("next"));
	CHECK(res::IsService("exit"));
	CHECK(!res::IsService("up"));
	CHECK(!res::IsService("12"));
	CHECK(res::IsPreviewActive(10.0f, 10.5f));
	CHECK(!res::IsPreviewActive(11.0f, 10.5f));
	CHECK_EQ_INT(res::PreviewVolume(50, 30), 50);
	CHECK_EQ_INT(res::PreviewVolume(0, 30), 30);
	CHECK_EQ_INT(res::PreviewVolume(0, 0), 10); // never silent
	// fade from t = 1 over 2 s: full at the start, half at the middle, silent at the end
	CHECK(res::FadeFactor(1.0f, 1.0f, 2.0f) > 0.99f);
	CHECK(res::FadeFactor(2.0f, 1.0f, 2.0f) > 0.49f && res::FadeFactor(2.0f, 1.0f, 2.0f) < 0.51f);
	CHECK(res::FadeFactor(3.0f, 1.0f, 2.0f) == 0.0f);
	CHECK(res::FadeFactor(4.0f, 1.0f, 2.0f) == 0.0f); // never negative
}

void TestMusicKit()
{
	Section("music kit mute: the two schema fields");
	// 0 and -1 both mean "no kit" (the reference's check, its MuteMvpKit)
	CHECK(!res::KitHasMusic(0));
	CHECK(!res::KitHasMusic(-1));
	CHECK(res::KitHasMusic(7));
	CHECK(res::KitHasMusic(5000));

	// the effective kit: the controller field first, the inventory one (m_unMusicID) when the
	// controller field is empty (0/-1) - the in-game case the mute used to miss
	CHECK_EQ_INT(res::KitEffective(7, 123), 7);
	CHECK_EQ_INT(res::KitEffective(0, 123), 123);
	CHECK_EQ_INT(res::KitEffective(-1, 123), 123);
	CHECK_EQ_INT(res::KitEffective(0, 0), 0);
	CHECK_EQ_INT(res::KitEffective(0, -1), -1); // nothing anywhere: nothing to save or zero

	// restore: only into a field that still reads 0 - a kit set meanwhile (the skin changer may
	// apply its own) stays its own; a saved 0 means there was nothing in that field
	CHECK_EQ_INT(res::KitRestoreValue(5000, 0), 5000);
	CHECK_EQ_INT(res::KitRestoreValue(5000, 7), 0);
	CHECK_EQ_INT(res::KitRestoreValue(5000, -1), 0);
	CHECK_EQ_INT(res::KitRestoreValue(0, 0), 0);
	CHECK_EQ_INT(res::KitRestoreValue(0, 7), 0);
}

void TestHashes()
{
	Section("soundevent hash and the packed volume parameter");
	const uint32_t uHash = res::SoundEventHash("Alko.Track.10");
	CHECK_EQ_INT(uHash, res::SoundEventHash("alko.track.10"));
	CHECK(uHash != res::SoundEventHash("Alko.Track.11"));
	std::printf("   hash(alko.track.10) = 0x%08X\n", uHash);

	const std::string sPacked = res::PackVolumeParam(0.3f);
	CHECK_EQ_INT(sPacked.size(), 11);
	CHECK_EQ_INT((unsigned char)sPacked[4], 4);
	CHECK_EQ_INT((unsigned char)sPacked[5], 4);
	CHECK_EQ_INT((unsigned char)sPacked[6], 0);
	float flValue = 0.0f;
	std::memcpy(&flValue, sPacked.data() + 7, 4);
	CHECK(flValue > 0.29f && flValue < 0.31f);
	// the same value always packs to the same bytes
	CHECK(res::PackVolumeParam(0.3f) == sPacked);
}

void TestHandles()
{
	Section("entity handle validity");
	// a CEntityHandle is a uint32 view (entity index in the low 15 bits, serial above): the pawn
	// handle 0xC912805D reads as the int32 -921534371 (index 93, serial 102949) and is VALID;
	// the sign means nothing
	CHECK(res::IsValidHandle(-921534371));
	CHECK(res::IsValidHandle(93));
	CHECK(!res::IsValidHandle(0));  // index 0 serial 0 = the world, and the "none" of the helpers
	CHECK(!res::IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main()
{
	TestConfig();
	TestPicker();
	TestPhrases();
	TestMusicKit();
	TestHashes();
	TestHandles();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
