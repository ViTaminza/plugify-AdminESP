// AdminESP — Plugify port of the Metamod plugin "AdminESP": admin_system admins see player
// outlines through walls.
//
// Every living player gets two prop_dynamic entities: an invisible relay that bone-merges onto the
// pawn and a glowing copy of the model that follows the relay. They exist only while at least one
// admin has ESP switched on, and ESP is off for everyone until an admin types !esp (or uses the
// admin menu).
//
// Who sees the outlines is decided in the CheckTransmit listener: an admin whose ESP is on and
// whom admin_system allows (admin_flag_all: always, admin_flag_death: only while dead or
// spectating) gets every outline but his own; everybody else gets none.
//
// Pure s2sdk, no pisexcompat and no s2guard (docs/PLUGIN_STANDARD.md §7): every safety rule the
// first port got from pisexcompat lives in this file.
//   * PITFALLS B1/B2 — the map gate g_mapReady. s2sdk client/entity/schema functions dereference
//     null before the first map and a torn-down client list between OnMapEnd and the next
//     OnMapStart. The OnMapStart/OnMapEnd listeners are registered in OnPluginStart, readiness is
//     latched by OnMapStart and by the first OnPluginUpdate of a plugin loaded into a running map
//     (when no OnMapEnd was seen since OnPluginStart), and dropped by OnMapEnd. No client, entity
//     or schema call is made while the gate is shut; OnMapEnd only clears state (PITFALLS B2).
//   * PITFALLS C1 — a CEntityHandle is `index | serial << 15`: 0 is the world and -1 is invalid,
//     but the sign means nothing (a serial above 65535 sets bit 31), so the handle check is
//     `handle == 0 || handle == -1`, never `handle <= 0`. Every stored outline handle is
//     re-resolved with EntHandleToEntIndex before it is used, and the retire list keeps hiding
//     removed entities until the engine has really freed them (the serial check makes a reused
//     index impossible to match).
//   * PITFALLS C2 — every client access goes through HasController/HasPawn/IsAlive/PlayerTeam:
//     slot 0..63, the map gate, IsClientConnected/IsClientInGame/!IsClientSourceTV and a
//     controller pointer (plus PlayerSlotToEntHandle/IsValidEntHandle/IsValidEntPointer when the
//     host exports them). The pawn and IsClientAlive are only asked after that.
//   * PITFALLS C8 — GetSchemaOffset is only called with the map gate open (PlayerTeam and
//     PlaysAlive reach it through HasController) and only successful offsets (>= 0) are cached;
//     the values are read with raw GetEntData at the field's real width (m_iTeamNum and
//     m_bControllingBot are 1 byte), never with a typed schema call.
//   * PITFALLS C11/C12 — "mm_esp" and "esp" are this plugin's own ConCommands (AddConsoleCommand)
//     and each answers only its own context. Chat "!esp"/"/esp" chain to the trigger-less name and
//     arrive as ConCommandContext::Chat; the console spelling is "mm_esp". "player_spawn",
//     "player_death", "player_team", "player_disconnect" and "round_prestart" are Pre hooks,
//     removed with UnhookEvent in OnPluginEnd; every handler answers ResultType::Continue so no
//     event is consumed for the engine or another plugin.
//   * PITFALLS A7/C12 — OnPluginEnd unregisters the transmit listener, detaches each command the
//     way it was registered (RemoveCommand only for names this plugin created,
//     RemoveCommandListener for a pre-existing name), unhooks the events, removes the
//     OnClientAuthenticated and map listeners, drops the admin API callbacks and the state, so
//     `plg reload` leaves nothing behind that points into the unloaded .so.
//   * PITFALLS D.6 — this plugin owns no polyhook detour. The CheckTransmit detour belongs to
//     s2sdk; only this plugin's callback is removed with OnServerCheckTransmit_Unregister, and
//     UnhookDetour is never called (unhooking s2sdk's shared detour would remove it for every
//     plugin). s2sdk removes the detour itself once its last callback is gone.
//   * PITFALLS A1 — OnPluginStart never returns Failed (that stops every Plugify plugin); a missing
//     binding only logs and disables the feature that needs it.
//
// Porting notes (Metamod -> Plugify):
//   * the KHook/SourceHook ISource2GameEntities::CheckTransmit vtable hook is replaced by the
//     s2sdk listener OnServerCheckTransmit (s2sdk hooks CheckTransmit itself and hands the
//     CCheckTransmitInfo list to plugins; GetTransmitInfoPlayerSlot replaces the hard-coded
//     recipient-slot offset, and ClearTransmitInfoEntity + SetTransmitInfoNonPlayer perform the
//     clear + "exists but not transmitted" pair from s2sdk's own TransmitManager: clear-only made
//     a client that lost the outline delete it and crash on the next delta with
//     "CopyExistingEntity: missing client entity"). The entity and always lists are only ever
//     cleared, never filled (PITFALLS: never force an entity into a snapshot). The optional
//     ClearTransmitInfoAlways is applied too, so an entity the engine put on its always-list
//     cannot leak to a non-viewer.
//   * entity creation uses s2sdk CreateEntityByName + DispatchSpawn2 with the same spawn values
//     the original built on the entity system's arena (model, spawnflags 256, rendermode 10 for
//     the relay; model, spawnflags 256, glowcolor, glowrange, glowrangemin, glowteam, glowtype,
//     glowstate, renderamt for the glow). FollowEntity(child, parent, true) replaces the
//     "FollowEntity" input, RemoveEntity the entity-system removal. glowcolor is handed over as a
//     "r g b a" string (DispatchSpawn2 has no colour value type; an int is not converted and the
//     glow then falls back to the engine's own colour).
//   * outline entities are tracked by whole CEntityHandle values; EntHandleToEntIndex's serial
//     check is what keeps a freed and reused index from ever being touched.
//   * the client view sync (mp_forcecamera 0 / mp_playerid 0 sent to the viewer's client only)
//     uses s2sdk FindConVar/GetConVarString/SendConVarValue - the exact operation
//     IPlayersApi::SetConVar performed.
//   * admin checks go through plugins/admin_system/include/admin_api.hpp (HasPermission with the
//     two config flags, the "players" category item, OnAdminConnected/OnCoreLoaded) exactly like
//     the other ported modules.
//   * config and phrases keep the Metamod names and formats: <configs>/AdminESP/adminesp.ini and
//     <configs>/AdminESP/translations/AdminESP.phrases.txt, with the original
//     addons/configs/... / addons/translations/... paths as a fallback. They are read by the
//     plugin-local KV1 reader and resolved by the plugin-local colour/phrase helpers
//     (docs/PLUGIN_STANDARD.md §7); the phrase language is <configs>/core.cfg "ServerLang", then
//     s2sdk::GetServerLanguage, then "en".
//   * a pisexcompat timer (0.25 s reconcile) is now a deadline accumulator driven by
//     OnPluginUpdate(dt), and the per-slot "rebuild after spawn / round start" delays are kept
//     together with the slot identity (steam id, controller handle for bots) they belong to: a
//     new player in a reused slot never inherits the previous occupant's pending rebuild, its
//     access or its ESP switch (B6: OnClientAuthenticated is not delivered for every client, so
//     the once-per-frame observation is the authoritative fallback).
//
// Needs a check on a real server: the outline colours (the engine reads the glowcolor keyvalue) and
// the removal of the parent/child pair (the original detached before removing - s2sdk has no
// "clear parent" input, so both entities are simply removed, glow first). No real game test was
// simulated here; tests/adminesp_plugin_tests.cpp only drives recording fakes.

#include "adminesp_core.hpp"

#include <admin_api.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kMaxSlots = 64;
// Handles of removed entities the engine may not have freed yet (the original's kRetiredSlots).
constexpr int32_t kRetiredSlots = 256;
// The reconcile timer also creates glows for players who just spawned, so it runs often;
// permissions are re-read from admin_system every fourth run.
constexpr int32_t kReconcileIntervalMs = 250;
constexpr int32_t kAccessRefreshEvery = 4;
// Wait this long after a spawn before copying the model, so agent changes applied right after
// spawning are picked up.
constexpr float kSpawnDelaySeconds = 0.3f;
// After a round restart the pawns are replaced; give the spawn events time to arrive.
constexpr float kRoundStartDelaySeconds = 1.5f;
constexpr int32_t kBoneMergeSpawnFlag = 256;
// hl2sdk's kRenderNone (the relay is never drawn).
constexpr int32_t kRenderNone = 10;
constexpr char kLogTag[] = "AdminESP";
constexpr char kPluginDir[] = "AdminESP";
constexpr char kConfigFile[] = "adminesp.ini";
constexpr char kPhrasesFile[] = "AdminESP.phrases.txt";
constexpr char kCoreConfigFile[] = "core.cfg";
constexpr char kPropDynamic[] = "prop_dynamic";
// The two spellings of the original's RegCommand call. "esp" is the trigger-less name s2sdk chains
// "!esp"/"/esp" to (it marks the call ConCommandContext::Chat); "mm_esp" is the console one.
constexpr char kMmEspCommand[] = "mm_esp";
constexpr char kEspCommand[] = "esp";
// PITFALLS C1: a handle is `index | serial << 15`; only 0 and -1 are invalid.
constexpr uint64_t kControllerIdentityBit = 0x8000000000000000ull;

