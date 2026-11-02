// AdminESP plugin tests — no engine: src/plugin.cpp is compiled into this TU (its handlers live in
// an anonymous namespace) and every __s2sdk_* / __admin_system_* binding slot is a recording fake.
//
// Covered:
//   * startup: every registration (map listeners, transmit callback, both commands, five events,
//     OnClientAuthenticated, OnAdminConnected/OnCoreLoaded) happens exactly once, the production
//     adminesp.ini and AdminESP.phrases.txt are read through the plugin-local KV1 reader, and no
//     client/entity/schema call is made before the first map (PITFALLS B1);
//   * the protected ESP end to end: an admin with @admin/zalupa toggles ESP with mm_esp; the
//     reconcile creates the relay+glow pair for every living player; a CheckTransmit pass clears
//     all outline handles for a non-viewer (including the "exists but not transmitted" bit and
//     the always-list bit) and only the viewer's own pair for the admin; SourceTV, an empty slot
//     and every unauthorised slot get nothing; losing the permission removes canSee and the same
//     pass hides everything again; a dead-only admin sees while dead/spectating but not while
//     alive, and not while dead but controlling a live bot (m_bControllingBot);
//   * signed entity handles: the fake entity system hands out *negative* CEntityHandle values
//     (serial >= 65535); the pair is created, kept across reconciles, removed, retired and hidden
//     from a non-viewer, and a freed handle is pruned from the retire list;
//   * the per-slot pending rebuild is tied to the slot identity: a player replacing the one that
//     just spawned does not inherit its spawn delay; a disconnect (event or observed) drops the
//     outline, access and ESP switch; round_prestart arms the delay for the current identity;
//   * command ownership (PITFALLS C11): console "mm_esp" answers only Console, chat "!esp" only
//     Chat and always swallows the chat line; an unauthorised player gets esp_no_access and never
//     toggles; OnPluginEnd uses RemoveCommand for a command this plugin created and
//     RemoveCommandListener for a pre-existing name, and a reload registers everything once;
//   * teardown (A7/C12): the transmit callback, commands, events, OnClientAuthenticated, map
//     listeners and admin callbacks are all removed, the outline entities are removed, the state
//     is dropped and no s2sdk call happens after the unload;
//   * the map gate (B1/B2): no schema/client/entity call before the first map or between OnMapEnd
//     and the next OnMapStart; the first OnPluginUpdate of a plugin loaded into a running map
//     latches readiness, an OnMapEnd seen before it does not;
//   * fail-soft (A1): a missing transmit/command/event/map-listener/admin binding logs and
//     disables only its feature, OnPluginStart/OnPluginEnd stay Ok, and no outline entity is ever
//     created when it could not be hidden;
//   * the local colour/phrase layer: the cs2-menus tag bytes arrive in chat, core.cfg ServerLang
//     (russian/ru) wins over s2sdk's ServerLanguage, an absent core.cfg falls back to s2sdk then
//     "en", and a malformed/missing adminesp.ini keeps the flags empty (ESP stays off).
//
// Run by ./test.sh (build/tests/adminesp_plugin_tests); no pisexcompat and no engine are involved.
// This is not a game-server test: it only drives recording fakes.

#include "../src/plugin.cpp"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEqInt(int64_t actual, int64_t expected, const std::string& what) {
	Check(actual == expected,
		  what + " (got " + std::to_string(actual) + ", want " + std::to_string(expected) + ")");
}

void CheckEqStr(const std::string& actual, const std::string& expected, const std::string& what) {
	Check(actual == expected, what + " (got \"" + actual + "\", want \"" + expected + "\")");
}

void Section(const char* name) {
	std::printf("== %s\n", name);
}

plg::string ToPlg(std::string_view text) {
	return plg::string(text);
}

// ==========================================================================================
// config fixture: <configs>/AdminESP/adminesp.ini + translations + <configs>/core.cfg
// ==========================================================================================

std::string g_configsDir;

std::string ConfigPath() {
	return g_configsDir + "/AdminESP/adminesp.ini";
}

std::string PhrasesPath() {
	return g_configsDir + "/AdminESP/translations/AdminESP.phrases.txt";
}

std::string CoreConfigPath() {
	return g_configsDir + "/core.cfg";
}

void WriteTextFile(const std::string& path, std::string_view content) {
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file << content;
}

bool CopyFile(const char* source, const std::string& target) {
	std::error_code error;
	std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
	return !error;
}

void SetUpConfigFiles() {
	g_configsDir = (std::filesystem::temp_directory_path() /
					("adminesp_plugin_tests_" + std::to_string(static_cast<long long>(::getpid()))))
					   .string();
	std::error_code error;
	std::filesystem::remove_all(g_configsDir, error);
	std::filesystem::create_directories(g_configsDir + "/AdminESP/translations", error);
	CopyFile(ADMINESP_TEST_CONFIG, ConfigPath());
	CopyFile(ADMINESP_TEST_PHRASES, PhrasesPath());
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\"    \"russian\"\n}\n");
}

void RestoreConfigFiles() {
	std::error_code error;
	std::filesystem::create_directories(g_configsDir + "/AdminESP/translations", error);
	CopyFile(ADMINESP_TEST_CONFIG, ConfigPath());
	CopyFile(ADMINESP_TEST_PHRASES, PhrasesPath());
	WriteTextFile(CoreConfigPath(), "\"Core\"\n{\n    \"ServerLang\"    \"russian\"\n}\n");
}

void RemoveCoreConfig() {
	std::error_code error;
	std::filesystem::remove(CoreConfigPath(), error);
}

plg::string FakeGetConfigsDir() {
	return ToPlg(g_configsDir);
}

// ==========================================================================================
// logging
// ==========================================================================================

std::vector<std::string> g_logLines;
std::vector<std::string> g_errorLogLines;

void FakeLog(std::string_view message, plg::Severity severity,
			 const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	if (severity == plg::Severity::Error || severity == plg::Severity::Fatal) {
		g_errorLogLines.emplace_back(message);
	}
}

bool Logged(const std::string& needle) {
	for (const std::string& line : g_logLines) {
		if (line.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

int ErrorLogsContaining(const std::string& needle) {
	int count = 0;
	for (const std::string& line : g_errorLogLines) {
		if (line.find(needle) != std::string::npos) {
			++count;
		}
	}
	return count;
}

// ==========================================================================================
// fake clients / entity system
// ==========================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool controller = false;
	bool pawn = false;
	bool alive = false;
	bool bot = false;
	bool controllingBot = false;
	int team = 0;
	uint64_t steamid = 0;
};

std::array<FakeClient, 64> g_fakeClients;

FakeClient Human(uint64_t steamid, int team, bool alive = true) {
	FakeClient client;
	client.connected = true;
	client.inGame = true;
	client.controller = true;
	client.pawn = true;
	client.alive = alive;
	client.team = team;
	client.steamid = steamid;
	return client;
}

FakeClient Bot(int team) {
	FakeClient client = Human(0, team);
	client.bot = true;
	return client;
}

constexpr int32_t kTeamOffset = 100;
constexpr int32_t kControllingBotOffset = 2272;
constexpr uint32_t kHandleBit = 0x80000000u;
constexpr int32_t kFirstEntityIndex = 200;

int32_t g_nextEntityIndex = kFirstEntityIndex;
std::map<int32_t, bool> g_fakeEntityAlive; // entity index -> alive
std::vector<int32_t> g_fakeCreatedHandles;
std::vector<int32_t> g_fakeRemovedHandles;
std::vector<int32_t> g_fakePendingFree; // handles removed but not yet freed by the "engine"
std::map<int32_t, std::pair<int32_t, bool>> g_fakeFollows; // child handle -> (parent, merge)
struct FakeSpawnValues {
	std::map<std::string, std::string> values;
};
std::map<int32_t, FakeSpawnValues> g_fakeSpawns; // handle -> spawned keyvalues

int g_fakeCreateEntityCalls = 0;
int g_fakeSchemaCalls = 0;
int g_fakeSchemaCallsBeforeMap = 0;
int g_fakeGetEntDataCalls = 0;
int g_fakeGetEntDataCallsBeforeMap = 0;
float g_fakeGameTime = 10.0f;
std::string g_fakeServerLanguage = "en";

int32_t EntityIndex(int32_t handle) {
	return handle & 0x7FFF;
}

int32_t MakeHandle(int32_t index) {
	return static_cast<int32_t>(kHandleBit | static_cast<uint32_t>(index));
}

void* ControllerPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x10000 + slot));
}

void* PawnPointer(int32_t slot) {
	return reinterpret_cast<void*>(static_cast<uintptr_t>(0x20000 + slot));
}

int32_t FakeEntHandleToEntIndex(int32_t handle) {
	if (handle == 0 || handle == -1) {
		return -1;
	}
	const int32_t index = EntityIndex(handle);
	if (index >= 1 && index <= 64) {
		return g_fakeClients[static_cast<std::size_t>(index - 1)].controller ? index : -1;
	}
	if (index >= 100 && index < 164) {
		return g_fakeClients[static_cast<std::size_t>(index - 100)].pawn ? index : -1;
	}
	const auto it = g_fakeEntityAlive.find(index);
	return it != g_fakeEntityAlive.end() && it->second ? index : -1;
}

int32_t FakeEntIndexToEntHandle(int32_t index) {
	return index > 0 ? MakeHandle(index) : -1;
}

