// AdminESP — Plugify port of the Metamod plugin "AdminESP" (ref/src/AdminESP): admin_system
// admins see player outlines through walls.
//
// Every living player gets two prop_dynamic entities: an invisible relay that bone-merges onto
// the pawn and a glowing copy of the model that follows the relay. They exist only while at least
// one admin has ESP switched on, and ESP is off for everyone until an admin types !esp (or uses
// the admin menu).
//
// Who sees the outlines is decided in the CheckTransmit listener: an admin whose ESP is on and
// whom admin_system allows (admin_flag_all: always, admin_flag_death: only while dead or
// spectating) gets every outline but his own; everybody else gets none.
//
// Porting notes (Metamod -> Plugify):
//   * the KHook/SourceHook ISource2GameEntities::CheckTransmit vtable hook is replaced by the
//     s2sdk listener OnServerCheckTransmit (s2sdk hooks CheckTransmit itself and hands the
//     CCheckTransmitInfo list to plugins; s2sdk also provides GetTransmitInfoPlayerSlot and
//     ClearTransmitInfoEntity, so the hard-coded recipient-slot offset of the Metamod build is
//     gone). Like the original, the callback only ever *clears* bits: it never forces an entity
//     into a snapshot and never touches the always-transmit list;
//   * entity creation uses s2sdk CreateEntityByName + DispatchSpawn2 with the same spawn
//     keyvalues the original built on the entity system's arena (model, spawnflags 256,
//     rendermode 10 for the relay; model, spawnflags 256, glowcolor, glowrange 5000,
//     glowrangemin -1000, glowteam -1, glowtype 2, glowstate 3, renderamt 1 for the glow);
//     FollowEntity(child, parent, true) replaces the "FollowEntity" input (variant "!activator"),
//     RemoveEntity replaces the entity-system removal;
//   * outline entities are tracked by whole CEntityHandle values (s2sdk's entity functions take
//     handles), and the transmit callback resolves them with EntHandleToEntIndex, whose serial
//     check is what keeps a freed and reused index from ever being touched;
//   * the client view sync (mp_forcecamera 0 / mp_playerid 0 sent to the viewer's client only)
//     uses s2sdk FindConVar/GetConVarString/SendConVarValue - the exact operation
//     IPlayersApi::SetConVar performed;
//   * admin checks go through plugins/admin_system/include/admin_api.hpp (HasPermission with the
//     two config flags, the "players" category item, OnAdminConnected/OnCoreLoaded) exactly like
//     the other ported modules;
//   * config and phrases keep the Metamod spellings: <configs>/AdminESP/adminesp.ini and
//     <configs>/AdminESP/translations/AdminESP.phrases.txt.
//
// Needs a live-server check: the outline colours (the engine reads the glowcolor keyvalue; the
// port passes the same packed RGBA value the original's SetColor stored) and the removal of the
// parent/child pair (the original detached before removing - s2sdk has no "clear parent" input,
// so both entities are simply removed, glow first).

#include "adminesp_core.hpp"

#include <admin_api.hpp>

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxSlots = 64;
// Handles of removed entities the engine may not have freed yet (the original's kRetiredSlots).
constexpr int kRetiredSlots = 256;
// The reconcile timer also creates glows for players who just spawned, so it runs often;
// permissions are re-read from admin_system every fourth run.
constexpr float kReconcileInterval = 0.25f;
constexpr int kAccessRefreshEvery = 4;
// Wait this long after a spawn before copying the model, so agent changes applied right after
// spawning are picked up.
constexpr float kSpawnDelaySeconds = 0.3f;
// After a round restart the pawns are replaced; give the spawn events time to arrive.
constexpr float kRoundStartDelaySeconds = 1.5f;
constexpr unsigned int kBoneMergeSpawnFlag = 256;
// hl2sdk's kRenderNone (the relay is never drawn).
constexpr int kRenderNone = 10;
constexpr char kLogTag[] = "AdminESP";
constexpr char kConfigPath[] = "addons/configs/adminesp.ini";
constexpr char kPhrasesPath[] = "addons/translations/AdminESP.phrases.txt";
constexpr char kPropDynamic[] = "prop_dynamic";

admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

adminesp::Settings g_settings;
pisex::PhraseMap g_phrases;

struct TargetGlow {
	int relay = 0; // CEntityHandle of the invisible bone-merge relay
	int glow = 0;  // CEntityHandle of the glowing copy
	std::string model;
	int team = 0;
	float createAfter = 0.0f;
};

TargetGlow g_glows[kMaxSlots];
adminesp::Access g_access[kMaxSlots] = {};
bool g_enabled[kMaxSlots] = {};
// Shared with the transmit callback: whether the slot currently sees the outlines.
bool g_canSee[kMaxSlots] = {};
int g_retired[kRetiredSlots] = {};
int g_reconcileRuns = 0;
pisex::CTimer* g_timer = nullptr;
// False when the s2sdk bindings this plugin needs are missing (no ESP at all then, but the
// plugin keeps running).
bool g_canTransmit = false;
uint64_t g_conVarForceCamera = 0;
uint64_t g_conVarPlayerId = 0;

// --- logs ---------------------------------------------------------------------------------------

void LogError(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogError(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	pisex::ErrorLog("[%s] %s", kLogTag, buffer.data());
}

void LogInfo(const char* format, ...) __attribute__((format(printf, 1, 2)));
void LogInfo(const char* format, ...) {
	std::array<char, 2048> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);
	if (plg::Log) {
		plg::Log(std::string_view(buffer.data()), plg::Severity::Info, plg::source_location::current());
	}
}

// --- small helpers ------------------------------------------------------------------------------

bool ValidSlot(int slot) {
	return slot >= 0 && slot < kMaxSlots;
}

float Now() {
	return pisex::GetCGlobalVars()->curtime;
}

const char* Phrase(const char* key) {
	const auto it = g_phrases.find(key);
	return it != g_phrases.end() ? it->second.c_str() : key;
}

bool IsRealPlayer(int slot) {
	return pisex::IsConnected(slot) && !pisex::IsFakeClient(slot);
}

// Every s2sdk function this feature calls has to be present; a server without the s2sdk plugin
// (or an older one) keeps running without ESP instead of crashing.
bool BindingsReady() {
	return __s2sdk_CreateEntityByName && __s2sdk_DispatchSpawn2 && __s2sdk_RemoveEntity && __s2sdk_FollowEntity &&
		   __s2sdk_GetClientModel && __s2sdk_EntHandleToEntIndex && __s2sdk_EntIndexToEntHandle &&
		   __s2sdk_OnServerCheckTransmit_Register && __s2sdk_OnServerCheckTransmit_Unregister &&
		   __s2sdk_GetTransmitInfoPlayerSlot && __s2sdk_ClearTransmitInfoEntity;
}

// --- config / phrases ---------------------------------------------------------------------------

void LoadConfig() {
	adminesp::Settings settings;
	pisex::KeyValues::AutoDelete kv("Config");
	if (!kv->LoadFromFile(kConfigPath)) {
		// Without the file nobody has a permission flag, so ESP stays off.
		LogError("Failed to load AdminESP config '%s'", kConfigPath);
		g_settings = settings; // both flags empty, colours at their defaults
		return;
	}
	g_settings = adminesp::ParseSettings(*kv);
}

void LoadPhrases() {
	g_phrases.clear();
	if (!pisex::LoadPhrasesFile(kPhrasesPath, g_phrases)) {
		LogError("Failed to load phrases '%s'", kPhrasesPath);
	}
}

// --- client view (mp_forcecamera / mp_playerid, viewer only) ------------------------------------

