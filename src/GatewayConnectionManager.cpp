#include "GatewayConnectionManager.h"

const char* const TAG = "GatewayConnectionManager";

#include "visual/VisualStateManager.h"
#include "visual/OledDisplayManager.h"

#include "captiveportal/Manager.h"
#include "config/Config.h"
#include "Core.h"
#include "GatewayClient.h"
#include "http/JsonAPI.h"
#include "Logging.h"
#include "serialization/WSLocal.h"

#include "SimpleMutex.h"

#include <atomic>
#include <algorithm>
#include <memory>
#include <Preferences.h>
#include <unordered_set>
#include <unordered_map>

//
//  ######  ########  ######  ##     ## ########  #### ######## ##    ##    ########  ####  ######  ##    ##
// ##    ## ##       ##    ## ##     ## ##     ##  ##     ##     ##  ##     ##     ##  ##  ##    ## ##   ##
// ##       ##       ##       ##     ## ##     ##  ##     ##      ####      ##     ##  ##  ##       ##  ##
//  ######  ######   ##       ##     ## ########   ##     ##       ##       ########   ##   ######  #####
//       ## ##       ##       ##     ## ##   ##    ##     ##       ##       ##   ##    ##        ## ##  ##
// ##    ## ##       ##    ## ##     ## ##    ##   ##     ##       ##       ##    ##   ##  ##    ## ##   ##
//  ######  ########  ######   #######  ##     ## ####    ##       ##       ##     ## ####  ######  ##    ##
//
// TODO: Fix loading CA Certificate bundles, currently fails with "[esp_crt_bundle.c:161] esp_crt_bundle_init(): Unable to allocate memory for bundle"
// This is probably due to the fact that the bundle is too large for the ESP32's heap or the bundle is incorrectly packedy them
//
#warning SSL certificate verification is currently not implemented, by RFC definition this is a security risk, and allows for MITM attacks, but the realistic risk is low

const char* const AUTH_TOKEN_FILE = "/authToken";

const uint8_t FLAG_NONE   = 0;
const uint8_t FLAG_HAS_IP = 1 << 0;
const uint8_t FLAG_LINKED = 1 << 1;

const uint8_t LINK_CODE_LENGTH = 6;

static std::atomic<uint8_t> s_flags                 = 0;
static std::atomic<int64_t> s_lastAuthFailure       = 0;
static std::atomic<int64_t> s_lastConnectionAttempt = 0;
static std::atomic<int64_t> s_lastHubInfoRefreshAttempt = 0;
static std::atomic<bool> s_lastGatewayConnected         = false;
static std::atomic<bool> s_hasEverConnected             = false;
static std::atomic<bool> s_onlineShockersLoaded = false;
static int64_t s_firstWifiConnectedAt = 0;
static std::atomic<int64_t> s_reconnectDelayUntilMs { 0 };
static std::atomic_flag s_isInitializing            = ATOMIC_FLAG_INIT;
static OpenShock::SimpleMutex s_clientMutex;
static OpenShock::SimpleMutex s_onlineShockersMutex;
static std::shared_ptr<OpenShock::GatewayClient> s_wsClient = nullptr;
static std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> s_onlineShockers;
static std::unordered_set<uint16_t> s_localRfIds;

const char* const ONLINE_SHOCKERS_PREFS_NAMESPACE = "gw_shockers";
const uint8_t ONLINE_SHOCKERS_MAX = 32;

static uint16_t FindFreeRfId(const std::unordered_set<uint16_t>& occupied, uint16_t preferredStart)
{
  for (uint32_t candidate = std::max<uint16_t>(preferredStart, 1); candidate <= UINT16_MAX; ++candidate) {
    if (occupied.find(static_cast<uint16_t>(candidate)) == occupied.end()) {
      return static_cast<uint16_t>(candidate);
    }
  }

  for (uint32_t candidate = 1; candidate < std::max<uint16_t>(preferredStart, 1); ++candidate) {
    if (occupied.find(static_cast<uint16_t>(candidate)) == occupied.end()) {
      return static_cast<uint16_t>(candidate);
    }
  }

  return preferredStart;
}