int32_t FakeEntPointerToEntIndex(void* pointer) {
	const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
	if (value >= 0x10000 && value < 0x10040) {
		return static_cast<int32_t>(value - 0x10000) + 1;
	}
	if (value >= 0x20000 && value < 0x20040) {
		return static_cast<int32_t>(value - 0x20000) + 100;
	}
	return -1;
}

int32_t FakeEntPointerToEntHandle(void* pointer) {
	return FakeEntIndexToEntHandle(FakeEntPointerToEntIndex(pointer));
}

bool FakeIsValidEntHandle(int32_t handle) {
	return FakeEntHandleToEntIndex(handle) > 0;
}

bool FakeIsValidEntPointer(void* pointer) {
	return FakeEntPointerToEntIndex(pointer) > 0;
}

int32_t FakeCreateEntityByName(const plg::string& /*classname*/) {
	++g_fakeCreateEntityCalls;
	const int32_t index = g_nextEntityIndex++;
	if (index > 0x7FFF) {
		return -1;
	}
	g_fakeEntityAlive[index] = true;
	const int32_t handle = MakeHandle(index);
	g_fakeCreatedHandles.push_back(handle);
	return handle;
}

std::string AnyText(const plg::any& value) {
	if (const auto* text = plg::get_if<plg::string>(&value)) {
		return ToStd(*text);
	}
	if (const auto* number = plg::get_if<int32_t>(&value)) {
		return std::to_string(*number);
	}
	if (const auto* number = plg::get_if<int64_t>(&value)) {
		return std::to_string(*number);
	}
	return {};
}

void FakeDispatchSpawn2(int32_t handle, const plg::vector<plg::string>& keys,
						const plg::vector<plg::any>& values) {
	FakeSpawnValues spawn;
	for (std::size_t i = 0; i < keys.size() && i < values.size(); ++i) {
		spawn.values[ToStd(keys[i])] = AnyText(values[i]);
	}
	g_fakeSpawns[handle] = std::move(spawn);
}

void FakeRemoveEntity(int32_t handle) {
	g_fakeRemovedHandles.push_back(handle);
	g_fakePendingFree.push_back(handle);
}

void FakeFollowEntity(int32_t child, int32_t parent, bool merge) {
	g_fakeFollows[child] = {parent, merge};
}

void FlushRemovals() {
	for (const int32_t handle : g_fakePendingFree) {
		g_fakeEntityAlive[EntityIndex(handle)] = false;
	}
	g_fakePendingFree.clear();
}

plg::string FakeGetClientModel(int32_t slot) {
	if (slot < 0 || slot >= 64 || !g_fakeClients[static_cast<std::size_t>(slot)].pawn) {
		return plg::string();
	}
	return ToPlg("models/fake/player_" + std::to_string(slot) + ".mdl");
}

int64_t FakeGetEntData(int32_t handle, int32_t offset, int32_t /*size*/) {
	++g_fakeGetEntDataCalls;
	if (!g_mapReady) {
		++g_fakeGetEntDataCallsBeforeMap;
	}
	const int32_t index = EntityIndex(handle);
	if (index >= 1 && index <= 64 && g_fakeClients[static_cast<std::size_t>(index - 1)].controller) {
		const FakeClient& client = g_fakeClients[static_cast<std::size_t>(index - 1)];
		if (offset == kTeamOffset) {
			return client.team;
		}
		if (offset == kControllingBotOffset) {
			return client.controllingBot ? 1 : 0;
		}
	}
	return 0;
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_fakeSchemaCalls;
	if (!g_mapReady) {
		++g_fakeSchemaCallsBeforeMap;
	}
	const std::string klass = ToStd(className);
	const std::string member = ToStd(memberName);
	if (klass == "CBaseEntity" && member == "m_iTeamNum") {
		return kTeamOffset;
	}
	if (klass == "CCSPlayerController" && member == "m_bControllingBot") {
		return kControllingBotOffset;
	}
	return -1;
}

float FakeGetGameTime() {
	return g_fakeGameTime;
}

plg::string FakeGetServerLanguage() {
	return ToPlg(g_fakeServerLanguage);
}

bool FakeIsClientConnected(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].sourceTV;
}

bool FakeIsClientAlive(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return false;
	}
	const FakeClient& client = g_fakeClients[static_cast<std::size_t>(slot)];
	return client.connected && client.pawn && client.alive;
}

bool FakeIsFakeClient(int32_t slot) {
	return slot < 0 || slot >= 64 || g_fakeClients[static_cast<std::size_t>(slot)].bot;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].connected
			   ? g_fakeClients[static_cast<std::size_t>(slot)].steamid
			   : 0;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].controller
			   ? ControllerPointer(slot)
			   : nullptr;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].controller
			   ? MakeHandle(1 + slot)
			   : -1;
}

void* FakeGetClientPawn(int32_t slot) {
	return slot >= 0 && slot < 64 && g_fakeClients[static_cast<std::size_t>(slot)].pawn
			   ? PawnPointer(slot)
			   : nullptr;
}

s2sdk::CSTeam FakeGetClientTeam(int32_t slot) {
	if (slot < 0 || slot >= 64) {
		return s2sdk::CSTeam::None;
	}
	return static_cast<s2sdk::CSTeam>(g_fakeClients[static_cast<std::size_t>(slot)].team);
}

// ==========================================================================================
// fake console / cvars / chat
// ==========================================================================================

struct FakeChat {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakeChat> g_fakeChat;
std::map<std::string, uint64_t> g_fakeConVars;
struct FakeSentConVar {
	int32_t slot = 0;
	std::string name;
	std::string value;
};
std::vector<FakeSentConVar> g_fakeSentConVars;

void FakePrintToChat(int32_t slot, const plg::string& text) {
	g_fakeChat.push_back(FakeChat{slot, ToStd(text)});
}

uint64_t FakeFindConVar(const plg::string& name) {
	const std::string key = ToStd(name);
	if (key == "mp_forcecamera") {
		g_fakeConVars[key] = 1001;
		return 1001;
	}
	if (key == "mp_playerid") {
		g_fakeConVars[key] = 1002;
		return 1002;
	}
	return 0;
}

plg::string FakeGetConVarString(uint64_t /*handle*/) {
	return ToPlg("1"); // the server keeps mp_forcecamera/mp_playerid at 1
}

void FakeSendConVarValue(int32_t slot, uint64_t handle, const plg::string& value) {
	std::string name;
	for (const auto& [key, id] : g_fakeConVars) {
		if (id == handle) {
			name = key;
			break;
		}
	}
	g_fakeSentConVars.push_back(FakeSentConVar{slot, name, ToStd(value)});
}

// ==========================================================================================
// fake commands
// ==========================================================================================

struct FakeCommandEntry {
	s2sdk::ConVarFlag flags = s2sdk::ConVarFlag::None;
	s2sdk::ConCommandCallback callback = nullptr;
	bool owned = false;
};

std::map<std::string, FakeCommandEntry> g_fakeCommands;
std::map<std::string, bool> g_fakeCommandOwned; // desired result of AddConsoleCommand
std::vector<std::string> g_fakeRemovedCommands;
std::vector<std::string> g_fakeRemovedListeners;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode /*mode*/) {
	const std::string key = ToStd(name);
	const auto configured = g_fakeCommandOwned.find(key);
	const bool owned = configured == g_fakeCommandOwned.end() || configured->second;
	g_fakeCommands[key] = FakeCommandEntry{flags, callback, owned};
	return owned;
}

bool FakeRemoveCommand(const plg::string& name, s2sdk::ConCommandCallback /*callback*/) {
	const std::string key = ToStd(name);
	g_fakeRemovedCommands.push_back(key);
	g_fakeCommands.erase(key);
	return true;
}

bool FakeRemoveCommandListener(const plg::string& name, s2sdk::ConCommandCallback /*callback*/,
							   s2sdk::HookMode /*mode*/) {
	g_fakeRemovedListeners.push_back(ToStd(name));
	g_fakeCommands.erase(ToStd(name));
	return true;
}

s2sdk::ResultType InvokeCommand(const char* name, int32_t slot, s2sdk::ConCommandContext context) {
	const auto it = g_fakeCommands.find(name);
	if (it == g_fakeCommands.end() || it->second.callback == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return it->second.callback(slot, context, plg::vector<plg::string>{});
}

// ==========================================================================================
// fake events
// ==========================================================================================

std::map<std::string, s2sdk::EventCallback> g_fakeEvents;
std::vector<std::string> g_fakeHookedEvents;
std::vector<std::string> g_fakeUnhookedEvents;
s2sdk::EventHookError g_fakeHookResult = s2sdk::EventHookError::Okay;
std::vector<std::string> g_fakeEventKeys;

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback,
									s2sdk::HookMode /*mode*/) {
	if (g_fakeHookResult != s2sdk::EventHookError::Okay) {
		return g_fakeHookResult;
	}
	const std::string key = ToStd(name);
	g_fakeEvents[key] = callback;
	g_fakeHookedEvents.push_back(key);
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback /*callback*/,
									  s2sdk::HookMode /*mode*/) {
	const std::string key = ToStd(name);
	g_fakeUnhookedEvents.push_back(key);
	g_fakeEvents.erase(key);
	return s2sdk::EventHookError::Okay;
}

struct FakeGameEvent {
	int32_t slot = -1;
};