void SyncClientView(int slot, bool canSee) {
	if (!IsRealPlayer(slot) || !__s2sdk_SendConVarValue || !__s2sdk_FindConVar) {
		return;
	}
	if (!g_conVarForceCamera) {
		g_conVarForceCamera = s2sdk::FindConVar(plg::string(std::string_view("mp_forcecamera")));
	}
	if (!g_conVarPlayerId) {
		g_conVarPlayerId = s2sdk::FindConVar(plg::string(std::string_view("mp_playerid")));
	}
	const uint64_t handles[] = {g_conVarForceCamera, g_conVarPlayerId};
	for (const uint64_t handle : handles) {
		if (!handle) {
			continue;
		}
		std::string value = "0";
		if (!canSee) {
			if (!__s2sdk_GetConVarString) {
				continue;
			}
			const plg::string server = s2sdk::GetConVarString(handle);
			value.assign(server.data(), server.size());
		}
		s2sdk::SendConVarValue(slot, handle, plg::string(std::string_view(value)));
	}
	LogInfo("slot %d client view: %s", slot, canSee ? "every player on the radar and names" : "server values back");
}

// --- per-slot state -----------------------------------------------------------------------------

void DestroyGlow(int target);

// The original asked controller->m_bPawnIsAlive(): alive means the pawn the controller currently
// drives, i.e. a bot's pawn while a dead admin controls a bot. pisex::IsAlive (s2sdk
// IsClientAlive) answers for the player's own pawn, which is dead then, so "dead only" kept the
// outlines on while the admin played the bot (seen live). m_bControllingBot is read by its schema
// offset (no typed schema call: those can crash at map start).
bool PlaysAlive(int slot) {
	if (pisex::IsAlive(slot)) {
		return true;
	}
	static const int controllingBot = pisex::SchemaOffset("CCSPlayerController", "m_bControllingBot");
	if (controllingBot < 0) {
		return false;
	}
	const int controller = pisex::GetControllerEntity(slot);
	const auto* entity = controller > 0 ? static_cast<const std::uint8_t*>(pisex::EntIndexToEntPointer(controller)) : nullptr;
	return entity != nullptr && entity[controllingBot] != 0;
}

// resync: also resend while the view stays on, after events that can follow a map change (the
// client then has the server's values again).
void UpdateViewer(int slot, bool resync = false) {
	bool canSee = false;
	if (pisex::IsGameReady() && g_access[slot] != adminesp::Access::None && g_enabled[slot] && pisex::IsConnected(slot)) {
		canSee = adminesp::CanSeeNow(g_access[slot], PlaysAlive(slot), pisex::GetPlayerTeam(slot));
	}
	const bool before = g_canSee[slot];
	g_canSee[slot] = canSee;
	if (before != canSee || (resync && canSee)) {
		SyncClientView(slot, canSee);
	}
}

void RefreshAccess(int slot) {
	adminesp::Access access = adminesp::Access::None;
	if (g_pAdminApi && IsRealPlayer(slot)) {
		const bool always = !g_settings.flagAlways.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagAlways.c_str());
		const bool deadOnly =
			!g_settings.flagDeadOnly.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagDeadOnly.c_str());
		access = adminesp::PickAccess(always, deadOnly);
	}
	g_access[slot] = access;
	UpdateViewer(slot);
}

bool AnyViewer() {
	for (int slot = 0; slot < kMaxSlots; ++slot) {
		if (g_access[slot] != adminesp::Access::None && g_enabled[slot]) {
			return true;
		}
	}
	return false;
}

bool HasGlow(int target) {
	return adminesp::IsValidHandle(g_glows[target].glow);
}

int CountGlows() {
	int count = 0;
	for (int target = 0; target < kMaxSlots; ++target) {
		if (HasGlow(target)) {
			++count;
		}
	}
	return count;
}

void RetireHandle(int handle) {
	if (!adminesp::IsValidHandle(handle)) {
		return;
	}
	for (int& entry : g_retired) {
		if (entry == 0) {
			entry = handle;
			return;
		}
	}
}