// s2sdk's ConVarFlag is a plain enum class without the plg bitmask trait, so the flags are combined
// through the underlying type (the same pattern as as_hide/vip_btw).
template <typename... TFlags>
constexpr s2sdk::ConVarFlag CombineFlags(TFlags... flags) {
	return static_cast<s2sdk::ConVarFlag>((static_cast<int64_t>(flags) | ...));
}

// The flags pisexcompat's RegisterS2sdkName gave every RegCommand name (including the chat
// spelling): Release | ClientCanExecute. Keeping them preserves the old port's console/chat
// behaviour exactly.
constexpr s2sdk::ConVarFlag kCommandFlags =
	CombineFlags(s2sdk::ConVarFlag::Release, s2sdk::ConVarFlag::ClientCanExecute);

// The admin_system consumer API (the original's g_pAdminApi). Set in OnPluginStart and dropped in
// OnPluginEnd, so a callback the engine still holds for a moment cannot reach it (PITFALLS A7).
admin_system::AdminApi* g_pAdminApi = nullptr;

adminesp::Settings g_settings;
adminesp::PhraseMap g_phrases;
// pisexcompat detected the phrase language once per process (g_languageDetected) and reused it
// for every LoadPhrasesFile call; the port keeps that behaviour per plugin load, so a later
// core.cfg edit only takes effect on a reload, exactly like the previous port. OnPluginStart /
// OnPluginEnd clear the cache.
std::string g_phrasesLanguage;
bool g_phrasesLanguageDetected = false;

// --- PITFALLS B1/B2: the map gate ----------------------------------------------------------------

bool g_mapReady = false;           // a map is up: every s2sdk client/entity/schema call is safe
bool g_mapEndedSinceStart = false; // OnMapEnd was seen since OnPluginStart
bool g_firstUpdateDone = false;    // the mid-map latch has already been decided
bool g_mapListeners = false;       // OnMapStart/OnMapEnd are registered with s2sdk

// --- per-slot viewer state ----------------------------------------------------------------------

// What the reconcile knows about a slot. `identity` is the steam id (bots: the controller handle)
// whose access/enabled/canSee values these are; when it changes, everything is reset before a new
// value can be used, so a slot reused by another player never inherits the previous ESP state.
struct ViewerSlot {
	adminesp::Access access = adminesp::Access::None;
	bool enabled = false;
	bool canSee = false;
	bool connected = false;
	uint64_t identity = 0;
};

ViewerSlot g_viewers[kMaxSlots];

// --- outline entities ----------------------------------------------------------------------------

struct TargetGlow {
	int32_t relay = 0; // CEntityHandle of the invisible bone-merge relay
	int32_t glow = 0;  // CEntityHandle of the glowing copy
	std::string model;
	int team = 0;
	// The slot identity the outline (or the pending rebuild) belongs to.
	uint64_t identity = 0;
	// A pending "rebuild after spawn/round start" deadline; only honoured for `identity`.
	float createAfter = 0.0f;
};

TargetGlow g_glows[kMaxSlots];
int32_t g_retired[kRetiredSlots] = {};
int32_t g_reconcileRuns = 0;
int32_t g_sinceReconcileMs = 0;
// Only used when the host does not export GetGameTime (tests, an old s2sdk): a map-local clock
// advanced from OnPluginUpdate, so the spawn/round delays still work.
float g_fallbackTime = 0.0f;
// False when the s2sdk bindings this feature needs are missing (no ESP at all then, but the
// plugin keeps running). It is also the hard security switch: no outline entity is ever created
// while it is false, because nothing could hide it from non-viewers.
bool g_canTransmit = false;
bool g_transmitRegistered = false;
uint64_t g_conVarForceCamera = 0;
uint64_t g_conVarPlayerId = 0;
// Only successful schema lookups are cached (PITFALLS C8); -1 means "not resolved yet".
int32_t g_teamOffset = -1;
int32_t g_controllingBotOffset = -1;

// --- logs ---------------------------------------------------------------------------------------

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Format(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	return std::string(buffer.data());
}

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(std::string("[") + kLogTag + "] " + std::string(message), plg::Severity::Error);
}

// --- small helpers ------------------------------------------------------------------------------

bool ValidSlot(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

float Now() {
	if (!g_mapReady) {
		return 0.0f;
	}
	if (__s2sdk_GetGameTime != nullptr) {
		try {
			return s2sdk::GetGameTime();
		} catch (...) {
		}
	}
	return g_fallbackTime;
}

const char* Phrase(const char* key) {
	const auto it = g_phrases.find(key);
	return it != g_phrases.end() ? it->second.c_str() : key;
}

void PrintToChat(int32_t slot, std::string_view text);

// --- config / phrases ---------------------------------------------------------------------------

std::string ConfigsRoot() {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	return ToStd(plg::GetConfigsDir());
}

std::string DataRoot() {
	if (plg::GetDataDir == nullptr) {
		return {};
	}
	return ToStd(plg::GetDataDir());
}

std::string JoinPath(std::string_view dir, std::string_view file) {
	if (dir.empty()) {
		return std::string(file);
	}
	std::string path(dir);
	if (path.back() != '/') {
		path.push_back('/');
	}
	if (!file.empty() && file.front() == '/') {
		file.remove_prefix(1);
	}
	path.append(file);
	return path;
}

// The plugin's config candidates in the exact order pisexcompat's ResolvePath tried them for
// "addons/configs/adminesp.ini": the original Metamod spelling relative to the server working
// directory first, then the plugin config dir, its translations subdir and the plugin data dir.
// The raw cwd candidate must stay first: a server that still keeps the Metamod file next to the
// game binary must keep loading exactly that file (and its flags/colours), not a different one
// from <configs>/AdminESP.
std::vector<std::string> ConfigCandidates() {
	std::vector<std::string> candidates{std::string("addons/configs/") + kConfigFile};
	// pisexcompat's ConfigDir() is JoinPath(GetConfigsDir(), "AdminESP"), and JoinPath("", name)
	// is the relative "AdminESP": keep that spelling so a host without a configs dir resolves the
	// same files the old port did.
	const std::string root = ConfigsRoot();
	const std::string pluginDir = root.empty() ? std::string(kPluginDir) : JoinPath(root, kPluginDir);
	candidates.push_back(JoinPath(pluginDir, kConfigFile));
	candidates.push_back(JoinPath(JoinPath(pluginDir, "translations"), kConfigFile));
	const std::string dataRoot = DataRoot();
	const std::string dataPluginDir =
		dataRoot.empty() ? std::string(kPluginDir) : JoinPath(dataRoot, kPluginDir);
	candidates.push_back(JoinPath(dataPluginDir, kConfigFile));
	return candidates;
}

// The exact candidates of pisexcompat's ResolveTranslationsPath for
// "addons/translations/AdminESP.phrases.txt": the raw cwd file first, then the two literal
// JoinPath(TranslationsDir/ConfigDir, full path) spellings it tried, then the stripped name in the
// plugin translations dir. Keeping the raw cwd file first preserves the old localisation: a server
// with the Metamod file next to the binary keeps using that exact phrase table.
std::vector<std::string> PhraseCandidates() {
	std::vector<std::string> candidates{std::string("addons/translations/") + kPhrasesFile};
	// Same ConfigDir()/TranslationsDir() spelling as ConfigCandidates (relative "AdminESP" when the
	// host exposes no configs dir).
	const std::string root = ConfigsRoot();
	const std::string pluginDir = root.empty() ? std::string(kPluginDir) : JoinPath(root, kPluginDir);
	const std::string translationsDir = JoinPath(pluginDir, "translations");
	const std::string fullPath = std::string("addons/translations/") + kPhrasesFile;
	candidates.push_back(JoinPath(translationsDir, fullPath));
	candidates.push_back(JoinPath(pluginDir, fullPath));
	candidates.push_back(JoinPath(translationsDir, kPhrasesFile));
	return candidates;
}

// Reads the first candidate that can be opened. `usedPath` always receives a printable path (the
// most likely one when nothing exists) for the error log.
bool ReadFirstExisting(const std::vector<std::string>& candidates, std::string& content, std::string& usedPath) {
	usedPath = candidates.empty() ? std::string(kConfigFile) : candidates.front();
	for (const std::string& candidate : candidates) {
		if (std::optional<std::string> text = adminesp::ReadTextFile(candidate); text) {
			content = std::move(*text);
			usedPath = candidate;
			return true;
		}
	}
	return false;
}

// <configs>/core.cfg "ServerLang" (the Metamod core setting pisexcompat read first); "" when the
// file is missing, malformed or has no ServerLang.
std::string CoreConfigLanguage() {
	const std::string root = ConfigsRoot();
	if (root.empty()) {
		return {};
	}
	const std::string path = JoinPath(root, kCoreConfigFile);
	const std::optional<std::string> text = adminesp::ReadTextFile(path);
	if (!text) {
		return {};
	}
	const std::optional<adminesp::KvNode> rootNode = adminesp::ParseKv1(*text);
	if (!rootNode) {
		return {};
	}
	return std::string(rootNode->GetString("ServerLang", ""));
}

// s2sdk's ServerLanguage (envs/plugins/s2sdk/settings.jsonc); available before the first map.
std::string ServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetServerLanguage());
	} catch (...) {
		return {};
	}
}