int32_t FakeGetEventPlayerSlot(void* event, const plg::string& key) {
	g_fakeEventKeys.push_back(ToStd(key));
	if (event == nullptr) {
		return -1;
	}
	return static_cast<FakeGameEvent*>(event)->slot;
}

void InvokeEvent(const char* name, int32_t slot) {
	const auto it = g_fakeEvents.find(name);
	if (it == g_fakeEvents.end() || it->second == nullptr) {
		return;
	}
	FakeGameEvent event{slot};
	it->second(ToPlg(name), &event, false);
}

// ==========================================================================================
// fake map / auth / transmit
// ==========================================================================================

s2sdk::OnMapStartCallback g_fakeMapStart = nullptr;
s2sdk::OnMapEndCallback g_fakeMapEnd = nullptr;
int g_fakeMapStartRegisters = 0;
int g_fakeMapStartUnregisters = 0;
int g_fakeMapEndRegisters = 0;
int g_fakeMapEndUnregisters = 0;

void FakeMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_fakeMapStart = callback;
	++g_fakeMapStartRegisters;
}

void FakeMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_fakeMapStart == callback) {
		g_fakeMapStart = nullptr;
	}
	++g_fakeMapStartUnregisters;
}

void FakeMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_fakeMapEnd = callback;
	++g_fakeMapEndRegisters;
}

void FakeMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_fakeMapEnd == callback) {
		g_fakeMapEnd = nullptr;
	}
	++g_fakeMapEndUnregisters;
}

void MapStart() {
	if (g_fakeMapStart != nullptr) {
		g_fakeMapStart();
	}
}

void MapEnd() {
	if (g_fakeMapEnd != nullptr) {
		g_fakeMapEnd();
	}
}

s2sdk::OnClientAuthenticatedCallback g_fakeAuth = nullptr;
int g_fakeAuthRegisters = 0;
int g_fakeAuthUnregisters = 0;

void FakeAuthRegister(s2sdk::OnClientAuthenticatedCallback callback) {
	g_fakeAuth = callback;
	++g_fakeAuthRegisters;
}

void FakeAuthUnregister(s2sdk::OnClientAuthenticatedCallback callback) {
	if (g_fakeAuth == callback) {
		g_fakeAuth = nullptr;
	}
	++g_fakeAuthUnregisters;
}

void Authenticate(int32_t slot, uint64_t steamid) {
	if (g_fakeAuth != nullptr) {
		g_fakeAuth(slot, steamid);
	}
}

s2sdk::OnServerCheckTransmitCallback g_fakeTransmit = nullptr;
int g_fakeTransmitRegisters = 0;
int g_fakeTransmitUnregisters = 0;

struct FakeTransmitInfo {
	int32_t slot = -1;
};

struct FakeTransmitClear {
	int32_t slot = -1;
	int32_t handle = 0;
	std::string kind;
};

std::vector<FakeTransmitClear> g_fakeTransmitClears;
std::vector<int32_t> g_fakeInvalidTransmitHandles;

void FakeTransmitRegister(s2sdk::OnServerCheckTransmitCallback callback) {
	g_fakeTransmit = callback;
	++g_fakeTransmitRegisters;
}

void FakeTransmitUnregister(s2sdk::OnServerCheckTransmitCallback callback) {
	if (g_fakeTransmit == callback) {
		g_fakeTransmit = nullptr;
	}
	++g_fakeTransmitUnregisters;
}

int32_t FakeGetTransmitInfoPlayerSlot(void* info) {
	return info != nullptr ? static_cast<FakeTransmitInfo*>(info)->slot : -1;
}

void RecordTransmitClear(void* info, int32_t handle, const char* kind) {
	if (handle == 0 || handle == -1) {
		g_fakeInvalidTransmitHandles.push_back(handle);
	}
	if (info == nullptr) {
		return;
	}
	g_fakeTransmitClears.push_back(
		FakeTransmitClear{static_cast<FakeTransmitInfo*>(info)->slot, handle, kind});
}

void FakeClearTransmitInfoEntity(void* info, int32_t handle) {
	RecordTransmitClear(info, handle, "entity");
}

void FakeSetTransmitInfoNonPlayer(void* info, int32_t handle) {
	RecordTransmitClear(info, handle, "nonplayer");
}

void FakeClearTransmitInfoAlways(void* info, int32_t handle) {
	RecordTransmitClear(info, handle, "always");
}

void TransmitFor(int32_t slot) {
	if (g_fakeTransmit == nullptr) {
		return;
	}
	FakeTransmitInfo info{slot};
	plg::vector<void*> infos;
	infos.push_back(&info);
	g_fakeTransmit(infos);
}

bool Cleared(int32_t slot, int32_t handle, const char* kind) {
	for (const FakeTransmitClear& clear : g_fakeTransmitClears) {
		if (clear.slot == slot && clear.handle == handle && clear.kind == kind) {
			return true;
		}
	}
	return false;
}

int CountClears(int32_t slot, const char* kind) {
	int count = 0;
	for (const FakeTransmitClear& clear : g_fakeTransmitClears) {
		if (clear.slot == slot && clear.kind == kind) {
			++count;
		}
	}
	return count;
}

// ==========================================================================================
// fake admin_system
// ==========================================================================================

std::set<std::pair<int32_t, std::string>> g_fakePermissions;
std::map<int32_t, std::set<std::string>> g_fakeFlags;
std::set<int32_t> g_fakeAdmins;
bool g_fakeCoreLoaded = true;

admin_system::AdminConnectedCallback g_fakeAdminConnected = nullptr;
admin_system::CoreLoadedCallback g_fakeCoreLoadedCallback = nullptr;
int g_fakeAdminConnectedRegisters = 0;
int g_fakeAdminConnectedUnregisters = 0;
int g_fakeCoreLoadedRegisters = 0;
int g_fakeCoreLoadedUnregisters = 0;

struct FakeItem {
	std::string identity;
	std::string name;
	std::string category;
	std::string flags;
	admin_system::ItemDisplayCallback display = nullptr;
	admin_system::ItemSelectCallback select = nullptr;
};

std::map<std::string, FakeItem> g_fakeItems;
std::vector<std::pair<std::string, std::string>> g_fakeCategories;
std::vector<std::pair<int32_t, std::string>> g_fakeShownCategories;

float FakeGetPluginVersion() {
	return 1.0f;
}

bool FakeHasPermission(int32_t slot, const plg::string& permission) {
	return g_fakePermissions.count({slot, ToStd(permission)}) != 0;
}

bool FakeHasFlag(int32_t slot, const plg::string& flag) {
	const auto it = g_fakeFlags.find(slot);
	return it != g_fakeFlags.end() && it->second.count(ToStd(flag)) != 0;
}

bool FakeIsAdmin(int32_t slot) {
	return g_fakeAdmins.count(slot) != 0;
}

bool FakeIsCoreLoaded() {
	return g_fakeCoreLoaded;
}

void FakeAdminConnectedRegister(admin_system::AdminConnectedCallback callback) {
	g_fakeAdminConnected = callback;
	++g_fakeAdminConnectedRegisters;
}

void FakeAdminConnectedUnregister(admin_system::AdminConnectedCallback callback) {
	if (g_fakeAdminConnected == callback) {
		g_fakeAdminConnected = nullptr;
	}
	++g_fakeAdminConnectedUnregisters;
}

void FakeCoreLoadedRegister(admin_system::CoreLoadedCallback callback) {
	g_fakeCoreLoadedCallback = callback;
	++g_fakeCoreLoadedRegisters;
}

void FakeCoreLoadedUnregister(admin_system::CoreLoadedCallback callback) {
	if (g_fakeCoreLoadedCallback == callback) {
		g_fakeCoreLoadedCallback = nullptr;
	}
	++g_fakeCoreLoadedUnregisters;
}

void FakeRegisterCategory(const plg::string& identity, const plg::string& name,
						  admin_system::CategoryDisplayCallback /*callback*/) {
	g_fakeCategories.emplace_back(ToStd(identity), ToStd(name));
}

void FakeRegisterItem(const plg::string& identity, const plg::string& name, const plg::string& category,
					  const plg::string& flags, admin_system::ItemDisplayCallback display,
					  admin_system::ItemSelectCallback select) {
	FakeItem item;
	item.identity = ToStd(identity);
	item.name = ToStd(name);
	item.category = ToStd(category);
	item.flags = ToStd(flags);
	item.display = display;
	item.select = select;
	g_fakeItems[item.identity] = std::move(item);
}

void FakeShowAdminCategoryMenu(int32_t slot, const plg::string& category) {
	g_fakeShownCategories.emplace_back(slot, ToStd(category));
}

// ==========================================================================================
// installation / reset
// ==========================================================================================

bool g_pluginStarted = false;