// Forgets retired handles once the engine has freed the entity (the serial number is part of the
// handle, so a reused index never matches).
void PruneRetired() {
	for (int& entry : g_retired) {
		if (entry != 0 && s2sdk::EntHandleToEntIndex(entry) <= 0) {
			entry = 0;
		}
	}
}

void ForgetGlows() {
	for (int target = 0; target < kMaxSlots; ++target) {
		g_glows[target] = TargetGlow();
	}
	for (int& entry : g_retired) {
		entry = 0;
	}
	for (bool& sees : g_canSee) {
		sees = false;
	}
}

void RemoveOutlineEntity(int handle) {
	// The original let go of the parent first (AcceptEntityInput "FollowEntity" with no argument);
	// s2sdk has no way to clear a parent, and the engine unlinks a deleted entity from its parent
	// itself. Glow first, then the relay it follows.
	if (s2sdk::EntHandleToEntIndex(handle) > 0) {
		s2sdk::RemoveEntity(handle);
	}
}

void DestroyGlow(int target) {
	TargetGlow& state = g_glows[target];
	const int glow = state.glow;
	const int relay = state.relay;
	// Removal happens at the end of the frame: keep hiding both entities from everybody until
	// the engine has really freed them.
	RetireHandle(glow);
	RetireHandle(relay);
	const float createAfter = state.createAfter;
	state = TargetGlow();
	state.createAfter = createAfter;
	if (adminesp::IsValidHandle(glow)) {
		RemoveOutlineEntity(glow);
	}
	if (adminesp::IsValidHandle(relay)) {
		RemoveOutlineEntity(relay);
	}
}

// Spawn keyvalues built the way the original built them (there: on the entity system's arena,
// here: through DispatchSpawn2).
void SpawnRelay(int relay, const plg::string& model) {
	const plg::vector<plg::string> keys{"model", "spawnflags", "rendermode"};
	const plg::vector<plg::any> values{model, static_cast<int64_t>(kBoneMergeSpawnFlag), static_cast<int32_t>(kRenderNone)};
	s2sdk::DispatchSpawn2(relay, keys, values);
}

