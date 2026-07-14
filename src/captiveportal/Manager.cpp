#include <freertos/FreeRTOS.h>

#include "captiveportal/Manager.h"

const char* const TAG = "CaptivePortal";

#include "captiveportal/CaptivePortalInstance.h"
#include "CommandHandler.h"
#include "config/Config.h"
#include "Core.h"
#include "GatewayConnectionManager.h"
#include "Logging.h"
#include "util/TaskUtils.h"
#include "wifi/WiFiManager.h"

#include <ESPAsyncWebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <Preferences.h>

#include <esp_timer.h>

#include "SimpleMutex.h"

#include <atomic>
#include <memory>

using namespace OpenShock;

static std::atomic<bool> s_alwaysEnabled                 = false;
static std::atomic<bool> s_apEnabled                     = true;
static std::atomic<bool> s_enabled                       = true;
static std::atomic<bool> s_forceClosed                   = false;
static std::atomic<bool> s_userDone                      = false;
static esp_timer_handle_t s_captivePortalUpdateLoopTimer = nullptr;
static SimpleMutex s_instanceMutex;
static std::shared_ptr<CaptivePortal::CaptivePortalInstance> s_instance = nullptr;

// AP bring-up (cold WiFi radio init, AsyncWebServer/LittleFS/DNS setup) is too stack-heavy to run
// directly on the esp_timer service task (small, shared stack) — it's deferred to this worker task.
static TaskHandle_t s_captivePortalWorkerTask = nullptr;
static std::atomic<bool> s_pendingStart = false;
static std::atomic<bool> s_pendingStop  = false;

// Absolute esp_timer timestamps (microseconds). 0 = not armed.
static std::atomic<int64_t> s_startupGraceExpiry = 0;  // Don't open portal until this time passes
static std::atomic<int64_t> s_autoCloseExpiry    = 0;  // Auto-close AP when no clients connected and device is online

static constexpr int64_t STARTUP_GRACE_PERIOD_US = 30LL * 1'000'000;     // 30 seconds
static constexpr int64_t AUTO_CLOSE_DELAY_US     = 5LL * 60 * 1'000'000; // 5 minutes

static bool isDeviceFullyConfigured()
{
  std::vector<Config::WiFiCredentials> credentialsList;
  if (!Config::GetWiFiCredentials(credentialsList) || credentialsList.empty()) {
    return false;
  }
  return Config::HasBackendAuthToken();
}

static std::shared_ptr<CaptivePortal::CaptivePortalInstance> GetInstance()
{
  ScopedLock lock__(&s_instanceMutex);
  return s_instance;
}
static void CreateInstance()
{
  ScopedLock lock__(&s_instanceMutex);
  s_instance = std::make_shared<CaptivePortal::CaptivePortalInstance>();
}
static void DestroyInstance()
{
  ScopedLock lock__(&s_instanceMutex);
  s_instance = nullptr;
}

static void persistApEnabled(bool enabled)
{
  Preferences prefs;
  if (!prefs.begin("oled_ui", false)) {
    return;
  }

  prefs.putBool("ap_en", enabled);
  prefs.end();
}

static bool captiveportal_start()
{
  if (GetInstance() != nullptr) {
    OS_LOGD(TAG, "Already started");
    return true;
  }

  OS_LOGI(TAG, "Starting captive portal");

  if (s_apEnabled.load(std::memory_order_relaxed)) {
    if (!WiFi.enableAP(true)) {
      OS_LOGE(TAG, "Failed to enable AP mode");
      return false;
    }

    if (!WiFi.softAP((OPENSHOCK_FW_AP_PREFIX + WiFi.macAddress()).c_str())) {
      OS_LOGE(TAG, "Failed to start AP");
      WiFi.softAPdisconnect(true);
      return false;
    }

    IPAddress apIP(4, 3, 2, 1);
    if (!WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0))) {
      OS_LOGE(TAG, "Failed to configure AP");
      WiFi.softAPdisconnect(true);
      return false;
    }
  } else {
    WiFi.softAPdisconnect(true);
    WiFi.enableAP(false);
  }

  std::string hostname;
  if (!Config::GetWiFiHostname(hostname)) {
    OS_LOGE(TAG, "Failed to get WiFi hostname, reverting to default");
    hostname = OPENSHOCK_FW_HOSTNAME;
  }

  CreateInstance();

  return true;
}
static void captiveportal_stop()
{
  if (GetInstance() == nullptr) {
    OS_LOGD(TAG, "Already stopped");
    return;
  }

  OS_LOGI(TAG, "Stopping captive portal");

  DestroyInstance();
  s_userDone = false;

  WiFi.softAPdisconnect(true);
}

// Runs captiveportal_start()/captiveportal_stop() on a dedicated task with a real stack, instead
// of inline on the caller's context (the esp_timer service task's stack is too small for a cold
// WiFi AP bring-up + AsyncWebServer/LittleFS/DNS setup, especially when STA was never enabled to
// warm up the WiFi driver first).
static void captiveportal_worker_task(void*)
{
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (s_pendingStop.exchange(false, std::memory_order_relaxed)) {
      captiveportal_stop();
    }
    if (s_pendingStart.exchange(false, std::memory_order_relaxed)) {
      captiveportal_start();
    }
  }
}