static void SaveOnlineShockersToPrefsUnlocked()
{
  Preferences p;
  if (!p.begin(ONLINE_SHOCKERS_PREFS_NAMESPACE, false)) {
    return;
  }

  const uint8_t count = static_cast<uint8_t>(std::min<std::size_t>(s_onlineShockers.size(), ONLINE_SHOCKERS_MAX));
  p.putUChar("count", count);

  for (uint8_t i = 0; i < count; ++i) {
    char keyId[12] = {};
    char keyName[12] = {};
    char keySrc[12] = {};
    char keyMap[12] = {};
    char keyModel[12] = {};
    char keyDisabled[12] = {};
    char keyLimit[12] = {};

    std::snprintf(keyId, sizeof(keyId), "id%u", i);
    std::snprintf(keyName, sizeof(keyName), "n%u", i);
    std::snprintf(keySrc, sizeof(keySrc), "s%u", i);
    std::snprintf(keyMap, sizeof(keyMap), "m%u", i);
    std::snprintf(keyModel, sizeof(keyModel), "p%u", i);
    std::snprintf(keyDisabled, sizeof(keyDisabled), "d%u", i);
    std::snprintf(keyLimit, sizeof(keyLimit), "l%u", i);

    p.putString(keyId, s_onlineShockers[i].id.c_str());
    p.putString(keyName, s_onlineShockers[i].displayName.c_str());
    p.putUShort(keySrc, s_onlineShockers[i].sourceRfId);
    p.putUShort(keyMap, s_onlineShockers[i].mappedRfId);
    p.putUChar(keyModel, static_cast<uint8_t>(s_onlineShockers[i].model));
    p.putBool(keyDisabled, s_onlineShockers[i].disabled);
    p.putUChar(keyLimit, std::min<uint8_t>(s_onlineShockers[i].limit, 99));
  }

  p.end();
}

static void RemapOnlineShockersUnlocked()
{
  std::unordered_set<uint16_t> occupied = s_localRfIds;

  // Keep source RF IDs if possible. Conflicting IDs are remapped while preserving uniqueness.
  for (auto& shocker : s_onlineShockers) {
    uint16_t mapped = shocker.sourceRfId;

    if (occupied.find(mapped) != occupied.end()) {
      const bool canKeepOldMapping = shocker.mappedRfId != 0 && occupied.find(shocker.mappedRfId) == occupied.end();
      if (canKeepOldMapping) {
        mapped = shocker.mappedRfId;
      } else {
        mapped = FindFreeRfId(occupied, shocker.sourceRfId);
      }
    }

    shocker.mappedRfId = mapped;
    occupied.insert(mapped);
  }
}

static void LoadOnlineShockersFromPrefs()
{
  Preferences p;
  if (!p.begin(ONLINE_SHOCKERS_PREFS_NAMESPACE, true)) {
    return;
  }

  const uint8_t count = std::min<uint8_t>(p.getUChar("count", 0), ONLINE_SHOCKERS_MAX);
  std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> loaded;
  loaded.reserve(count);

  for (uint8_t i = 0; i < count; ++i) {
    char keyId[12] = {};
    char keyName[12] = {};
    char keySrc[12] = {};
    char keyMap[12] = {};
    char keyModel[12] = {};
    char keyDisabled[12] = {};
    char keyLimit[12] = {};

    std::snprintf(keyId, sizeof(keyId), "id%u", i);
    std::snprintf(keyName, sizeof(keyName), "n%u", i);
    std::snprintf(keySrc, sizeof(keySrc), "s%u", i);
    std::snprintf(keyMap, sizeof(keyMap), "m%u", i);
    std::snprintf(keyModel, sizeof(keyModel), "p%u", i);
    std::snprintf(keyDisabled, sizeof(keyDisabled), "d%u", i);
    std::snprintf(keyLimit, sizeof(keyLimit), "l%u", i);

    const String id = p.getString(keyId, "");
    if (id.isEmpty()) {
      continue;
    }

    const uint16_t sourceRfId = p.getUShort(keySrc, 0);
    String displayName = p.getString(keyName, "");
    if (displayName.isEmpty()) {
      displayName = String(sourceRfId);
    }

    const auto model = static_cast<OpenShock::ShockerModelType>(p.getUChar(keyModel, static_cast<uint8_t>(OpenShock::ShockerModelType::CaiXianlin)));
    loaded.push_back(OpenShock::GatewayConnectionManager::OnlineShockerInfo {
      .id = id.c_str(),
      .displayName = displayName.c_str(),
      .sourceRfId = sourceRfId,
      .mappedRfId = p.getUShort(keyMap, sourceRfId),
      .model = model,
      .disabled = p.getBool(keyDisabled, false),
      .limit = std::min<uint8_t>(p.getUChar(keyLimit, 99), 99),
    });
  }

  p.end();

  {
    OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
    s_onlineShockers = std::move(loaded);
    RemapOnlineShockersUnlocked();
    SaveOnlineShockersToPrefsUnlocked();
  }

  s_onlineShockersLoaded.store(true, std::memory_order_relaxed);
}