void SpawnGlow(int glow, const plg::string& model, const adminesp::Color& color) {
	const plg::vector<plg::string> keys{"model",        "spawnflags", "glowcolor",  "glowrange",
										"glowrangemin", "glowteam",   "glowtype",   "glowstate",
										"renderamt"};
	// glowcolor is a colour keyvalue: the original set it with SetColor. s2sdk's DispatchSpawn2 has
	// no colour value type, and an int is not converted - the glow then fell back to the engine's
	// own colour (every outline one shade, flickering). A string is parsed by the field's type,
	// like the "R G B A" a map sets.
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

int g_rebuildDiagBudget = 60; // TEMPORARY: caps the [esp-diag] lines

bool CreateGlow(int target) {
	if (HasGlow(target)) {
		return true;
	}
	if (!pisex::IsConnected(target) || !pisex::IsAlive(target)) {
		return false;
	}
	const int team = pisex::GetPlayerTeam(target);
	if (team != adminesp::kTeamT && team != adminesp::kTeamCT) {
		return false;
	}
	const plg::string model = s2sdk::GetClientModel(target);
	if (model.empty()) {
		return false;
	}
	const int pawn = s2sdk::EntIndexToEntHandle(pisex::GetPlayerPawn(target));
	if (!adminesp::IsValidHandle(pawn)) {
		return false;
	}

	const int relay = s2sdk::CreateEntityByName(plg::string(std::string_view(kPropDynamic)));
	const int glow = s2sdk::CreateEntityByName(plg::string(std::string_view(kPropDynamic)));
	if (!adminesp::IsValidHandle(relay) || !adminesp::IsValidHandle(glow)) {
		if (adminesp::IsValidHandle(relay)) {
			s2sdk::RemoveEntity(relay);
		}
		if (adminesp::IsValidHandle(glow)) {
			s2sdk::RemoveEntity(glow);
		}
		LogError("could not create the outline entities for slot %d", target);
		return false;
	}

	// Same order as the original: glow, relay, then the parenting.
	SpawnGlow(glow, model, team == adminesp::kTeamT ? g_settings.colorT : g_settings.colorCT);
	SpawnRelay(relay, model);
	s2sdk::FollowEntity(relay, pawn, true); // bone-merge onto the pawn
	s2sdk::FollowEntity(glow, relay, true); // the glowing copy follows the relay

	TargetGlow& state = g_glows[target];
	state.relay = relay;
	state.glow = glow;
	state.model.assign(model.data(), model.size());
	state.team = team;
	// From here on the transmit callback hides both entities from everybody who may not see them.
	return true;
}

// --- reconcile ----------------------------------------------------------------------------------

float Reconcile() {
	if (!pisex::IsGameReady()) {
		return kReconcileInterval;
	}
	PruneRetired();
	if (++g_reconcileRuns >= kAccessRefreshEvery) {
		g_reconcileRuns = 0;
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			RefreshAccess(slot);
		}
	} else {
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			UpdateViewer(slot);
		}
	}

	const bool anyViewer = AnyViewer();
	const float now = Now();
	for (int target = 0; target < kMaxSlots; ++target) {
		TargetGlow& state = g_glows[target];
		const bool has = HasGlow(target);
		const bool alive = pisex::IsConnected(target) && pisex::IsAlive(target) && pisex::GetPlayerTeam(target) != 0;
		if (!anyViewer || !alive) {
			if (has) {
				DestroyGlow(target);
			}
			continue;
		}
		if (now < state.createAfter) {
			continue;
		}
		if (has) {
			const plg::string model = s2sdk::GetClientModel(target);
			const std::string modelString(model.data(), model.size());
			const bool glowGone = s2sdk::EntHandleToEntIndex(state.glow) <= 0;
			const bool relayGone = s2sdk::EntHandleToEntIndex(state.relay) <= 0;
			const int teamNow = pisex::GetPlayerTeam(target);
			const bool stale = glowGone || relayGone || state.team != teamNow || model.empty() || state.model != modelString;
			if (!stale) {
				continue;
			}
			// TEMPORARY live diagnostics (outline colour flips T/CT on 27035): why the pair is rebuilt.
			if (g_rebuildDiagBudget > 0) {
				--g_rebuildDiagBudget;
				LogInfo("[esp-diag] rebuild slot %d: glowGone=%d relayGone=%d team %d->%d model '%s' -> '%s'", target,
						glowGone ? 1 : 0, relayGone ? 1 : 0, state.team, teamNow, state.model.c_str(), modelString.c_str());
			}
			DestroyGlow(target);
		}
		CreateGlow(target);
	}
	return kReconcileInterval;
}

// --- toggle / diagnostics ------------------------------------------------------------------------

void Toggle(int slot) {
	g_enabled[slot] = !g_enabled[slot];
	UpdateViewer(slot);
	pisex::PrintToChat(slot, "%s", Phrase(g_enabled[slot] ? "esp_enabled" : "esp_disabled"));
	LogInfo("slot %d turned ESP %s (access: %s, sees outlines right now: %s, outlines on the map: %d)", slot,
			g_enabled[slot] ? "on" : "off", adminesp::AccessName(g_access[slot]), g_canSee[slot] ? "yes" : "no",
			CountGlows());
}

// A refusal used to be silent, and on 30 Sep that looked like a broken plugin on three servers.
// Say so in chat and log what admin_system reported.
void ReportRefused(int slot) {
	pisex::PrintToChat(slot, "%s", Phrase("esp_no_access"));
	const auto yes = [](bool value) { return value ? "yes" : "no"; };
	LogInfo("slot %d was refused ESP: connected=%s bot=%s admin=%s flag_z=%s flag_a=%s %s=%s %s=%s", slot,
			yes(pisex::IsConnected(slot)), yes(pisex::IsFakeClient(slot)), yes(g_pAdminApi->IsAdmin(slot)),
			yes(g_pAdminApi->HasFlag(slot, "z")), yes(g_pAdminApi->HasFlag(slot, "a")), g_settings.flagAlways.c_str(),
			yes(!g_settings.flagAlways.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagAlways.c_str())),
			g_settings.flagDeadOnly.c_str(),
			yes(!g_settings.flagDeadOnly.empty() && g_pAdminApi->HasPermission(slot, g_settings.flagDeadOnly.c_str())));
}

