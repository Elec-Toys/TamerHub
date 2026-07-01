#include "OtaUpdateManager.h"

const char* const TAG = "OtaUpdateManager";

#include "captiveportal/Manager.h"
#include "Common.h"
#include "config/Config.h"
#include "Core.h"
#include "GatewayConnectionManager.h"
#include "Hashing.h"
#include "http/HTTPRequestManager.h"
#include "Logging.h"
#include "SemVer.h"
#include "serialization/WSGateway.h"
#include "SimpleMutex.h"
#include "util/HexUtils.h"
#include "util/PartitionUtils.h"
#include "util/StringUtils.h"
#include "util/TaskUtils.h"
#include "visual/OledDisplayManager.h"
#include "wifi/WiFiManager.h"

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <atomic>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>

#include <sstream>
#include <string_view>

using namespace std::string_view_literals;

#define GITHUB_DEFAULT_REPO "Elec-Toys/TamerHub"

/// @brief Stops initArduino() from handling OTA rollbacks
/// @todo Get rid of Arduino entirely. >:(
///
/// @see .platformio/packages/framework-arduinoespressif32/cores/esp32/esp32-hal-misc.c
/// @return true
bool verifyRollbackLater()
{
  return true;
}

enum OtaTaskEventFlag : uint32_t {
  OTA_TASK_EVENT_UPDATE_REQUESTED  = 1 << 0,
  OTA_TASK_EVENT_WIFI_DISCONNECTED = 1 << 1,  // If both connected and disconnected are set, disconnected takes priority.
  OTA_TASK_EVENT_WIFI_CONNECTED    = 1 << 2,
  OTA_TASK_EVENT_CHECK_REQUESTED   = 1 << 3,  // Manual "check now" from OLED
};

static esp_ota_img_states_t _otaImageState;
static OpenShock::FirmwareBootType _bootType;
static TaskHandle_t _taskHandle = nullptr;
static OpenShock::SemVer _requestedVersion;
static OpenShock::SimpleMutex _requestedVersionMutex = {};
static char s_cachedLatestVersion[32] = {};

// ── Persistent OTA settings (NVS) ────────────────────────────────────────────
static Preferences         s_otaPrefs;
static bool                s_otaPrefsReady     = false;
static char                s_otaRepoSlug[64]   = GITHUB_DEFAULT_REPO;
static std::atomic<int8_t> s_pendingUserDecision { -1 };   // -1=waiting, 0=No, 1=Never, 2=Yes
static char                s_pendingUpdateVersion[32] = {};
static bool                s_checkRequested    = false;
static std::atomic<int8_t> s_lastCheckStatus   { 0 };      // 0=idle,1=checking,2=upToDate,3=failed,4=noNetwork
static std::atomic<bool>   s_isUpdating        { false };  // true while actively flashing firmware

constexpr char kOtaPrefsNamespace[] = "ota_cfg";
constexpr char kPrefAutoUpdate[]    = "auto_upd";
constexpr char kPrefPromptUpdates[] = "prompt";
constexpr char kPrefNeverPrompt[]   = "never";
constexpr char kPrefRepoSlug[]      = "repo_slug";
constexpr char kPrefLatestVer[]     = "latest_ver";

static void otaum_ensurePrefs()
{
  if (s_otaPrefsReady) return;
  s_otaPrefsReady = s_otaPrefs.begin(kOtaPrefsNamespace, false);
  if (!s_otaPrefsReady) return;
  String slug   = s_otaPrefs.getString(kPrefRepoSlug, GITHUB_DEFAULT_REPO);
  String latest = s_otaPrefs.getString(kPrefLatestVer, "");
  strncpy(s_otaRepoSlug,         slug.c_str(),   sizeof(s_otaRepoSlug) - 1);
  strncpy(s_cachedLatestVersion, latest.c_str(), sizeof(s_cachedLatestVersion) - 1);
}
// ─────────────────────────────────────────────────────────────────────────────

using namespace OpenShock;

static bool otaum_try_notify_task(uint32_t eventFlag)
{
  if (_taskHandle == nullptr) {
    OS_LOGW(TAG, "Unable to notify OTA task, task handle is null");
    return false;
  }

  if (xTaskNotify(_taskHandle, eventFlag, eSetBits) != pdPASS) {
    OS_LOGE(TAG, "Failed to notify OTA task (event: 0x%08x)", eventFlag);
    return false;
  }

  return true;
}