static bool ReplaceOnlineShockersFromHubInfo(const std::vector<OpenShock::Serialization::JsonAPI::HubInfoResponse::ShockerInfo>& hubShockers)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);

  std::unordered_set<uint16_t> occupied = s_localRfIds;
  std::unordered_map<std::string, uint16_t> previousMapped;
  std::unordered_map<std::string, std::string> previousNames;
  std::unordered_map<std::string, bool> previousDisabled;
  std::unordered_map<std::string, uint8_t> previousLimits;
  previousMapped.reserve(s_onlineShockers.size());
  for (const auto& shocker : s_onlineShockers) {
    previousMapped[shocker.id] = shocker.mappedRfId;
    previousNames[shocker.id] = shocker.displayName;
    previousDisabled[shocker.id] = shocker.disabled;
    previousLimits[shocker.id] = std::min<uint8_t>(shocker.limit, 99);
  }

  std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> next;
  next.reserve(hubShockers.size());

  for (const auto& shocker : hubShockers) {
    uint16_t mapped = shocker.rfId;
    std::string displayName = std::to_string(shocker.rfId);
    bool disabled = false;
    uint8_t limit = 99;

    if (occupied.find(mapped) != occupied.end()) {
      auto prevIt = previousMapped.find(shocker.id);
      if (prevIt != previousMapped.end() && prevIt->second != 0 && occupied.find(prevIt->second) == occupied.end()) {
        mapped = prevIt->second;
      } else {
        mapped = FindFreeRfId(occupied, shocker.rfId);
      }
    }

    auto nameIt = previousNames.find(shocker.id);
    if (nameIt != previousNames.end() && !nameIt->second.empty()) {
      displayName = nameIt->second;
    }

    auto disabledIt = previousDisabled.find(shocker.id);
    if (disabledIt != previousDisabled.end()) {
      disabled = disabledIt->second;
    }

    auto limitIt = previousLimits.find(shocker.id);
    if (limitIt != previousLimits.end()) {
      limit = std::min<uint8_t>(limitIt->second, 99);
    }

    next.push_back(OpenShock::GatewayConnectionManager::OnlineShockerInfo {
      .id = shocker.id,
      .displayName = std::move(displayName),
      .sourceRfId = shocker.rfId,
      .mappedRfId = mapped,
      .model = shocker.model,
      .disabled = disabled,
      .limit = limit,
    });

    occupied.insert(mapped);
  }

  bool changed = next.size() != s_onlineShockers.size();
  if (!changed) {
    for (std::size_t i = 0; i < next.size(); ++i) {
      if (next[i].id != s_onlineShockers[i].id || next[i].sourceRfId != s_onlineShockers[i].sourceRfId || next[i].mappedRfId != s_onlineShockers[i].mappedRfId || next[i].model != s_onlineShockers[i].model) {
        changed = true;
        break;
      }

      if (next[i].displayName != s_onlineShockers[i].displayName || next[i].disabled != s_onlineShockers[i].disabled || next[i].limit != s_onlineShockers[i].limit) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    s_onlineShockers = std::move(next);
    SaveOnlineShockersToPrefsUnlocked();
    OpenShock::OledDisplayManager::RequestRefresh();
  }

  return changed;
}

static std::shared_ptr<OpenShock::GatewayClient> GetClient()
{
  OpenShock::ScopedLock lock__(&s_clientMutex);
  return s_wsClient;
}
static void CreateClient(const std::string& authToken)
{
  OpenShock::ScopedLock lock__(&s_clientMutex);
  s_wsClient = std::make_shared<OpenShock::GatewayClient>(authToken);
}
static void DestroyClient()
{
  OpenShock::ScopedLock lock__(&s_clientMutex);
  s_wsClient = nullptr;
  s_hasEverConnected.store(false, std::memory_order_relaxed);
}

