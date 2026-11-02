// Tests for plugins/AdminESP/src/adminesp_core.hpp (engine-independent core of the ported
// "AdminESP" plugin).
//
//   * the real production files parsed exactly like the plugin does it: adminesp.ini and
//     AdminESP.phrases.txt through the plugin-local KV1 reader (no pisexcompat),
//   * the colour parser (#rrggbb, "r g b", "r,g,b", invalid input),
//   * the access precedence (admin_flag_all over admin_flag_death, empty flags never grant) and
//     the dead-or-spectating rule,
//   * signed/negative entity handles (0 and -1 invalid, everything else valid),
//   * the cs2-menus colour tags and the phrase language chain (<configs>/core.cfg ServerLang ->
//     s2sdk ServerLanguage -> "en"), including the "en" phrase fallback,
//   * the KV1 grammar (comments, escapes, BOM, #include skip, malformed files rejected whole).

#include "adminesp_core.hpp"

#include <cstdio>
#include <string>
#include <string_view>

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
		const std::string a_{(actual)};                                                        \
		const std::string e_{(expected)};                                                      \
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

// Reads and parses a production file the way src/plugin.cpp does.
bool LoadProductionKv(const char* path, adminesp::KvNode& out) {
	const std::optional<std::string> text = adminesp::ReadTextFile(path);
	if (!text) {
		return false;
	}
	const std::optional<adminesp::KvNode> root = adminesp::ParseKv1(*text);
	if (!root) {
		return false;
	}
	out = *root;
	return true;
}

// --- the production config -----------------------------------------------------------------