static bool otaum_try_queue_update_request(const OpenShock::SemVer& version)
{
  if (!_requestedVersionMutex.lock(pdMS_TO_TICKS(1000))) {
    OS_LOGE(TAG, "Failed to take requested version mutex");
    return false;
  }

  _requestedVersion = version;

  _requestedVersionMutex.unlock();

  otaum_try_notify_task(OTA_TASK_EVENT_UPDATE_REQUESTED);

  return true;
}

static bool _tryGetRequestedVersion(OpenShock::SemVer& version)
{
  if (!_requestedVersionMutex.lock(pdMS_TO_TICKS(1000))) {
    OS_LOGE(TAG, "Failed to take requested version mutex");
    return false;
  }

  version = _requestedVersion;

  _requestedVersionMutex.unlock();

  return true;
}

static void otaum_evh_wifidisconnected(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  (void)event_handler_arg;
  (void)event_base;
  (void)event_id;
  (void)event_data;

  otaum_try_notify_task(OTA_TASK_EVENT_WIFI_DISCONNECTED);
}

static void otaum_evh_ipevent(void* event_handler_arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  (void)event_handler_arg;
  (void)event_base;
  (void)event_data;

  switch (event_id) {
    case IP_EVENT_GOT_IP6:
    case IP_EVENT_STA_GOT_IP:
      otaum_try_notify_task(OTA_TASK_EVENT_WIFI_CONNECTED);
      break;
    case IP_EVENT_STA_LOST_IP:
      otaum_try_notify_task(OTA_TASK_EVENT_WIFI_DISCONNECTED);
      break;
    default:
      return;
  }
}

static bool otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask task, float progress)
{
  int32_t updateId;
  if (!Config::GetOtaUpdateId(updateId)) {
    OS_LOGW(TAG, "No OTA update ID for progress message, skipping");
    return true;  // Non-fatal: gateway may be disconnected during update
  }

  if (!Serialization::Gateway::SerializeOtaUpdateProgressMessage(updateId, task, progress, GatewayConnectionManager::SendMessageBIN)) {
    OS_LOGW(TAG, "Failed to send OTA progress (gateway not available), continuing");
  }

  return true;  // Always continue — gateway telemetry is optional
}

static bool _sendFailureMessage(std::string_view message, bool fatal = false)
{
  int32_t updateId;
  if (!Config::GetOtaUpdateId(updateId)) {
    OS_LOGW(TAG, "No OTA update ID for failure message, skipping: %.*s", (int)message.size(), message.data());
    return true;
  }

  if (!Serialization::Gateway::SerializeOtaUpdateFailedMessage(updateId, message, fatal, GatewayConnectionManager::SendMessageBIN)) {
    OS_LOGW(TAG, "Failed to send OTA failure (gateway not available): %.*s", (int)message.size(), message.data());
  }

  return true;  // Non-fatal
}

static bool otaum_flash_app_partition(const esp_partition_t* partition, std::string_view remoteUrl, const uint8_t (&remoteHash)[32])
{
  OS_LOGD(TAG, "Flashing app partition");

  if (!otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::FlashingApplication, 0.0f)) {
    return false;
  }

  auto onProgress = [](std::size_t current, std::size_t total, float progress) -> bool {
    OS_LOGD(TAG, "Flashing app partition: %u / %u (%.2f%%)", current, total, progress * 100.0f);

    otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::FlashingApplication, progress);

    return true;
  };

  if (!OpenShock::FlashPartitionFromUrl(partition, remoteUrl, remoteHash, onProgress)) {
    OS_LOGE(TAG, "Failed to flash app partition");
    _sendFailureMessage("Failed to flash app partition"sv);
    return false;
  }

  if (!otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::MarkingApplicationBootable, 0.0f)) {
    return false;
  }

  // Set app partition bootable.
  if (esp_ota_set_boot_partition(partition) != ESP_OK) {
    OS_LOGE(TAG, "Failed to set app partition bootable");
    _sendFailureMessage("Failed to set app partition bootable"sv);
    return false;
  }

  return true;
}

