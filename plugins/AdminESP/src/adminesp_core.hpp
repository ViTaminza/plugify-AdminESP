// AdminESP — engine-free core: adminesp.ini settings, the access decision and the outline
// colours.
//
// The original Metamod plugin kept five settings in one struct, parsed the two colour
// strings by hand and decided per slot between "no access", "only while dead or spectating" and
// "always" from two permission checks against the admin system. All of that is pure logic, so it
// lives here and test/adminesp_core_tests.cpp drives it against the production adminesp.ini.
//
// Access precedence stays exactly as the original: admin_flag_all wins over admin_flag_death, and
// an empty flag never grants anything.

#pragma once

#include <pisex/keyvalues.hpp>

#include <cstdint>
#include <string>

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

// Reads every key with the original's defaults; missing/invalid colours keep the defaults, an
// empty flag stays empty (nobody gets access).
Settings ParseSettings(pisex::KeyValues& root);

// The permission precedence of the original RefreshAccess().
Access PickAccess(bool hasAlwaysFlag, bool hasDeadOnlyFlag);

// True when an admin with that access level sees outlines right now (dead_only needs the player
// dead or spectating).
bool CanSeeNow(Access access, bool alive, int team);

const char* AccessName(Access access);

} // namespace adminesp