static void evh_gotIP(arduino_event_t* event)
{
  (void)event;

  s_flags.fetch_or(FLAG_HAS_IP, std::memory_order_relaxed);
  if (s_firstWifiConnectedAt == 0) {
    s_firstWifiConnectedAt = OpenShock::millis();
  }
  OS_LOGD(TAG, "Got IP address");
}

static void evh_wiFiDisconnected(arduino_event_t* event)
{
  (void)event;

  s_flags.store(FLAG_NONE, std::memory_order_relaxed);
  DestroyClient();
  OS_LOGD(TAG, "Lost IP address");
}

static bool checkIsDeAuthRateLimited(int64_t millis)
{
  return s_lastAuthFailure != 0 && (millis - s_lastAuthFailure) < 300'000;  // 5 Minutes
}
static bool checkIsConnectionRateLimited(int64_t millis)
{
  return s_lastConnectionAttempt != 0 && (millis - s_lastConnectionAttempt) < 20'000;  // 20 seconds
}

using namespace OpenShock;
namespace JsonAPI = OpenShock::Serialization::JsonAPI;

bool GatewayConnectionManager::Init()
{
  WiFi.onEvent(evh_gotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(evh_gotIP, ARDUINO_EVENT_WIFI_STA_GOT_IP6);
  WiFi.onEvent(evh_wiFiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  LoadOnlineShockersFromPrefs();

  return true;
}

bool GatewayConnectionManager::IsConnected()
{
  auto client = GetClient();
  if (client == nullptr) {
    return false;
  }

  return client->state() == GatewayClientState::Connected;
}

bool GatewayConnectionManager::IsLinked()
{
  return (s_flags.load(std::memory_order_relaxed) & FLAG_LINKED) != 0;
}

AccountLinkResultCode GatewayConnectionManager::Link(std::string_view linkCode, bool persistConfig)
{
  if ((s_flags.load(std::memory_order_relaxed) & FLAG_HAS_IP) == 0) {
    return AccountLinkResultCode::NoInternetConnection;
  }

  DestroyClient();

  OS_LOGD(TAG, "Attempting to link to account using code %.*s", linkCode.length(), linkCode.data());

  if (linkCode.length() != LINK_CODE_LENGTH) {
    OS_LOGE(TAG, "Invalid link code length");
    return AccountLinkResultCode::InvalidCode;
  }

  auto response = HTTP::JsonAPI::LinkAccount(linkCode);

  if (response.code == 404) {
    return AccountLinkResultCode::InvalidCode;
  }

  if (response.result == HTTP::RequestResult::RateLimited) {
    OS_LOGW(TAG, "Account Link request got ratelimited");
    return AccountLinkResultCode::RateLimited;
  }
  if (response.result != HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Error while getting auth token: %s %d", response.ResultToString(), response.code);

    return AccountLinkResultCode::InternalError;
  }

  if (response.code != 200) {
    OS_LOGE(TAG, "Unexpected response code: %d", response.code);
    return AccountLinkResultCode::InternalError;
  }

  if (response.data.authToken.empty()) {
    OS_LOGE(TAG, "Received empty auth token");
    return AccountLinkResultCode::InternalError;
  }

  if (persistConfig) {
    if (!Config::SetBackendAuthToken(std::move(response.data.authToken))) {
      OS_LOGE(TAG, "Failed to save auth token");
      return AccountLinkResultCode::InternalError;
    }
  } else {
    OS_LOGI(TAG, "Linked account in runtime-only mode; auth token not persisted");
  }

  s_flags.fetch_or(FLAG_LINKED, std::memory_order_relaxed);
  OS_LOGD(TAG, "Successfully linked to account");

  return AccountLinkResultCode::Success;
}
void GatewayConnectionManager::UnLink()
{
  s_flags.fetch_and(static_cast<uint8_t>(~FLAG_LINKED), std::memory_order_relaxed);
  DestroyClient();
  Config::ClearBackendAuthToken();
}

bool GatewayConnectionManager::SendMessageTXT(std::string_view data)
{
  auto client = GetClient();
  if (client == nullptr) {
    return false;
  }

  return client->sendMessageTXT(data);
}

bool GatewayConnectionManager::SendMessageBIN(tcb::span<const uint8_t> data)
{
  auto client = GetClient();
  if (client == nullptr) {
    return false;
  }

  return client->sendMessageBIN(data);
}

void GatewayConnectionManager::MarkPingReceived()
{
  auto client = GetClient();
  if (client == nullptr) {
    return;
  }

  client->markPingReceived();
}

std::vector<GatewayConnectionManager::OnlineShockerInfo> GatewayConnectionManager::GetOnlineShockers()
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
  return s_onlineShockers;
}

uint16_t GatewayConnectionManager::ResolveOnlineRfId(uint16_t sourceRfId)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
  for (const auto& shocker : s_onlineShockers) {
    if (shocker.sourceRfId == sourceRfId) {
      return shocker.mappedRfId;
    }
  }

  return sourceRfId;
}

