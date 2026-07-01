#include <freertos/FreeRTOS.h>

#include "config/Config.h"

const char* const TAG = "Config";

#include "Chipset.h"
#include "Common.h"
#include "config/RootConfig.h"
#include "Logging.h"
#include "ReadWriteMutex.h"

#include <FS.h>
#include <LittleFS.h>
#include <esp_partition.h>

#include <cJSON.h>

#include <algorithm>
#include <bitset>

using namespace OpenShock;

static fs::LittleFSFS _configFS;
static Config::RootConfig _configData;
static ReadWriteMutex _configMutex;

static const char* const CONFIG_FILE_PATH      = "/config";
static const char* const CONFIG_FILE_TMP_PATH  = "/config.tmp";
static const char* const CONFIG_FILE_BAK_PATH  = "/config.bak";
static const char* const CONFIG_PARTITION_NAME = "config";

static bool trySaveConfig();

#define CONFIG_LOCK_READ_ACTION(retval, action)  \
  ScopedReadLock lock__(&_configMutex);          \
  if (!lock__.isLocked()) {                      \
    OS_LOGE(TAG, "Failed to acquire read lock"); \
    action;                                      \
    return retval;                               \
  }

#define CONFIG_LOCK_WRITE_ACTION(retval, action)  \
  ScopedWriteLock lock__(&_configMutex);          \
  if (!lock__.isLocked()) {                       \
    OS_LOGE(TAG, "Failed to acquire write lock"); \
    action;                                       \
    return retval;                                \
  }

#define CONFIG_LOCK_READ(retval)  CONFIG_LOCK_READ_ACTION(retval, {})
#define CONFIG_LOCK_WRITE(retval) CONFIG_LOCK_WRITE_ACTION(retval, {})

static bool tryDeserializeConfig(const uint8_t* buffer, std::size_t bufferLen, OpenShock::Config::RootConfig& config)
{
  if (buffer == nullptr || bufferLen < sizeof(flatbuffers::uoffset_t)) {
    OS_LOGE(TAG, "Buffer is null or too small");
    return false;
  }

  // Validate buffer before accessing
  flatbuffers::Verifier::Options verifierOptions {
    .max_size = 4096,  // Should be enough
  };
  flatbuffers::Verifier verifier(buffer, bufferLen, verifierOptions);
  if (!verifier.VerifyBuffer<Serialization::Configuration::HubConfig>()) {
    OS_LOGE(TAG, "Failed to verify config file integrity");
    return false;
  }

  // Deserialize (safe after verification)
  auto fbsConfig = flatbuffers::GetRoot<Serialization::Configuration::HubConfig>(buffer);

  // Read config
  if (!config.FromFlatbuffers(fbsConfig)) {
    OS_LOGE(TAG, "Failed to read config file");
    return false;
  }

  return true;
}
static bool isConfigPartitionLikelyBlank()
{
  const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, CONFIG_PARTITION_NAME);
  if (partition == nullptr) {
    OS_LOGW(TAG, "Config partition not found while checking blank state");
    return false;
  }

  constexpr std::size_t probeSize = 256;
  uint8_t probe[probeSize] = {};

  if (esp_partition_read(partition, 0, probe, sizeof(probe)) != ESP_OK) {
    OS_LOGW(TAG, "Failed to probe config partition contents");
    return false;
  }

  return std::all_of(std::begin(probe), std::end(probe), [](uint8_t b) { return b == 0xFF; });
}

static bool tryLoadConfigFile(const char* path, TinyVec<uint8_t>& buffer)
{
  File file = _configFS.open(path, "rb");
  if (!file) {
    OS_LOGW(TAG, "Failed to open config file for reading: %s", path);
    return false;
  }

  // Get file size
  std::size_t size = file.size();
  if (size < sizeof(flatbuffers::uoffset_t)) {
    OS_LOGW(TAG, "Config file is too small: %s", path);
    file.close();
    return false;
  }

  // Resize buffer
  buffer.resize(size);

  // Read file
  if (file.read(buffer.data(), buffer.size()) != buffer.size()) {
    OS_LOGE(TAG, "Failed to read config file, size mismatch: %s", path);
    file.close();
    return false;
  }

  file.close();

  return true;
}