// <configs>/core.cfg "ServerLang" -> s2sdk ServerLanguage -> "en", detected once per plugin load
// (the pisexcompat phrase cache did not re-read core.cfg on every map either).
std::string PhrasesLanguage() {
	if (!g_phrasesLanguageDetected) {
		g_phrasesLanguage = adminesp::PickLanguage(CoreConfigLanguage(), ServerLanguage());
		g_phrasesLanguageDetected = true;
	}
	return g_phrasesLanguage;
}

void LoadConfig() {
	std::string content;
	std::string path;
	if (!ReadFirstExisting(ConfigCandidates(), content, path)) {
		// Without the file nobody has a permission flag, so ESP stays off.
		LogError("Failed to load AdminESP config '" + path + "'");
		g_settings = adminesp::Settings(); // both flags empty, colours at their defaults
		return;
	}
	const std::optional<adminesp::KvNode> root = adminesp::ParseKv1(content);
	if (!root) {
		LogError("Failed to parse AdminESP config '" + path + "'");
		g_settings = adminesp::Settings();
		return;
	}
	g_settings = adminesp::ParseSettings(*root);
}

void LoadPhrases() {
	g_phrases.clear();
	std::string content;
	std::string path;
	if (!ReadFirstExisting(PhraseCandidates(), content, path)) {
		LogError("Failed to load phrases '" + path + "'");
		return;
	}
	const std::optional<adminesp::KvNode> root = adminesp::ParseKv1(content);
	if (!root) {
		LogError("Failed to parse phrases '" + path + "'");
		return;
	}
	g_phrases = adminesp::ResolvePhrases(*root, PhrasesLanguage());
}

// --- guarded s2sdk helpers ----------------------------------------------------------------------

// The map gate and the slot range: from here on an s2sdk client call cannot touch a torn-down
// client list (PITFALLS B2) or an out-of-range slot (PITFALLS C2).
bool CanQueryClient(int32_t slot) {
	return g_mapReady && ValidSlot(slot);
}

bool IsConnectedClient(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool IsFakeClient(int32_t slot) {
	if (!CanQueryClient(slot) || __s2sdk_IsFakeClient == nullptr) {
		return true;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true;
	}
}

// A connected, in-game, human-addressable slot whose controller entity exists: the original's
// CCSPlayerController::FromSlot() != nullptr. The controller handle is validated when the host
// exports the checks. PITFALLS C1: only 0 and -1 are invalid, a negative handle may be valid.
bool HasController(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientConnected(slot) || !s2sdk::IsClientInGame(slot) ||
			s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr) {
			return false;
		}
		if (__s2sdk_PlayerSlotToEntHandle && __s2sdk_IsValidEntHandle) {
			const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
			if (handle == 0 || handle == -1 || !s2sdk::IsValidEntHandle(handle)) {
				return false;
			}
		}
		if (__s2sdk_IsValidEntPointer && !s2sdk::IsValidEntPointer(controller)) {
			return false;
		}
		return true;
	} catch (...) {
		return false;
	}
}

// The validated controller handle of a slot, or 0.
int32_t ControllerHandle(int32_t slot) {
	if (!HasController(slot) || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_EntHandleToEntIndex == nullptr) {
		return 0;
	}
	try {
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!adminesp::IsValidHandle(handle)) {
			return 0;
		}
		return s2sdk::EntHandleToEntIndex(handle) > 0 ? handle : 0;
	} catch (...) {
		return 0;
	}
}

// The controller also has a current pawn (the original's GetPlayerPawn() != nullptr). s2sdk's
// pawn-based helpers print "Cannot execute '...' on invalid player pawn" for a controller without
// one, so nothing reaches them before this check (PITFALLS C2).
bool HasPawn(int32_t slot) {
	if (!HasController(slot) || __s2sdk_GetClientPawn == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientPawn(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

// The original's m_bPawnIsAlive(): the pawn is guaranteed by HasPawn(), IsClientAlive dereferences
// it, so it is asked last.
bool IsAlive(int32_t slot) {
	if (!HasPawn(slot) || __s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAlive(slot);
	} catch (...) {
		return false;
	}
}

// The pawn handle: the original's GetRefEHandle() on the player pawn. The pointer is converted to
// an index first (the pisexcompat chain), with EntPointerToEntHandle as a direct fast path.
int32_t PawnHandle(int32_t slot) {
	if (!HasPawn(slot) || __s2sdk_EntIndexToEntHandle == nullptr) {
		return 0;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(slot);
		if (pawn == nullptr) {
			return 0;
		}
		int32_t handle = 0;
		if (__s2sdk_EntPointerToEntHandle != nullptr) {
			handle = s2sdk::EntPointerToEntHandle(pawn);
		} else if (__s2sdk_EntPointerToEntIndex != nullptr) {
			handle = s2sdk::EntIndexToEntHandle(s2sdk::EntPointerToEntIndex(pawn));
		}
		if (!adminesp::IsValidHandle(handle)) {
			return 0;
		}
		if (__s2sdk_EntHandleToEntIndex && s2sdk::EntHandleToEntIndex(handle) <= 0) {
			return 0;
		}
		return handle;
	} catch (...) {
		return 0;
	}
}

int32_t ResolveIndex(int32_t handle) {
	if (!adminesp::IsValidHandle(handle) || __s2sdk_EntHandleToEntIndex == nullptr) {
		return -1;
	}
	try {
		return s2sdk::EntHandleToEntIndex(handle);
	} catch (...) {
		return -1;
	}
}

bool ValidCreatedHandle(int32_t handle) {
	return adminesp::IsValidHandle(handle) && (__s2sdk_EntHandleToEntIndex == nullptr || ResolveIndex(handle) > 0);
}

// The controller's team (CBaseEntity::m_iTeamNum, 1 byte). pisexcompat read the field at a cached
// schema offset instead of GetClientTeam because the s2sdk helper goes through the pawn and
// answers 0 - with a console warning - for a client without one (a spectator). The offset is
// resolved only with the map gate open and only a successful lookup is cached (PITFALLS C8).
int PlayerTeam(int32_t slot) {
	const int32_t controller = ControllerHandle(slot);
	if (controller != 0) {
		if (g_teamOffset < 0 && __s2sdk_GetSchemaOffset != nullptr) {
			try {
				const int32_t offset =
					s2sdk::GetSchemaOffset(plg::string(std::string_view("CBaseEntity")),
										   plg::string(std::string_view("m_iTeamNum")));
				if (offset >= 0) {
					g_teamOffset = offset;
				}
			} catch (...) {
			}
		}
		if (g_teamOffset >= 0 && __s2sdk_GetEntData != nullptr) {
			try {
				return static_cast<int>(s2sdk::GetEntData(controller, g_teamOffset, 1)) & 0xFF;
			} catch (...) {
			}
		}
	}
	// No offset/binding: the pawn-based export, asked only when a pawn exists (silent otherwise).
	if (HasPawn(slot) && __s2sdk_GetClientTeam != nullptr) {
		try {
			return static_cast<int>(s2sdk::GetClientTeam(slot));
		} catch (...) {
		}
	}
	return 0;
}

// The original's UpdateViewer comparison: alive means the pawn the controller currently drives,
// i.e. a bot's pawn while a dead admin controls a bot. pisex::IsAlive (s2sdk IsClientAlive)
// answers for the player's own pawn, which is dead then, so m_bControllingBot is read by its
// schema offset (no typed schema call: PITFALLS C6/C8; the field is 1 byte).
bool PlaysAlive(int32_t slot) {
	if (IsAlive(slot)) {
		return true;
	}
	const int32_t controller = ControllerHandle(slot);
	if (controller == 0) {
		return false;
	}
	if (g_controllingBotOffset < 0) {
		if (__s2sdk_GetSchemaOffset == nullptr) {
			return false;
		}
		try {
			const int32_t offset =
				s2sdk::GetSchemaOffset(plg::string(std::string_view("CCSPlayerController")),
									   plg::string(std::string_view("m_bControllingBot")));
			if (offset < 0) {
				return false; // not cached: a later map/update retries
			}
			g_controllingBotOffset = offset;
		} catch (...) {
			return false;
		}
	}
	if (__s2sdk_GetEntData == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetEntData(controller, g_controllingBotOffset, 1) != 0;
	} catch (...) {
		return false;
	}
}

// --- slot identity / observation -----------------------------------------------------------------

// The steam id of the player in the slot; for a bot (steam id 0) the controller handle with a tag
// bit, so a bot slot reused by another bot still gets a different identity.
uint64_t SlotIdentity(int32_t slot) {
	if (!IsConnectedClient(slot)) {
		return 0;
	}
	if (__s2sdk_GetClientSteamID64 != nullptr) {
		try {
			const uint64_t steamid = s2sdk::GetClientSteamID64(slot);
			if (steamid != 0) {
				return steamid;
			}
		} catch (...) {
		}
	}
	const int32_t controller = ControllerHandle(slot);
	if (controller == 0) {
		return 0;
	}
	return kControllerIdentityBit | static_cast<uint64_t>(static_cast<uint32_t>(controller));
}

bool IsRealPlayer(int32_t slot) {
	return CanQueryClient(slot) && IsConnectedClient(slot) && !IsFakeClient(slot);
}

// Drops the access decision of a slot but keeps the identity/connected bookkeeping; used when a
// client leaves, a new identity appears, or the map changes.
void ResetViewerState(int32_t slot) {
	ViewerSlot& viewer = g_viewers[slot];
	viewer.access = adminesp::Access::None;
	viewer.enabled = g_settings.enabledByDefault;
	viewer.canSee = false;
}

// Full reset of a slot (disconnect, unload): the next occupant starts from the config default.
void ResetSlot(int32_t slot) {
	ResetViewerState(slot);
	g_viewers[slot].connected = false;
	g_viewers[slot].identity = 0;
	g_glows[slot] = TargetGlow();
}

// Watches a slot every frame/reconcile: notices disconnects and slot reuse even when the
// OnClientAuthenticated listener and the player_disconnect event are not delivered (PITFALLS B6).
// Returns true while the slot holds a connected client.
bool ObserveSlot(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return false;
	}
	ViewerSlot& viewer = g_viewers[slot];
	const bool connected = IsConnectedClient(slot);
	if (!connected) {
		if (viewer.connected) {
			ResetViewerState(slot);
		}
		viewer.connected = false;
		viewer.identity = 0;
		return false;
	}
	const uint64_t identity = SlotIdentity(slot);
	if (!viewer.connected || viewer.identity != identity) {
		// Another player (or the first observation of this one) owns the slot now: the previous
		// access decision, ESP switch and pending rebuild must not leak into the new occupant.
		ResetViewerState(slot);
		viewer.connected = true;
		viewer.identity = identity;
	}
	return true;
}

// --- per-slot outline state -----------------------------------------------------------------------

bool HasGlow(int32_t target) {
	return adminesp::IsValidHandle(g_glows[target].glow);
}

int CountGlows() {
	int count = 0;
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		if (HasGlow(target)) {
			++count;
		}
	}
	return count;
}

void RetireHandle(int32_t handle) {
	if (!adminesp::IsValidHandle(handle)) {
		return;
	}
	for (int32_t& entry : g_retired) {
		if (entry == 0) {
			entry = handle;
			return;
		}
	}
}

// Forgets retired handles once the engine has freed the entity (the serial number is part of the
// handle, so a reused index never matches).
void PruneRetired() {
	for (int32_t& entry : g_retired) {
		if (entry != 0 && ResolveIndex(entry) <= 0) {
			entry = 0;
		}
	}
}

void ForgetGlows() {
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		g_glows[target] = TargetGlow();
	}
	for (int32_t& entry : g_retired) {
		entry = 0;
	}
	for (ViewerSlot& viewer : g_viewers) {
		viewer.canSee = false;
	}
}