static void captiveportal_request_start()
{
  s_pendingStart.store(true, std::memory_order_relaxed);
  if (s_captivePortalWorkerTask != nullptr) {
    xTaskNotifyGive(s_captivePortalWorkerTask);
  }
}

static void captiveportal_request_stop()
{
  s_pendingStop.store(true, std::memory_order_relaxed);
  if (s_captivePortalWorkerTask != nullptr) {
    xTaskNotifyGive(s_captivePortalWorkerTask);
  }
}

static void captiveportal_updateloop(void*)
{
  int64_t now = esp_timer_get_time();

  if (!s_enabled.load(std::memory_order_relaxed)) {
    if (GetInstance() != nullptr) {
      captiveportal_request_stop();
    }
    return;
  }

  // Force-close overrides everything — including alwaysEnabled — so OTA updates
  // can always shut down the portal and free its memory/AP resources.
  // Keep it closed while WiFi is connected, but clear the latch when WiFi drops
  // so AP can recover automatically for offline reconfiguration.
  if (s_forceClosed) {
    if (!WiFiManager::IsConnected()) {
      s_forceClosed = false;
      OS_LOGI(TAG, "Clearing captive portal force-close latch (WiFi disconnected)");
    } else {
      if (GetInstance() != nullptr) {
        OS_LOGD(TAG, "Force-closing captive portal");
        captiveportal_request_stop();
      }
      return;
    }
  }

  // If always enabled, ensure portal is running and skip all grace/auto-close logic
  if (s_alwaysEnabled.load(std::memory_order_relaxed)) {
    if (GetInstance() == nullptr) {
      captiveportal_request_start();
    }
    return;
  }

  // Startup grace period: device is fully configured, wait for gateway connection
  int64_t graceExpiry = s_startupGraceExpiry.load(std::memory_order_relaxed);
  if (graceExpiry != 0) {
    if (GatewayConnectionManager::IsConnected()) {
      // Gateway connected during grace — clear grace, never open portal
      s_startupGraceExpiry.store(0, std::memory_order_relaxed);
      return;
    }
    if (now < graceExpiry) {
      // Still within grace period, don't open portal yet
      return;
    }
    // Grace expired without gateway connection — open portal normally
    s_startupGraceExpiry.store(0, std::memory_order_relaxed);
  }

  // User completed setup — close portal once device is fully online
  if (s_userDone && GatewayConnectionManager::IsConnected()) {
    if (GetInstance() != nullptr) {
      OS_LOGI(TAG, "User completed setup, closing captive portal");
      captiveportal_request_stop();
    }
    return;
  }

  // Auto-close: no clients connected, WiFi + gateway are up, 5 minutes elapsed
  auto instance = GetInstance();
  if (instance != nullptr && !s_alwaysEnabled && GatewayConnectionManager::IsConnected()) {
    if (instance->hasClients()) {
      // Clients still connected — reset timer
      s_autoCloseExpiry.store(0, std::memory_order_relaxed);
    } else {
      int64_t expiry = s_autoCloseExpiry.load(std::memory_order_relaxed);
      if (expiry == 0) {
        s_autoCloseExpiry.store(now + AUTO_CLOSE_DELAY_US, std::memory_order_relaxed);
      } else if (now >= expiry) {
        OS_LOGI(TAG, "Auto-closing captive portal AP (no clients for 5 minutes)");
        captiveportal_request_stop();
        return;
      }
    }
  } else {
    s_autoCloseExpiry.store(0, std::memory_order_relaxed);
  }

  // Open portal if not running and device needs setup, or when WiFi is down.
  if (instance == nullptr) {
    bool commandHandlerOk = CommandHandler::Ok();
    bool wifiConnected    = WiFiManager::IsConnected();
    bool shouldStart      = s_alwaysEnabled || !wifiConnected || !commandHandlerOk || !isDeviceFullyConfigured();
    if (shouldStart) {
      OS_LOGD(TAG, "Starting captive portal");
      captiveportal_request_start();
    }
  }
}