static bool tryLoadConfigFromFile(const char* path, OpenShock::Config::RootConfig& config)
{
  TinyVec<uint8_t> buffer;
  if (!tryLoadConfigFile(path, buffer)) {
    return false;
  }

  return tryDeserializeConfig(buffer.data(), buffer.size(), config);
}

static bool tryLoadConfig(TinyVec<uint8_t>& buffer)
{
  if (tryLoadConfigFile(CONFIG_FILE_PATH, buffer)) {
    return true;
  }

  return tryLoadConfigFile(CONFIG_FILE_BAK_PATH, buffer);
}

static bool tryLoadConfig()
{
  OpenShock::Config::RootConfig loaded;
  if (tryLoadConfigFromFile(CONFIG_FILE_PATH, loaded)) {
    _configData = std::move(loaded);
    return true;
  }

  OS_LOGW(TAG, "Primary config failed to load, attempting backup");

  if (tryLoadConfigFromFile(CONFIG_FILE_BAK_PATH, loaded)) {
    OS_LOGW(TAG, "Recovered config from backup file");
    _configData = std::move(loaded);

    // Rewrite primary config from recovered in-memory state.
    if (!trySaveConfig()) {
      OS_LOGE(TAG, "Failed to rewrite primary config after backup recovery");
    }

    return true;
  }

  return false;
}
static bool trySaveConfig(const uint8_t* data, std::size_t dataLen)
{
  File file = _configFS.open(CONFIG_FILE_TMP_PATH, "wb");
  if (!file) {
    OS_LOGE(TAG, "Failed to open temp config file for writing");
    return false;
  }

  // Write file
  if (file.write(data, dataLen) != dataLen) {
    OS_LOGE(TAG, "Failed to write temp config file");
    file.close();
    _configFS.remove(CONFIG_FILE_TMP_PATH);
    return false;
  }

  file.close();

  if (_configFS.exists(CONFIG_FILE_BAK_PATH) && !_configFS.remove(CONFIG_FILE_BAK_PATH)) {
    OS_LOGE(TAG, "Failed to remove stale config backup file");
    _configFS.remove(CONFIG_FILE_TMP_PATH);
    return false;
  }

  if (_configFS.exists(CONFIG_FILE_PATH) && !_configFS.rename(CONFIG_FILE_PATH, CONFIG_FILE_BAK_PATH)) {
    OS_LOGE(TAG, "Failed to rotate current config into backup");
    _configFS.remove(CONFIG_FILE_TMP_PATH);
    return false;
  }

  if (!_configFS.rename(CONFIG_FILE_TMP_PATH, CONFIG_FILE_PATH)) {
    OS_LOGE(TAG, "Failed to promote temp config to primary file");
    if (_configFS.exists(CONFIG_FILE_BAK_PATH)) {
      (void)_configFS.rename(CONFIG_FILE_BAK_PATH, CONFIG_FILE_PATH);
    }
    _configFS.remove(CONFIG_FILE_TMP_PATH);
    return false;
  }

  return true;
}
static bool trySaveConfig()
{
  flatbuffers::FlatBufferBuilder builder;

  auto fbsConfig = _configData.ToFlatbuffers(builder, true);

  Serialization::Configuration::FinishHubConfigBuffer(builder, fbsConfig);

  return trySaveConfig(builder.GetBufferPointer(), builder.GetSize());
}

void Config::Init()
{
  CONFIG_LOCK_WRITE();

  if (!_configFS.begin(false, "/config", 3, CONFIG_PARTITION_NAME)) {
    if (isConfigPartitionLikelyBlank()) {
      OS_LOGW(TAG, "Config partition appears blank, formatting for first use");
      if (!_configFS.begin(true, "/config", 3, CONFIG_PARTITION_NAME)) {
        OS_PANIC(TAG, "Unable to mount config LittleFS partition after format!");
      }
    } else {
      OS_PANIC(TAG, "Unable to mount config LittleFS partition (refusing automatic format to prevent data loss)");
    }
  }

  if (tryLoadConfig()) {
    return;
  }

  OS_LOGW(TAG, "Failed to load config, writing default config");

  _configData.ToDefault();

  if (!trySaveConfig()) {
    OS_PANIC(TAG, "Failed to save default config. Recommend formatting microcontroller and re-flashing firmware");
  }
}