void InstallFakes() {
	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_CreateEntityByName = &FakeCreateEntityByName;
	__s2sdk_DispatchSpawn2 = &FakeDispatchSpawn2;
	__s2sdk_RemoveEntity = &FakeRemoveEntity;
	__s2sdk_FollowEntity = &FakeFollowEntity;
	__s2sdk_GetClientModel = &FakeGetClientModel;
	__s2sdk_EntHandleToEntIndex = &FakeEntHandleToEntIndex;
	__s2sdk_EntIndexToEntHandle = &FakeEntIndexToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_EntPointerToEntHandle = &FakeEntPointerToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetEntData = &FakeGetEntData;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_GetGameTime = &FakeGetGameTime;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_GetClientTeam = &FakeGetClientTeam;

	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_FindConVar = &FakeFindConVar;
	__s2sdk_GetConVarString = &FakeGetConVarString;
	__s2sdk_SendConVarValue = &FakeSendConVarValue;

	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__s2sdk_OnMapStart_Register = &FakeMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeMapEndUnregister;
	__s2sdk_OnClientAuthenticated_Register = &FakeAuthRegister;
	__s2sdk_OnClientAuthenticated_Unregister = &FakeAuthUnregister;

	__s2sdk_OnServerCheckTransmit_Register = &FakeTransmitRegister;
	__s2sdk_OnServerCheckTransmit_Unregister = &FakeTransmitUnregister;
	__s2sdk_GetTransmitInfoPlayerSlot = &FakeGetTransmitInfoPlayerSlot;
	__s2sdk_ClearTransmitInfoEntity = &FakeClearTransmitInfoEntity;
	__s2sdk_SetTransmitInfoNonPlayer = &FakeSetTransmitInfoNonPlayer;
	__s2sdk_ClearTransmitInfoAlways = &FakeClearTransmitInfoAlways;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;

	__admin_system_GetPluginVersion = &FakeGetPluginVersion;
	__admin_system_HasPermission = &FakeHasPermission;
	__admin_system_HasFlag = &FakeHasFlag;
	__admin_system_IsAdmin = &FakeIsAdmin;
	__admin_system_IsCoreLoaded = &FakeIsCoreLoaded;
	__admin_system_OnAdminConnected_Register = &FakeAdminConnectedRegister;
	__admin_system_OnAdminConnected_Unregister = &FakeAdminConnectedUnregister;
	__admin_system_OnCoreLoaded_Register = &FakeCoreLoadedRegister;
	__admin_system_OnCoreLoaded_Unregister = &FakeCoreLoadedUnregister;
	__admin_system_RegisterCategory = &FakeRegisterCategory;
	__admin_system_RegisterItem = &FakeRegisterItem;
	__admin_system_ShowAdminCategoryMenu = &FakeShowAdminCategoryMenu;
}

void ResetWorld() {
	InstallFakes();
	RestoreConfigFiles();

	g_fakeClients.fill(FakeClient{});
	g_fakeCreatedHandles.clear();
	g_fakeRemovedHandles.clear();
	g_fakePendingFree.clear();
	g_fakeEntityAlive.clear();
	g_fakeFollows.clear();
	g_fakeSpawns.clear();
	g_nextEntityIndex = kFirstEntityIndex;
	g_fakeCreateEntityCalls = 0;
	g_fakeSchemaCalls = 0;
	g_fakeSchemaCallsBeforeMap = 0;
	g_fakeGetEntDataCalls = 0;
	g_fakeGetEntDataCallsBeforeMap = 0;
	g_fakeGameTime = 10.0f;
	g_fakeServerLanguage = "en";

	g_fakeChat.clear();
	g_fakeConVars.clear();
	g_fakeSentConVars.clear();

	g_fakeCommands.clear();
	g_fakeCommandOwned.clear();
	g_fakeRemovedCommands.clear();
	g_fakeRemovedListeners.clear();

	g_fakeEvents.clear();
	g_fakeHookedEvents.clear();
	g_fakeUnhookedEvents.clear();
	g_fakeHookResult = s2sdk::EventHookError::Okay;
	g_fakeEventKeys.clear();

	g_fakeMapStart = nullptr;
	g_fakeMapEnd = nullptr;
	g_fakeMapStartRegisters = 0;
	g_fakeMapStartUnregisters = 0;
	g_fakeMapEndRegisters = 0;
	g_fakeMapEndUnregisters = 0;
	g_fakeAuth = nullptr;
	g_fakeAuthRegisters = 0;
	g_fakeAuthUnregisters = 0;
	g_fakeTransmit = nullptr;
	g_fakeTransmitRegisters = 0;
	g_fakeTransmitUnregisters = 0;
	g_fakeTransmitClears.clear();
	g_fakeInvalidTransmitHandles.clear();

	g_fakePermissions.clear();
	g_fakeFlags.clear();
	g_fakeAdmins.clear();
	g_fakeCoreLoaded = true;
	g_fakeAdminConnected = nullptr;
	g_fakeCoreLoadedCallback = nullptr;
	g_fakeAdminConnectedRegisters = 0;
	g_fakeAdminConnectedUnregisters = 0;
	g_fakeCoreLoadedRegisters = 0;
	g_fakeCoreLoadedUnregisters = 0;
	g_fakeItems.clear();
	g_fakeCategories.clear();
	g_fakeShownCategories.clear();

	g_logLines.clear();
	g_errorLogLines.clear();
	admin_system::AdminApi::Get().ReloadTranslations();
}

void PrepareTest() {
	InstallFakes();
	RestoreConfigFiles();
	if (g_pluginStarted) {
		g_adminEspPlugin.OnPluginEnd();
		g_pluginStarted = false;
	}
	ResetWorld();
}