// --- events --------------------------------------------------------------------------------------

void OnPlayerSpawn(const char*, pisex::IGameEvent* event, bool) {
	const int slot = event ? event->GetPlayerSlot("userid") : -1;
	if (!ValidSlot(slot) || !pisex::IsGameReady()) {
		return;
	}
	// A respawned model may be different; rebuild the outline after the delay.
	DestroyGlow(slot);
	g_glows[slot].createAfter = Now() + kSpawnDelaySeconds;
	// A dead-only viewer loses the view on respawn.
	RefreshAccess(slot);
	if (g_canSee[slot]) {
		SyncClientView(slot, true);
	}
}

void OnPlayerDeath(const char*, pisex::IGameEvent* event, bool) {
	const int slot = event ? event->GetPlayerSlot("userid") : -1;
	if (!ValidSlot(slot) || !pisex::IsGameReady()) {
		return;
	}
	// Otherwise the outline would stay on the corpse.
	DestroyGlow(slot);
	UpdateViewer(slot, true);
}

void OnPlayerTeam(const char*, pisex::IGameEvent* event, bool) {
	const int slot = event ? event->GetPlayerSlot("userid") : -1;
	if (!ValidSlot(slot) || !pisex::IsGameReady()) {
		return;
	}
	// Recreated in the new team's colour by the reconcile timer.
	DestroyGlow(slot);
	UpdateViewer(slot, true);
}

void OnPlayerDisconnect(const char*, pisex::IGameEvent* event, bool) {
	const int slot = event ? event->GetPlayerSlot("userid") : -1;
	if (!ValidSlot(slot)) {
		return;
	}
	if (pisex::IsGameReady()) {
		DestroyGlow(slot);
	}
	g_access[slot] = adminesp::Access::None;
	g_enabled[slot] = g_settings.enabledByDefault;
	g_canSee[slot] = false;
}

void OnRoundPrestart(const char*, pisex::IGameEvent*, bool) {
	if (!pisex::IsGameReady()) {
		return;
	}
	// The pawns are replaced now; the spawn events bring the outlines back.
	const float resume = Now() + kRoundStartDelaySeconds;
	for (int target = 0; target < kMaxSlots; ++target) {
		DestroyGlow(target);
		g_glows[target].createAfter = resume;
	}
}

void OnClientAuthorized(int slot, uint64_t) {
	if (!ValidSlot(slot)) {
		return;
	}
	g_enabled[slot] = g_settings.enabledByDefault;
	RefreshAccess(slot);
}

// --- commands / menu ------------------------------------------------------------------------------

bool OnEspCommand(int slot, const char*) {
	if (!ValidSlot(slot)) {
		return false;
	}
	RefreshAccess(slot);
	if (g_access[slot] == adminesp::Access::None) {
		ReportRefused(slot);
		return true;
	}
	Toggle(slot);
	return true;
}