bool GatewayConnectionManager::ResolveOnlineRfId(uint16_t sourceRfId, uint16_t& outMappedRfId)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
  for (const auto& shocker : s_onlineShockers) {
    if (shocker.sourceRfId == sourceRfId) {
      outMappedRfId = shocker.mappedRfId;
      return true;
    }
  }

  return false;
}

bool GatewayConnectionManager::IsOnlineRfIdReserved(uint16_t rfId)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
  for (const auto& shocker : s_onlineShockers) {
    if (shocker.sourceRfId == rfId || shocker.mappedRfId == rfId) {
      return true;
    }
  }

  return false;
}

void GatewayConnectionManager::SetLocalRfIds(tcb::span<const uint16_t> rfIds)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);

  s_localRfIds.clear();
  s_localRfIds.reserve(rfIds.size());
  for (uint16_t rfId : rfIds) {
    if (rfId != 0) {
      s_localRfIds.insert(rfId);
    }
  }

  // OLED/local config initializes before the gateway cache is loaded from prefs.
  // In that phase we only want to remember local RF IDs, not overwrite cached online shockers.
  if (!s_onlineShockersLoaded.load(std::memory_order_relaxed)) {
    return;
  }

  std::vector<OnlineShockerInfo> before = s_onlineShockers;
  RemapOnlineShockersUnlocked();
  SaveOnlineShockersToPrefsUnlocked();

  bool changed = before.size() != s_onlineShockers.size();
  if (!changed) {
    for (std::size_t i = 0; i < before.size(); ++i) {
      if (before[i].mappedRfId != s_onlineShockers[i].mappedRfId || before[i].sourceRfId != s_onlineShockers[i].sourceRfId || before[i].id != s_onlineShockers[i].id || before[i].model != s_onlineShockers[i].model) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    OpenShock::OledDisplayManager::RequestRefresh();
  }
}

bool GatewayConnectionManager::SetOnlineShockerDisplayName(std::string_view id, std::string_view displayName)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);

  for (auto& shocker : s_onlineShockers) {
    if (shocker.id != id) {
      continue;
    }

    shocker.displayName = displayName;
    SaveOnlineShockersToPrefsUnlocked();
    OpenShock::OledDisplayManager::RequestRefresh();
    return true;
  }

  return false;
}

bool GatewayConnectionManager::SetOnlineShockerDisabled(std::string_view id, bool disabled)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);

  for (auto& shocker : s_onlineShockers) {
    if (shocker.id != id) {
      continue;
    }

    shocker.disabled = disabled;
    SaveOnlineShockersToPrefsUnlocked();
    OpenShock::OledDisplayManager::RequestRefresh();
    return true;
  }

  return false;
}

bool GatewayConnectionManager::SetOnlineShockerLimit(std::string_view id, uint8_t limit)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);
  const uint8_t clamped = std::min<uint8_t>(limit, 99);

  for (auto& shocker : s_onlineShockers) {
    if (shocker.id != id) {
      continue;
    }

    shocker.limit = clamped;
    SaveOnlineShockersToPrefsUnlocked();
    OpenShock::OledDisplayManager::RequestRefresh();
    return true;
  }

  return false;
}