void RemoveOutlineEntity(int32_t handle) {
	// The original let go of the parent first (AcceptEntityInput "FollowEntity" with no argument);
	// s2sdk has no way to clear a parent, and the engine unlinks a deleted entity from its parent
	// itself. Glow first, then the relay it follows.
	if (adminesp::IsValidHandle(handle) && ResolveIndex(handle) > 0 && __s2sdk_RemoveEntity != nullptr) {
		try {
			s2sdk::RemoveEntity(handle);
		} catch (...) {
		}
	}
}

void DestroyGlow(int32_t target) {
	TargetGlow& state = g_glows[target];
	const int32_t glow = state.glow;
	const int32_t relay = state.relay;
	// Removal happens at the end of the frame: keep hiding both entities from everybody until
	// the engine has really freed them.
	RetireHandle(glow);
	RetireHandle(relay);
	const float createAfter = state.createAfter;
	const uint64_t identity = state.identity;
	state = TargetGlow();
	state.createAfter = createAfter;
	state.identity = identity;
	if (adminesp::IsValidHandle(glow)) {
		RemoveOutlineEntity(glow);
	}
	if (adminesp::IsValidHandle(relay)) {
		RemoveOutlineEntity(relay);
	}
}

// Spawn keyvalues built the way the original built them (there: on the entity system's arena,
// here: through DispatchSpawn2).
void SpawnRelay(int32_t relay, const plg::string& model) {
	const plg::vector<plg::string> keys{"model", "spawnflags", "rendermode"};
	const plg::vector<plg::any> values{model, static_cast<int64_t>(kBoneMergeSpawnFlag),
									   static_cast<int32_t>(kRenderNone)};
	s2sdk::DispatchSpawn2(relay, keys, values);
}

void SpawnGlow(int32_t glow, const plg::string& model, const adminesp::Color& color) {
	const plg::vector<plg::string> keys{"model",        "spawnflags", "glowcolor",  "glowrange",
										"glowrangemin", "glowteam",   "glowtype",   "glowstate",
										"renderamt"};
	// glowcolor is a colour keyvalue: the original set it with SetColor. DispatchSpawn2 has no
	// colour value type, and an int is not converted - the glow then fell back to the engine's own
	// colour (every outline one shade, flickering). A string is parsed by the field's type, like
	// the "R G B A" a map sets.
	const std::string colorText = std::to_string(color.r) + " " + std::to_string(color.g) + " " +
								  std::to_string(color.b) + " 255";
	const plg::vector<plg::any> values{model,
									   static_cast<int64_t>(kBoneMergeSpawnFlag),
									   plg::string(colorText),
									   static_cast<int32_t>(g_settings.glowRange),
									   static_cast<int32_t>(g_settings.glowRangeMin),
									   static_cast<int32_t>(-1),
									   static_cast<int32_t>(g_settings.glowType),
									   static_cast<int32_t>(g_settings.glowState),
									   static_cast<int32_t>(1)};
	s2sdk::DispatchSpawn2(glow, keys, values);
}

bool CreateGlow(int32_t target) {
	// Nothing may be created while the transmit control is unavailable: an outline that cannot be
	// hidden would show every player's model to every client.
	if (!g_canTransmit || HasGlow(target) || !CanQueryClient(target)) {
		return false;
	}
	if (!IsConnectedClient(target) || !IsAlive(target)) {
		return false;
	}
	const int team = PlayerTeam(target);
	if (team != adminesp::kTeamT && team != adminesp::kTeamCT) {
		return false;
	}
	const plg::string model = s2sdk::GetClientModel(target);
	if (model.empty()) {
		return false;
	}
	const int32_t pawn = PawnHandle(target);
	if (!adminesp::IsValidHandle(pawn)) {
		return false;
	}
	if (__s2sdk_CreateEntityByName == nullptr) {
		return false;
	}

	int32_t relay = 0;
	int32_t glow = 0;
	try {
		relay = s2sdk::CreateEntityByName(plg::string(std::string_view(kPropDynamic)));
		glow = s2sdk::CreateEntityByName(plg::string(std::string_view(kPropDynamic)));
	} catch (...) {
		relay = 0;
		glow = 0;
	}
	if (!ValidCreatedHandle(relay) || !ValidCreatedHandle(glow)) {
		if (ValidCreatedHandle(relay)) {
			s2sdk::RemoveEntity(relay);
		}
		if (ValidCreatedHandle(glow)) {
			s2sdk::RemoveEntity(glow);
		}
		LogError(Format("could not create the outline entities for slot %d", target));
		return false;
	}

	// Same order as the original: glow, relay, then the parenting. The handles are stored before
	// the spawn calls so a throwing boundary cannot leave an untracked outline visible: any
	// failure from here on goes through DestroyGlow, which retires both handles (the transmit
	// callback keeps hiding them until the engine frees them).
	TargetGlow& state = g_glows[target];
	state.relay = relay;
	state.glow = glow;
	state.team = team;
	state.identity = g_viewers[target].identity != 0 ? g_viewers[target].identity : SlotIdentity(target);
	try {
		SpawnGlow(glow, model, team == adminesp::kTeamT ? g_settings.colorT : g_settings.colorCT);
		SpawnRelay(relay, model);
		s2sdk::FollowEntity(relay, pawn, true); // bone-merge onto the pawn
		s2sdk::FollowEntity(glow, relay, true); // the glowing copy follows the relay
	} catch (...) {
		DestroyGlow(target);
		return false;
	}
	state.model.assign(model.data(), model.size());
	// From here on the transmit callback hides both entities from everybody who may not see them.
	return true;
}