void OnAdminCoreLoaded() {
	if (g_settings.menuCategory.empty()) {
		return;
	}
	const std::string& flag = !g_settings.flagDeadOnly.empty() ? g_settings.flagDeadOnly : g_settings.flagAlways;
	if (flag.empty()) {
		return;
	}
	if (g_settings.menuCategory == "players") {
		g_pAdminApi->RegisterCategory("players", "Category_Players", nullptr);
	}
	// admin_api.hpp copies the strings (the original's strdup() workaround for the Pisex core
	// holding the pointers is not needed here).
	g_pAdminApi->RegisterItem(
		"adminesp", "Item_AdminESP", g_settings.menuCategory.c_str(), flag.c_str(),
		[](int slot, const char*, const char*, std::string& item) {
			if (ValidSlot(slot)) {
				item = Phrase(g_enabled[slot] ? "esp_menu_on" : "esp_menu_off");
			}
		},
		[](int slot, const char* category, const char*, const char*) {
			if (!ValidSlot(slot)) {
				return;
			}
			RefreshAccess(slot);
			if (g_access[slot] == adminesp::Access::None) {
				ReportRefused(slot);
				return;
			}
			Toggle(slot);
			g_pAdminApi->ShowAdminCategoryMenu(slot, category);
		});
}

// --- transmit -------------------------------------------------------------------------------------