void StartPlugin() {
	const plg::PluginResult result = g_adminEspPlugin.OnPluginStart();
	g_pluginStarted = true;
	Check(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart is Ok");
}

void Advance(int64_t milliseconds) {
	g_fakeGameTime += static_cast<float>(milliseconds) / 1000.0f;
	g_adminEspPlugin.OnPluginUpdate(std::chrono::milliseconds(milliseconds));
}

void Reconciles(int count) {
	for (int i = 0; i < count; ++i) {
		Advance(250);
	}
}

void SetClient(int32_t slot, const FakeClient& client) {
	if (slot >= 0 && slot < 64) {
		g_fakeClients[static_cast<std::size_t>(slot)] = client;
	}
}

void Grant(int32_t slot, const char* permission) {
	g_fakePermissions.insert({slot, permission});
}

constexpr uint64_t kSteamAdmin = 76561198000000001ULL;
constexpr uint64_t kSteamPlayer = 76561198000000002ULL;
constexpr uint64_t kSteamThird = 76561198000000003ULL;
constexpr uint64_t kSteamFourth = 76561198000000004ULL;

void SetUpThreePlayers() {
	SetClient(0, Human(kSteamAdmin, adminesp::kTeamT));
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	SetClient(2, Human(kSteamThird, adminesp::kTeamT));
}

void EnableEsp(int32_t slot) {
	const s2sdk::ResultType result = InvokeCommand("mm_esp", slot, s2sdk::ConCommandContext::Console);
	Check(result == s2sdk::ResultType::Handled, "the console toggle is handled for a valid admin slot");
}

bool RetiredContains(int32_t handle) {
	for (const int32_t entry : g_retired) {
		if (entry == handle) {
			return true;
		}
	}
	return false;
}

// ==========================================================================================
// A. startup and registration
// ==========================================================================================

void TestStartupAndRegistration() {
	Section("A. startup: production config, phrases, registrations");
	PrepareTest();
	StartPlugin();

	CheckEqStr(g_settings.flagAlways, "@admin/zalupa", "production admin_flag_all");
	CheckEqStr(g_settings.flagDeadOnly, "@admin/spec", "production admin_flag_death");
	CheckEqInt(g_settings.colorCT.b, 0xF7, "production ct_color");
	const auto phrase = g_phrases.find("esp_no_access");
	Check(phrase != g_phrases.end(), "production phrases loaded");
	if (phrase != g_phrases.end()) {
		Check(phrase->second.find("Нет доступа") != std::string::npos,
			  "the phrase language came from <configs>/core.cfg ServerLang=russian");
	}

	CheckEqInt(g_fakeMapStartRegisters, 1, "one OnMapStart registration");
	CheckEqInt(g_fakeMapEndRegisters, 1, "one OnMapEnd registration");
	CheckEqInt(g_fakeTransmitRegisters, 1, "one CheckTransmit registration");
	CheckEqInt(g_fakeAuthRegisters, 1, "one OnClientAuthenticated registration");
	CheckEqInt(g_fakeAdminConnectedRegisters, 1, "one admin-connected registration");
	CheckEqInt(g_fakeCoreLoadedRegisters, 1, "one admin-core-loaded registration");
	CheckEqInt(static_cast<int>(g_fakeCommands.size()), 2, "both commands registered");
	CheckEqInt(static_cast<int>(g_fakeEvents.size()), 5, "all five events hooked");

	// The item of the original RegisterMenuItem, with the dead-only flag taking precedence.
	const auto item = g_fakeItems.find("adminesp");
	Check(item != g_fakeItems.end(), "the admin menu item is registered");
	if (item != g_fakeItems.end()) {
		CheckEqStr(item->second.category, "players", "menu item category");
		CheckEqStr(item->second.flags, "@admin/spec", "menu item flags (dead-only wins)");
	}
	Check(!g_fakeCategories.empty(), "the players category was registered");

	// PITFALLS B1: no map yet -> no client/entity/schema call.
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	CheckEqInt(g_fakeCreateEntityCalls, 0, "no entity is created at plugin start");
	CheckEqInt(g_fakeSchemaCalls, 0, "no schema offset is looked up at plugin start");
	CheckEqInt(g_fakeGetEntDataCalls, 0, "no entity data is read at plugin start");

	MapStart();
	Advance(300);
	Check(g_mapReady, "the map gate is open after OnMapStart");
	CheckEqInt(g_fakeCreateEntityCalls, 0, "no outline exists while nobody enabled ESP");
	MapEnd();
	Check(!g_mapReady, "the map gate is closed by OnMapEnd");
}

// ==========================================================================================
// B. protected ESP: the security-critical scenario
// ==========================================================================================

void TestProtectedEsp() {
	Section("B. protected ESP: only the authorised viewer sees the outlines");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");

	EnableEsp(0);
	Check(g_canTransmit, "the all-bindings host has protected ESP enabled");
	Check(g_viewers[0].access == adminesp::Access::Always, "slot 0 got the always access");
	Check(g_viewers[0].enabled, "slot 0 switched ESP on");
	Check(g_viewers[0].canSee, "slot 0 sees outlines now");
	Check(!g_viewers[1].canSee && !g_viewers[2].canSee, "nobody else sees them");
	Reconciles(1);

	Check(HasGlow(0) && HasGlow(1) && HasGlow(2), "an outline pair exists for every living player");
	Check(g_glows[0].relay < 0 && g_glows[0].glow < 0, "the fake entity system hands out negative handles");
	Check(adminesp::IsValidHandle(g_glows[0].relay) && adminesp::IsValidHandle(g_glows[0].glow),
		  "negative handles are valid (only 0/-1 are invalid)");

	// Spawn values / parenting of the pair.
	const auto relaySpawn = g_fakeSpawns.find(g_glows[1].relay);
	const auto glowSpawn = g_fakeSpawns.find(g_glows[1].glow);
	Check(relaySpawn != g_fakeSpawns.end(), "the relay was spawned through DispatchSpawn2");
	Check(glowSpawn != g_fakeSpawns.end(), "the glow was spawned through DispatchSpawn2");
	if (relaySpawn != g_fakeSpawns.end()) {
		CheckEqStr(relaySpawn->second.values["spawnflags"], "256", "relay spawnflags");
		CheckEqStr(relaySpawn->second.values["rendermode"], "10", "relay rendermode");
	}
	const auto tSpawn = g_fakeSpawns.find(g_glows[2].glow);
	Check(tSpawn != g_fakeSpawns.end(), "the T player's glow was spawned");
	if (tSpawn != g_fakeSpawns.end()) {
		CheckEqStr(tSpawn->second.values["glowcolor"], "226 173 54 255", "T glow colour");
	}
	if (glowSpawn != g_fakeSpawns.end()) {
		CheckEqStr(glowSpawn->second.values["spawnflags"], "256", "glow spawnflags");
		CheckEqStr(glowSpawn->second.values["glowcolor"], "73 104 247 255", "CT glow colour");
		CheckEqStr(glowSpawn->second.values["glowrange"], "5000", "glow range");
		CheckEqStr(glowSpawn->second.values["glowrangemin"], "-1000", "glow range min");
		CheckEqStr(glowSpawn->second.values["glowteam"], "-1", "glow team");
		CheckEqStr(glowSpawn->second.values["glowtype"], "2", "glow type");
		CheckEqStr(glowSpawn->second.values["glowstate"], "3", "glow state");
		CheckEqStr(glowSpawn->second.values["renderamt"], "1", "render amount");
	}
	const auto relayFollow = g_fakeFollows.find(g_glows[1].relay);
	const auto glowFollow = g_fakeFollows.find(g_glows[1].glow);
	Check(relayFollow != g_fakeFollows.end(), "the relay follows the pawn");
	if (relayFollow != g_fakeFollows.end()) {
		Check(relayFollow->second.first == FakeEntIndexToEntHandle(100 + 1), "relay parent is the pawn handle");
		Check(relayFollow->second.second, "the relay bone-merges");
	}
	Check(glowFollow != g_fakeFollows.end(), "the glow follows the relay");
	if (glowFollow != g_fakeFollows.end()) {
		Check(glowFollow->second.first == g_glows[1].relay, "glow parent is the relay");
	}

	// The viewer's client view was told mp_forcecamera/mp_playerid 0.
	bool forceCamera = false;
	bool playerId = false;
	for (const FakeSentConVar& sent : g_fakeSentConVars) {
		if (sent.slot != 0) {
			continue;
		}
		if (sent.name == "mp_forcecamera" && sent.value == "0") {
			forceCamera = true;
		}
		if (sent.name == "mp_playerid" && sent.value == "0") {
			playerId = true;
		}
	}
	Check(forceCamera && playerId, "the viewer's client got mp_forcecamera 0 and mp_playerid 0");
	for (const FakeSentConVar& sent : g_fakeSentConVars) {
		Check(sent.slot == 0, "no convar value was sent to a non-viewer");
	}

	// A non-viewer pass: all six live handles are cleared as entity + non-player + always.
	g_fakeTransmitClears.clear();
	g_fakeInvalidTransmitHandles.clear();
	TransmitFor(1);
	CheckEqInt(CountClears(1, "entity"), 6, "a non-viewer gets every outline cleared");
	CheckEqInt(CountClears(1, "nonplayer"), 6, "and every outline marked as non-transmitted");
	CheckEqInt(CountClears(1, "always"), 6, "and every outline cleared from the always-list");
	Check(Cleared(1, g_glows[0].relay, "entity") && Cleared(1, g_glows[0].glow, "entity"),
		  "the admin's own pair is hidden from the ordinary player");
	Check(Cleared(1, g_glows[2].relay, "entity"), "the third player's pair is hidden too");
	Check(g_fakeInvalidTransmitHandles.empty(), "no 0/-1 handle was passed to a transmit export");

	// A viewer pass: only his own pair (never his own outline) is cleared.
	g_fakeTransmitClears.clear();
	TransmitFor(0);
	CheckEqInt(CountClears(0, "entity"), 2, "a viewer gets exactly his own pair cleared");
	Check(Cleared(0, g_glows[0].relay, "entity") && Cleared(0, g_glows[0].glow, "entity"),
		  "the viewer's own outline is hidden from him");
	Check(!Cleared(0, g_glows[1].relay, "entity") && !Cleared(0, g_glows[2].glow, "entity"),
		  "other players' outlines reach the viewer");

	// A slot reused by another player before the next reconcile must not inherit the old
	// viewer decision (the identity is re-checked inside the transmit callback).
	SetClient(0, Human(kSteamFourth, adminesp::kTeamCT));
	g_fakeTransmitClears.clear();
	TransmitFor(0);
	CheckEqInt(CountClears(0, "entity"), 6,
			   "a reused slot is treated as a non-viewer until the reconcile re-evaluates it");
	SetClient(0, Human(kSteamAdmin, adminesp::kTeamT));

	// SourceTV / an empty slot / an out-of-range recipient: no outline.
	FakeClient sourceTV;
	sourceTV.connected = true;
	sourceTV.inGame = true;
	sourceTV.sourceTV = true;
	sourceTV.steamid = kSteamFourth;
	SetClient(5, sourceTV);
	Reconciles(1);
	g_fakeTransmitClears.clear();
	TransmitFor(5);
	CheckEqInt(CountClears(5, "entity"), 6, "SourceTV is not a viewer");
	Check(!g_viewers[5].canSee, "SourceTV never gets canSee");
	g_fakeTransmitClears.clear();
	TransmitFor(63);
	CheckEqInt(CountClears(63, "entity"), 6, "an empty slot is not a viewer");
	g_fakeTransmitClears.clear();
	TransmitFor(-1);
	CheckEqInt(CountClears(-1, "entity"), 6, "an invalid recipient is not a viewer");

	// An unauthorised player cannot toggle ESP, gets the phrase, and no permissions/leak happen.
	const std::size_t chatBefore = g_fakeChat.size();
	Check(InvokeCommand("esp", 1, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Handled,
		  "the chat line is still handled (swallowed) for an unauthorised player");
	Check(!g_viewers[1].enabled, "an unauthorised player never switches ESP on");
	Check(g_fakeChat.size() == chatBefore + 1, "the refusal phrase was printed");
	if (g_fakeChat.size() > chatBefore) {
		Check(g_fakeChat.back().slot == 1, "the refusal goes to the caller");
		Check(g_fakeChat.back().text.find("\x02[ESP]\x01") != std::string::npos,
			  "the phrase went through the cs2-menus colour table");
		Check(g_fakeChat.back().text.find("Нет доступа") != std::string::npos, "Russian refusal phrase");
	}

	// Losing the permission removes canSee immediately; the same transmit pass hides everything.
	g_fakePermissions.erase({0, "@admin/zalupa"});
	RefreshAccess(0);
	Check(g_viewers[0].access == adminesp::Access::None, "access is dropped when the permission is gone");
	Check(!g_viewers[0].canSee, "the former viewer no longer sees outlines");
	g_fakeTransmitClears.clear();
	TransmitFor(0);
	CheckEqInt(CountClears(0, "entity"), 6, "the former viewer now gets every outline cleared");

	// The outline entities are removed once nobody has ESP enabled any more.
	Reconciles(4);
	Check(!HasGlow(0) && !HasGlow(1) && !HasGlow(2), "without a viewer all outlines are destroyed");
}

void TestDeadOnlyAccess() {
	Section("B2. dead-only access, spectating and a controlled bot");
	PrepareTest();
	StartPlugin();
	MapStart();
	FakeClient dead = Human(kSteamAdmin, adminesp::kTeamT, false);
	SetClient(0, dead);
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	Grant(0, "@admin/spec");
	EnableEsp(0);

	Check(g_viewers[0].access == adminesp::Access::DeadOnly, "the dead-only flag gives DeadOnly");
	Check(g_viewers[0].canSee, "a dead admin sees outlines");

	// Alive in a team: no outlines.
	g_fakeClients[0].alive = true;
	g_fakeClients[0].team = adminesp::kTeamT;
	Advance(300);
	Check(!g_viewers[0].canSee, "a living dead-only admin does not see outlines");

	// Dead but controlling a live bot: m_bControllingBot makes PlaysAlive true.
	g_fakeClients[0].alive = false;
	g_fakeClients[0].controllingBot = true;
	Advance(300);
	Check(!g_viewers[0].canSee, "controlling a live bot counts as alive for the dead-only rule");
	Check(g_fakeSchemaCalls > 0, "m_bControllingBot was resolved through the schema offset");

	// Spectating: team 1, even while IsClientAlive() says alive.
	g_fakeClients[0].controllingBot = false;
	g_fakeClients[0].alive = true;
	g_fakeClients[0].team = adminesp::kTeamSpectator;
	Advance(300);
	Check(g_viewers[0].canSee, "a spectator sees outlines with dead-only access");

	// A bot can never be an admin: no access even with a matching permission entry.
	FakeClient bot = Bot(adminesp::kTeamT);
	SetClient(2, bot);
	Grant(2, "@admin/spec");
	RefreshAccess(2);
	Check(g_viewers[2].access == adminesp::Access::None, "a bot slot never gets admin access");
}

// ==========================================================================================
// C. signed handles and the retire list
// ==========================================================================================

void TestSignedHandlesAndRetired() {
	Section("C. signed handles: creation, keep, removal, retire, prune");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Reconciles(1);

	const int32_t relay = g_glows[1].relay;
	const int32_t glow = g_glows[1].glow;
	Check(relay < 0 && glow < 0, "handles with bit 31 set are negative");
	Check(adminesp::IsValidHandle(relay) && adminesp::IsValidHandle(glow), "but they are valid");
	Reconciles(3);
	Check(g_glows[1].relay == relay && g_glows[1].glow == glow,
		  "the reconcile does not rebuild a negative handle pair");

	// Death destroys the pair and retires both handles.
	InvokeEvent("player_death", 1);
	Check(!HasGlow(1), "the outline is gone after the death event");
	Check(std::find(g_fakeRemovedHandles.begin(), g_fakeRemovedHandles.end(), relay) !=
			  g_fakeRemovedHandles.end(),
		  "the negative glow handle was passed to RemoveEntity");
	Check(RetiredContains(relay) && RetiredContains(glow), "both removed handles are retired");

	// Until the engine frees them, a non-viewer pass still clears the retired handles.
	g_fakeTransmitClears.clear();
	TransmitFor(5);
	Check(Cleared(5, relay, "entity") && Cleared(5, glow, "entity"),
		  "retired handles are still hidden after removal");
	Check(Cleared(5, relay, "nonplayer"), "and marked as non-transmitted");

	// The engine frees them: the next reconcile prunes the retire list.
	FlushRemovals();
	Reconciles(1);
	Check(!RetiredContains(relay) && !RetiredContains(glow), "freed handles are pruned from the retire list");
	g_fakeTransmitClears.clear();
	TransmitFor(5);
	Check(!Cleared(5, relay, "entity"), "a freed handle is no longer cleared");
}

// ==========================================================================================
// D. pending rebuild lifecycle by slot identity
// ==========================================================================================

void TestPendingIdentity() {
	Section("D. spawn/round pending rebuild is tied to the slot identity");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetClient(0, Human(kSteamAdmin, adminesp::kTeamT));
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Reconciles(1);
	Check(HasGlow(1), "the player has an outline before the spawn event");

	// The spawn event arms a 0.3 s rebuild for that player only.
	InvokeEvent("player_spawn", 1);
	Check(!HasGlow(1), "the spawn event drops the old outline");
	const float armedAt = g_fakeGameTime;
	Check(g_glows[1].identity == kSteamPlayer, "the pending rebuild is tagged with the spawner's steam id");
	Check(g_glows[1].createAfter > armedAt, "a spawn delay is armed");

	// Another player takes the slot before the delay elapses: the pending rebuild is dropped and
	// the new occupant is handled immediately (PITFALLS B6: no reliance on the auth callback).
	SetClient(1, Human(kSteamFourth, adminesp::kTeamT));
	Advance(250);
	Check(HasGlow(1), "the new occupant is not held back by the previous player's spawn delay");
	Check(g_glows[1].identity == kSteamFourth, "the outline belongs to the new occupant");

	// An observed disconnect drops the outline, the access decision and the ESP switch.
	g_fakeClients[1].connected = false;
	Advance(250);
	Check(!HasGlow(1), "a disconnect destroys the outline");
	Check(!g_viewers[1].connected, "the slot is observed as disconnected");
	Check(g_viewers[1].access == adminesp::Access::None, "the access decision is dropped");
	Check(!g_viewers[1].enabled, "the ESP switch is reset to the config default");

	// The same reset happens through the event and through OnClientAuthenticated.
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	Advance(250);
	Check(g_viewers[1].connected, "the returning player is observed");
	InvokeEvent("player_disconnect", 1);
	Check(!g_viewers[1].connected && !g_viewers[1].enabled, "the disconnect event resets the slot");
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	Advance(250);
	Authenticate(1, kSteamPlayer);
	Check(g_viewers[1].identity == kSteamPlayer, "OnClientAuthenticated tags the new identity");
	Check(!g_viewers[1].enabled, "OnClientAuthenticated resets the ESP switch");

	// round_prestart arms the rebuild for the current identity; a replacement in the slot is not
	// delayed by it.
	InvokeEvent("round_prestart", -1);
	Check(g_glows[1].createAfter > g_fakeGameTime, "round_prestart arms the round delay");
	Check(g_glows[1].identity == kSteamPlayer, "the round delay is tagged with the current identity");
	SetClient(1, Human(kSteamFourth, adminesp::kTeamCT));
	Advance(250);
	Check(HasGlow(1), "the replacement is not held back by the round delay");
	Check(g_glows[1].identity == kSteamFourth, "the outline belongs to the replacement");
}

// ==========================================================================================
// E. commands: contexts, ownership, reload
// ==========================================================================================

void TestCommandContexts() {
	Section("E. commands: contexts and chat behaviour");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	Advance(300);

	Check(InvokeCommand("esp", 0, s2sdk::ConCommandContext::Console) == s2sdk::ResultType::Continue,
		  "a console 'esp' is not ours");
	Check(!g_viewers[0].enabled, "and it does nothing");
	Check(InvokeCommand("mm_esp", 0, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Continue,
		  "a chat '!mm_esp' is not one of the original's chat spellings");
	Check(!g_viewers[0].enabled, "and it does nothing");

	Check(InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Handled,
		  "the chat '!esp' is handled and swallowed");
	Check(g_viewers[0].enabled, "and it enables ESP");

	// The console spelling toggles back off, and the server console (-1) is still not handled.
	Check(InvokeCommand("mm_esp", -1, s2sdk::ConCommandContext::Console) == s2sdk::ResultType::Continue,
		  "the server console has no player slot");
	Check(InvokeCommand("mm_esp", 0, s2sdk::ConCommandContext::Console) == s2sdk::ResultType::Handled,
		  "the console spelling is handled for the admin");
	Check(!g_viewers[0].enabled, "and it toggles ESP off");

	// The flags pisexcompat gave a RegCommand name.
	for (const auto& [name, command] : g_fakeCommands) {
		const int64_t flags = static_cast<int64_t>(command.flags);
		Check((flags & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0,
			  name + " keeps the Release flag");
		Check((flags & static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0,
			  name + " keeps the ClientCanExecute flag");
	}
}

void TestCommandOwnership() {
	Section("E2. command ownership and teardown");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Reconciles(1);

	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;
	Check(!g_fakeRemovedCommands.empty(), "an owned command was removed with RemoveCommand");
	Check(g_fakeRemovedListeners.empty(), "no RemoveCommandListener is used for owned commands");
	bool mmRemoved = false;
	bool espRemoved = false;
	for (const std::string& name : g_fakeRemovedCommands) {
		mmRemoved = mmRemoved || name == "mm_esp";
		espRemoved = espRemoved || name == "esp";
	}
	Check(mmRemoved && espRemoved, "both owned commands were removed");

	// A pre-existing name: s2sdk attached our callback as a listener, so its teardown must use
	// RemoveCommandListener and never RemoveCommand.
	PrepareTest();
	g_fakeCommandOwned["esp"] = false;
	StartPlugin();
	MapStart();
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;
	Check(g_fakeRemovedListeners.size() == 1 && g_fakeRemovedListeners[0] == "esp",
		  "a pre-existing 'esp' is detached with RemoveCommandListener");
	bool mmOwnedRemoved = false;
	bool espOwnedRemoved = false;
	for (const std::string& name : g_fakeRemovedCommands) {
		mmOwnedRemoved = mmOwnedRemoved || name == "mm_esp";
		espOwnedRemoved = espOwnedRemoved || name == "esp";
	}
	Check(mmOwnedRemoved && !espOwnedRemoved, "RemoveCommand is only used for the name this plugin created");
}

void TestReload() {
	Section("E3. reload registers everything exactly once");
	PrepareTest();
	StartPlugin();
	MapStart();
	CheckEqInt(g_fakeMapStartRegisters, 1, "one map-start registration before the reload");
	CheckEqInt(g_fakeTransmitRegisters, 1, "one transmit registration before the reload");
	CheckEqInt(static_cast<int>(g_fakeCommands.size()), 2, "both commands before the reload");
	CheckEqInt(static_cast<int>(g_fakeEvents.size()), 5, "five events before the reload");

	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;
	StartPlugin();
	MapStart();
	CheckEqInt(g_fakeMapStartRegisters, 2, "the map listeners are registered again");
	CheckEqInt(g_fakeTransmitRegisters, 2, "the transmit callback is registered again");
	CheckEqInt(g_fakeAuthRegisters, 2, "OnClientAuthenticated is registered again");
	CheckEqInt(static_cast<int>(g_fakeCommands.size()), 2, "both commands exist exactly once");
	CheckEqInt(static_cast<int>(g_fakeEvents.size()), 5, "five events exist exactly once");

	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Reconciles(1);
	Check(HasGlow(0) && HasGlow(1) && HasGlow(2), "the reloaded plugin creates outlines again");
}

// ==========================================================================================
// F. teardown
// ==========================================================================================

void TestTeardown() {
	Section("F. teardown removes every registration and every entity");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Reconciles(1);
	const int32_t relay = g_glows[1].relay;
	const int32_t glow = g_glows[1].glow;

	const std::size_t removedBefore = g_fakeRemovedHandles.size();
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	Check(g_fakeTransmit == nullptr, "the CheckTransmit callback is unregistered");
	CheckEqInt(g_fakeTransmitUnregisters, 1, "exactly one CheckTransmit unregistration");
	Check(g_fakeMapStart == nullptr && g_fakeMapEnd == nullptr, "the map listeners are unregistered");
	CheckEqInt(g_fakeMapStartUnregisters, 1, "one OnMapStart unregistration");
	CheckEqInt(g_fakeMapEndUnregisters, 1, "one OnMapEnd unregistration");
	Check(g_fakeAuth == nullptr, "OnClientAuthenticated is unregistered");
	CheckEqInt(g_fakeAuthUnregisters, 1, "one OnClientAuthenticated unregistration");
	Check(g_fakeAdminConnected == nullptr && g_fakeCoreLoadedCallback == nullptr,
		  "the admin callbacks are unregistered");
	CheckEqInt(g_fakeAdminConnectedUnregisters, 1, "one admin-connected unregistration");
	CheckEqInt(g_fakeCoreLoadedUnregisters, 1, "one admin-core-loaded unregistration");
	Check(g_fakeCommands.empty(), "both commands are detached");
	Check(g_fakeEvents.empty(), "all events are unhooked");
	CheckEqInt(static_cast<int>(g_fakeUnhookedEvents.size()), 5, "five UnhookEvent calls");

	Check(g_fakeRemovedHandles.size() > removedBefore, "the outline entities are removed on unload");
	Check(std::find(g_fakeRemovedHandles.begin(), g_fakeRemovedHandles.end(), relay) !=
			  g_fakeRemovedHandles.end(),
		  "the relay is removed");
	Check(std::find(g_fakeRemovedHandles.begin(), g_fakeRemovedHandles.end(), glow) !=
			  g_fakeRemovedHandles.end(),
		  "the glow is removed");

	Check(!g_canTransmit, "the transmit flag is off after the unload");
	Check(!g_mapReady, "the map gate is shut after the unload");
	Check(g_pAdminApi == nullptr, "the admin API pointer is dropped");
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		Check(!HasGlow(slot), "no outline handle survives the unload");
		Check(g_viewers[slot].access == adminesp::Access::None, "no access decision survives the unload");
		Check(!g_viewers[slot].enabled, "no ESP switch survives the unload");
		Check(!g_viewers[slot].connected, "no slot observation survives the unload");
	}

	// A late callback (should never happen) is inert.
	g_fakeTransmitClears.clear();
	FakeTransmitInfo info{5};
	plg::vector<void*> infos;
	infos.push_back(&info);
	OnCheckTransmit(infos);
	Check(g_fakeTransmitClears.empty(), "a transmit pass after the unload clears nothing");
}

// ==========================================================================================
// G. map gate and schema gate
// ==========================================================================================

void TestMapGateAndSchema() {
	Section("G. map gate and schema lookups");
	PrepareTest();
	StartPlugin();
	CheckEqInt(g_fakeSchemaCalls, 0, "no schema lookup before the first map");
	CheckEqInt(g_fakeGetEntDataCalls, 0, "no entity data read before the first map");

	MapStart();
	SetClient(0, Human(kSteamAdmin, adminesp::kTeamT, false));
	SetClient(1, Human(kSteamPlayer, adminesp::kTeamCT));
	Grant(0, "@admin/zalupa");
	EnableEsp(0);
	Check(g_viewers[0].canSee, "the dead viewer sees outlines");
	Check(g_fakeSchemaCalls > 0, "schema offsets are resolved after the map started");
	CheckEqInt(g_fakeSchemaCallsBeforeMap, 0, "never before the map gate opened");

	const int schemaAfterMap = g_fakeSchemaCalls;
	const int entDataAfterMap = g_fakeGetEntDataCalls;
	const std::size_t createdAfterMap = g_fakeCreatedHandles.size();
	MapEnd();
	SetClient(2, Human(kSteamThird, adminesp::kTeamT));
	Advance(500);
	Check(!g_mapReady, "the gate stays closed between maps");
	CheckEqInt(g_fakeSchemaCalls, schemaAfterMap, "no schema lookup in the map-change window");
	CheckEqInt(g_fakeGetEntDataCalls, entDataAfterMap, "no entity data read in the map-change window");
	CheckEqInt(static_cast<int64_t>(g_fakeCreatedHandles.size()), static_cast<int64_t>(createdAfterMap),
			   "no outline is created in the map-change window");

	// The next map re-opens the gate.
	MapStart();
	Advance(300);
	Check(g_mapReady, "a new OnMapStart opens the gate again");
}

void TestMidMapLoadLatch() {
	Section("G2. a plugin loaded into a running map latches readiness on its first update");
	PrepareTest();
	StartPlugin();
	// No OnMapStart (the plugin was loaded mid-map), and no OnMapEnd was seen since the start.
	SetClient(0, Human(kSteamAdmin, adminesp::kTeamT));
	Grant(0, "@admin/zalupa");
	Advance(300);
	Check(g_mapReady, "the first OnPluginUpdate latched the gate");
	EnableEsp(0);
	Reconciles(1);
	Check(HasGlow(0), "outlines are created on a late-loaded plugin");

	// If an OnMapEnd was seen before the first update, it must not latch.
	PrepareTest();
	StartPlugin();
	MapEnd();
	Advance(500);
	Check(!g_mapReady, "an OnMapEnd before the first update keeps the gate shut");
}

// ==========================================================================================
// H. fail-soft starts
// ==========================================================================================

void TestFailSoft() {
	Section("H. fail-soft starts (PITFALLS A1)");

	Section("H1. no map listeners: the gate never opens");
	PrepareTest();
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	StartPlugin();
	Check(Logged("map listeners are unavailable"), "the missing map listeners are logged");
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	Advance(500);
	Check(!g_mapReady, "without the map listeners the gate stays shut");
	Check(InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Handled,
		  "the chat line is still swallowed");
	Check(!g_viewers[0].enabled, "and no ESP is switched on");
	CheckEqInt(g_fakeCreateEntityCalls, 0, "no outline is created");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;

	Section("H2. missing transmit binding: no outline is ever created");
	PrepareTest();
	__s2sdk_SetTransmitInfoNonPlayer = nullptr;
	StartPlugin();
	Check(!g_canTransmit, "the transmit flag is off");
	Check(Logged("transmit bindings are missing"), "the missing binding is logged");
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat);
	Reconciles(4);
	CheckEqInt(g_fakeCreateEntityCalls, 0, "nothing is created without the ability to hide it");
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	Section("H3. no command manager");
	PrepareTest();
	__s2sdk_AddConsoleCommand = nullptr;
	StartPlugin();
	Check(Logged("AddConsoleCommand is unavailable"), "the missing binding is logged");
	Check(g_fakeCommands.empty(), "no command is registered");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;

	Section("H4. no event manager");
	PrepareTest();
	__s2sdk_HookEvent = nullptr;
	StartPlugin();
	Check(Logged("event bindings are unavailable"), "the missing binding is logged");
	Check(g_fakeEvents.empty(), "no event is hooked");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;

	Section("H5. no admin_system: the command is inert");
	PrepareTest();
	__admin_system_GetPluginVersion = nullptr;
	__admin_system_HasPermission = nullptr;
	__admin_system_HasFlag = nullptr;
	__admin_system_IsAdmin = nullptr;
	__admin_system_IsCoreLoaded = nullptr;
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	Check(InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Handled,
		  "the chat line is still ours");
	Check(!g_viewers[0].enabled, "no ESP without the admin core");
	Reconciles(2);
	CheckEqInt(g_fakeCreateEntityCalls, 0, "no outline without a permission check");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;

	Section("H5b. no ClearTransmitInfoAlways: ESP is disabled before any entity is created");
	PrepareTest();
	__s2sdk_ClearTransmitInfoAlways = nullptr;
	StartPlugin();
	Check(!g_canTransmit, "the always-list binding is mandatory (fail-closed)");
	Check(Logged("transmit bindings are missing"), "the missing binding is logged at startup");
	CheckEqInt(ErrorLogsContaining("AdminESP is disabled"), 1,
			   "exactly one error log says ESP is disabled");
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	Check(InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat) == s2sdk::ResultType::Handled,
		  "the chat line is still handled");
	Reconciles(2);
	CheckEqInt(g_fakeCreateEntityCalls, 0, "no outline entity is created without the always-list clear");
	Check(!HasGlow(0) && !HasGlow(1) && !HasGlow(2), "no outline pair exists");
	Check(!g_viewers[0].enabled, "ESP is not switched on");
	Check(!g_viewers[0].canSee, "the admin is not marked as a viewer");
	Check(g_fakeSentConVars.empty(), "no client-view convar is sent either");
	Check(g_fakeTransmit == nullptr, "the transmit callback is not registered");
	g_fakeTransmitClears.clear();
	TransmitFor(1);
	Check(g_fakeTransmitClears.empty(), "there is no callback to clear anything");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;

	Section("H6. no controller pointer: no outline, no crash");
	PrepareTest();
	__s2sdk_PlayerSlotToEntPointer = nullptr;
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat);
	Reconciles(2);
	Check(!g_viewers[0].canSee, "without the controller pointer nobody sees outlines");
	CheckEqInt(g_fakeCreateEntityCalls, 0, "and nothing is created");
	Check(g_adminEspPlugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd is Ok");
	g_pluginStarted = false;
}

// ==========================================================================================
// I0. raw cwd files win (the pisexcompat ResolvePath/ResolveTranslationsPath order)
// ==========================================================================================

void TestRawCwdPrecedence() {
	Section("I0. a raw addons/configs and addons/translations file in cwd wins");
	PrepareTest();
	const std::filesystem::path originalCwd = std::filesystem::current_path();
	const std::filesystem::path raw =
		std::filesystem::temp_directory_path() /
		("adminesp_raw_cwd_" + std::to_string(static_cast<long long>(::getpid())));
	std::error_code error;
	std::filesystem::remove_all(raw, error);
	std::filesystem::create_directories(raw / "addons/configs", error);
	std::filesystem::create_directories(raw / "addons/translations", error);
	WriteTextFile((raw / "addons/configs/adminesp.ini").string(), R"("Config"
{
    "admin_flag_all" "@admin/raw"
    "admin_flag_death" "@admin/raw_spec"
    "ct_color" "1 2 3"
}
)");
	WriteTextFile((raw / "addons/translations/AdminESP.phrases.txt").string(), R"("Phrases"
{
    "esp_no_access"
    {
        "ru" "RAW NO ACCESS"
    }
}
)");

	// <configs>/AdminESP still holds the production files; the raw cwd files must be chosen first.
	std::filesystem::current_path(raw);
	StartPlugin();
	CheckEqStr(g_settings.flagAlways, "@admin/raw", "the raw cwd adminesp.ini wins (flagged)");
	CheckEqStr(g_settings.flagDeadOnly, "@admin/raw_spec", "the raw cwd adminesp.ini wins (dead-only)");
	CheckEqInt(g_settings.colorCT.r, 1, "the raw cwd colour wins");
	const auto phrase = g_phrases.find("esp_no_access");
	Check(phrase != g_phrases.end(), "the raw cwd phrase file was loaded");
	if (phrase != g_phrases.end()) {
		CheckEqStr(phrase->second, "RAW NO ACCESS", "the raw cwd phrase table wins (localisation)");
	}
	MapStart();
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;
	std::filesystem::current_path(originalCwd);
	std::filesystem::remove_all(raw, error);
}

// ==========================================================================================
// I. config / phrase failures and the language chain
// ==========================================================================================

void TestConfigFailures() {
	Section("I. config and phrase failures keep ESP off / the keys visible");

	Section("I1. malformed adminesp.ini");
	PrepareTest();
	WriteTextFile(ConfigPath(), "\"Config\" { \"admin_flag_all\" \"@admin/zalupa\" ");
	StartPlugin();
	Check(Logged("Failed to parse AdminESP config"), "the malformed config is logged");
	CheckEqStr(g_settings.flagAlways, "", "a malformed config grants no flag");
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");
	InvokeCommand("esp", 0, s2sdk::ConCommandContext::Chat);
	Check(!g_viewers[0].enabled, "no ESP without a readable config");
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	Section("I2. missing adminesp.ini");
	PrepareTest();
	std::error_code error;
	std::filesystem::remove(ConfigPath(), error);
	StartPlugin();
	Check(Logged("Failed to load AdminESP config"), "the missing config is logged");
	CheckEqStr(g_settings.flagAlways, "", "no flag without the file");
	CheckEqInt(g_settings.colorCT.r, 0x49, "the colours stay at their defaults");
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	Section("I3. missing phrases file: Phrase() falls back to the key");
	PrepareTest();
	std::filesystem::remove(PhrasesPath(), error);
	StartPlugin();
	Check(Logged("Failed to load phrases"), "the missing phrase file is logged");
	MapStart();
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	Section("I4. language chain: core.cfg -> s2sdk -> en");
	PrepareTest();
	RemoveCoreConfig();
	g_fakeServerLanguage = "english";
	StartPlugin();
	const auto englishPhrase = g_phrases.find("esp_no_access");
	Check(englishPhrase != g_phrases.end(), "the phrases were loaded");
	if (englishPhrase != g_phrases.end()) {
		Check(englishPhrase->second.find("You have no ESP access") != std::string::npos,
			  "s2sdk's ServerLanguage is the second link of the chain");
	}
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;

	PrepareTest();
	RemoveCoreConfig();
	g_fakeServerLanguage.clear();
	StartPlugin();
	const auto fallbackPhrase = g_phrases.find("esp_no_access");
	Check(fallbackPhrase != g_phrases.end(), "the phrases were loaded without any language source");
	if (fallbackPhrase != g_phrases.end()) {
		Check(fallbackPhrase->second.find("You have no ESP access") != std::string::npos,
			  "the chain falls back to 'en'");
	}
	g_adminEspPlugin.OnPluginEnd();
	g_pluginStarted = false;
}

// ==========================================================================================
// J. menu item and client-view restore
// ==========================================================================================

void TestMenuAndClientView() {
	Section("J. admin menu item and the client-view restore");
	PrepareTest();
	StartPlugin();
	MapStart();
	SetUpThreePlayers();
	Grant(0, "@admin/zalupa");

	const auto item = g_fakeItems.find("adminesp");
	Check(item != g_fakeItems.end(), "the menu item exists");
	if (item != g_fakeItems.end()) {
		plg::string text;
		item->second.display(0, ToPlg("players"), ToPlg("adminesp"), text);
		CheckEqStr(ToStd(text), "ESP [выкл]", "the item shows the off phrase (ru)");

		item->second.select(0, ToPlg("players"), ToPlg("adminesp"), ToPlg(""));
		Check(g_viewers[0].enabled, "selecting the item toggles ESP on");
		Check(!g_fakeShownCategories.empty() && g_fakeShownCategories.back().first == 0,
			  "and re-opens the players category");
		plg::string onText;
		item->second.display(0, ToPlg("players"), ToPlg("adminesp"), onText);
		CheckEqStr(ToStd(onText), "ESP [вкл]", "the item shows the on phrase after the toggle");

		// An unauthorised player selecting the item is refused and does not toggle.
		item->second.select(1, ToPlg("players"), ToPlg("adminesp"), ToPlg(""));
		Check(!g_viewers[1].enabled, "an unauthorised menu selection does not toggle");
	}

	// Toggling off restores the server's convar values on the viewer's client.
	g_fakeSentConVars.clear();
	Check(InvokeCommand("mm_esp", 0, s2sdk::ConCommandContext::Console) == s2sdk::ResultType::Handled,
		  "the console toggle is handled");
	Check(!g_viewers[0].enabled, "ESP is now off");
	bool restoredForceCamera = false;
	bool restoredPlayerId = false;
	for (const FakeSentConVar& sent : g_fakeSentConVars) {
		if (sent.slot == 0 && sent.name == "mp_forcecamera" && sent.value == "1") {
			restoredForceCamera = true;
		}
		if (sent.slot == 0 && sent.name == "mp_playerid" && sent.value == "1") {
			restoredPlayerId = true;
		}
	}
	Check(restoredForceCamera && restoredPlayerId, "the server values are restored on the client");
}

} // namespace

int main() {
	SetUpConfigFiles();
	InstallFakes();

	TestStartupAndRegistration();
	TestProtectedEsp();
	TestDeadOnlyAccess();
	TestSignedHandlesAndRetired();
	TestPendingIdentity();
	TestCommandContexts();
	TestCommandOwnership();
	TestReload();
	TestTeardown();
	TestMapGateAndSchema();
	TestMidMapLoadLatch();
	TestFailSoft();
	TestRawCwdPrecedence();
	TestConfigFailures();
	TestMenuAndClientView();

	// Leave the plugin unloaded for the final report and remove the isolated config fixture.
	if (g_pluginStarted) {
		g_adminEspPlugin.OnPluginEnd();
		g_pluginStarted = false;
	}
	std::error_code cleanupError;
	std::filesystem::remove_all(g_configsDir, cleanupError);

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