static bool otaum_flash_fs_partition(const esp_partition_t* parition, std::string_view remoteUrl, const uint8_t (&remoteHash)[32])
{
  if (!otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::PreparingForUpdate, 0.0f)) {
    return false;
  }

  // Make sure captive portal is stopped, timeout after 5 seconds.
  if (!CaptivePortal::ForceClose(5000U)) {
    OS_LOGE(TAG, "Failed to force close captive portal (timed out)");
    _sendFailureMessage("Failed to force close captive portal (timed out)"sv);
    return false;
  }

  OS_LOGD(TAG, "Flashing filesystem partition");

  if (!otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::FlashingFilesystem, 0.0f)) {
    return false;
  }

  auto onProgress = [](std::size_t current, std::size_t total, float progress) -> bool {
    OS_LOGD(TAG, "Flashing filesystem partition: %u / %u (%.2f%%)", current, total, progress * 100.0f);

    otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::FlashingFilesystem, progress);

    return true;
  };

  if (!OpenShock::FlashPartitionFromUrl(parition, remoteUrl, remoteHash, onProgress)) {
    OS_LOGE(TAG, "Failed to flash filesystem partition");
    _sendFailureMessage("Failed to flash filesystem partition"sv);
    return false;
  }

  if (!otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::VerifyingFilesystem, 0.0f)) {
    return false;
  }

  // Attempt to mount filesystem.
  fs::LittleFSFS test;
  if (!test.begin(false, "/static", 10, "static0")) {
    OS_LOGE(TAG, "Failed to mount filesystem");
    _sendFailureMessage("Failed to mount filesystem"sv);
    return false;
  }
  test.end();

  return true;
}

static void otaum_restore_wdt_timeout()
{
  if (esp_task_wdt_init(5, true) != ESP_OK) {
    OS_LOGE(TAG, "Failed to restore task watchdog timeout");
  }
};