bool CaptivePortal::Init()
{
  // Bandaid: force AP + captive portal OFF at boot regardless of what's persisted. Booting with
  // AP persisted on while STA was off has caused a boot crash loop; forcing both off here
  // sidesteps that state entirely. The real persisted preference is re-applied a few seconds
  // after boot via ApplyPersistedState(), once the rest of boot has settled.
  s_apEnabled.store(false, std::memory_order_relaxed);
  s_enabled.store(false, std::memory_order_relaxed);
  s_alwaysEnabled.store(false, std::memory_order_relaxed);

  // If device is already fully configured, set a startup grace period before opening portal
  if (isDeviceFullyConfigured()) {
    s_startupGraceExpiry.store(esp_timer_get_time() + STARTUP_GRACE_PERIOD_US, std::memory_order_relaxed);
    OS_LOGI(TAG, "Device fully configured, startup grace period of 30s before opening portal");
  }

  // Pinned to core 1 (same as WiFiManager's own update task) — it calls WiFi.* APIs, which is fine
  // from any core, it just shouldn't compete with the WiFi driver's internal tasks on core 0.
  if (TaskUtils::TaskCreateUniversal(captiveportal_worker_task, "captiveportal_worker", 8192, nullptr, 1, &s_captivePortalWorkerTask, 1) != pdPASS) {
    OS_LOGE(TAG, "Failed to create captive portal worker task");
    return false;
  }

  esp_timer_create_args_t args = {
    .callback              = captiveportal_updateloop,
    .arg                   = nullptr,
    .dispatch_method       = ESP_TIMER_TASK,
    .name                  = "captive_portal_update",
    .skip_unhandled_events = true,
  };

  esp_err_t err;

  err = esp_timer_create(&args, &s_captivePortalUpdateLoopTimer);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to create captive portal update timer");
    return false;
  }

  err = esp_timer_start_periodic(s_captivePortalUpdateLoopTimer, 500'000);  // 500ms
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to start captive portal update timer");
    return false;
  }

  return true;
}

void CaptivePortal::ApplyPersistedState()
{
  bool apEnabled = true;
  Preferences prefs;
  if (prefs.begin("oled_ui", false)) {
    apEnabled = prefs.getBool("ap_en", true);
    prefs.end();
  }

  bool alwaysEnabled = false;
  Config::CaptivePortalConfig portalConfig;
  if (Config::GetCaptivePortalConfig(portalConfig)) {
    alwaysEnabled = portalConfig.alwaysEnabled;
  }

  SetApEnabled(apEnabled, false);
  SetEnabled(alwaysEnabled);
  SetAlwaysEnabled(alwaysEnabled, false);
}

void CaptivePortal::SetEnabled(bool enabled)
{
  if (enabled) {
    // Clear force-close latch so the portal can start again
    s_forceClosed.store(false, std::memory_order_relaxed);
  }
  s_enabled.store(enabled, std::memory_order_relaxed);
}

bool CaptivePortal::IsEnabled()
{
  return s_enabled.load(std::memory_order_relaxed);
}

void CaptivePortal::SetApEnabled(bool enabled, bool persistConfig)
{
  bool previous = s_apEnabled.exchange(enabled, std::memory_order_relaxed);
  if (persistConfig) {
    persistApEnabled(enabled);
  }

  if (previous == enabled) {
    return;
  }

  if (GetInstance() != nullptr) {
    captiveportal_request_stop();

    if (s_enabled.load(std::memory_order_relaxed)) {
      captiveportal_request_start();
    }
  } else if (!enabled) {
    WiFi.softAPdisconnect(true);
    WiFi.enableAP(false);
  }
}

bool CaptivePortal::IsApEnabled()
{
  return s_apEnabled.load(std::memory_order_relaxed);
}

void CaptivePortal::SetUserDone()
{
  s_userDone = true;
}

void CaptivePortal::SetAlwaysEnabled(bool alwaysEnabled, bool persistConfig)
{
  s_alwaysEnabled = alwaysEnabled;
  if (!persistConfig) {
    return;
  }

  Config::SetCaptivePortalConfig({
    .alwaysEnabled = alwaysEnabled,
  });
}
bool CaptivePortal::IsAlwaysEnabled()
{
  return s_alwaysEnabled;
}

bool CaptivePortal::ForceClose(uint32_t timeoutMs)
{
  s_forceClosed = true;

  if (GetInstance() == nullptr) return true;

  while (timeoutMs > 0) {
    uint32_t delay = std::min(timeoutMs, static_cast<uint32_t>(10U));

    vTaskDelay(pdMS_TO_TICKS(delay));

    timeoutMs -= delay;

    if (GetInstance() == nullptr) return true;
  }

  return false;
}

bool CaptivePortal::IsRunning()
{
  return GetInstance() != nullptr;
}

bool CaptivePortal::SendMessageTXT(uint8_t socketId, std::string_view data)
{
  auto instance = GetInstance();
  if (instance == nullptr) return false;

  instance->sendMessageTXT(socketId, data);

  return true;
}
bool CaptivePortal::SendMessageBIN(uint8_t socketId, tcb::span<const uint8_t> data)
{
  auto instance = GetInstance();
  if (instance == nullptr) return false;

  instance->sendMessageBIN(socketId, data);

  return true;
}

bool CaptivePortal::BroadcastMessageTXT(std::string_view data)
{
  auto instance = GetInstance();
  if (instance == nullptr) return false;

  instance->broadcastMessageTXT(data);

  return true;
}
bool CaptivePortal::BroadcastMessageBIN(tcb::span<const uint8_t> data)
{
  auto instance = GetInstance();
  if (instance == nullptr) return false;

  instance->broadcastMessageBIN(data);

  return true;
}