// --- client view (mp_forcecamera / mp_playerid, viewer only) ------------------------------------

void SyncClientView(int32_t slot, bool canSee) {
	if (!CanQueryClient(slot) || !IsRealPlayer(slot)) {
		return;
	}
	if (__s2sdk_SendConVarValue == nullptr || __s2sdk_FindConVar == nullptr) {
		return;
	}
	if (!g_conVarForceCamera) {
		try {
			g_conVarForceCamera = s2sdk::FindConVar(plg::string(std::string_view("mp_forcecamera")));
		} catch (...) {
		}
	}
	if (!g_conVarPlayerId) {
		try {
			g_conVarPlayerId = s2sdk::FindConVar(plg::string(std::string_view("mp_playerid")));
		} catch (...) {
		}
	}
	const uint64_t handles[] = {g_conVarForceCamera, g_conVarPlayerId};
	for (const uint64_t handle : handles) {
		if (!handle) {
			continue;
		}
		std::string value = "0";
		if (!canSee) {
			if (__s2sdk_GetConVarString == nullptr) {
				continue;
			}
			try {
				value = ToStd(s2sdk::GetConVarString(handle));
			} catch (...) {
				continue;
			}
		}
		try {
			s2sdk::SendConVarValue(slot, handle, plg::string(std::string_view(value)));
		} catch (...) {
		}
	}
	LogInfo(Format("slot %d client view: %s", slot,
				   canSee ? "every player on the radar and names" : "server values back"));
}

// resync: also resend while the view stays on, after events that can follow a map change (the
// client then has the server's values again).
void UpdateViewer(int32_t slot, bool resync = false) {
	if (!CanQueryClient(slot)) {
		return;
	}
	ViewerSlot& viewer = g_viewers[slot];
	bool canSee = false;
	// The original's EngineReady() + controller->IsConnected(): a slot without a live controller
	// never sees outlines, even if a stale access bit says otherwise.
	if (g_canTransmit && viewer.access != adminesp::Access::None && viewer.enabled && HasController(slot)) {
		canSee = adminesp::CanSeeNow(viewer.access, PlaysAlive(slot), PlayerTeam(slot));
	}
	const bool before = viewer.canSee;
	viewer.canSee = canSee;
	if (before != canSee || (resync && canSee)) {
		SyncClientView(slot, canSee);
	}
}

void RefreshAccess(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	adminesp::Access access = adminesp::Access::None;
	// Never reveal the outlines to a non-authorised client: the two permission checks are the
	// only source of Access::Always/DeadOnly, and an empty flag grants nothing (HasPermission is
	// never asked with "").
	if (g_canTransmit && g_pAdminApi != nullptr && CanQueryClient(slot) && g_viewers[slot].connected &&
		IsRealPlayer(slot)) {
		const bool always =
			!g_settings.flagAlways.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagAlways.c_str());
		const bool deadOnly =
			!g_settings.flagDeadOnly.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagDeadOnly.c_str());
		access = adminesp::PickAccess(always, deadOnly);
	}
	g_viewers[slot].access = access;
	UpdateViewer(slot);
}

bool AnyViewer() {
	for (const ViewerSlot& viewer : g_viewers) {
		if (viewer.access != adminesp::Access::None && viewer.enabled) {
			return true;
		}
	}
	return false;
}

// --- reconcile ----------------------------------------------------------------------------------

void Reconcile() {
	if (!g_mapReady || !g_canTransmit) {
		return;
	}
	PruneRetired();
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		ObserveSlot(slot);
	}
	if (++g_reconcileRuns >= kAccessRefreshEvery) {
		g_reconcileRuns = 0;
		for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
			RefreshAccess(slot);
		}
	} else {
		for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
			UpdateViewer(slot);
		}
	}

	const bool anyViewer = AnyViewer();
	const float now = Now();
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		ViewerSlot& viewer = g_viewers[target];
		TargetGlow& state = g_glows[target];
		if (!viewer.connected) {
			if (HasGlow(target)) {
				DestroyGlow(target);
			}
			// Drop the identity/pending rebuild of the player who left: the next occupant starts
			// with a clean slot even when the disconnect event was missed.
			if (state.identity != 0 || state.createAfter != 0.0f) {
				g_glows[target] = TargetGlow();
			}
			continue;
		}
		// A pending rebuild (spawn/round start) belongs to one identity. A different player in
		// the slot must not wait for it, and the glow built for the previous occupant is stale.
		if (state.identity != viewer.identity) {
			DestroyGlow(target);
			g_glows[target] = TargetGlow();
			g_glows[target].identity = viewer.identity;
		}
		const bool alive = IsAlive(target) && PlayerTeam(target) != 0;
		if (!anyViewer || !alive) {
			if (HasGlow(target)) {
				DestroyGlow(target);
			}
			continue;
		}
		if (now < g_glows[target].createAfter) {
			continue;
		}
		if (HasGlow(target)) {
			const plg::string model = s2sdk::GetClientModel(target);
			const std::string modelString(model.data(), model.size());
			const bool glowGone = ResolveIndex(g_glows[target].glow) <= 0;
			const bool relayGone = ResolveIndex(g_glows[target].relay) <= 0;
			const int teamNow = PlayerTeam(target);
			const bool stale =
				glowGone || relayGone || g_glows[target].team != teamNow || model.empty() || g_glows[target].model != modelString;
			if (!stale) {
				continue;
			}
			DestroyGlow(target);
		}
		CreateGlow(target);
	}
}

// --- toggle / diagnostics -----------------------------------------------------------------------

void Toggle(int32_t slot) {
	ViewerSlot& viewer = g_viewers[slot];
	viewer.enabled = !viewer.enabled;
	UpdateViewer(slot);
	PrintToChat(slot, Phrase(viewer.enabled ? "esp_enabled" : "esp_disabled"));
	LogInfo(Format("slot %d turned ESP %s (access: %s, sees outlines right now: %s, outlines on the map: %d)",
				   slot, viewer.enabled ? "on" : "off", adminesp::AccessName(viewer.access),
				   viewer.canSee ? "yes" : "no", CountGlows()));
}

// A refusal used to be silent, which looked like a broken plugin. Say so in chat and log what
// admin_system reported.
void ReportRefused(int32_t slot) {
	PrintToChat(slot, Phrase("esp_no_access"));
	const auto yes = [](bool value) { return value ? "yes" : "no"; };
	if (g_pAdminApi == nullptr) {
		LogInfo(Format("slot %d was refused ESP: admin_system is unavailable", slot));
		return;
	}
	LogInfo(Format("slot %d was refused ESP: connected=%s bot=%s admin=%s flag_z=%s flag_a=%s %s=%s %s=%s", slot,
				   yes(IsConnectedClient(slot)), yes(IsFakeClient(slot)), yes(g_pAdminApi->IsAdmin(slot)),
				   yes(g_pAdminApi->HasFlag(slot, "z")), yes(g_pAdminApi->HasFlag(slot, "a")),
				   g_settings.flagAlways.c_str(),
				   yes(!g_settings.flagAlways.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagAlways.c_str())),
				   g_settings.flagDeadOnly.c_str(),
				   yes(!g_settings.flagDeadOnly.empty() &&
					   g_pAdminApi->HasPermission(slot, g_settings.flagDeadOnly.c_str()))));
}

// Chat output: pisex::PrintToChat(slot, "%s", phrase) went through the cs2-menus colour table; the
// local Colorize() produces exactly the same bytes.
void PrintToChat(int32_t slot, std::string_view text) {
	if (!CanQueryClient(slot) || !IsConnectedClient(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(adminesp::Colorize(text)));
	} catch (...) {
	}
}

// --- commands / menu ----------------------------------------------------------------------------

bool HandleEspCommand(int32_t slot) {
	if (!ValidSlot(slot)) {
		return false; // server console (-1): not handled, like the original's false
	}
	if (!CanQueryClient(slot)) {
		// A valid slot without a map: the original's RefreshAccess found no access and the chat
		// printer was a no-op, but the command still answered true. Nothing is toggled here.
		return true;
	}
	ObserveSlot(slot);
	RefreshAccess(slot);
	if (g_viewers[slot].access == adminesp::Access::None) {
		ReportRefused(slot);
		return true;
	}
	Toggle(slot);
	return true;
}