bool GatewayConnectionManager::RemoveOnlineShocker(std::string_view id)
{
  OpenShock::ScopedLock lock__(&s_onlineShockersMutex);

  const auto oldSize = s_onlineShockers.size();
  s_onlineShockers.erase(
    std::remove_if(s_onlineShockers.begin(), s_onlineShockers.end(), [id](const auto& shocker) { return shocker.id == id; }),
    s_onlineShockers.end()
  );

  if (s_onlineShockers.size() == oldSize) {
    return false;
  }

  SaveOnlineShockersToPrefsUnlocked();
  OpenShock::OledDisplayManager::RequestRefresh();
  return true;
}

bool FetchHubInfo(std::string authToken, bool verboseLogs)
{
  // TODO: this function is very slow, should be optimized!
  if ((s_flags.load(std::memory_order_relaxed) & FLAG_HAS_IP) == 0) {
    return false;
  }

  if (checkIsDeAuthRateLimited(OpenShock::millis())) {
    return false;
  }

  auto response = HTTP::JsonAPI::GetHubInfo(std::move(authToken));

  if (response.code == 401) {
    OS_LOGD(TAG, "Auth token is invalid, waiting 5 minutes before checking again");
    s_lastAuthFailure = OpenShock::millis();
    return false;
  }

  if (response.result == HTTP::RequestResult::RateLimited) {
    return false;  // Just return false, don't spam the console with errors
  }
  if (response.result != HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Error while fetching hub info: %s %d", response.ResultToString(), response.code);
    return false;
  }

  if (response.code != 200) {
    OS_LOGE(TAG, "Unexpected response code: %d", response.code);
    return false;
  }

  const bool changed = ReplaceOnlineShockersFromHubInfo(response.data.shockers);
  s_lastHubInfoRefreshAttempt = OpenShock::millis();

  if (verboseLogs) {
    OS_LOGI(TAG, "Hub ID:   %s", response.data.hubId.c_str());
    OS_LOGI(TAG, "Hub Name: %s", response.data.hubName.c_str());
    OS_LOGI(TAG, "Shockers:");
    for (auto& shocker : response.data.shockers) {
      OS_LOGI(TAG, "  [%s] rf=%u model=%u", shocker.id.c_str(), shocker.rfId, shocker.model);
    }
  } else if (changed) {
    OS_LOGI(TAG, "Online shocker list updated (%u entries)", response.data.shockers.size());
  }

  s_flags.fetch_or(FLAG_LINKED, std::memory_order_relaxed);

  return true;
}

bool GatewayConnectionManager::RefreshOnlineShockers()
{
  if ((s_flags.load(std::memory_order_relaxed) & FLAG_HAS_IP) == 0) {
    return false;
  }

  if (!Config::HasBackendAuthToken()) {
    return false;
  }

  std::string authToken;
  if (!Config::GetBackendAuthToken(authToken)) {
    return false;
  }

  return FetchHubInfo(std::move(authToken), false);
}

bool StartConnectingToLCG()
{
  auto client = GetClient();
  if (client == nullptr) {
    OS_LOGD(TAG, "wsClient is null");
    return false;
  }

  if (client->state() != GatewayClientState::Disconnected) {
    OS_LOGD(TAG, "WebSocketClient is not disconnected, waiting...");
    client->disconnect();
    return false;
  }

  int64_t msNow = OpenShock::millis();
  if (checkIsDeAuthRateLimited(msNow) || checkIsConnectionRateLimited(msNow)) {
    return false;
  }
  s_lastConnectionAttempt = msNow;

  if (!Config::HasBackendAuthToken()) {
    OS_LOGD(TAG, "No auth token, can't connect to LCG");
    return false;
  }

  std::string authToken;
  if (!Config::GetBackendAuthToken(authToken)) {
    OS_LOGE(TAG, "Failed to get auth token");
    return false;
  }

  auto response = HTTP::JsonAPI::AssignLcg(std::move(authToken));

  if (response.code == 401) {
    OS_LOGD(TAG, "Auth token is invalid, waiting 5 minutes before retrying");
    s_lastAuthFailure = OpenShock::millis();
    return false;
  }

  if (response.result == HTTP::RequestResult::RateLimited) {
    return false;  // Just return false, don't spam the console with errors
  }
  if (response.result != HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Error while fetching LCG endpoint: %s %d", response.ResultToString(), response.code);
    return false;
  }

  if (response.code != 200) {
    OS_LOGE(TAG, "Unexpected response code: %d", response.code);
    return false;
  }

  OS_LOGI(TAG, "Connecting to LCG endpoint { host: '%s', port: %hu, path: '%s' } in country %s", response.data.host.c_str(), response.data.port, response.data.path.c_str(), response.data.country.c_str());
  client->connect(response.data.host, response.data.port, response.data.path);

  return true;
}