static void otaum_updatetask(void* arg)
{
  (void)arg;

  OS_LOGD(TAG, "OTA update task started");

  bool connected           = false;
  bool updateRequested     = false;
  int64_t lastUpdateCheck  = 0;
  int64_t firstConnectedMs = 0;

  // Update task loop.
  while (true) {
    // When waiting for the startup check (connected but never checked), poll quickly
    // so we catch the heap-free window before the gateway grabs TLS memory.
    const bool pendingFirstCheck = connected && lastUpdateCheck == 0;
    const TickType_t waitTicks   = pendingFirstCheck ? pdMS_TO_TICKS(200) : pdMS_TO_TICKS(5000);

    // Wait for event.
    uint32_t eventBits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &eventBits, waitTicks);  // TODO: wait for rest time

    updateRequested  |= (eventBits & OTA_TASK_EVENT_UPDATE_REQUESTED) != 0;
    s_checkRequested |= (eventBits & OTA_TASK_EVENT_CHECK_REQUESTED)  != 0;

    if ((eventBits & OTA_TASK_EVENT_WIFI_DISCONNECTED) != 0) {
      OS_LOGD(TAG, "WiFi disconnected");
      connected = false;
      s_lastCheckStatus.store(4, std::memory_order_release);  // NoNetwork
      continue;  // No further processing needed.
    }

    if ((eventBits & OTA_TASK_EVENT_WIFI_CONNECTED) != 0 && !connected) {
      OS_LOGD(TAG, "WiFi connected");
      connected = true;
      if (firstConnectedMs == 0) firstConnectedMs = OpenShock::millis();
    }

    // If we're not connected, skip (but show NoNetwork if manual check was requested).
    if (!connected) {
      if (s_checkRequested) {
        s_checkRequested = false;
        s_lastCheckStatus.store(4, std::memory_order_release);  // NoNetwork
        OledDisplayManager::RequestRefresh();
      }
      continue;
    }

    int64_t now = OpenShock::millis();

    Config::OtaUpdateConfig config;
    if (!Config::GetOtaUpdateConfig(config)) {
      OS_LOGE(TAG, "Failed to get OTA update config");
      continue;
    }

    if (!config.isEnabled) {
      OS_LOGD(TAG, "OTA updates are disabled, skipping update check");
      if (s_checkRequested) {
        s_checkRequested = false;
        s_lastCheckStatus.store(0, std::memory_order_release);  // Idle
        OledDisplayManager::RequestRefresh();
      }
      continue;
    }

    bool firstCheck  = lastUpdateCheck == 0;
    int64_t diff     = now - lastUpdateCheck;
    int64_t diffMins = diff / 60'000LL;

    // Fire startup check 500 ms after WiFi connects, before the gateway starts its
    // TLS sessions. GatewayConnectionManager delays its init by 3 s, so at 500 ms
    // we have ~168 KB free — enough for the GitHub HTTPS handshake.
    int64_t msSinceConnect = (firstConnectedMs > 0) ? (now - firstConnectedMs) : 0;
    bool gatewayReady      = msSinceConnect >= 500;

    bool check = false;
    check |= firstCheck && gatewayReady;                                     // Startup check (after gateway settles)
    check |= config.checkPeriodically && diffMins >= config.checkInterval;  // Periodically
    check |= updateRequested && (firstCheck || diffMins >= 1);              // Update requested (specific version)
    check |= s_checkRequested;                                              // Manual check from OLED
    if (s_checkRequested) s_checkRequested = false;

    if (!check) {
      continue;
    }

    lastUpdateCheck = now;
    s_lastCheckStatus.store(1, std::memory_order_release);  // Checking

    otaum_ensurePrefs();

    OpenShock::SemVer version;
    if (updateRequested) {
      updateRequested = false;

      if (!_tryGetRequestedVersion(version)) {
        OS_LOGE(TAG, "Failed to get requested version");
        s_lastCheckStatus.store(3, std::memory_order_release);  // Failed
        OledDisplayManager::RequestRefresh();
        continue;
      }
    } else {
      OS_LOGD(TAG, "Checking for updates");

      // If heap is tight (WebSocket TLS holds ~40 KB permanently), disconnect the
      // gateway first so GitHub HTTPS has enough heap for its own TLS handshake.
      // 30s reconnect delay keeps the gateway from racing back before the check
      // finishes. Threshold is 80 KB — the realistic post-disconnect ceiling on
      // this device (~44 KB base + ~40 KB freed by gateway TLS = ~84 KB).
      if (esp_get_free_heap_size() < 100000U) {
        OS_LOGI(TAG, "Heap low (%u B), disconnecting gateway for check", esp_get_free_heap_size());
        GatewayConnectionManager::SetReconnectDelay(30000);
        GatewayConnectionManager::Disconnect();
        for (int i = 0; i < 50 && esp_get_free_heap_size() < 80000U; i++) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        OS_LOGI(TAG, "Heap after gateway disconnect: %u B", esp_get_free_heap_size());
      }

      // Fetch current version.
      if (!OtaUpdateManager::TryGetLatestVersion(version)) {
        OS_LOGE(TAG, "Failed to fetch latest firmware version from GitHub");
        s_lastCheckStatus.store(3, std::memory_order_release);  // Failed
        OledDisplayManager::RequestRefresh();
        continue;
      }
    }

    std::string versionStr = version.toString();  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this

    // Use SemVer comparison so that local-newer-than-online is also "up to date".
    // A plain string equality check would treat any mismatch (including local > online)
    // as a new version available, which would offer a downgrade.
    if (!(version > OPENSHOCK_FW_VERSION ""sv)) {
      OS_LOGI(TAG, "Already running latest or newer firmware (local: %s, online: %s)", OPENSHOCK_FW_VERSION, versionStr.c_str());
      s_lastCheckStatus.store(2, std::memory_order_release);  // UpToDate
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // ── Decide whether to auto-update or prompt the user ────────────────────
    bool autoUpdate    = s_otaPrefs.getBool(kPrefAutoUpdate,    false);
    bool promptUpdates = s_otaPrefs.getBool(kPrefPromptUpdates, true);
    bool neverPrompt   = s_otaPrefs.getBool(kPrefNeverPrompt,   false);

    if (!updateRequested && !autoUpdate) {
      // Manual mode
      if (promptUpdates && !neverPrompt) {
        // Set pending prompt and wait for the user's decision (up to 5 min).
        strncpy(s_pendingUpdateVersion, versionStr.c_str(), sizeof(s_pendingUpdateVersion) - 1);
        s_pendingUpdateVersion[sizeof(s_pendingUpdateVersion) - 1] = '\0';
        s_pendingUserDecision.store(-1, std::memory_order_release);
        OledDisplayManager::RequestRefresh();  // Wake the OLED to show the prompt

        const int64_t deadline = OpenShock::millis() + 300'000LL;  // 5 min timeout
        while (s_pendingUserDecision.load(std::memory_order_acquire) < 0 && OpenShock::millis() < deadline) {
          vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Clear version FIRST so HasPendingUpdatePrompt() immediately returns false,
        // then exchange decision to signal the OTA task is done waiting.
        s_pendingUpdateVersion[0] = '\0';
        const int8_t decision = s_pendingUserDecision.exchange(-1, std::memory_order_acq_rel);
        OledDisplayManager::RequestRefresh();  // Let OLED clear the prompt

        if (decision == 1) {  // Never
          if (s_otaPrefsReady) s_otaPrefs.putBool(kPrefNeverPrompt, true);
          s_lastCheckStatus.store(0, std::memory_order_release);  // Idle
          continue;
        }
        if (decision != 2) {  // No or timeout
          s_lastCheckStatus.store(0, std::memory_order_release);  // Idle
          continue;
        }
        // decision == 2 (Yes) → fall through to update
      } else {
        OS_LOGD(TAG, "New version %s available but prompt disabled — skipping", versionStr.c_str());
        s_lastCheckStatus.store(0, std::memory_order_release);  // Idle
        continue;
      }
    }
    // ────────────────────────────────────────────────────────────────────────

    OS_LOGD(TAG, "Updating to version: %.*s", versionStr.length(), versionStr.data());

    // Signal the OLED to show update-in-progress screen before the long download.
    s_isUpdating.store(true, std::memory_order_release);
    OledDisplayManager::RequestRefresh();

    // Disconnect gateway and hold reconnect for 2 minutes so the WebSocket TLS
    // doesn't compete with the firmware download TLS handshake.
    OS_LOGI(TAG, "Disconnecting gateway for firmware download (heap: %u B)", esp_get_free_heap_size());
    GatewayConnectionManager::SetReconnectDelay(120000);
    GatewayConnectionManager::Disconnect();
    // Poll until the ~40 KB held by the gateway TLS session is freed.
    // 80 KB threshold matches the realistic post-disconnect ceiling on this device.
    // 120 s reconnect delay ensures the gateway cannot race back during download.
    for (int i = 0; i < 50 && esp_get_free_heap_size() < 80000U; i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    OS_LOGI(TAG, "Heap ready for firmware download: %u B", esp_get_free_heap_size());

    // Generate random int32_t for this update.
    int32_t updateId = static_cast<int32_t>(esp_random());
    if (!Config::SetOtaUpdateId(updateId)) {
      OS_LOGE(TAG, "Failed to set OTA update ID");
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }
    if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Updating)) {
      OS_LOGE(TAG, "Failed to set OTA update step");
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Gateway telemetry is best-effort — don't abort the update if it fails.
    if (!Serialization::Gateway::SerializeOtaUpdateStartedMessage(updateId, version, GatewayConnectionManager::SendMessageBIN)) {
      OS_LOGW(TAG, "Failed to send OTA update started (gateway disconnected, continuing)");
    }

    otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::FetchingMetadata, 0.0f);

    // Fetch current release.
    OtaUpdateManager::FirmwareRelease release;
    if (!OtaUpdateManager::TryGetFirmwareRelease(version, release)) {
      OS_LOGE(TAG, "Failed to fetch firmware release");  // TODO: Send error message to server
      _sendFailureMessage("Failed to fetch firmware release"sv);
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Print release.
    OS_LOGD(TAG, "Firmware release:");
    OS_LOGD(TAG, "  Version:                %.*s", versionStr.length(), versionStr.data());
    OS_LOGD(TAG, "  App binary URL:         %.*s", release.appBinaryUrl.length(), release.appBinaryUrl.data());
    OS_LOGD(TAG, "  App binary hash:        %s", HexUtils::ToHex<32>(release.appBinaryHash).data());
    OS_LOGD(TAG, "  Filesystem binary URL:  %.*s", release.filesystemBinaryUrl.length(), release.filesystemBinaryUrl.data());
    OS_LOGD(TAG, "  Filesystem binary hash: %s", HexUtils::ToHex<32>(release.filesystemBinaryHash).data());

    // Get available app update partition.
    const esp_partition_t* appPartition = esp_ota_get_next_update_partition(nullptr);
    if (appPartition == nullptr) {
      OS_LOGE(TAG, "Failed to get app update partition");
      _sendFailureMessage("Failed to get app update partition"sv);
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Get filesystem partition.
    const esp_partition_t* filesystemPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "static0");
    if (filesystemPartition == nullptr) {
      OS_LOGE(TAG, "Failed to find filesystem partition");
      _sendFailureMessage("Failed to find filesystem partition"sv);
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Increase task watchdog timeout.
    // Prevents panics on some ESP32s when clearing large partitions.
    if (esp_task_wdt_init(15, true) != ESP_OK) {
      OS_LOGE(TAG, "Failed to increase task watchdog timeout");
      _sendFailureMessage("Failed to increase task watchdog timeout"sv);
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Flash app and filesystem partitions.
    if (!otaum_flash_fs_partition(filesystemPartition, release.filesystemBinaryUrl, release.filesystemBinaryHash)) {
      otaum_restore_wdt_timeout();
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }
    if (!otaum_flash_app_partition(appPartition, release.appBinaryUrl, release.appBinaryHash)) {
      otaum_restore_wdt_timeout();
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Set OTA boot type in config.
    if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Updated)) {
      OS_LOGE(TAG, "Failed to set OTA update step");
      _sendFailureMessage("Failed to set OTA update step"sv);
      otaum_restore_wdt_timeout();
      s_isUpdating.store(false, std::memory_order_release);
      s_lastCheckStatus.store(5, std::memory_order_release);
      OledDisplayManager::RequestRefresh();
      continue;
    }

    // Set task watchdog timeout back to default.
    otaum_restore_wdt_timeout();

    // Send reboot message.
    otaum_send_progress_msg(Serialization::Types::OtaUpdateProgressTask::Rebooting, 0.0f);

    // Reboot into new firmware.
    OS_LOGI(TAG, "Restarting into new firmware...");
    vTaskDelay(pdMS_TO_TICKS(200));
    break;
  }

  // Restart.
  esp_restart();
}

bool OtaUpdateManager::Init()
{
  esp_err_t err;

  OS_LOGN(TAG, "Fetching current partition");

  // Fetch current partition info.
  const esp_partition_t* partition = esp_ota_get_running_partition();
  if (partition == nullptr) {
    OS_PANIC(TAG, "Failed to get currently running partition");
    return false;  // Unreachable, here to make tooling happy
  }

  OS_LOGD(TAG, "Fetching partition state");

  // Get OTA state for said partition.
  err = esp_ota_get_state_partition(partition, &_otaImageState);
  if (err != ESP_OK) {
    OS_PANIC(TAG, "Failed to get partition state: %s", esp_err_to_name(err));
    return false;  // Unreachable, here to make tooling happy
  }

  OS_LOGD(TAG, "Fetching previous update step");
  OtaUpdateStep updateStep;
  if (!Config::GetOtaUpdateStep(updateStep)) {
    OS_LOGE(TAG, "Failed to get OTA update step");
    return false;
  }

  // Infer boot type from update step.
  switch (updateStep) {
    case OtaUpdateStep::Updated:
      _bootType = FirmwareBootType::NewFirmware;
      break;
    case OtaUpdateStep::Validating:  // If the update step is validating, we have failed in the middle of validating the new firmware, meaning this is a rollback.
    case OtaUpdateStep::RollingBack:
      _bootType = FirmwareBootType::Rollback;
      break;
    default:
      _bootType = FirmwareBootType::Normal;
      break;
  }

  if (updateStep == OtaUpdateStep::Updated) {
    if (!Config::SetOtaUpdateStep(OtaUpdateStep::Validating)) {
      OS_PANIC(TAG, "Failed to set OTA update step in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
    }
  }

  // Start OTA update task.
  if (TaskUtils::TaskCreateExpensive(otaum_updatetask, "OTA Update", 16'384, nullptr, 1, &_taskHandle) != pdPASS) {  // PROFILED: 6.2KB stack usage
    OS_LOGE(TAG, "Failed to create OTA update task");
    return false;
  }

  err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, otaum_evh_ipevent, nullptr);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to register event handler for IP_EVENT: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, otaum_evh_wifidisconnected, nullptr);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to register event handler for WIFI_EVENT: %s", esp_err_to_name(err));
    return false;
  }

  return true;
}

bool OtaUpdateManager::TryGetLatestVersion(OpenShock::SemVer& version)
{
  otaum_ensurePrefs();
  OS_LOGD(TAG, "Fetching latest firmware version from GitHub (repo: %s)", s_otaRepoSlug);

  char apiUrl[128];
  snprintf(apiUrl, sizeof(apiUrl), "https://api.github.com/repos/%s/releases/latest", s_otaRepoSlug);

  auto response = OpenShock::HTTP::GetJSON<std::string>(
    apiUrl,
    {
      {"Accept",               "application/vnd.github+json"},
      {"User-Agent",           OpenShock::Constants::FW_USERAGENT},
      {"X-GitHub-Api-Version", "2022-11-28"}
    },
    [](int, const cJSON* json, std::string& tagName) -> bool {
      const cJSON* tagItem = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
      if (!cJSON_IsString(tagItem) || tagItem->valuestring == nullptr) {
        return false;
      }
      tagName = tagItem->valuestring;
      return true;
    },
    std::array<uint16_t, 1> {200}
  );

  if (response.result != OpenShock::HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Failed to fetch latest release from GitHub: %s [%u]", response.ResultToString(), response.code);
    return false;
  }

  std::string_view tagView = response.data;
  if (!tagView.empty() && tagView[0] == 'v') {
    tagView = tagView.substr(1);
  }

  if (!OpenShock::TryParseSemVer(tagView, version)) {
    OS_LOGE(TAG, "Failed to parse version from GitHub tag: %s", response.data.c_str());
    return false;
  }

  snprintf(s_cachedLatestVersion, sizeof(s_cachedLatestVersion), "%.*s", static_cast<int>(tagView.size()), tagView.data());

  if (s_otaPrefsReady) {
    s_otaPrefs.putString(kPrefLatestVer, s_cachedLatestVersion);
  }

  return true;
}

const char* OtaUpdateManager::GetCachedLatestVersion()
{
  otaum_ensurePrefs();
  return s_cachedLatestVersion[0] != '\0' ? s_cachedLatestVersion : nullptr;
}

void OtaUpdateManager::TriggerManualCheck()
{
  // Set to "checking" immediately so the OLED doesn't clear s_checkInProgress
  // before the OTA task wakes from its xTaskNotifyWait sleep.
  s_lastCheckStatus.store(1, std::memory_order_release);
  otaum_try_notify_task(OTA_TASK_EVENT_CHECK_REQUESTED);
}

bool OtaUpdateManager::GetOtaUpdateSettings(bool& autoUpdate, bool& promptUpdates, bool& neverPrompt)
{
  otaum_ensurePrefs();
  autoUpdate    = s_otaPrefs.getBool(kPrefAutoUpdate,    false);
  promptUpdates = s_otaPrefs.getBool(kPrefPromptUpdates, true);
  neverPrompt   = s_otaPrefs.getBool(kPrefNeverPrompt,   false);
  return s_otaPrefsReady;
}

bool OtaUpdateManager::SetOtaUpdateSettings(bool autoUpdate, bool promptUpdates, bool neverPrompt)
{
  otaum_ensurePrefs();
  if (!s_otaPrefsReady) return false;
  s_otaPrefs.putBool(kPrefAutoUpdate,    autoUpdate);
  s_otaPrefs.putBool(kPrefPromptUpdates, promptUpdates);
  s_otaPrefs.putBool(kPrefNeverPrompt,   neverPrompt);
  return true;
}

bool OtaUpdateManager::GetOtaRepoSlug(char* buf, size_t len)
{
  if (buf == nullptr || len == 0) return false;
  otaum_ensurePrefs();
  strncpy(buf, s_otaRepoSlug, len - 1);
  buf[len - 1] = '\0';
  return true;
}

bool OtaUpdateManager::SetOtaRepoSlug(const char* slug)
{
  if (slug == nullptr || slug[0] == '\0') return false;
  strncpy(s_otaRepoSlug, slug, sizeof(s_otaRepoSlug) - 1);
  s_otaRepoSlug[sizeof(s_otaRepoSlug) - 1] = '\0';
  otaum_ensurePrefs();
  if (s_otaPrefsReady) {
    s_otaPrefs.putString(kPrefRepoSlug, slug);
  }
  return true;
}

bool OtaUpdateManager::HasPendingUpdatePrompt(char* versionBuf, size_t len)
{
  if (s_pendingUpdateVersion[0] == '\0') return false;
  if (s_pendingUserDecision.load(std::memory_order_acquire) >= 0) return false;
  if (versionBuf != nullptr && len > 0) {
    strncpy(versionBuf, s_pendingUpdateVersion, len - 1);
    versionBuf[len - 1] = '\0';
  }
  return true;
}

void OtaUpdateManager::SetUserUpdateDecision(int8_t decision)
{
  s_pendingUserDecision.store(decision, std::memory_order_release);
}

int8_t OtaUpdateManager::GetLastCheckStatus()
{
  return s_lastCheckStatus.load(std::memory_order_acquire);
}

static bool _tryParseIntoHash(std::string_view hash, uint8_t (&hashBytes)[32])
{
  if (HexUtils::TryParseHex(hash.data(), hash.size(), hashBytes, 32) != 32) {
    OS_LOGE(TAG, "Failed to parse hash: %.*s", hash.size(), hash.data());
    return false;
  }

  return true;
}

bool OtaUpdateManager::TryGetFirmwareRelease(const OpenShock::SemVer& version, FirmwareRelease& release)
{
  otaum_ensurePrefs();
  auto versionStr = version.toString();

  char downloadBase[160];
  snprintf(downloadBase, sizeof(downloadBase), "https://github.com/%s/releases/download/v%s/", s_otaRepoSlug, versionStr.c_str());
  release.appBinaryUrl        = std::string(downloadBase) + "app.bin";
  release.filesystemBinaryUrl = std::string(downloadBase) + "staticfs.bin";

  std::string sha256HashesUrl = std::string(downloadBase) + "hashes.sha256.txt";

  auto sha256HashesResponse = OpenShock::HTTP::GetString(
    sha256HashesUrl,
    {
      {"Accept",     "text/plain"},
      {"User-Agent", OpenShock::Constants::FW_USERAGENT}
    },
    std::array<uint16_t, 1> {200}
  );
  if (sha256HashesResponse.result != OpenShock::HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Failed to fetch hashes from GitHub: %s [%u] %s", sha256HashesResponse.ResultToString(), sha256HashesResponse.code, sha256HashesResponse.data.c_str());
    return false;
  }

  // Strip UTF-8 BOM (EF BB BF) if present — PowerShell Set-Content -Encoding utf8 adds one.
  std::string& hashesData = sha256HashesResponse.data;
  if (hashesData.size() >= 3
      && static_cast<uint8_t>(hashesData[0]) == 0xEF
      && static_cast<uint8_t>(hashesData[1]) == 0xBB
      && static_cast<uint8_t>(hashesData[2]) == 0xBF)
  {
    hashesData.erase(0, 3);
  }

  auto hashesLines = OpenShock::StringSplitNewLines(hashesData);

  bool foundAppHash = false, foundFilesystemHash = false;
  for (std::string_view line : hashesLines) {
    auto parts = OpenShock::StringSplitWhiteSpace(line);
    if (parts.size() != 2) {
      OS_LOGE(TAG, "Invalid hashes entry: %.*s", line.size(), line.data());
      return false;
    }

    auto hash = OpenShock::StringTrim(parts[0]);
    auto file = OpenShock::StringTrim(parts[1]);

    file = OpenShock::StringRemovePrefix(file, "./"sv);

    if (hash.size() != 64) {
      OS_LOGE(TAG, "Invalid hash: %.*s", hash.size(), hash.data());
      return false;
    }

    if (file == "app.bin") {
      if (foundAppHash) {
        OS_LOGE(TAG, "Duplicate hash for app.bin");
        return false;
      }
      if (!_tryParseIntoHash(hash, release.appBinaryHash)) {
        return false;
      }
      foundAppHash = true;
    } else if (file == "staticfs.bin" || file == "littlefs.bin") {
      if (foundFilesystemHash) {
        OS_LOGE(TAG, "Duplicate hash for filesystem binary (%.*s)", file.size(), file.data());
        return false;
      }
      if (!_tryParseIntoHash(hash, release.filesystemBinaryHash)) {
        return false;
      }
      // Update the URL to match the actual filename used in this release
      release.filesystemBinaryUrl = std::string(downloadBase) + std::string(file);
      foundFilesystemHash = true;
    }
  }

  if (!foundAppHash) {
    OS_LOGE(TAG, "Missing hash for app.bin");
    return false;
  }

  if (!foundFilesystemHash) {
    OS_LOGE(TAG, "Missing hash for staticfs.bin or littlefs.bin");
    return false;
  }

  return true;
}

bool OtaUpdateManager::TryStartFirmwareUpdate(const OpenShock::SemVer& version)
{
  OS_LOGD(TAG, "Requesting firmware version %s", version.toString().c_str());  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this

  return otaum_try_queue_update_request(version);
}

FirmwareBootType OtaUpdateManager::GetFirmwareBootType()
{
  return _bootType;
}

bool OtaUpdateManager::IsValidatingApp()
{
  return _otaImageState == ESP_OTA_IMG_PENDING_VERIFY;
}

void OtaUpdateManager::InvalidateAndRollback()
{
  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::RollingBack)) {
    OS_PANIC(TAG, "Failed to set OTA firmware boot type in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
    return;
  }

  switch (esp_ota_mark_app_invalid_rollback_and_reboot()) {
    case ESP_FAIL:
      OS_LOGE(TAG, "Rollback failed (ESP_FAIL)");
      break;
    case ESP_ERR_OTA_ROLLBACK_FAILED:
      OS_LOGE(TAG, "Rollback failed (ESP_ERR_OTA_ROLLBACK_FAILED)");
      break;
    default:
      OS_LOGE(TAG, "Rollback failed (Unknown)");
      break;
  }

  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::None)) {
    OS_LOGE(TAG, "Failed to set OTA firmware boot type");
  }

  esp_restart();
}

void OtaUpdateManager::ValidateApp()
{
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    OS_PANIC(TAG, "Unable to mark app as valid, WTF?");  // TODO: Wtf do we do here?
  }

  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Validated)) {
    OS_PANIC(TAG, "Failed to set OTA firmware boot type in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
  }

  _otaImageState = ESP_OTA_IMG_VALID;
}

bool OtaUpdateManager::IsUpdateInProgress()
{
  return s_isUpdating.load(std::memory_order_acquire);
}