void OnAdminCoreLoaded() {
	if (g_pAdminApi == nullptr || g_settings.menuCategory.empty()) {
		return;
	}
	const std::string& flag = !g_settings.flagDeadOnly.empty() ? g_settings.flagDeadOnly : g_settings.flagAlways;
	if (flag.empty()) {
		return;
	}
	if (g_settings.menuCategory == "players") {
		g_pAdminApi->RegisterCategory("players", "Category_Players", nullptr);
	}
	// admin_api.hpp copies the strings into the generated plg::string boundary (and the core copies
	// them again), so the original's strdup() workaround for the Pisex core is not needed here.
	g_pAdminApi->RegisterItem(
		"adminesp", "Item_AdminESP", g_settings.menuCategory.c_str(), flag.c_str(),
		[](int32_t slot, const char*, const char*, std::string& item) {
			if (ValidSlot(slot)) {
				item = Phrase(g_viewers[slot].enabled ? "esp_menu_on" : "esp_menu_off");
			}
		},
		[](int32_t slot, const char* category, const char*, const char*) {
			if (!ValidSlot(slot) || !CanQueryClient(slot)) {
				return;
			}
			ObserveSlot(slot);
			RefreshAccess(slot);
			if (g_viewers[slot].access == adminesp::Access::None) {
				ReportRefused(slot);
				return;
			}
			Toggle(slot);
			if (g_pAdminApi != nullptr && category != nullptr) {
				g_pAdminApi->ShowAdminCategoryMenu(slot, category);
			}
		});
}

// --- events -------------------------------------------------------------------------------------

void HandlePlayerSpawn(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return;
	}
	ObserveSlot(slot);
	// A respawned model may be different; rebuild the outline after the delay, for this identity.
	DestroyGlow(slot);
	TargetGlow& state = g_glows[slot];
	state.identity = g_viewers[slot].identity;
	state.createAfter = Now() + kSpawnDelaySeconds;
	// A dead-only viewer loses the view on respawn.
	RefreshAccess(slot);
	if (g_viewers[slot].canSee) {
		SyncClientView(slot, true);
	}
}

void HandlePlayerDeath(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return;
	}
	// Otherwise the outline would stay on the corpse.
	DestroyGlow(slot);
	UpdateViewer(slot, true);
}

void HandlePlayerTeam(int32_t slot) {
	if (!CanQueryClient(slot)) {
		return;
	}
	// Recreated in the new team's colour by the reconcile timer.
	DestroyGlow(slot);
	UpdateViewer(slot, true);
}

void HandlePlayerDisconnect(int32_t slot) {
	if (!ValidSlot(slot)) {
		return;
	}
	if (g_mapReady) {
		DestroyGlow(slot);
	}
	ResetSlot(slot);
}

void HandleRoundPrestart() {
	if (!g_mapReady) {
		return;
	}
	// The pawns are replaced now; the spawn events bring the outlines back. Each pending rebuild
	// is tagged with the current slot identity so a player joining during the delay is not held
	// back by the previous occupant's timer.
	const float resume = Now() + kRoundStartDelaySeconds;
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		ObserveSlot(target);
		DestroyGlow(target);
		TargetGlow& state = g_glows[target];
		state.identity = g_viewers[target].identity;
		state.createAfter = resume;
	}
}

void HandleClientAuthenticated(int32_t slot, uint64_t steamid) {
	if (!ValidSlot(slot)) {
		return;
	}
	// A slot now holds a new player: the original's OnClientAuthorized reset enabled/access. The
	// live observation below is authoritative (PITFALLS B6: this callback is not delivered for
	// every client), but doing it here makes the reset immediate.
	ResetViewerState(slot);
	g_viewers[slot].connected = false;
	g_viewers[slot].identity = steamid;
	if (g_mapReady) {
		ObserveSlot(slot);
		RefreshAccess(slot);
	}
}

// --- transmit -----------------------------------------------------------------------------------

// Clears the transmit bit, marks the entity as "exists but not transmitted" and drops it from the
// engine's always-list. Without the second bit the client's reconcile pass deletes the entity
// while the server still counts it as held, and the next delta for it kills the client
// ("CopyExistingEntity: missing client entity"). Same fix as CS2Fixes 2a7db489; s2sdk's own
// TransmitManager::OnCheckTransmit does both.
void HideTransmitEntity(void* info, int32_t handle) {
	s2sdk::ClearTransmitInfoEntity(info, handle);
	s2sdk::SetTransmitInfoNonPlayer(info, handle);
	if (__s2sdk_ClearTransmitInfoAlways != nullptr) {
		try {
			s2sdk::ClearTransmitInfoAlways(info, handle);
		} catch (...) {
		}
	}
}

// Called for every CheckTransmit pass (s2sdk hooks ISource2GameEntities::CheckTransmit); clears
// only, for entities whose handle still resolves to ours.
//
// The engine calls this once per tick with every client's info, so the pass starts with a pure
// local scan of the handle arrays: while no outline exists (no ESP admin has switched on) not a
// single s2sdk call is made. Every handle that is passed to ClearTransmitInfoEntity is resolved
// first - the export logs "Cannot execute ... on invalid entity handle" for one that does not
// resolve - and a cleared handle is re-resolved only once per pass, not once per client.
// A recipient is a viewer only while the canSee decision still belongs to the player who is in
// the slot *now*. The reconcile refreshes canSee at most four times a second, so a disconnected
// admin's slot could be reused by another player in that window; re-checking the identity here
// (only for the very few slots whose canSee is true) keeps the outlines hidden from the new
// occupant until the next reconcile has recomputed his own access.
bool IsCurrentViewer(int32_t slot) {
	if (!ValidSlot(slot) || !g_viewers[slot].canSee) {
		return false;
	}
	if (!CanQueryClient(slot) || !IsConnectedClient(slot)) {
		return false;
	}
	const uint64_t identity = SlotIdentity(slot);
	return identity != 0 && identity == g_viewers[slot].identity;
}

void OnCheckTransmit(const plg::vector<void*>& infos) {
	if (!g_canTransmit || !g_mapReady) {
		return; // no map: the stale client/entity lists must not be touched (PITFALLS B2)
	}

	bool any = false;
	for (int32_t target = 0; target < kMaxSlots && !any; ++target) {
		any = g_glows[target].relay != 0 || g_glows[target].glow != 0;
	}
	for (int32_t r = 0; r < kRetiredSlots && !any; ++r) {
		any = g_retired[r] != 0;
	}
	if (!any) {
		return;
	}

	int32_t hideRelay[kMaxSlots];
	int32_t hideGlow[kMaxSlots];
	// The live handles again as one compact list: a non-viewer pass iterates this instead of
	// walking 64 slots x 2 arrays (every clear is an entity-system lookup + bitvec clear, and
	// this callback runs once per client per tick).
	int32_t hideAll[2 * kMaxSlots];
	int32_t hideAllCount = 0;
	int32_t retired[kRetiredSlots];
	int32_t retiredCount = 0;
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		hideRelay[target] = ResolveIndex(g_glows[target].relay);
		hideGlow[target] = ResolveIndex(g_glows[target].glow);
		if (hideRelay[target] > 0) {
			hideAll[hideAllCount++] = g_glows[target].relay;
		}
		if (hideGlow[target] > 0) {
			hideAll[hideAllCount++] = g_glows[target].glow;
		}
	}
	for (const int32_t handle : g_retired) {
		// an empty slot is skipped without the export call (the array is 256 entries deep)
		if (handle != 0 && ResolveIndex(handle) > 0) {
			retired[retiredCount++] = handle;
		}
	}
	if (hideAllCount == 0 && retiredCount == 0) {
		return; // stale handles only: nothing to clear
	}

	for (void* info : infos) {
		if (info == nullptr) {
			continue;
		}
		int32_t slot = -1;
		if (__s2sdk_GetTransmitInfoPlayerSlot != nullptr) {
			try {
				slot = s2sdk::GetTransmitInfoPlayerSlot(info);
			} catch (...) {
				slot = -1;
			}
		}
		// Anyone who is not an admin with ESP on (SourceTV included) gets no outline.
		const bool viewer = IsCurrentViewer(slot);
		if (viewer) {
			// The engine decides what a viewer gets; a viewer never sees his own outline, so the
			// whole pass for him is two clears at most.
			if (hideRelay[slot] > 0) {
				HideTransmitEntity(info, g_glows[slot].relay);
			}
			if (hideGlow[slot] > 0) {
				HideTransmitEntity(info, g_glows[slot].glow);
			}
		} else {
			for (int32_t i = 0; i < hideAllCount; ++i) {
				HideTransmitEntity(info, hideAll[i]);
			}
		}
		for (int32_t r = 0; r < retiredCount; ++r) {
			HideTransmitEntity(info, retired[r]);
		}
	}
}

// --- registrations ------------------------------------------------------------------------------

struct CommandRegistration {
	const char* name = nullptr;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	bool attempted = false;
	bool owned = false;
};

CommandRegistration g_commands[] = {{kMmEspCommand, nullptr, kCommandFlags}, {kEspCommand, nullptr, kCommandFlags}};

s2sdk::ResultType OnMmEspCommand(int32_t caller, s2sdk::ConCommandContext context,
								 const plg::vector<plg::string>& /*args*/);
s2sdk::ResultType OnEspCommand(int32_t caller, s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& /*args*/);

struct EventRegistration {
	const char* name = nullptr;
	s2sdk::EventCallback callback = nullptr;
	bool hooked = false;
};