static cJSON* getAsCJSON(bool withSensitiveData)
{
  CONFIG_LOCK_READ(nullptr);

  return _configData.ToJSON(withSensitiveData);
}

std::string Config::GetAsJSON(bool withSensitiveData)
{
  cJSON* root = getAsCJSON(withSensitiveData);
  if (root == nullptr) {
    OS_LOGE(TAG, "Failed to get config as JSON");
    return {};
  }

  char* json = cJSON_PrintUnformatted(root);

  std::string result(json);

  free(json);

  cJSON_Delete(root);

  return result;
}
bool Config::SaveFromJSON(std::string_view json)
{
  cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
  if (root == nullptr) {
    OS_LOGE(TAG, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
    return false;
  }

  CONFIG_LOCK_WRITE_ACTION(false, cJSON_Delete(root));

  bool result = _configData.FromJSON(root);

  cJSON_Delete(root);

  if (!result) {
    OS_LOGE(TAG, "Failed to read JSON");
    return false;
  }

  return trySaveConfig();
}

flatbuffers::Offset<Serialization::Configuration::HubConfig> Config::GetAsFlatBuffer(flatbuffers::FlatBufferBuilder& builder, bool withSensitiveData)
{
  CONFIG_LOCK_READ(0);

  return _configData.ToFlatbuffers(builder, withSensitiveData);
}

bool Config::SaveFromFlatBuffer(const Serialization::Configuration::HubConfig* config)
{
  CONFIG_LOCK_WRITE(false);

  if (!_configData.FromFlatbuffers(config)) {
    OS_LOGE(TAG, "Failed to read config file");
    return false;
  }

  return trySaveConfig();
}

bool Config::GetRaw(TinyVec<uint8_t>& buffer)
{
  CONFIG_LOCK_READ(false);

  return tryLoadConfig(buffer);
}

bool Config::SetRaw(const uint8_t* buffer, std::size_t size)
{
  CONFIG_LOCK_WRITE(false);

  OpenShock::Config::RootConfig config;
  if (!tryDeserializeConfig(buffer, size, config)) {
    OS_LOGE(TAG, "Failed to deserialize config");
    return false;
  }

  return trySaveConfig(buffer, size);
}

void Config::FactoryReset()
{
  CONFIG_LOCK_WRITE();

  _configData.ToDefault();

  if (!_configFS.remove(CONFIG_FILE_TMP_PATH) && _configFS.exists(CONFIG_FILE_TMP_PATH)) {
    OS_PANIC(TAG, "Failed to remove temporary config file during factory reset. Recommend formatting microcontroller and re-flashing firmware");
  }

  if (!_configFS.remove(CONFIG_FILE_BAK_PATH) && _configFS.exists(CONFIG_FILE_BAK_PATH)) {
    OS_PANIC(TAG, "Failed to remove backup config file during factory reset. Recommend formatting microcontroller and re-flashing firmware");
  }

  if (!_configFS.remove(CONFIG_FILE_PATH) && _configFS.exists(CONFIG_FILE_PATH)) {
    OS_PANIC(TAG, "Failed to remove existing config file for factory reset. Reccomend formatting microcontroller and re-flashing firmware");
  }

  if (!trySaveConfig()) {
    OS_PANIC(TAG, "Failed to save default config. Recommend formatting microcontroller and re-flashing firmware");
  }

  OS_LOGI(TAG, "Factory reset complete");
}

bool Config::GetRFConfig(Config::RFConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf;

  return true;
}

bool Config::GetWiFiConfig(Config::WiFiConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi;

  return true;
}

bool Config::GetCaptivePortalConfig(Config::CaptivePortalConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.captivePortal;

  return true;
}

bool Config::GetBackendConfig(Config::BackendConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend;

  return true;
}

bool Config::GetSerialInputConfig(Config::SerialInputConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.serialInput;

  return true;
}

bool Config::GetOtaUpdateConfig(Config::OtaUpdateConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate;

  return true;
}

bool Config::GetEStop(Config::EStopConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop;

  return true;
}

bool Config::SetRFConfig(const Config::RFConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf = config;
  return trySaveConfig();
}

bool Config::SetWiFiConfig(const Config::WiFiConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi = config;
  return trySaveConfig();
}

bool Config::SetCaptivePortalConfig(const Config::CaptivePortalConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.captivePortal = config;
  return trySaveConfig();
}

bool Config::SetBackendConfig(const Config::BackendConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend = config;
  return trySaveConfig();
}

bool Config::SetSerialInputConfig(const Config::SerialInputConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.serialInput = config;
  return trySaveConfig();
}

bool Config::SetOtaUpdateConfig(const Config::OtaUpdateConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.otaUpdate = config;
  return trySaveConfig();
}

bool Config::SetEStop(const Config::EStopConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.estop = config;
  return trySaveConfig();
}

bool Config::GetWiFiCredentials(std::vector<Config::WiFiCredentials>& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi.credentialsList;

  return true;
}

bool Config::GetWiFiCredentials(cJSON* array, bool withSensitiveData)
{
  CONFIG_LOCK_READ(false);

  for (auto& creds : _configData.wifi.credentialsList) {
    cJSON* jsonCreds = creds.ToJSON(withSensitiveData);

    cJSON_AddItemToArray(array, jsonCreds);
  }

  return true;
}

bool Config::SetWiFiCredentials(const std::vector<Config::WiFiCredentials>& credentials)
{
  bool foundZeroId = std::any_of(credentials.begin(), credentials.end(), [](const Config::WiFiCredentials& creds) { return creds.id == 0; });
  if (foundZeroId) {
    OS_LOGE(TAG, "Cannot set WiFi credentials: credential ID cannot be 0");
    return false;
  }

  CONFIG_LOCK_WRITE(false);

  _configData.wifi.credentialsList = credentials;
  return trySaveConfig();
}

bool Config::GetRFConfigTxPin(gpio_num_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf.txPin;

  return true;
}

bool Config::SetRFConfigTxPin(gpio_num_t txPin)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf.txPin = txPin;
  return trySaveConfig();
}

bool Config::GetRFConfigKeepAliveEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf.keepAliveEnabled;

  return true;
}

bool Config::SetRFConfigKeepAliveEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf.keepAliveEnabled = enabled;
  return trySaveConfig();
}

bool Config::AnyWiFiCredentials(std::function<bool(const Config::WiFiCredentials&)> predicate)
{
  CONFIG_LOCK_READ(false);

  auto& creds = _configData.wifi.credentialsList;

  return std::any_of(creds.begin(), creds.end(), predicate);
}

uint8_t Config::AddWiFiCredentials(std::string_view ssid, std::string_view password, wifi_auth_mode_t authMode)
{
  CONFIG_LOCK_WRITE(0);

  uint8_t id = 0;

  std::bitset<255> bits;
  for (auto it = _configData.wifi.credentialsList.begin(); it != _configData.wifi.credentialsList.end();) {
    auto& creds = *it;

    if (std::string_view(creds.ssid) == ssid) {
      creds.password = password;
      if (authMode != WIFI_AUTH_MAX) {
        creds.authMode = authMode;
      }

      if (!trySaveConfig()) {
        OS_LOGE(TAG, "Failed to persist updated WiFi credentials for SSID %.*s", static_cast<int>(ssid.size()), ssid.data());
        return 0;
      }
      return creds.id;
    }

    if (creds.id == 0) {
      OS_LOGW(TAG, "Found WiFi credentials with ID 0, removing");
      it = _configData.wifi.credentialsList.erase(it);
      continue;
    }

    // Mark ID as used
    bits[creds.id - 1] = true;
    ++it;
  }

  // Get first available ID
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (!bits[i]) {
      id = i + 1;
      break;
    }
  }

  if (id == 0) {
    OS_LOGE(TAG, "Failed to add WiFi credentials: no available IDs");
    return 0;
  }

  _configData.wifi.credentialsList.emplace_back(id, ssid, password, authMode);
  trySaveConfig();

  return id;
}

