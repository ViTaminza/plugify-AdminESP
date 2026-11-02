// AdminESP — engine-free core: adminesp.ini settings, the access decision, the outline colours
// and the plugin-local replacements for the pieces pisexcompat used to provide (KV1 reader,
// cs2-menus colour tags, phrase resolution and the phrase language chain).
//
// The original Metamod plugin kept five settings in one struct, parsed the two colour strings by
// hand and decided per slot between "no access", "only while dead or spectating" and "always"
// from two permission checks against the admin system. All of that is pure logic, so it lives
// here and test/adminesp_core_tests.cpp drives it against the production adminesp.ini and
// AdminESP.phrases.txt.
//
// Pure s2sdk, no pisexcompat (docs/PLUGIN_STANDARD.md §7). The production files keep their
// Metamod/KeyValues format: ParseKv1() in src/kv1.hpp is the local reader, ResolvePhrases() is the
// language -> "en" -> any-language fallback pisexcompat's LoadPhrasesFile performed, and
// PickLanguage() is the <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en" chain
// (the file/s2sdk I/O itself is in src/plugin.cpp).
//
// Access precedence stays exactly as the original: admin_flag_all wins over admin_flag_death, and
// an empty flag never grants anything.

#pragma once

#include "kv1.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace adminesp {

// Entity HANDLES are int32 views of the engine's uint32 CEntityHandle (index | serial << 15):
// valid <=> h != INVALID_EHANDLE_INDEX (-1) && h != 0 (index 0 is the world; CreateEntityByName
// answers -1 for "creation failed"). The sign means nothing - once the engine's serial counter
// passes 65535 bit 31 is set and the handle is a negative int; every `h > 0` test silently drops
// those outline entities (they are never removed then).
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// What an admin may do with the outlines (the original's Access enum).
enum class Access : int {
	None = 0,
	DeadOnly = 1,
	Always = 2,
};

// The two teams that carry outlines (CS2 teams; 1 is spectator).
inline constexpr int kTeamSpectator = 1;
inline constexpr int kTeamT = 2;
inline constexpr int kTeamCT = 3;

// An RGB colour of the config (0-255 per channel, alpha is always opaque). Packed() is the
// Source RGBA value (Color) the spawn keyvalue and the schema field carry.
struct Color {
	int r = 0;
	int g = 0;
	int b = 0;

	[[nodiscard]] int Packed() const {
		return r | (g << 8) | (b << 16) | (255 << 24);
	}
};

struct Settings {
	std::string flagAlways;   // admin_flag_all:  "@admin/zalupa"
	std::string flagDeadOnly; // admin_flag_death: "@admin/spec"
	Color colorCT{0x49, 0x68, 0xF7}; // ct_color  "#4968F7"
	Color colorT{0xE2, 0xAD, 0x36};  // t_color   "#E2AD36"
	// Off until the admin asks for it, like the original plugin.
	bool enabledByDefault = false;
	std::string menuCategory = "players";
	// The original plugin's outline, as spawn keyvalues (keyvalue numbers, not schema numbers).
	int glowRange = 5000;
	int glowRangeMin = -1000;
	int glowType = 2;
	int glowState = 3;
};

// "#rrggbb" or "r g b" / "r,g,b". Returns false (and leaves `out` untouched) on anything else.
bool ParseColor(const char* text, Color& out);

// Reads every key with the original's defaults from the parsed adminesp.ini. `root` is the node
// ParseKv1() returned (the flattened "Config" block); a file without that block is accepted too,
// exactly like KeyValues::GetString looked at the loaded root. Missing/invalid colours keep the
// defaults, an empty flag stays empty (nobody gets access).
Settings ParseSettings(const KvNode& root);

// The permission precedence of the original RefreshAccess().
Access PickAccess(bool hasAlwaysFlag, bool hasDeadOnlyFlag);

// True when an admin with that access level sees outlines right now (dead_only needs the player
// dead or spectating).
bool CanSeeNow(Access access, bool alive, int team);

const char* AccessName(Access access);

// --- pisexcompat's local replacements -----------------------------------------------------------

// The cs2-menus colour tags the production phrase files carry ({DEFAULT} {RED} {GREEN} ...,
// "\\n"): replaced with exactly the chat control bytes the Metamod servers sent, byte for byte
// the table pisexcompat carried (cs2-menus utils.h colors_text[]/colors_hex[]). The last entry is
// the chat line break, which the CS2 chat renderer shows as U+2029.
std::string Colorize(std::string_view text);

// Source language name ("russian", "english", ...) or code -> the code the phrase files use
// ("ru", "en", ...). Unknown names are returned unchanged (pisexcompat's NormalizeLanguage).
std::string NormalizeLanguage(std::string_view name);

// The language a phrase table is resolved for: <configs>/core.cfg "ServerLang" first, then
// s2sdk's ServerLanguage, then "en". Both inputs are normalized; an empty core value falls
// through, an empty server value becomes "en".
std::string PickLanguage(std::string_view coreLanguage, std::string_view serverLanguage);

using PhraseMap = std::map<std::string, std::string>;

// A parsed phrases file ("Key" { "ru" "..." "en" "..." }): every block child resolved for
// `language`, then for "en", then for the first language that has a non-empty text. Plain values
// next to the blocks are not phrases and are skipped. The original had the same fallback chain
// (pisexcompat's LoadPhrasesFile); a key with no text at all stays empty and Phrase() prints the
// key itself.
PhraseMap ResolvePhrases(const KvNode& root, std::string_view language);

} // namespace adminesp