// Called for every CheckTransmit pass (s2sdk hooks ISource2GameEntities::CheckTransmit); clears
// only, for entities whose handle still resolves to ours.
//
// The engine calls this once per tick with every client's info, so the pass starts with a pure
// local scan of the handle arrays: while no outline exists (no ESP admin has switched on) not a
// single s2sdk call is made. Every handle that is passed to ClearTransmitInfoEntity is resolved
// first - the export logs "Cannot execute ... on invalid entity handle" for one that does not
// resolve - and a cleared handle is re-resolved only once per pass, not once per client.
void OnCheckTransmit(const plg::vector<void*>& infos) {
	if (!g_canTransmit) {
		return;
	}

	bool any = false;
	for (int target = 0; target < kMaxSlots && !any; ++target) {
		any = g_glows[target].relay != 0 || g_glows[target].glow != 0;
	}
	for (int r = 0; r < kRetiredSlots && !any; ++r) {
		any = g_retired[r] != 0;
	}
	if (!any) {
		return;
	}

	int hideRelay[kMaxSlots];
	int hideGlow[kMaxSlots];
	int retired[kRetiredSlots];
	int retiredCount = 0;
	bool anyClearable = false;
	for (int target = 0; target < kMaxSlots; ++target) {
		hideRelay[target] = s2sdk::EntHandleToEntIndex(g_glows[target].relay);
		hideGlow[target] = s2sdk::EntHandleToEntIndex(g_glows[target].glow);
		anyClearable = anyClearable || hideRelay[target] > 0 || hideGlow[target] > 0;
	}
	for (const int handle : g_retired) {
		// an empty slot is skipped without the export call (the array is 256 entries deep)
		if (handle != 0 && s2sdk::EntHandleToEntIndex(handle) > 0) {
			retired[retiredCount++] = handle;
		}
	}
	if (!anyClearable && retiredCount == 0) {
		return; // stale handles only: nothing to clear
	}

	for (void* info : infos) {
		if (!info) {
			continue;
		}
		const int slot = s2sdk::GetTransmitInfoPlayerSlot(info);
		// Anyone who is not an admin with ESP on (SourceTV included) gets no outline.
		const bool viewer = ValidSlot(slot) && g_canSee[slot];
		if (viewer) {
			// The engine decides what a viewer gets; a viewer never sees his own outline, so the
			// whole pass for him is two clears at most.
			if (hideRelay[slot] > 0) {
				s2sdk::ClearTransmitInfoEntity(info, g_glows[slot].relay);
			}
			if (hideGlow[slot] > 0) {
				s2sdk::ClearTransmitInfoEntity(info, g_glows[slot].glow);
			}
		} else {
			for (int target = 0; target < kMaxSlots; ++target) {
				if (hideRelay[target] > 0) {
					s2sdk::ClearTransmitInfoEntity(info, g_glows[target].relay);
				}
				if (hideGlow[target] > 0) {
					s2sdk::ClearTransmitInfoEntity(info, g_glows[target].glow);
				}
			}
		}
		for (int r = 0; r < retiredCount; ++r) {
			s2sdk::ClearTransmitInfoEntity(info, retired[r]);
		}
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

void OnStartupServer() {
	// The previous map took its entities with it.
	ForgetGlows();
	LoadConfig();
	LoadPhrases();
	LogInfo("startup: flags '%s' / '%s', colours #%02X%02X%02X / #%02X%02X%02X", g_settings.flagAlways.c_str(),
			g_settings.flagDeadOnly.c_str(), g_settings.colorCT.r, g_settings.colorCT.g, g_settings.colorCT.b,
			g_settings.colorT.r, g_settings.colorT.g, g_settings.colorT.b);
}

class AdminEspPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override {
		try {
			// Never Failed: a plugin that fails OnPluginStart stops every Plugify plugin.
			pisex::Init("AdminESP"); // <configs>/AdminESP (+ translations)
			LoadConfig();
			LoadPhrases();
			for (int slot = 0; slot < kMaxSlots; ++slot) {
				g_enabled[slot] = g_settings.enabledByDefault;
			}

			g_canTransmit = BindingsReady();
			if (g_canTransmit) {
				s2sdk::OnServerCheckTransmit_Register(&OnCheckTransmit);
			} else {
				LogError("s2sdk bindings are missing: AdminESP is disabled");
			}

			// The map start reloads the config/phrases and drops the state of the previous map;
			// the callback runs from Poll(), i.e. with a live map.
			pisex::g_pUtils->MapStartHook([](const char*) { OnStartupServer(); });

			pisex::g_pUtils->HookEvent("player_spawn", OnPlayerSpawn);
			pisex::g_pUtils->HookEvent("player_death", OnPlayerDeath);
			pisex::g_pUtils->HookEvent("player_team", OnPlayerTeam);
			pisex::g_pUtils->HookEvent("player_disconnect", OnPlayerDisconnect);
			pisex::g_pUtils->HookEvent("round_prestart", OnRoundPrestart);
			pisex::HookOnClientAuthorized(OnClientAuthorized);

			g_pAdminApi->OnAdminConnected([](int slot) {
				if (ValidSlot(slot)) {
					RefreshAccess(slot);
				}
			});
			g_pAdminApi->OnCoreLoaded(OnAdminCoreLoaded);
			if (g_pAdminApi->IsCoreLoaded()) {
				OnAdminCoreLoaded(); // late load: the event will not fire twice
			}

			pisex::RegCommand({"mm_esp"}, {"!esp"}, OnEspCommand);

			g_timer = pisex::CreateTimer(kReconcileInterval, Reconcile);
			if (!g_timer) {
				LogError("could not create the reconcile timer");
			}
			LogInfo("started (transmit control: %s)", g_canTransmit ? "yes" : "no");
			return plg::PluginResult{};
		} catch (const std::exception& ex) {
			LogError("OnPluginStart failed: %s", ex.what());
			return plg::PluginResult{};
		} catch (...) {
			LogError("OnPluginStart failed with an unknown exception");
			return plg::PluginResult{};
		}
	}

	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds) override {
		try {
			pisex::Poll();
		} catch (const std::exception& ex) {
			LogError("update failed: %s", ex.what());
		} catch (...) {
			LogError("update failed with an unknown exception");
		}
		return plg::PluginResult{};
	}

	plg::PluginResult OnPluginEnd() override {
		try {
			if (g_canTransmit) {
				s2sdk::OnServerCheckTransmit_Unregister(&OnCheckTransmit);
				g_canTransmit = false;
			}
			if (g_timer) {
				pisex::RemoveTimer(g_timer);
				g_timer = nullptr;
			}
			if (pisex::IsGameReady()) {
				for (int target = 0; target < kMaxSlots; ++target) {
					DestroyGlow(target);
				}
			}
			g_pAdminApi->OnCoreLoadedUnregister();
			g_pAdminApi->OnAdminConnectedUnregister();
			pisex::Shutdown();
		} catch (...) {
		}
		return plg::PluginResult{};
	}
};

AdminEspPlugin g_adminEspPlugin;

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_adminEspPlugin)
