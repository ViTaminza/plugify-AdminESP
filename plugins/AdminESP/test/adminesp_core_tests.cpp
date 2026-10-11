// Tests for plugins/AdminESP/src/adminesp_core.hpp (engine-independent core of the ported
// "AdminESP" plugin).
//
//   * the real production files parsed exactly like the plugin does it: adminesp.ini
//     and the AdminESP.phrases.txt translation,
//   * the colour parser (#rrggbb, "r g b", "r,g,b", invalid input),
//   * the access precedence (admin_flag_all over admin_flag_death, empty flags never grant) and
//     the dead-or-spectating rule.

#include <plg/plugin.hpp>

#include <pisex/keyvalues.hpp>
#include <pisex/phrases.hpp>

#include <cstdio>
#include <string>
#include <string_view>

#include "adminesp_core.hpp"

// A real plugin gets these from PLUGIFY_PLUGIN; the test leaves them null (also the state the
// code has to survive: no plugify dirs, no logger).
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

namespace {

int g_checks = 0;
int g_failures = 0;
std::string_view g_section = "?";

void Section(std::string_view name) {
	g_section = name;
	std::printf("-- %.*s\n", static_cast<int>(name.size()), name.data());
}

#define CHECK(expr)                                                                            \
	do {                                                                                       \
		++g_checks;                                                                            \
		if (!(expr)) {                                                                         \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: %s\n", static_cast<int>(g_section.size()),         \
						g_section.data(), __FILE__, __LINE__, #expr);                          \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_STR(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const std::string a_ = (actual);                                                       \
		const std::string e_ = (expected);                                                     \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got \"%s\", expected \"%s\"\n",                    \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_.c_str(), e_.c_str());                                     \
		}                                                                                      \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                         \
	do {                                                                                       \
		++g_checks;                                                                            \
		const long long a_ = static_cast<long long>(actual);                                   \
		const long long e_ = static_cast<long long>(expected);                                 \
		if (a_ != e_) {                                                                        \
			++g_failures;                                                                      \
			std::printf("FAIL [%.*s] %s:%d: got %lld, expected %lld\n",                        \
						static_cast<int>(g_section.size()), g_section.data(), __FILE__,       \
						__LINE__, a_, e_);                                                     \
		}                                                                                      \
	} while (false)

// --- the production config -----------------------------------------------------------------

void TestRealConfig() {
	Section("config: production adminesp.ini");

	pisex::KeyValues::AutoDelete kv("Config");
	CHECK(kv->LoadFromFile(ADMINESP_TEST_CONFIG));
	const adminesp::Settings settings = adminesp::ParseSettings(*kv);

	CHECK_EQ_STR(settings.flagAlways, "@admin/zalupa");
	CHECK_EQ_STR(settings.flagDeadOnly, "@admin/spec");
	CHECK_EQ_INT(settings.colorCT.r, 0x49);
	CHECK_EQ_INT(settings.colorCT.g, 0x68);
	CHECK_EQ_INT(settings.colorCT.b, 0xF7);
	CHECK_EQ_INT(settings.colorT.r, 0xE2);
	CHECK_EQ_INT(settings.colorT.g, 0xAD);
	CHECK_EQ_INT(settings.colorT.b, 0x36);
	// Packed RGBA as the engine Color stores it (alpha 255).
	CHECK_EQ_INT(settings.colorCT.Packed(), static_cast<int>(0xFFF76849u));
	CHECK_EQ_INT(settings.colorT.Packed(), static_cast<int>(0xFF36ADE2u));
	// Optional keys the production file does not have:
	CHECK(!settings.enabledByDefault);
	CHECK_EQ_STR(settings.menuCategory, "players");
	CHECK_EQ_INT(settings.glowRange, 5000);
	CHECK_EQ_INT(settings.glowRangeMin, -1000);
	CHECK_EQ_INT(settings.glowType, 2);
	CHECK_EQ_INT(settings.glowState, 3);
}

void TestRealPhrases() {
	Section("phrases: production AdminESP.phrases.txt");

	// The plugin loads the file through pisex::LoadPhrasesFile (Metamod spelling resolved inside
	// the translations dir); here the production file is handed over directly.
	pisex::PhraseMap phrases;
	CHECK(pisex::LoadPhrasesFile(ADMINESP_TEST_PHRASES, phrases, "ru"));
	CHECK_EQ_INT(phrases.size(), 5);
	const auto it = phrases.find("esp_no_access");
	CHECK(it != phrases.end());
	if (it != phrases.end()) {
		CHECK_EQ_STR(it->second, " {RED}[ESP]{DEFAULT} Нет доступа к ESP на этом сервере");
	}
	const auto enabled = phrases.find("esp_enabled");
	CHECK(enabled != phrases.end());
	if (enabled != phrases.end()) {
		CHECK_EQ_STR(enabled->second, " {GREEN}[ESP]{DEFAULT} Вы успешно {GREEN}включили{DEFAULT} ESP");
	}
}

// --- parser ---------------------------------------------------------------------------------

void TestParseColor() {
	Section("colours");
	adminesp::Color color;

	CHECK(adminesp::ParseColor("#4968F7", color));
	CHECK_EQ_INT(color.r, 0x49);
	CHECK_EQ_INT(color.g, 0x68);
	CHECK_EQ_INT(color.b, 0xF7);

	CHECK(adminesp::ParseColor("255 128 0", color));
	CHECK_EQ_INT(color.r, 255);
	CHECK_EQ_INT(color.g, 128);
	CHECK_EQ_INT(color.b, 0);

	CHECK(adminesp::ParseColor("1,2,3", color));
	CHECK_EQ_INT(color.r, 1);
	CHECK_EQ_INT(color.g, 2);
	CHECK_EQ_INT(color.b, 3);

	// Rejected input leaves the colour untouched.
	adminesp::Color untouched{7, 8, 9};
	CHECK(!adminesp::ParseColor("", untouched));
	CHECK(!adminesp::ParseColor(nullptr, untouched));
	CHECK(!adminesp::ParseColor("#12345", untouched));
	CHECK(!adminesp::ParseColor("300 0 0", untouched));
	CHECK(!adminesp::ParseColor("red", untouched));
	CHECK_EQ_INT(untouched.r, 7);
	CHECK_EQ_INT(untouched.g, 8);
	CHECK_EQ_INT(untouched.b, 9);
}

void TestSettingsFallbacks() {
	Section("settings: defaults and optional keys");

	pisex::KeyValues::AutoDelete kv("Config");
	CHECK(kv->LoadFromBuffer(R"("Config" { "ct_color" "0 0 255" "glow_range" "9000" "default_enabled" "1" "menu_category" "server" })"));
	const adminesp::Settings settings = adminesp::ParseSettings(*kv);

	CHECK_EQ_INT(settings.colorCT.b, 255);
	CHECK_EQ_INT(settings.colorCT.r, 0);
	CHECK_EQ_INT(settings.glowRange, 9000);
	CHECK(settings.enabledByDefault);
	CHECK_EQ_STR(settings.menuCategory, "server");
	// Untouched keys keep the original's defaults.
	CHECK_EQ_STR(settings.flagAlways, "");
	CHECK_EQ_STR(settings.flagDeadOnly, "");
	CHECK_EQ_INT(settings.colorT.r, 0xE2);
	CHECK_EQ_INT(settings.glowRangeMin, -1000);
	CHECK_EQ_INT(settings.glowType, 2);
	CHECK_EQ_INT(settings.glowState, 3);

	// An invalid colour falls back to the default instead of turning black.
	pisex::KeyValues::AutoDelete bad("Config");
	CHECK(bad->LoadFromBuffer(R"("Config" { "ct_color" "nonsense" })"));
	const adminesp::Settings fallback = adminesp::ParseSettings(*bad);
	CHECK_EQ_INT(fallback.colorCT.r, 0x49);
	CHECK_EQ_INT(fallback.colorCT.b, 0xF7);
}

void TestAccess() {
	Section("access: precedence and the dead-or-spectating rule");

	// admin_flag_all wins, like the original RefreshAccess().
	CHECK(adminesp::PickAccess(true, true) == adminesp::Access::Always);
	CHECK(adminesp::PickAccess(true, false) == adminesp::Access::Always);
	CHECK(adminesp::PickAccess(false, true) == adminesp::Access::DeadOnly);
	CHECK(adminesp::PickAccess(false, false) == adminesp::Access::None);

	// Always sees everything (alive/dead/spectating/T/CT).
	CHECK(adminesp::CanSeeNow(adminesp::Access::Always, true, adminesp::kTeamCT));
	CHECK(adminesp::CanSeeNow(adminesp::Access::Always, false, adminesp::kTeamT));
	// Dead-only: dead or spectating, never while alive in a team.
	CHECK(adminesp::CanSeeNow(adminesp::Access::DeadOnly, false, adminesp::kTeamT));
	CHECK(adminesp::CanSeeNow(adminesp::Access::DeadOnly, true, adminesp::kTeamSpectator));
	CHECK(!adminesp::CanSeeNow(adminesp::Access::DeadOnly, true, adminesp::kTeamT));
	CHECK(!adminesp::CanSeeNow(adminesp::Access::DeadOnly, true, adminesp::kTeamCT));
	// No access: nothing.
	CHECK(!adminesp::CanSeeNow(adminesp::Access::None, false, adminesp::kTeamSpectator));

	CHECK_EQ_STR(adminesp::AccessName(adminesp::Access::Always), "always");
	CHECK_EQ_STR(adminesp::AccessName(adminesp::Access::DeadOnly), "dead or spectating");
	CHECK_EQ_STR(adminesp::AccessName(adminesp::Access::None), "none");
}

void TestHandleValidity() {
	// A real handle: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(adminesp::IsValidHandle(-921534371));
	CHECK(adminesp::IsValidHandle(0x7FFFFFFF));
	CHECK(adminesp::IsValidHandle(1));
	CHECK(!adminesp::IsValidHandle(0));  // "none" / index 0 (the world)
	CHECK(!adminesp::IsValidHandle(-1)); // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main() {
	TestRealConfig();
	TestRealPhrases();
	TestParseColor();
	TestSettingsFallbacks();
	TestAccess();
	TestHandleValidity();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