void InitializeClient()
{
  DestroyClient();

  // No client — check prerequisites
  if ((s_flags.load(std::memory_order_relaxed) & FLAG_HAS_IP) == 0 || !Config::HasBackendAuthToken()) {
    return;
  }

  std::string authToken;
  if (!Config::GetBackendAuthToken(authToken)) {
    OS_LOGE(TAG, "Failed to get auth token");
    return;
  }

  const bool fetchOk = FetchHubInfo(authToken, true);
  if (!fetchOk && checkIsDeAuthRateLimited(OpenShock::millis())) {
    return;  // 401 auth failure — token is invalid, do not connect
  }

  if (fetchOk) {
    s_flags.fetch_or(FLAG_LINKED, std::memory_order_relaxed);
    OS_LOGD(TAG, "Successfully verified auth token");
    Serialization::Local::SerializeAccountLinkStatusEvent(true, CaptivePortal::BroadcastMessageBIN);
  } else {
    OS_LOGW(TAG, "Hub info fetch failed (transient), connecting anyway — shockers will sync on first poll");
  }

  CreateClient(authToken);
}

void GatewayConnectionManager::Update()
{
  auto client = GetClient();
  if (client != nullptr) {
    const bool connectedNow = client->state() == GatewayClientState::Connected;

    if (connectedNow) {
      const bool wasConnected      = s_lastGatewayConnected.exchange(true);
      const bool hadConnectedBefore = s_hasEverConnected.exchange(true);
      const int64_t now            = OpenShock::millis();
      // Refresh on: reconnect, first-ever connection when boot fetch was skipped, or every 60 s.
      const bool isReconnect    = !wasConnected && hadConnectedBefore;
      const bool neverRefreshed = s_lastHubInfoRefreshAttempt.load(std::memory_order_relaxed) == 0;
      const bool shouldRefresh  = neverRefreshed || isReconnect || (now - s_lastHubInfoRefreshAttempt.load(std::memory_order_relaxed)) > 60'000LL;
      if (shouldRefresh && Config::HasBackendAuthToken()) {
        std::string authToken;
        if (Config::GetBackendAuthToken(authToken)) {
          s_lastHubInfoRefreshAttempt = now;
          (void)FetchHubInfo(std::move(authToken), false);
        }
      }
    } else {
      s_lastGatewayConnected.store(false);
    }

    // Client exists — run its loop and optionally reconnect
    if (client->loop()) {
      return;
    }

    // WebSocket dropped — reset so the next connect doesn't trigger a competing FetchHubInfo
    // while the WebSocket TLS is re-establishing (not enough heap for both simultaneously).
    s_hasEverConnected.store(false, std::memory_order_relaxed);
    StartConnectingToLCG();
    return;
  }

  // On first boot, hold off gateway init for 3 s so the OTA startup check
  // can run with exclusive heap before WebSocket TLS permanently holds ~40 KB.
  if (s_firstWifiConnectedAt > 0 && (OpenShock::millis() - s_firstWifiConnectedAt) < 3000) {
    return;
  }

  // OTA task may request a reconnect hold while it uses TLS heap for GitHub checks.
  if (OpenShock::millis() < s_reconnectDelayUntilMs.load(std::memory_order_relaxed)) {
    return;
  }

  if (s_isInitializing.test_and_set()) {
    OS_LOGE(TAG, "Was about to initialize GatewayClient, but encountered race condition, yielding.");
    return;
  }

  InitializeClient();
  s_lastGatewayConnected.store(false);

  s_isInitializing.clear();
}

void GatewayConnectionManager::Disconnect()
{
  DestroyClient();
}

void GatewayConnectionManager::SetReconnectDelay(int64_t delayMs)
{
  s_reconnectDelayUntilMs.store(OpenShock::millis() + delayMs, std::memory_order_relaxed);
}