s2sdk::ResultType OnPlayerSpawnEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnPlayerDeathEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnPlayerTeamEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnRoundPrestartEvent(const plg::string& name, void* event, bool dontBroadcast);

EventRegistration g_events[] = {{"player_spawn", &OnPlayerSpawnEvent, false},
								{"player_death", &OnPlayerDeathEvent, false},
								{"player_team", &OnPlayerTeamEvent, false},
								{"player_disconnect", &OnPlayerDisconnectEvent, false},
								{"round_prestart", &OnRoundPrestartEvent, false}};

bool g_clientAuthRegistered = false;

void AttachCommand(CommandRegistration& command) {
	if (command.attempted || command.callback == nullptr) {
		return;
	}
	if (__s2sdk_AddConsoleCommand == nullptr) {
		LogError(std::string("s2sdk AddConsoleCommand is unavailable - '") + command.name + "' is not registered");
		return;
	}
	try {
		command.owned = s2sdk::AddConsoleCommand(plg::string(std::string_view(command.name)), plg::string(), command.flags,
												 command.callback, s2sdk::HookMode::Pre);
		command.attempted = true;
	} catch (...) {
		LogError(std::string("AddConsoleCommand failed for '") + command.name + "'");
		return;
	}
	if (!command.owned) {
		// false = the name already existed (engine or another plugin) and s2sdk attached our
		// callback as a listener: the command keeps working through it, and OnPluginEnd must
		// detach only that listener (PITFALLS C11).
		LogInfo(std::string("'") + command.name + "' already exists; reusing the entry (the listener is attached)");
	}
}

void DetachCommand(CommandRegistration& command) {
	if (!command.attempted) {
		return;
	}
	command.attempted = false;
	if (command.owned) {
		// This plugin created the command: RemoveCommand detaches the listener and the command.
		if (__s2sdk_RemoveCommand != nullptr) {
			try {
				s2sdk::RemoveCommand(plg::string(std::string_view(command.name)), command.callback);
			} catch (...) {
			}
		}
	} else if (__s2sdk_RemoveCommandListener != nullptr) {
		// The name already existed: only the listener is ours. RemoveCommand would unregister
		// somebody else's command (PITFALLS C11).
		try {
			s2sdk::RemoveCommandListener(plg::string(std::string_view(command.name)), command.callback,
										 s2sdk::HookMode::Pre);
		} catch (...) {
		}
	}
	command.owned = false;
}

void RegisterCommands() {
	g_commands[0].callback = &OnMmEspCommand;
	g_commands[1].callback = &OnEspCommand;
	for (CommandRegistration& command : g_commands) {
		AttachCommand(command);
	}
}

void RemoveCommands() {
	for (CommandRegistration& command : g_commands) {
		DetachCommand(command);
	}
}

void RegisterEvents() {
	if (__s2sdk_HookEvent == nullptr) {
		LogError("the s2sdk event bindings are unavailable - player/round events are not hooked");
		return;
	}
	for (EventRegistration& event : g_events) {
		if (event.callback == nullptr) {
			continue;
		}
		try {
			event.hooked = s2sdk::HookEvent(plg::string(std::string_view(event.name)), event.callback,
											s2sdk::HookMode::Pre) == s2sdk::EventHookError::Okay;
		} catch (...) {
			event.hooked = false;
		}
		if (!event.hooked) {
			LogError(std::string("hooking '") + event.name + "' failed - that event is disabled");
		}
	}
}

void RemoveEvents() {
	for (EventRegistration& event : g_events) {
		if (!event.hooked) {
			continue;
		}
		event.hooked = false;
		if (__s2sdk_UnhookEvent != nullptr) {
			try {
				s2sdk::UnhookEvent(plg::string(std::string_view(event.name)), event.callback,
								   s2sdk::HookMode::Pre);
			} catch (...) {
			}
		}
	}
}

void RegisterClientAuth() {
	if (__s2sdk_OnClientAuthenticated_Register == nullptr) {
		LogError("the s2sdk OnClientAuthenticated binding is unavailable - a reused slot is only reset by the reconcile");
		return;
	}
	try {
		s2sdk::OnClientAuthenticated_Register(&HandleClientAuthenticated);
		g_clientAuthRegistered = true;
	} catch (...) {
		LogError("could not register the s2sdk OnClientAuthenticated listener");
	}
}

void UnregisterClientAuth() {
	if (!g_clientAuthRegistered) {
		return;
	}
	g_clientAuthRegistered = false;
	if (__s2sdk_OnClientAuthenticated_Unregister != nullptr) {
		try {
			s2sdk::OnClientAuthenticated_Unregister(&HandleClientAuthenticated);
		} catch (...) {
		}
	}
}

// --- lifecycle ----------------------------------------------------------------------------------

void OnMapStartGame() {
	g_mapReady = true;
	g_mapEndedSinceStart = false;
	g_fallbackTime = 0.0f;
	g_reconcileRuns = 0;
	g_sinceReconcileMs = 0;
	// The previous map took its entities with it.
	ForgetGlows();
	for (ViewerSlot& viewer : g_viewers) {
		viewer.access = adminesp::Access::None;
		viewer.canSee = false;
		if (!viewer.connected) {
			viewer.enabled = g_settings.enabledByDefault;
		}
	}
	LoadConfig();
	LoadPhrases();
	LogInfo(Format("startup: flags '%s' / '%s', colours #%02X%02X%02X / #%02X%02X%02X", g_settings.flagAlways.c_str(),
				   g_settings.flagDeadOnly.c_str(), g_settings.colorCT.r, g_settings.colorCT.g, g_settings.colorCT.b,
				   g_settings.colorT.r, g_settings.colorT.g, g_settings.colorT.b));
}

void OnMapEndGame() {
	// PITFALLS B1/B2: close the gate before any state cleanup, and remember that an OnMapEnd was
	// seen so the first-update latch of a plugin loaded during a changelevel cannot re-open it.
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	// No client/entity query here (PITFALLS B2): only the plugin state. The next map's first
	// observation re-establishes identities; until then no client sees an outline.
	ForgetGlows();
	for (ViewerSlot& viewer : g_viewers) {
		viewer.access = adminesp::Access::None;
		viewer.canSee = false;
	}
	g_reconcileRuns = 0;
	g_sinceReconcileMs = 0;
	g_fallbackTime = 0.0f;
}

void ResetAllState() {
	g_mapReady = false;
	g_mapEndedSinceStart = true;
	g_firstUpdateDone = false;
	g_mapListeners = false;
	g_canTransmit = false;
	g_transmitRegistered = false;
	g_clientAuthRegistered = false;
	g_reconcileRuns = 0;
	g_sinceReconcileMs = 0;
	g_fallbackTime = 0.0f;
	g_phrasesLanguage.clear();
	g_phrasesLanguageDetected = false;
	g_teamOffset = -1;
	g_controllingBotOffset = -1;
	g_conVarForceCamera = 0;
	g_conVarPlayerId = 0;
	for (TargetGlow& state : g_glows) {
		state = TargetGlow();
	}
	for (int32_t& entry : g_retired) {
		entry = 0;
	}
	for (ViewerSlot& viewer : g_viewers) {
		viewer = ViewerSlot{};
	}
	for (CommandRegistration& command : g_commands) {
		command.attempted = false;
		command.owned = false;
	}
	for (EventRegistration& event : g_events) {
		event.hooked = false;
	}
}

void RegisterMapListeners() {
	if (g_mapListeners) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		LogError("the s2sdk map listeners are unavailable - client calls stay disabled");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartGame);
		s2sdk::OnMapEnd_Register(&OnMapEndGame);
		g_mapListeners = true;
	} catch (...) {
		LogError("could not register the s2sdk map listeners - client calls stay disabled");
	}
}

void UnregisterMapListeners() {
	if (!g_mapListeners) {
		return;
	}
	g_mapListeners = false;
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartGame);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndGame);
		} catch (...) {
		}
	}
}

