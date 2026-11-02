#include "adminesp_core.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace adminesp {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (Lower(a[i]) != Lower(b[i])) {
			return false;
		}
	}
	return true;
}

// cs2-menus/utils.h colors_text[] / colors_hex[], same order, same bytes. The last entry is the
// chat line break: "\n" -> U+2029 (e2 80 a9), which the CS2 chat renderer shows as a new line.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
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

} // namespace

bool ParseColor(const char* text, Color& out) {
	if (!text || !text[0]) {
		return false;
	}
	unsigned int r = 0;
	unsigned int g = 0;
	unsigned int b = 0;
	if (text[0] == '#') {
		if (std::strlen(text) != 7 || std::sscanf(text + 1, "%02x%02x%02x", &r, &g, &b) != 3) {
			return false;
		}
	} else if (std::sscanf(text, "%u %u %u", &r, &g, &b) != 3 && std::sscanf(text, "%u,%u,%u", &r, &g, &b) != 3) {
		return false;
	}
	if (r > 255 || g > 255 || b > 255) {
		return false;
	}
	out = Color{static_cast<int>(r), static_cast<int>(g), static_cast<int>(b)};
	return true;
}

Settings ParseSettings(const KvNode& root) {
	// ParseKv1() flattens a file that is a single "Config" { ... } block. A file with another
	// wrapping key (or a hand-written root without the block) still reads its direct keys.
	const KvNode* config = root.Find("Config");
	const KvNode& node = config != nullptr ? *config : root;

	Settings settings;
	settings.flagAlways = std::string(node.GetString("admin_flag_all", ""));
	settings.flagDeadOnly = std::string(node.GetString("admin_flag_death", ""));
	Color color = settings.colorCT;
	const std::string ctText(node.GetString("ct_color", ""));
	if (ParseColor(ctText.c_str(), color)) {
		settings.colorCT = color;
	}
	color = settings.colorT;
	const std::string tText(node.GetString("t_color", ""));
	if (ParseColor(tText.c_str(), color)) {
		settings.colorT = color;
	}
	// Optional keys the original config does not have.
	settings.enabledByDefault = node.GetInt("default_enabled", 0) != 0;
	settings.menuCategory = std::string(node.GetString("menu_category", "players"));
	settings.glowRange = node.GetInt("glow_range", settings.glowRange);
	settings.glowRangeMin = node.GetInt("glow_range_min", settings.glowRangeMin);
	settings.glowType = node.GetInt("glow_type", settings.glowType);
	settings.glowState = node.GetInt("glow_state", settings.glowState);
	return settings;
}

Access PickAccess(bool hasAlwaysFlag, bool hasDeadOnlyFlag) {
	if (hasAlwaysFlag) {
		return Access::Always;
	}
	if (hasDeadOnlyFlag) {
		return Access::DeadOnly;
	}
	return Access::None;
}

bool CanSeeNow(Access access, bool alive, int team) {
	switch (access) {
		case Access::Always:
			return true;
		case Access::DeadOnly:
			return !alive || team == kTeamSpectator;
		default:
			return false;
	}
}

const char* AccessName(Access access) {
	switch (access) {
		case Access::Always:
			return "always";
		case Access::DeadOnly:
			return "dead or spectating";
		default:
			return "none";
	}
}

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

std::string NormalizeLanguage(std::string_view name) {
	std::string lower;
	lower.reserve(name.size());
	for (const char c : name) {
		lower.push_back(Lower(c));
	}
	// Source language names -> the codes used by the Pisex phrase files.
	if (lower == "russian" || lower == "ru") {
		return "ru";
	}
	if (lower == "english" || lower == "en") {
		return "en";
	}
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") {
		return "ua";
	}
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") {
		return "pt-BR";
	}
	if (lower == "spanish" || lower == "es") {
		return "es";
	}
	if (lower == "german" || lower == "de") {
		return "de";
	}
	if (lower == "french" || lower == "fr") {
		return "fr";
	}
	if (lower == "polish" || lower == "pl") {
		return "pl";
	}
	if (lower == "turkish" || lower == "tr") {
		return "tr";
	}
	if (lower == "schinese" || lower == "zh-cn") {
		return "zh-CN";
	}
	return std::string(name);
}

std::string PickLanguage(std::string_view coreLanguage, std::string_view serverLanguage) {
	if (!coreLanguage.empty()) {
		return NormalizeLanguage(coreLanguage);
	}
	if (!serverLanguage.empty()) {
		return NormalizeLanguage(serverLanguage);
	}
	return "en";
}

PhraseMap ResolvePhrases(const KvNode& root, std::string_view language) {
	constexpr std::string_view kFallback = "en";
	PhraseMap phrases;
	for (const KvNode& key : root.children) {
		if (key.children.empty()) {
			continue; // a plain value ("language" "ru"), not a phrase
		}
		std::string text;
		if (const KvNode* localized = key.Find(language); localized != nullptr && localized->hasValue) {
			text = localized->value;
		}
		if (text.empty() && !EqualNoCase(language, kFallback)) {
			if (const KvNode* english = key.Find(kFallback); english != nullptr && english->hasValue) {
				text = english->value;
			}
		}
		if (text.empty()) {
			// Last resort: any language the file has, so a partially translated file still shows
			// something instead of an empty chat line (pisexcompat did the same).
			for (const KvNode& candidate : key.children) {
				if (candidate.hasValue) {
					text = candidate.value;
					break;
				}
			}
		}
		phrases[key.name] = std::move(text);
	}
	return phrases;
}

} // namespace adminesp