bool Config::TryGetWiFiCredentialsByID(uint8_t id, Config::WiFiCredentials& credentials)
{
  CONFIG_LOCK_READ(false);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.id == id) {
      credentials = creds;
      return true;
    }
  }

  return false;
}

bool Config::TryGetWiFiCredentialsBySSID(const char* ssid, Config::WiFiCredentials& credentials)
{
  CONFIG_LOCK_READ(false);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.ssid == ssid) {
      credentials = creds;
      return true;
    }
  }

  return false;
}

uint8_t Config::GetWiFiCredentialsIDbySSID(const char* ssid)
{
  CONFIG_LOCK_READ(0);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.ssid == ssid) {
      return creds.id;
    }
  }

  return 0;
}

bool Config::PinWiFiCredentialsBSSID(uint8_t id, const uint8_t (&bssid)[6])
{
  CONFIG_LOCK_WRITE(false);

  for (auto& creds : _configData.wifi.credentialsList) {
    if (creds.id == id) {
      memcpy(creds.bssid.data(), bssid, 6);
      return trySaveConfig();
    }
  }

  return false;
}

bool Config::RemoveWiFiCredentials(uint8_t id)
{
  CONFIG_LOCK_WRITE(false);

  for (auto it = _configData.wifi.credentialsList.begin(); it != _configData.wifi.credentialsList.end(); ++it) {
    if (it->id == id) {
      _configData.wifi.credentialsList.erase(it);
      return trySaveConfig();
    }
  }

  return false;
}

bool Config::ClearWiFiCredentials()
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi.credentialsList.clear();

  return trySaveConfig();
}

bool Config::GetWiFiHostname(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi.hostname;

  return true;
}

bool Config::SetWiFiHostname(std::string hostname)
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi.hostname = std::move(hostname);

  return trySaveConfig();
}

bool Config::GetBackendDomain(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend.domain;

  return true;
}

bool Config::SetBackendDomain(std::string domain)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.domain = std::move(domain);
  return trySaveConfig();
}

bool Config::HasBackendAuthToken()
{
  CONFIG_LOCK_READ(false);

  return !_configData.backend.authToken.empty();
}

bool Config::GetBackendAuthToken(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend.authToken;

  return true;
}

bool Config::SetBackendAuthToken(std::string token)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.authToken = std::move(token);
  return trySaveConfig();
}

bool Config::ClearBackendAuthToken()
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.authToken.clear();
  return trySaveConfig();
}

bool Config::GetSerialInputConfigEchoEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.serialInput.echoEnabled;
  return true;
}

bool Config::SetSerialInputConfigEchoEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.serialInput.echoEnabled = enabled;
  return trySaveConfig();
}

bool Config::GetOtaUpdateId(int32_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate.updateId;

  return true;
}

bool Config::SetOtaUpdateId(int32_t updateId)
{
  CONFIG_LOCK_WRITE(false);

  if (_configData.otaUpdate.updateId == updateId) {
    return true;
  }

  _configData.otaUpdate.updateId = updateId;
  return trySaveConfig();
}

bool Config::GetOtaUpdateStep(OtaUpdateStep& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate.updateStep;

  return true;
}

bool Config::SetOtaUpdateStep(OtaUpdateStep updateStep)
{
  CONFIG_LOCK_WRITE(false);

  if (_configData.otaUpdate.updateStep == updateStep) {
    return true;
  }

  _configData.otaUpdate.updateStep = updateStep;
  return trySaveConfig();
}

bool Config::GetEStopEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop.enabled;

  return true;
}

bool Config::SetEStopEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.estop.enabled = enabled;
  return trySaveConfig();
}

bool Config::GetEStopGpioPin(gpio_num_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop.gpioPin;

  return true;
}

bool Config::SetEStopGpioPin(gpio_num_t gpioPin)
{
  CONFIG_LOCK_WRITE(false);

  if (!OpenShock::IsValidInputPin(gpioPin)) {
    OS_LOGE(TAG, "Invalid EStop GPIO Pin: %d", gpioPin);
    return false;
  }

  _configData.estop.gpioPin = gpioPin;
  return trySaveConfig();
}