// Every s2sdk function the protected-ESP path calls has to be present; a host without the s2sdk
// plugin (or an older one) keeps running without ESP instead of creating an entity that cannot be
// hidden. This is fail-closed for the three transmit bitvecs: ClearTransmitInfoEntity clears
// m_pTransmitEntity, SetTransmitInfoNonPlayer sets the "exists but not transmitted" bit, and
// ClearTransmitInfoAlways clears the separate m_pTransmitAlways list (s2sdk transmit.cpp); without
// the last one an outline the engine put on its always-list could reach every client, so a host
// that does not export it gets no ESP at all (one error log in OnPluginStart, no entity created).
// Optional helpers (IsValidEntHandle/IsValidEntPointer/EntPointerToEntHandle, GetGameTime,
// convar/print bindings) are checked where they are used.
bool CoreBindingsReady() {
	return __s2sdk_CreateEntityByName != nullptr && __s2sdk_DispatchSpawn2 != nullptr &&
		   __s2sdk_RemoveEntity != nullptr && __s2sdk_FollowEntity != nullptr &&
		   __s2sdk_GetClientModel != nullptr && __s2sdk_EntHandleToEntIndex != nullptr &&
		   __s2sdk_EntIndexToEntHandle != nullptr && __s2sdk_EntPointerToEntIndex != nullptr &&
		   __s2sdk_GetClientPawn != nullptr && __s2sdk_GetSchemaOffset != nullptr &&
		   __s2sdk_GetEntData != nullptr && __s2sdk_IsClientConnected != nullptr &&
		   __s2sdk_IsClientInGame != nullptr && __s2sdk_IsClientSourceTV != nullptr &&
		   __s2sdk_IsFakeClient != nullptr && __s2sdk_IsClientAlive != nullptr &&
		   __s2sdk_PlayerSlotToEntPointer != nullptr && __s2sdk_PlayerSlotToEntHandle != nullptr &&
		   __s2sdk_OnServerCheckTransmit_Register != nullptr &&
		   __s2sdk_OnServerCheckTransmit_Unregister != nullptr &&
		   __s2sdk_GetTransmitInfoPlayerSlot != nullptr && __s2sdk_ClearTransmitInfoEntity != nullptr &&
		   __s2sdk_SetTransmitInfoNonPlayer != nullptr && __s2sdk_ClearTransmitInfoAlways != nullptr;
}

// --- the event trampolines (plain function pointers, called on the game thread) -----------------

s2sdk::ResultType OnPlayerSpawnEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (!g_mapReady || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The raw event "userid" is not a slot in CS2; GetEventPlayerSlot resolves it.
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string(std::string_view("userid")));
		if (ValidSlot(slot)) {
			HandlePlayerSpawn(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_spawn failed: ") + ex.what());
	} catch (...) {
		LogError("player_spawn failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDeathEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (!g_mapReady || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string(std::string_view("userid")));
		if (ValidSlot(slot)) {
			HandlePlayerDeath(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_death failed: ") + ex.what());
	} catch (...) {
		LogError("player_death failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerTeamEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (!g_mapReady || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string(std::string_view("userid")));
		if (ValidSlot(slot)) {
			HandlePlayerTeam(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_team failed: ") + ex.what());
	} catch (...) {
		LogError("player_team failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDisconnectEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		if (event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// Deliberately not gated: a disconnect must drop the slot state even in the map-change
		// window (B2: only state, no client/entity call happens here).
		const int32_t slot = s2sdk::GetEventPlayerSlot(event, plg::string(std::string_view("userid")));
		if (ValidSlot(slot)) {
			HandlePlayerDisconnect(slot);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("player_disconnect failed: ") + ex.what());
	} catch (...) {
		LogError("player_disconnect failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundPrestartEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		if (g_mapReady) {
			HandleRoundPrestart();
		}
	} catch (const std::exception& ex) {
		LogError(std::string("round_prestart failed: ") + ex.what());
	} catch (...) {
		LogError("round_prestart failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// "mm_esp": the console spelling of the original. Only the console context is answered - a chat
// "!mm_esp" is not one of the original's chat spellings, so it returns Continue and stays an
// ordinary say line (PITFALLS C11). The server console (-1) is not a player slot, so it returns
// Continue exactly like the original's callback returned false.
s2sdk::ResultType OnMmEspCommand(int32_t caller, s2sdk::ConCommandContext context,
								 const plg::vector<plg::string>& /*args*/) {
	if (context != s2sdk::ConCommandContext::Console) {
		return s2sdk::ResultType::Continue;
	}
	try {
		if (HandleEspCommand(caller)) {
			return s2sdk::ResultType::Handled;
		}
	} catch (const std::exception& ex) {
		LogError(std::string("'mm_esp' failed: ") + ex.what());
	} catch (...) {
		LogError("'mm_esp' failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// "esp": the chat spelling of the original ("!esp"/"/esp" chain to the trigger-less name, and
// s2sdk marks the call ConCommandContext::Chat). A console "esp" is not ours: it answers Continue
// and behaves exactly as if this plugin were not loaded (PITFALLS C11) - the original's console
// spelling is "mm_esp". The chat line is ours whatever the handler did, so it is never broadcast.
s2sdk::ResultType OnEspCommand(int32_t caller, s2sdk::ConCommandContext context,
							   const plg::vector<plg::string>& /*args*/) {
	if (context != s2sdk::ConCommandContext::Chat) {
		return s2sdk::ResultType::Continue;
	}
	try {
		HandleEspCommand(caller);
	} catch (const std::exception& ex) {
		LogError(std::string("'!esp' failed: ") + ex.what());
	} catch (...) {
		LogError("'!esp' failed with an unknown exception");
	}
	return s2sdk::ResultType::Handled;
}

// --- the plugin ---------------------------------------------------------------------------------

class AdminEspPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;
};

AdminEspPlugin g_adminEspPlugin;

plg::PluginResult AdminEspPlugin::OnPluginStart() {
	try {
		ResetAllState();
		// No OnMapEnd has been seen since this start: a plugin loaded into a running map must be
		// allowed to latch readiness on its first OnPluginUpdate (PITFALLS B1).
		g_mapEndedSinceStart = false;
		g_pAdminApi = &admin_system::AdminApi::Get();
		LoadConfig();
		LoadPhrases();
		for (ViewerSlot& viewer : g_viewers) {
			viewer.enabled = g_settings.enabledByDefault;
		}

		// PITFALLS B1: registering the listeners touches no client, so it is safe here. Without
		// them nothing could ever open the gate again, so a missing binding keeps it shut.
		RegisterMapListeners();

		g_canTransmit = CoreBindingsReady();
		if (g_canTransmit) {
			try {
				s2sdk::OnServerCheckTransmit_Register(&OnCheckTransmit);
				g_transmitRegistered = true;
			} catch (...) {
				g_transmitRegistered = false;
			}
		}
		if (!g_canTransmit || !g_transmitRegistered) {
			g_canTransmit = false;
			LogError("s2sdk transmit bindings are missing: AdminESP is disabled (no outline is created)");
		}

		RegisterCommands();
		RegisterEvents();
		RegisterClientAuth();

		if (g_pAdminApi != nullptr) {
			g_pAdminApi->OnAdminConnected([](int32_t slot) {
				if (ValidSlot(slot) && g_mapReady) {
					ObserveSlot(slot);
					RefreshAccess(slot);
				}
			});
			g_pAdminApi->OnCoreLoaded(OnAdminCoreLoaded);
			if (g_pAdminApi->IsCoreLoaded()) {
				OnAdminCoreLoaded(); // late load: the event will not fire twice
			}
		}
		LogInfo(Format("started (transmit control: %s)", g_canTransmit ? "yes" : "no"));
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed (PITFALLS A1: that stops every plugify plugin)
}

plg::PluginResult AdminEspPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first
		// update is the "a map is up" signal. After an OnMapEnd only a new OnMapStart re-opens the
		// gate: the host keeps pumping updates through a changelevel.
		if (!g_firstUpdateDone) {
			g_firstUpdateDone = true;
			if (g_mapListeners && !g_mapEndedSinceStart) {
				g_mapReady = true;
			}
		}
		if (!g_mapReady) {
			return {};
		}
		const int64_t elapsed = dt.count() > 0 ? static_cast<int64_t>(dt.count()) : 0;
		g_fallbackTime += static_cast<float>(elapsed) / 1000.0f;
		g_sinceReconcileMs += static_cast<int32_t>(std::min<int64_t>(elapsed, 1000));
		if (g_sinceReconcileMs >= kReconcileIntervalMs) {
			g_sinceReconcileMs -= kReconcileIntervalMs;
			if (g_sinceReconcileMs >= kReconcileIntervalMs) {
				g_sinceReconcileMs = 0; // one pass per frame at most, like the original timer
			}
			Reconcile();
		}
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult AdminEspPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7/C12/D.6: every registration of this library is removed again. The
		// CheckTransmit detour belongs to s2sdk; only this plugin's callback is detached, and the
		// shared detour is never unhooked here.
		if (g_transmitRegistered) {
			g_transmitRegistered = false;
			if (__s2sdk_OnServerCheckTransmit_Unregister != nullptr) {
				try {
					s2sdk::OnServerCheckTransmit_Unregister(&OnCheckTransmit);
				} catch (...) {
				}
			}
		}
		g_canTransmit = false;
		RemoveCommands();
		RemoveEvents();
		UnregisterClientAuth();
		UnregisterMapListeners();
		if (g_pAdminApi != nullptr) {
			g_pAdminApi->OnCoreLoadedUnregister();
			g_pAdminApi->OnAdminConnectedUnregister();
		}
		if (g_mapReady) {
			for (int32_t target = 0; target < kMaxSlots; ++target) {
				DestroyGlow(target);
			}
		}
		ResetAllState();
		g_pAdminApi = nullptr;
	} catch (const std::exception& ex) {
		LogError(std::string("OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_adminEspPlugin)
