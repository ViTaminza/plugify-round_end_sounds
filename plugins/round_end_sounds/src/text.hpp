// round_end_sounds — the cs2-menus colour tags of the production phrases file.
//
// The HEAD port sent every chat line through pisexcompat's PrintToChat(), which ran the phrase
// through pisex::Colorize() before calling s2sdk::PrintToChat. s2sdk's PrintToChat sends the text
// as it is (PrintToChatColored uses a different tag table), so the {TAG} words of
// round_end_sounds.phrases.txt are replaced here with exactly the control bytes the Metamod
// cs2-menus server sent - the same table, order and bytes pisexcompat used:
//
//     {DEFAULT} {WHITE} {RED} {LIGHTPURPLE} {GREEN} {LIME} {LIGHTGREEN} {DARKRED} {GRAY}
//     {LIGHTOLIVE} {OLIVE} {LIGHTBLUE} {BLUE} {PURPLE} {LIGHTRED} {GRAYBLUE} and "\n" (U+2029)
//
// Centre-HTML text is deliberately NOT colorized: that phrase is HTML (<font color=...>), not a
// chat line - the HEAD port passed it to PrintToCenterHtml untouched as well.
//
// Header-only and engine-free: round_end_sounds_core_tests checks every tag byte without s2sdk,
// a Plugify host or an engine.

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace res {
namespace detail {

struct ColorTag
{
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus' utils.h colors_text[]/colors_hex[], same order and bytes as pisexcompat. The last
// entry is the chat line break: the phrase files spell it "\n" (backslash + n) and a chat line
// wants U+2029, the engine's line separator.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text)
{
	std::string out(text);
	for (const detail::ColorTag& color : detail::kColorTags)
	{
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos)
		{
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

} // namespace res
