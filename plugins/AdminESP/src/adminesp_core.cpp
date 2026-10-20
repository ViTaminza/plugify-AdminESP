#include "adminesp_core.hpp"

#include <cstdio>
#include <cstring>

namespace adminesp {

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

Settings ParseSettings(pisex::KeyValues& root) {
	Settings settings;
	settings.flagAlways = root.GetString("admin_flag_all", "");
	settings.flagDeadOnly = root.GetString("admin_flag_death", "");
	Color color = settings.colorCT;
	if (ParseColor(root.GetString("ct_color", ""), color)) {
		settings.colorCT = color;
	}
	color = settings.colorT;
	if (ParseColor(root.GetString("t_color", ""), color)) {
		settings.colorT = color;
	}
	// Optional keys the original config does not have.
	settings.enabledByDefault = root.GetInt("default_enabled", 0) != 0;
	settings.menuCategory = root.GetString("menu_category", "players");
	settings.glowRange = root.GetInt("glow_range", settings.glowRange);
	settings.glowRangeMin = root.GetInt("glow_range_min", settings.glowRangeMin);
	settings.glowType = root.GetInt("glow_type", settings.glowType);
	settings.glowState = root.GetInt("glow_state", settings.glowState);
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

} // namespace adminesp