void TestRealConfig() {
	Section("config: production adminesp.ini");

	adminesp::KvNode root;
	CHECK(LoadProductionKv(ADMINESP_TEST_CONFIG, root));
	const adminesp::Settings settings = adminesp::ParseSettings(root);

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

	adminesp::KvNode root;
	CHECK(LoadProductionKv(ADMINESP_TEST_PHRASES, root));
	const adminesp::PhraseMap phrases = adminesp::ResolvePhrases(root, "ru");
	CHECK_EQ_INT(phrases.size(), 5);
	const auto noAccess = phrases.find("esp_no_access");
	CHECK(noAccess != phrases.end());
	if (noAccess != phrases.end()) {
		CHECK_EQ_STR(noAccess->second, " {RED}[ESP]{DEFAULT} Нет доступа к ESP на этом сервере");
	}
	const auto enabled = phrases.find("esp_enabled");
	CHECK(enabled != phrases.end());
	if (enabled != phrases.end()) {
		CHECK_EQ_STR(enabled->second, " {GREEN}[ESP]{DEFAULT} Вы успешно {GREEN}включили{DEFAULT} ESP");
	}

	// A language the file does not have falls back to "en", exactly like pisexcompat's loader.
	const adminesp::PhraseMap german = adminesp::ResolvePhrases(root, "de");
	const auto germanNoAccess = german.find("esp_no_access");
	CHECK(germanNoAccess != german.end());
	if (germanNoAccess != german.end()) {
		CHECK_EQ_STR(germanNoAccess->second, " {RED}[ESP]{DEFAULT} You have no ESP access on this server");
	}

	// A partially translated file still shows any language it has.
	adminesp::KvNode partial = *adminesp::ParseKv1(R"("Phrases" { "only" { "ru" "текст" } })");
	const adminesp::PhraseMap onlyRu = adminesp::ResolvePhrases(partial, "de");
	CHECK_EQ_INT(onlyRu.size(), 1);
	const auto onlyText = onlyRu.find("only");
	CHECK(onlyText != onlyRu.end());
	if (onlyText != onlyRu.end()) {
		CHECK_EQ_STR(onlyText->second, "текст");
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

	const auto custom = adminesp::ParseKv1(
		R"("Config" { "ct_color" "0 0 255" "glow_range" "9000" "default_enabled" "1" "menu_category" "server" })");
	CHECK(custom.has_value());
	const adminesp::Settings settings = adminesp::ParseSettings(*custom);

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
	const auto bad = adminesp::ParseKv1(R"("Config" { "ct_color" "nonsense" })");
	CHECK(bad.has_value());
	const adminesp::Settings fallback = adminesp::ParseSettings(*bad);
	CHECK_EQ_INT(fallback.colorCT.r, 0x49);
	CHECK_EQ_INT(fallback.colorCT.b, 0xF7);

	// A file that is not wrapped in "Config" still reads its direct keys, and an empty Config
	// block keeps every default.
	const auto unwrapped = adminesp::ParseKv1(R"("admin_flag_all" "@admin/zalupa")");
	CHECK(unwrapped.has_value());
	CHECK_EQ_STR(adminesp::ParseSettings(*unwrapped).flagAlways, "@admin/zalupa");
	const auto empty = adminesp::ParseKv1(R"("Config" { })");
	CHECK(empty.has_value());
	const adminesp::Settings emptySettings = adminesp::ParseSettings(*empty);
	CHECK_EQ_STR(emptySettings.flagAlways, "");
	CHECK_EQ_INT(emptySettings.colorCT.r, 0x49);
}

void TestKv1Grammar() {
	Section("KV1 grammar");

	// Comments, unquoted tokens, a key with a value and a block, case-insensitive lookup.
	const auto tree = adminesp::ParseKv1(R"(
		// leading comment
		"Config"
		{
			"Flag"     "@admin/x"     // trailing comment
			/* block comment */
			"nested" { "inner" "1" }
			bare value
			"escaped" "a\"b\\c\nd"
			#define SOMETHING 1
		}
	)");
	CHECK(tree.has_value());
	if (tree.has_value()) {
		CHECK_EQ_STR(tree->GetString("flag", "?"), "@admin/x");
		const adminesp::KvNode* nested = tree->Find("NESTED");
		CHECK(nested != nullptr);
		if (nested != nullptr) {
			CHECK_EQ_INT(nested->GetInt("inner", 0), 1);
		}
		CHECK_EQ_STR(tree->GetString("bare", "?"), "value");
		CHECK_EQ_STR(tree->GetString("escaped", "?"), "a\"b\\c\nd");
	}

	// A UTF-8 BOM is skipped.
	const auto bom = adminesp::ParseKv1("\xEF\xBB\xBF\"Config\" { \"a\" \"1\" }");
	CHECK(bom.has_value());
	if (bom.has_value()) {
		CHECK_EQ_INT(bom->GetInt("a", 0), 1);
	}

	// Malformed input is rejected as a whole: unterminated string, missing '}', stray '}'.
	CHECK(!adminesp::ParseKv1(R"("Config" { "a" "unterminated )").has_value());
	CHECK(!adminesp::ParseKv1(R"("Config" { "a" "1")").has_value());
	CHECK(!adminesp::ParseKv1(R"("Config" { } })").has_value());

	// An empty file is an empty root (the plugin then keeps its defaults), not a parse error.
	const auto empty = adminesp::ParseKv1("");
	CHECK(empty.has_value());
	if (empty.has_value()) {
		CHECK(empty->children.empty());
	}

	// Valve's atoi fallback ("1.5" -> 1, "x" -> 0) and the caller's default for a missing key.
	const auto ints = adminesp::ParseKv1(R"("Config" { "a" "1.5" "b" "x" "c" "" })");
	CHECK(ints.has_value());
	if (ints.has_value()) {
		CHECK_EQ_INT(ints->GetInt("a", 9), 1);
		CHECK_EQ_INT(ints->GetInt("b", 9), 0);
		CHECK_EQ_INT(ints->GetInt("c", 9), 9);
		CHECK_EQ_INT(ints->GetInt("missing", 9), 9);
	}
}

void TestColorize() {
	Section("cs2-menus colour tags");

	CHECK_EQ_STR(adminesp::Colorize("{GREEN}on{DEFAULT}"), std::string("\x04on\x01"));
	CHECK_EQ_STR(adminesp::Colorize("{RED}{BLUE}{GRAYBLUE}"), std::string("\x02\x0C\x0A"));
	CHECK_EQ_STR(adminesp::Colorize("a\\nb"), std::string("a\xe2\x80\xa9" "b"));
	CHECK_EQ_STR(adminesp::Colorize("plain {UNKNOWN}"), "plain {UNKNOWN}");
}

void TestLanguage() {
	Section("phrase language chain");

	// core.cfg ServerLang wins, then s2sdk ServerLanguage, then "en"; both are normalized.
	CHECK_EQ_STR(adminesp::PickLanguage("russian", "english"), "ru");
	CHECK_EQ_STR(adminesp::PickLanguage("ru", ""), "ru");
	CHECK_EQ_STR(adminesp::PickLanguage("", "English"), "en");
	CHECK_EQ_STR(adminesp::PickLanguage("", ""), "en");
	CHECK_EQ_STR(adminesp::PickLanguage("", "schinese"), "zh-CN");
	CHECK_EQ_STR(adminesp::NormalizeLanguage("portuguese"), "pt-BR");
	CHECK_EQ_STR(adminesp::NormalizeLanguage("klingon"), "klingon");
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
	Section("entity handles: only 0 and -1 are invalid");

	// A real handle: index 93, serial 102949 -> 0xC912805D = -921534371, a valid handle the
	// game hands out once its serial counter has passed 65535 (bit 31 set).
	CHECK(adminesp::IsValidHandle(-921534371));
	CHECK(adminesp::IsValidHandle(0x7FFFFFFF));
	CHECK(adminesp::IsValidHandle(1));
	CHECK(adminesp::IsValidHandle(-2));   // 0xFFFFFFFE: a negative serial/index view
	CHECK(!adminesp::IsValidHandle(0));   // "none" / index 0 (the world)
	CHECK(!adminesp::IsValidHandle(-1));  // INVALID_EHANDLE_INDEX (0xFFFFFFFF)
}

} // namespace

int main() {
	TestRealConfig();
	TestRealPhrases();
	TestParseColor();
	TestSettingsFallbacks();
	TestKv1Grammar();
	TestColorize();
	TestLanguage();
	TestAccess();
	TestHandleValidity();

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
