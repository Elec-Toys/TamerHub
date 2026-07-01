#include "visual/OledDisplayManager.h"

const char* const TAG = "OledDisplayManager";

#include "captiveportal/Manager.h"
#include "CommandHandler.h"
#include "config/Config.h"
#include "OtaUpdateManager.h"
#include "estop/EStopManager.h"
#include "events/Events.h"
#include "GatewayConnectionManager.h"
#include "input/RotaryEncoderManager.h"
#include "Logging.h"
#include "util/TaskUtils.h"
#include "wifi/WiFiManager.h"
#include "wifi/WiFiScanManager.h"

#include <U8g2lib.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_event.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <freertos/FreeRTOS.h>

extern const unsigned char logo[];

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef OPENSHOCK_OLED_ENABLED
#define OPENSHOCK_OLED_ENABLED 0
#endif

#ifndef OPENSHOCK_OLED_I2C_ADDRESS
#define OPENSHOCK_OLED_I2C_ADDRESS 0x78
#endif

#ifndef OPENSHOCK_OLED_SDA_PIN
#define OPENSHOCK_OLED_SDA_PIN -1
#endif

#ifndef OPENSHOCK_OLED_SCL_PIN
#define OPENSHOCK_OLED_SCL_PIN -1
#endif

#ifndef OPENSHOCK_OLED_DETECT_PIN
#define OPENSHOCK_OLED_DETECT_PIN -1
#endif

#ifndef OPENSHOCK_OLED_POWER_PIN
#define OPENSHOCK_OLED_POWER_PIN -1
#endif

#ifndef OPENSHOCK_DISPLAY_ONLY_BOOT
#define OPENSHOCK_DISPLAY_ONLY_BOOT 0
#endif

#ifndef OPENSHOCK_WHITE_SCREEN_ONLY_BOOT
#define OPENSHOCK_WHITE_SCREEN_ONLY_BOOT 0
#endif

#ifndef OPENSHOCK_BATTERY_SENSE_PIN
  #if defined(A4)
    #define OPENSHOCK_BATTERY_SENSE_PIN A4
  #else
    #define OPENSHOCK_BATTERY_SENSE_PIN -1
  #endif
#endif

#ifndef OPENSHOCK_POWER_HOLD_PIN
#define OPENSHOCK_POWER_HOLD_PIN 13
#endif

namespace {
  void requestRefresh();
  void setDisplayPower(bool enabled);

  enum class OledControllerType : uint8_t {
    SH1106,
    SSD1309,
  };

  OledControllerType detectOledControllerType()
  {
    if (OPENSHOCK_OLED_DETECT_PIN < 0) {
      return OledControllerType::SH1106;
    }

    pinMode(OPENSHOCK_OLED_DETECT_PIN, INPUT_PULLDOWN);
    const int level = digitalRead(OPENSHOCK_OLED_DETECT_PIN);

    if (level == HIGH) {
      OS_LOGI(TAG, "OLED detect pin %d is HIGH, selecting SSD1309", OPENSHOCK_OLED_DETECT_PIN);
      return OledControllerType::SSD1309;
    }

    OS_LOGI(TAG, "OLED detect pin %d is LOW, selecting SH1106", OPENSHOCK_OLED_DETECT_PIN);
    return OledControllerType::SH1106;
  }

  uint8_t detectOledI2CAddress()
  {
    const uint8_t configuredAddress7Bit = static_cast<uint8_t>(OPENSHOCK_OLED_I2C_ADDRESS >> 1);
    const uint8_t candidates[] = { configuredAddress7Bit, 0x3C, 0x3D };

    for (uint8_t candidate : candidates) {
      Wire.beginTransmission(candidate);
      if (Wire.endTransmission() == 0) {
        const uint8_t detectedAddress = static_cast<uint8_t>(candidate << 1);
        OS_LOGI(TAG, "Detected OLED on I2C address 0x%02X", detectedAddress);
        return detectedAddress;
      }
    }

    OS_LOGW(TAG, "No OLED ACK detected on expected I2C addresses, using configured address 0x%02X", OPENSHOCK_OLED_I2C_ADDRESS);
    return OPENSHOCK_OLED_I2C_ADDRESS;
  }

  constexpr uint8_t kPageMain = 0;
  constexpr uint8_t kPageSettings = 1;
  constexpr uint8_t kPageCount = 2;
  constexpr uint8_t kSettingsVisibleCount = 4;
  constexpr uint8_t kSettingsItemCount = 6;
  constexpr uint8_t kShockerDetailItemCount = 5;  // name-edit, model, limit, test, delete
  constexpr uint8_t kShockerListStaticItemCount = 2;  // keepalive toggle + add shocker
  constexpr uint8_t kMaxShockers = 10;
  constexpr uint8_t kNetworkItemCount = 5;
  constexpr uint8_t kSystemItemCount = 6;
  constexpr uint8_t kConnectNetworkItemCount = 3;
  constexpr uint8_t kAccountMenuMaxItemCount = 2;  // Link Account (always) + Delete Token (if token saved)
  constexpr uint8_t kUpdateItemCount = 5;
  constexpr uint8_t kPasswordPickerItemCount = 96;  // backspace + ASCII 32..126
  constexpr uint8_t kAccountCodeLength = 6;
  constexpr uint8_t kAccountDigitItemCount = 10;
  constexpr uint16_t kInfoPopupDurationMs = 3000;
  constexpr int64_t kPowerHoldBootDelayMs = 1000;
  constexpr int64_t kPowerOffMessageDurationMs = 1000;
  constexpr int64_t kChargingModeLongPressMs = 1000;
  constexpr uint32_t kChargingScreenDurationMs = 5000;
  constexpr int kPowerHoldPin = OPENSHOCK_POWER_HOLD_PIN;
  constexpr int kDisplayPowerPin = OPENSHOCK_OLED_POWER_PIN;
  constexpr int kLogoWidth = 128;
  constexpr int kLogoHeight = 64;
  constexpr uint16_t kBatteryDefaultMaxMv = 4100;

  enum class PowerUiState : uint8_t {
    BootDelay,
    Ready,
    ConfirmPowerOff,
    Goodbye,
    PowerCut,
  };

  enum class SettingsView : uint8_t {
    Root,
    Network,
    System,
    SystemScreenSleepEdit,
    SystemDeviceSleepEdit,
    BatteryCalibrationConfirmClear,
    BatteryCalibrationConfirmCharged,
    About,
    AccountMenu,
    AccountLink,
    Update,
    Connect,
    ConnectNetwork,
    ConnectPassword,
    ShockerList,
    ShockerDetail,
    ShockerNameEdit,
    ShockerLimitEdit,
    ShockerProtocolEdit,
    UpdatePrompt,
    UpdateRepoEdit,
  };

#if OPENSHOCK_OLED_ENABLED
  // 1.3" OLED modules are commonly SH1106 at 128x64.
  U8G2_SH1106_128X64_NONAME_F_HW_I2C s_sh1106Display(U8G2_R0, U8X8_PIN_NONE);
  U8G2_SSD1309_128X64_NONAME0_F_HW_I2C s_ssd1309Display(U8G2_R0, U8X8_PIN_NONE);
  U8G2* s_displayPtr = &s_sh1106Display;
  OledControllerType s_detectedControllerType = OledControllerType::SH1106;

  #define s_display (*s_displayPtr)

  bool s_initialized = false;
  TaskHandle_t s_refreshTask = nullptr;
  std::atomic<uint8_t> s_currentPage = kPageMain;
  std::atomic<bool> s_forceRedraw = true;
  uint8_t s_lastMainLimitDrawn = 0xFF;
  uint8_t s_lastMainWifiStrength = 0xFF;
  bool s_lastMainWifiConnected = false;
  bool s_lastMainGwOnline = false;
  bool s_lastMainAccountLinked = false;
  uint8_t s_mainBatteryPercent = 1;
  bool s_batteryInitialized = false;
  uint16_t s_batteryFilteredMv = 0;
  int64_t s_lastBatterySampleAt = 0;
  int8_t s_batteryPendingDirection = 0;
  uint8_t s_batteryPendingCount = 0;
  uint16_t s_batteryMaxMv = kBatteryDefaultMaxMv;
  bool s_batteryCalibrationPending = false;
  TaskHandle_t s_batteryCalibrationTask = nullptr;
  // ── Main-page active shocker state ──
  bool s_activeShockerIsOnline = false;
  uint8_t s_activeShockerIdx = 0;
  bool s_mainShockActive = false;
  bool s_mainVibrateActive = false;
  int64_t s_mainCommandLastSentMs = 0;
  int64_t s_mainCommandAutoStopAtMs = 0;
  std::vector<std::pair<uint16_t, uint8_t>> s_localMainIntensityByRf {};
  std::vector<std::pair<uint16_t, uint8_t>> s_onlineMainIntensityByRf {};
  // ────────────────────────────────────
  uint8_t s_settingsSelection = 0;
  uint8_t s_settingsFirstVisible = 0;
  SettingsView s_settingsView = SettingsView::Root;
  uint8_t s_networkSelection = 0;
  uint8_t s_networkFirstVisible = 0;
  uint8_t s_systemSelection = 0;
  uint8_t s_systemFirstVisible = 0;
  uint8_t s_connectSelection = 0;
  uint8_t s_connectFirstVisible = 0;
  uint8_t s_connectNetworkSelection = 0;
  uint8_t s_connectNetworkFirstVisible = 0;
  uint8_t s_passwordCharSelection = static_cast<uint8_t>('a' - 31);
  uint8_t s_passwordLength = 0;
  int64_t s_lastConnectScanAt = 0;
  int64_t s_lastDefaultConnectAttemptAt = 0;
  bool s_networkWifiEnabled = true;
  bool s_networkAccessPointEnabled = true;
  bool s_networkCaptivePortalEnabled = true;
  bool s_shockerKeepAliveEnabled = true;
  bool s_networkStatusOverlayOpen = false;
  bool s_passwordConnectPending = false;
  bool s_pendingConnectFromPasswordEntry = false;
  int64_t s_passwordConnectStartedAt = 0;
  uint8_t s_accountDigitSelection = 0;
  uint8_t s_accountCodeLength = 0;
  bool s_accountLinkPending = false;
  char s_accountCodeInput[kAccountCodeLength + 1] = {};
  char s_selectedConnectSsid[33] = {};
  uint8_t s_selectedConnectCredentialsId = 0;
  wifi_auth_mode_t s_selectedConnectAuthMode = WIFI_AUTH_OPEN;
  char s_defaultNetworkSsid[33] = {};
  char s_passwordInput[65] = {};
    // ── Shocker state ──
    struct ShockerEntry {
      char name[20];
      uint8_t protocol;
      uint8_t limit;
      uint16_t rfId;
    };

    enum class ShockerSelectionSource : uint8_t {
      Local,
      Online,
    };

    ShockerEntry s_shockers[kMaxShockers] {};
    uint8_t s_shockerCount = 0;
    uint8_t s_shockerListSelection = 0;
    uint8_t s_shockerListFirstVisible = 0;
    uint8_t s_selectedShockerIndex = 0;
    uint8_t s_selectedOnlineShockerIndex = 0;
    ShockerSelectionSource s_selectedShockerSource = ShockerSelectionSource::Local;
    uint8_t s_shockerDetailSelection = 0;
    uint8_t s_shockerDetailFirstVisible = 0;
    uint8_t s_pendingLimit = 0;
    uint8_t s_originalLimit = 0;
    uint8_t s_pendingProtocol = 0;
    uint8_t s_originalProtocol = 0;
    // Name-edit reuses s_passwordCharSelection, s_passwordLength, s_passwordInput
    char s_shockerNameBackup[20] = {};
    // ──────────────────
  char s_infoPopupMessage[96] = {};
  int64_t s_infoPopupHideAt = 0;
  bool s_infoPopupWasVisible = false;
  bool s_persistentInfoPopup = false;
  uint16_t s_screenSleepSeconds = 0;
  uint16_t s_deviceSleepMinutes = 0;
  bool s_screenSaverEnabled = false;
  bool s_batteryIconEnabled = true;
  bool s_batteryPercentEnabled = true;
  uint16_t s_pendingSystemValue = 0;
  uint16_t s_originalSystemValue = 0;
  bool s_screenSleepActive = false;
  std::atomic<uint32_t> s_lastUserInputAt = 0;
  std::atomic<uint32_t> s_inputBlockUntilAt = 0;
  PowerUiState s_powerUiState = PowerUiState::BootDelay;
  int64_t s_powerUiDeadlineMs = 0;
  bool s_powerHoldPinConfigured = false;
  bool s_displayPowerPinConfigured = false;
  bool s_powerCutIssued = false;
  gpio_num_t s_encoderButtonPin = GPIO_NUM_NC;
  TaskHandle_t s_accountLinkTask = nullptr;
  uint8_t s_accountMenuSelection = 0;
  uint8_t s_accountMenuFirstVisible = 0;
  uint8_t s_updateSelection = 0;
  uint8_t s_updateFirstVisible = 0;
  bool s_otaEnabled        = true;
  bool s_otaAutoUpdate     = false;
  bool s_otaPromptUpdates  = true;
  bool s_otaNeverPrompt    = false;
  char s_otaRepoSlug[64]   = {};
  uint8_t s_updatePromptSelection = 0;
  char s_pendingUpdateVersionDisplay[32] = {};
  SettingsView s_prevSettingsViewBeforePrompt = SettingsView::Root;
  uint8_t s_prevPageBeforePrompt = 0;
  bool s_inputLocked = false;
  int64_t s_lockFlashUntilMs = 0;
  bool s_checkInProgress = false;
  Preferences s_oledPrefs;
  bool s_oledPrefsReady = false;

  constexpr char kPrefsNamespace[] = "oled_ui";
  constexpr char kPrefWiFiEnabled[] = "wifi_en";
  constexpr char kPrefApEnabled[] = "ap_en";
  constexpr char kPrefCaptivePortalEnabled[] = "cp_en";
  constexpr char kPrefDefaultSsid[] = "def_ssid";
  constexpr char kPrefScreenSleepSeconds[] = "scr_slp_s";
  constexpr char kPrefDeviceSleepMinutes[] = "dev_slp_m";
  constexpr char kPrefScreenSaverEnabled[] = "scr_sv_en";
  constexpr char kPrefBatteryIconEnabled[] = "bat_ic_en";
  constexpr char kPrefBatteryPctEnabled[] = "bat_pc_en";
  constexpr char kPrefBatteryMaxMv[] = "bat_max_mv";
  constexpr char kShockerPrefsNamespace[] = "shockers";

  constexpr std::array<std::string_view, kSettingsItemCount> kSettingsItems {
    "Shockers",
    "Network",
    "Account",
    "System",
    "Update",
    "About",
  };

  constexpr std::array<std::string_view, kNetworkItemCount> kNetworkItems {
    "Wi-Fi",
    "Access Point",
    "Captive Portal",
    "Connect",
    "Status",
  };

  constexpr std::array<std::string_view, kSystemItemCount> kSystemItems {
    "Screen Sleep",
    "Screen Saver",
    "Device Sleep",
    "Battery Icon",
    "Battery Level",
    "Battery Calibration",
  };

  constexpr std::array<std::string_view, kConnectNetworkItemCount> kConnectNetworkItems {
    "Connect",
    "Set Default",
    "Forget",
  };

  constexpr std::array<std::string_view, kAccountMenuMaxItemCount> kAccountMenuItems {
    "Link Account",
    "Delete Token",
  };

  constexpr std::array<std::string_view, kUpdateItemCount> kUpdateItems {
    "Check For Updates",
    "Auto Update",
    "Prompt To Update",
    "GitHub Source",
    "Version Info",
  };

  constexpr int kShockerListPrefixX = 13;
  constexpr int kShockerListTextX = 24;

  // Icons sourced from MusicalCreeper01/OLED-Icons- (wifi, wifimed, wifilow).
  constexpr std::array<std::string_view, 3> kWifiStrengthIconCodes {
    "11770000000000111000100000100110010100001010100000000",  // strong
    "11770000000000000000000000000110000100000010100000000",  // medium
    "11770000000000000000000000000000000000000000100000000",  // low
  };

  // Link/chain icon: two interlocked rectangular links (7x7)
  constexpr std::string_view kLinkIconCode { "11770111000100010011111000011111001000100011100000000" };
  constexpr int kBatterySensePin = OPENSHOCK_BATTERY_SENSE_PIN;
  constexpr uint16_t kBatteryMinMv = 3000;
  constexpr int64_t kBatterySamplePeriodMs = 750;
  constexpr uint8_t kBatterySamplesPerCycle = 8;
  // 625 µs × 8 samples = 5 ms = exactly one 200 Hz noise cycle → sinusoidal noise sums to zero
  constexpr uint32_t kBatterySampleDelayUs = 625;
  constexpr uint8_t kBatteryNeighborConfirmCount = 6;
  constexpr float kBatteryCalibrationMarginRatio = 0.95f;
  constexpr uint16_t kBatteryCalibrationSamples = 50;
  constexpr uint32_t kBatteryCalibrationIntervalMs = 20;  // 50 * 20ms = ~1s

  constexpr std::array<OpenShock::ShockerModelType, 5> kProtocolOptions {
    OpenShock::ShockerModelType::CaiXianlin,
    OpenShock::ShockerModelType::Petrainer,
    OpenShock::ShockerModelType::Petrainer998DR,
    OpenShock::ShockerModelType::WellturnT330,
    OpenShock::ShockerModelType::D80,
  };

  std::string_view getSettingsItem(SettingsView view, uint8_t index)
  {
    if (view == SettingsView::Network) {
      return kNetworkItems[index];
    }

    if (view == SettingsView::System) {
      return kSystemItems[index];
    }

    if (view == SettingsView::ConnectNetwork) {
      return kConnectNetworkItems[index];
    }

    if (view == SettingsView::AccountMenu) {
      return (index < kAccountMenuMaxItemCount) ? kAccountMenuItems[index] : kAccountMenuItems[0];
    }

    if (view == SettingsView::Update) {
      return (index < kUpdateItemCount) ? kUpdateItems[index] : kUpdateItems[0];
    }

    return kSettingsItems[index];
  }

  uint8_t getAccountMenuItemCount()
  {
    std::string token;
    return (OpenShock::Config::GetBackendAuthToken(token) && !token.empty()) ? 2 : 1;
  }

  // ── Shocker NVS helpers ──
  const char* shockerModelLabel(OpenShock::ShockerModelType model)
  {
    switch (model) {
      case OpenShock::ShockerModelType::Petrainer:
        return "Petrainer";
      case OpenShock::ShockerModelType::Petrainer998DR:
        return "Petrainer998DR";
      case OpenShock::ShockerModelType::WellturnT330:
        return "WellturnT330";
      case OpenShock::ShockerModelType::CaiXianlin:
        return "CaiXianlin";
      case OpenShock::ShockerModelType::D80:
        return "D80";
      default:
        return "CaiXianlin";
    }
  }

  OpenShock::ShockerModelType shockerModelForStoredValue(uint8_t value)
  {
    switch (static_cast<OpenShock::ShockerModelType>(value)) {
      case OpenShock::ShockerModelType::Petrainer:
      case OpenShock::ShockerModelType::Petrainer998DR:
      case OpenShock::ShockerModelType::WellturnT330:
      case OpenShock::ShockerModelType::CaiXianlin:
      case OpenShock::ShockerModelType::D80:
        return static_cast<OpenShock::ShockerModelType>(value);
      default:
        return OpenShock::ShockerModelType::CaiXianlin;
    }
  }

  uint8_t findProtocolOptionIndex(OpenShock::ShockerModelType model)
  {
    for (uint8_t i = 0; i < kProtocolOptions.size(); ++i) {
      if (kProtocolOptions[i] == model) {
        return i;
      }
    }

    return 0;
  }

  std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> getOnlineShockersSnapshot()
  {
    return OpenShock::GatewayConnectionManager::GetOnlineShockers();
  }

  uint8_t getTotalShockerListItemCount(const std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo>& onlineShockers)
  {
    const std::size_t total = kShockerListStaticItemCount + static_cast<std::size_t>(s_shockerCount) + onlineShockers.size();
    return static_cast<uint8_t>(std::min<std::size_t>(total, 255));
  }

  void updateGatewayLocalRfReservations()
  {
    std::array<uint16_t, kMaxShockers> localRfIds {};
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      localRfIds[i] = s_shockers[i].rfId;
    }

    OpenShock::GatewayConnectionManager::SetLocalRfIds(tcb::span<const uint16_t>(localRfIds.data(), s_shockerCount));
  }

  uint16_t findNextLocalRfId()
  {
    for (uint32_t candidate = 1; candidate <= UINT16_MAX; ++candidate) {
      const uint16_t rfId = static_cast<uint16_t>(candidate);
      bool usedByLocal = false;
      for (uint8_t i = 0; i < s_shockerCount; ++i) {
        if (s_shockers[i].rfId == rfId) {
          usedByLocal = true;
          break;
        }
      }

      if (usedByLocal) {
        continue;
      }

      if (OpenShock::GatewayConnectionManager::IsOnlineRfIdReserved(rfId)) {
        continue;
      }

      return rfId;
    }

    return 1;
  }

  void saveShockerPrefs()
  {
    Preferences p;
    if (!p.begin(kShockerPrefsNamespace, false)) {
      return;
    }

    p.putUChar("count", s_shockerCount);
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      char nameKey[8], limKey[8], protKey[8], rfKey[8];
      std::snprintf(nameKey, sizeof(nameKey), "n%u", i);
      std::snprintf(limKey, sizeof(limKey), "l%u", i);
      std::snprintf(protKey, sizeof(protKey), "p%u", i);
      std::snprintf(rfKey, sizeof(rfKey), "r%u", i);
      p.putString(nameKey, s_shockers[i].name);
      p.putUChar(protKey, s_shockers[i].protocol);
      p.putUChar(limKey, s_shockers[i].limit);
      p.putUShort(rfKey, s_shockers[i].rfId);
    }
    p.end();

    updateGatewayLocalRfReservations();
  }

  void loadShockerPrefs()
  {
    Preferences p;
    if (!p.begin(kShockerPrefsNamespace, true)) {
      return;
    }

    s_shockerCount = std::min<uint8_t>(p.getUChar("count", 0), kMaxShockers);
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      char nameKey[8], limKey[8], protKey[8], rfKey[8];
      std::snprintf(nameKey, sizeof(nameKey), "n%u", i);
      std::snprintf(limKey, sizeof(limKey), "l%u", i);
      std::snprintf(protKey, sizeof(protKey), "p%u", i);
      std::snprintf(rfKey, sizeof(rfKey), "r%u", i);
      String n = p.getString(nameKey, "");
      std::memset(s_shockers[i].name, 0, sizeof(s_shockers[i].name));
      std::strncpy(s_shockers[i].name, n.c_str(), sizeof(s_shockers[i].name) - 1);
      s_shockers[i].protocol = static_cast<uint8_t>(shockerModelForStoredValue(p.getUChar(protKey, static_cast<uint8_t>(OpenShock::ShockerModelType::CaiXianlin))));
      s_shockers[i].limit = p.getUChar(limKey, 50);
      s_shockers[i].rfId = p.getUShort(rfKey, static_cast<uint16_t>(i + 1));
    }
    p.end();

    // Backfill legacy entries that didn't persist RF IDs and enforce online conflict safety.
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      if (s_shockers[i].rfId == 0) {
        s_shockers[i].rfId = static_cast<uint16_t>(i + 1);
      }
    }

    updateGatewayLocalRfReservations();

    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      bool conflict = false;
      for (uint8_t j = 0; j < s_shockerCount; ++j) {
        if (i != j && s_shockers[i].rfId == s_shockers[j].rfId) {
          conflict = true;
          break;
        }
      }

      if (!conflict && !OpenShock::GatewayConnectionManager::IsOnlineRfIdReserved(s_shockers[i].rfId)) {
        continue;
      }

      s_shockers[i].rfId = findNextLocalRfId();
      updateGatewayLocalRfReservations();
    }

    saveShockerPrefs();
  }
  // ────────────────────────

  // Returns the label for a shocker detail row
  uint8_t getShockerDetailItemCount()
  {
    return kShockerDetailItemCount;
  }

  const char* getSelectedShockerName()
  {
    if (s_selectedShockerSource == ShockerSelectionSource::Local) {
      return s_shockers[s_selectedShockerIndex].name;
    }

    static char onlineName[24] = {};
    const auto onlineShockers = getOnlineShockersSnapshot();
    if (s_selectedOnlineShockerIndex >= onlineShockers.size()) {
      std::strncpy(onlineName, "Unavailable", sizeof(onlineName) - 1);
      onlineName[sizeof(onlineName) - 1] = '\0';
      return onlineName;
    }

    std::memset(onlineName, 0, sizeof(onlineName));
    std::strncpy(onlineName, onlineShockers[s_selectedOnlineShockerIndex].displayName.c_str(), sizeof(onlineName) - 1);
    return onlineName;
  }

  void getShockerDetailItem(uint8_t row, char* buf, uint8_t bufLen)
  {
    if (s_selectedShockerSource == ShockerSelectionSource::Online) {
      const auto onlineShockers = getOnlineShockersSnapshot();
      if (s_selectedOnlineShockerIndex >= onlineShockers.size()) {
        std::strncpy(buf, "Unavailable", bufLen - 1);
        buf[bufLen - 1] = '\0';
        return;
      }

      const auto& shocker = onlineShockers[s_selectedOnlineShockerIndex];
      if (row == 0) {
        std::strncpy(buf, shocker.displayName.c_str(), bufLen - 1);
        buf[bufLen - 1] = '\0';
      } else if (row == 1) {
        std::snprintf(buf, bufLen, "Model: %s", shockerModelLabel(shocker.model));
      } else if (row == 2) {
        std::snprintf(buf, bufLen, "Limit: %u", shocker.limit);
      } else if (row == 3) {
        std::strncpy(buf, "Test", bufLen - 1);
        buf[bufLen - 1] = '\0';
      } else {
        const bool gatewayOnline = OpenShock::GatewayConnectionManager::IsConnected();
        std::strncpy(buf, gatewayOnline ? (shocker.disabled ? "Enable Shocker" : "Disable Shocker") : "Delete Shocker", bufLen - 1);
        buf[bufLen - 1] = '\0';
      }

      return;
    }

    if (row == 0) {
      std::strncpy(buf, s_shockers[s_selectedShockerIndex].name, bufLen - 1);
      buf[bufLen - 1] = '\0';
    } else if (row == 1) {
      std::snprintf(buf, bufLen, "Model: %s", shockerModelLabel(shockerModelForStoredValue(s_shockers[s_selectedShockerIndex].protocol)));
    } else if (row == 2) {
      std::snprintf(buf, bufLen, "Limit: %u", s_shockers[s_selectedShockerIndex].limit);
    } else if (row == 3) {
      std::strncpy(buf, "Test", bufLen - 1);
      buf[bufLen - 1] = '\0';
    } else {
      std::strncpy(buf, "Delete Shocker", bufLen - 1);
      buf[bufLen - 1] = '\0';
    }
  }

  bool isHelpButtonVisibleForCurrentView()
  {
    if (s_settingsView == SettingsView::Connect || s_settingsView == SettingsView::ConnectPassword || s_settingsView == SettingsView::ShockerNameEdit || s_settingsView == SettingsView::ShockerLimitEdit || s_settingsView == SettingsView::ShockerProtocolEdit || s_settingsView == SettingsView::AccountLink || s_settingsView == SettingsView::BatteryCalibrationConfirmClear || s_settingsView == SettingsView::BatteryCalibrationConfirmCharged || s_settingsView == SettingsView::UpdatePrompt || s_settingsView == SettingsView::UpdateRepoEdit) {
      return false;
    }

    if (s_settingsView == SettingsView::Network && s_networkStatusOverlayOpen) {
      return false;
    }

    return true;
  }

  const char* getCurrentHelpText()
  {
    if (s_settingsView == SettingsView::Root) {
      switch (s_settingsSelection) {
        case 0:
          return "Manage saved shocker profiles";
        case 1:
          return "Configure Wi-Fi and AP";
        case 2:
          return "Link this hub to your account";
        case 3:
          return "Display and sleep behavior settings";
        case 4:
          return "Firmware update options";
        case 5:
          return "Show device information";
        default:
          return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::Network) {
      switch (s_networkSelection) {
        case 0:
          return "Toggle station Wi-Fi mode";
        case 1:
          return "Toggle captive portal AP";
        case 2:
          return "Toggle always-on config page";
        case 3:
          return "Scan and join networks";
        case 4:
          return "Show SSID and IP status";
        default:
          return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::System) {
      switch (s_systemSelection) {
        case 0:
          return "Set display timeout in seconds (0=never)";
        case 1:
          return "Show screensaver text instead of blank display";
        case 2:
          return "Set device sleep timeout in minutes (0=never)";
        case 3:
          return "Show or hide the battery icon on the main page";
        case 4:
          return "Show or hide the battery level on the main page";
        case 5:
          return "Recalibrate the battery percentage reading";
        default:
          return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::About) {
      return "About this hub";
    }

    if (s_settingsView == SettingsView::AccountMenu) {
      switch (s_accountMenuSelection) {
        case 0:
          return "Link this hub to your account";
        case 1:
          return "Remove the saved auth token";
        default:
          return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::Update) {
      switch (s_updateSelection) {
        case 0: return "Check for a firmware update right now";
        case 1: return s_otaAutoUpdate ? "Auto update: ON (no prompt)" : "Auto update: OFF (manual mode)";
        case 2: return (s_otaPromptUpdates && !s_otaNeverPrompt) ? "Prompts: ON (ask before updating)" : "Prompts: OFF (silent skip)";
        case 3: return "Change the GitHub repo used for updates";
        case 4: return "Show installed and latest firmware versions";
        default: return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::ConnectNetwork) {
      switch (s_connectNetworkSelection) {
        case 0:
          return "Connect to selected network";
        case 1:
          return "Save as preferred network";
        case 2:
          return "Forget saved credentials";
        default:
          return "Select an option";
      }
    }

    if (s_settingsView == SettingsView::ShockerList) {
      if (s_shockerListSelection == 0) {
        return "Toggle RF keep-alive packets for active shockers";
      }

      if (s_shockerListSelection == 1) {
        return "Create a new shocker entry";
      }

      return "Open this shocker settings";
    }

    if (s_settingsView == SettingsView::ShockerDetail) {
      if (s_selectedShockerSource == ShockerSelectionSource::Online) {
        switch (s_shockerDetailSelection) {
          case 0:
            return "Rename this online shocker";
          case 1:
            return "RF protocol model from account";
          case 2:
            return "Source and remapped RF IDs";
          case 3:
            return "Send 1s beep test command";
          case 4:
            return OpenShock::GatewayConnectionManager::IsConnected() ? "Disable or enable this online shocker locally" : "Delete this cached online shocker";
          default:
            return "Select an option";
        }
      }

      switch (s_shockerDetailSelection) {
        case 0:
          return "Rename this shocker";
        case 1:
          return "Select RF model protocol";
        case 2:
          return "Set max intensity 0-99";
        case 3:
          return "Send 1s beep test command";
        case 4:
          return "Delete this shocker profile";
        default:
          return "Select an option";
      }
    }

    return "No help available";
  }

  void saveNetworkSettingsPreferenceState()
  {
    if (!s_oledPrefsReady) {
      return;
    }

    s_oledPrefs.putBool(kPrefWiFiEnabled, s_networkWifiEnabled);
    s_oledPrefs.putBool(kPrefApEnabled, s_networkAccessPointEnabled);
    s_oledPrefs.putBool(kPrefCaptivePortalEnabled, s_networkCaptivePortalEnabled);
    s_oledPrefs.putString(kPrefDefaultSsid, s_defaultNetworkSsid);
    s_oledPrefs.putUShort(kPrefScreenSleepSeconds, s_screenSleepSeconds);
    s_oledPrefs.putUShort(kPrefDeviceSleepMinutes, s_deviceSleepMinutes);
    s_oledPrefs.putBool(kPrefScreenSaverEnabled, s_screenSaverEnabled);
    s_oledPrefs.putBool(kPrefBatteryIconEnabled, s_batteryIconEnabled);
    s_oledPrefs.putBool(kPrefBatteryPctEnabled, s_batteryPercentEnabled);
  }

  void loadNetworkSettingsPreferenceState()
  {
    s_oledPrefsReady = s_oledPrefs.begin(kPrefsNamespace, false);
    if (!s_oledPrefsReady) {
      return;
    }

    s_networkWifiEnabled = s_oledPrefs.getBool(kPrefWiFiEnabled, true);
    s_networkAccessPointEnabled = s_oledPrefs.getBool(kPrefApEnabled, true);
    s_networkCaptivePortalEnabled = s_oledPrefs.getBool(kPrefCaptivePortalEnabled, true);

    String preferredSsid = s_oledPrefs.getString(kPrefDefaultSsid, "");
    std::memset(s_defaultNetworkSsid, 0, sizeof(s_defaultNetworkSsid));
    if (!preferredSsid.isEmpty()) {
      std::strncpy(s_defaultNetworkSsid, preferredSsid.c_str(), sizeof(s_defaultNetworkSsid) - 1);
    }

    s_screenSleepSeconds = s_oledPrefs.getUShort(kPrefScreenSleepSeconds, 0);
    s_deviceSleepMinutes = s_oledPrefs.getUShort(kPrefDeviceSleepMinutes, 0);
    s_screenSaverEnabled = s_oledPrefs.getBool(kPrefScreenSaverEnabled, false);
    s_batteryIconEnabled = s_oledPrefs.getBool(kPrefBatteryIconEnabled, true);
    s_batteryPercentEnabled = s_oledPrefs.getBool(kPrefBatteryPctEnabled, true);
    s_batteryMaxMv = s_oledPrefs.getUShort(kPrefBatteryMaxMv, kBatteryDefaultMaxMv);

    OpenShock::CaptivePortal::SetApEnabled(s_networkAccessPointEnabled, false);
    OpenShock::CaptivePortal::SetEnabled(s_networkCaptivePortalEnabled);
    OpenShock::CaptivePortal::SetAlwaysEnabled(s_networkCaptivePortalEnabled, false);
  }

  void markUserActivity()
  {
    s_lastUserInputAt.store(static_cast<uint32_t>(OpenShock::millis()), std::memory_order_relaxed);

    if (s_screenSleepActive) {
      const bool wasPhysicallyOff = !s_screenSaverEnabled;
      s_screenSleepActive = false;
      if (wasPhysicallyOff) {
        setDisplayPower(true);
        delay(10);
        s_display.begin();
      }
      s_display.setPowerSave(0);
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
  }

  bool isWithinInputBlockWindow(uint32_t now)
  {
    const uint32_t blockUntil = s_inputBlockUntilAt.load(std::memory_order_relaxed);
    if (blockUntil == 0) {
      return false;
    }

    return static_cast<int32_t>(blockUntil - now) > 0;
  }

  void armInputBlockWindow(uint32_t now)
  {
    constexpr uint32_t kWakeInputDeadTimeMs = 180;
    s_inputBlockUntilAt.store(now + kWakeInputDeadTimeMs, std::memory_order_relaxed);
  }

  bool wakeOnlyInputIfSleeping()
  {
    const uint32_t now = static_cast<uint32_t>(OpenShock::millis());

    if (isWithinInputBlockWindow(now)) {
      markUserActivity();
      return true;
    }

    const bool wasSleeping = s_screenSleepActive;
    markUserActivity();

    if (wasSleeping) {
      armInputBlockWindow(now);
      if (s_refreshTask != nullptr) {
        xTaskNotifyGive(s_refreshTask);
      }
      return true;
    }

    return false;
  }

  void updateScreenSleepState()
  {
    const uint32_t now = static_cast<uint32_t>(OpenShock::millis());
    const uint32_t lastInputAt = s_lastUserInputAt.load(std::memory_order_relaxed);
    const uint32_t elapsedMs = now - lastInputAt;
    const uint32_t timeoutMs = static_cast<uint32_t>(s_screenSleepSeconds) * 1000U;
    const bool shouldSleep = (s_screenSleepSeconds > 0) && (elapsedMs >= timeoutMs);

    if (shouldSleep != s_screenSleepActive) {
      s_screenSleepActive = shouldSleep;
      if (s_screenSleepActive && !s_screenSaverEnabled) {
        s_display.setPowerSave(1);
        setDisplayPower(false);
      } else {
        s_display.setPowerSave(0);
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
    } else if (!s_screenSleepActive) {
      s_display.setPowerSave(0);
    }
  }

  void ensurePowerHoldPinConfigured()
  {
    if (kPowerHoldPin < 0 || s_powerHoldPinConfigured) {
      return;
    }

    pinMode(kPowerHoldPin, OUTPUT);
    s_powerHoldPinConfigured = true;
  }

  void setPowerHoldState(bool enabled)
  {
    if (kPowerHoldPin < 0) {
      return;
    }

    ensurePowerHoldPinConfigured();
    digitalWrite(kPowerHoldPin, enabled ? HIGH : LOW);
  }

  void setDisplayPower(bool enabled)
  {
    if (kDisplayPowerPin < 0) {
      return;
    }

    if (!s_displayPowerPinConfigured) {
      pinMode(kDisplayPowerPin, OUTPUT);
      s_displayPowerPinConfigured = true;
    }

    digitalWrite(kDisplayPowerPin, enabled ? HIGH : LOW);
  }

  void beginPowerOffSequence()
  {
    if (s_powerUiState == PowerUiState::Goodbye || s_powerUiState == PowerUiState::PowerCut) {
      return;
    }

    s_powerUiState = PowerUiState::Goodbye;
    s_powerUiDeadlineMs = OpenShock::millis() + kPowerOffMessageDurationMs;
    s_mainShockActive = false;
    s_mainVibrateActive = false;
    s_mainCommandAutoStopAtMs = 0;
    if (s_screenSleepActive && !s_screenSaverEnabled) {
      setDisplayPower(true);
      delay(10);
      s_display.begin();
    }
    s_screenSleepActive = false;
    s_display.setPowerSave(0);
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  }

  void drawLogoCropToDisplay()
  {
    // Logo is a standard MSB-first horizontal bitmap: leftmost pixel = bit 7 of each byte.
    // drawBitmap reads directly from PROGMEM with that convention, 16 bytes per row (128 px).
    s_display.drawBitmap(0, 0, kLogoWidth / 8, kLogoHeight, logo);
  }

  bool updatePowerUiState(int64_t now)
  {
    if (s_powerUiState == PowerUiState::BootDelay) {
      if (now < s_powerUiDeadlineMs) {
        return false;
      }

      s_powerUiState = PowerUiState::Ready;
      markUserActivity();
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return true;
    }

    if (s_powerUiState == PowerUiState::Goodbye && now >= s_powerUiDeadlineMs && !s_powerCutIssued) {
      s_powerCutIssued = true;
      s_powerUiState = PowerUiState::PowerCut;
      setPowerHoldState(false);
      // If the power-hold circuit doesn't cut power (e.g. USB charger keeps the device
      // alive), fall back to deep sleep so the device behaves as off.
      // Arm the encoder button as an ext1 RTC-GPIO wakeup source so the user can press
      // it to restart the device (deep sleep = full restart, RunChargingModeIfNeeded
      // will detect the held button and proceed to normal boot).
      if (s_encoderButtonPin != GPIO_NUM_NC) {
        const esp_err_t wakeErr = esp_sleep_enable_ext1_wakeup(
          1ULL << static_cast<int>(s_encoderButtonPin), ESP_EXT1_WAKEUP_ANY_HIGH
        );
        if (wakeErr != ESP_OK) {
          OS_LOGW(TAG, "Could not set encoder button as deep-sleep wake source: %s", esp_err_to_name(wakeErr));
        }
      }
      s_display.setPowerSave(1);
      esp_deep_sleep_start();
      return false;  // Unreachable if deep sleep starts
    }

    return false;
  }

  void drawYesNoPromptPage(const char* line1, const char* line2 = nullptr)
  {
    s_display.clearBuffer();
    s_display.setFont(u8g2_font_5x8_tf);
    s_display.setDrawColor(0);
    s_display.drawRBox(14, 12, 100, (line2 != nullptr) ? 34 : 24, 3);
    s_display.setDrawColor(1);
    s_display.drawRFrame(13, 11, 100, (line2 != nullptr) ? 34 : 24, 3);

    if (line2 == nullptr) {
      s_display.drawStr((128 - s_display.getStrWidth(line1)) / 2, 28, line1);
    } else {
      s_display.drawStr((128 - s_display.getStrWidth(line1)) / 2, 24, line1);
      s_display.drawStr((128 - s_display.getStrWidth(line2)) / 2, 36, line2);
    }

    constexpr int kButtonY = 54;
    constexpr int kButtonH = 9;
    constexpr int kButtonW = 38;
    constexpr int kButtonGap = 5;
    constexpr int kButtonX0 = 2;
    constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
    constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;

    auto drawButton = [&](int x, const char* label) {
      s_display.drawRFrame(x, kButtonY, kButtonW, kButtonH, 2);
      const int textX = x + ((kButtonW - s_display.getStrWidth(label)) / 2);
      s_display.drawStr(textX, kButtonY + 7, label);
    };

    drawButton(kButtonX0, "No");
    drawButton(kButtonX1, "");
    drawButton(kButtonX2, "Yes");
    s_display.sendBuffer();
  }

  void drawPowerOffPromptPage()
  {
    drawYesNoPromptPage("Power off?");
  }

  void drawUpdatePromptPage()
  {
    s_display.clearBuffer();
    s_display.setFont(u8g2_font_5x8_tf);

    // Title
    const char* title = "Update Available!";
    s_display.drawStr((128 - s_display.getStrWidth(title)) / 2, 10, title);

    // Version line: "v0.21.422 > v0.21.423"
    char versionLine[40];
    std::snprintf(versionLine, sizeof(versionLine), "v" OPENSHOCK_FW_VERSION " > v%s", s_pendingUpdateVersionDisplay);
    s_display.drawStr(std::max(0, (128 - s_display.getStrWidth(versionLine)) / 2), 22, versionLine);

    // Three outlined button boxes at the bottom, matching physical Left / Middle / Right
    // Left=No, Middle=Never, Right=Yes
    constexpr int kBtnY     = 50;
    constexpr int kBtnH     = 13;
    constexpr int kBtnW     = 38;
    constexpr int kGap      = 2;
    constexpr int kTotalW   = 3 * kBtnW + 2 * kGap;
    constexpr int kStartX   = (128 - kTotalW) / 2;
    constexpr int kTextY    = kBtnY + 10;

    const char* labels[3] = { "No", "Never", "Yes" };
    for (int i = 0; i < 3; ++i) {
      const int x = kStartX + i * (kBtnW + kGap);
      s_display.drawRFrame(x, kBtnY, kBtnW, kBtnH, 2);
      const int lx = x + (kBtnW - s_display.getStrWidth(labels[i])) / 2;
      s_display.drawStr(lx, kTextY, labels[i]);
    }

    s_display.sendBuffer();
  }

  void drawUpdateInProgressPage()
  {
    s_display.clearBuffer();
    s_display.setFont(u8g2_font_6x10_tf);

    const char* title = "Updating firmware";
    s_display.drawStr((128 - s_display.getStrWidth(title)) / 2, 16, title);

    // Animated dot row: cycles 1-4 dots every 500 ms
    const int dotCount = static_cast<int>((OpenShock::millis() / 500) % 4) + 1;
    char dots[5] = {};
    for (int i = 0; i < dotCount; ++i) dots[i] = '.';
    s_display.drawStr((128 - s_display.getStrWidth(dots)) / 2, 32, dots);

    s_display.setFont(u8g2_font_5x8_tf);
    const char* warn = "Do not power off!";
    s_display.drawStr((128 - s_display.getStrWidth(warn)) / 2, 48, warn);
    const char* wait = "This may take a minute";
    s_display.drawStr((128 - s_display.getStrWidth(wait)) / 2, 58, wait);

    s_display.sendBuffer();
  }

  void drawGoodbyePage()
  {
    s_display.clearBuffer();
    s_display.setFont(u8g2_font_6x10_tf);
    const char* text = "See you next time!";
    s_display.drawStr((128 - s_display.getStrWidth(text)) / 2, 34, text);
    s_display.sendBuffer();
  }

  void drawScreenSaverPage()
  {
    s_display.clearBuffer();
    drawLogoCropToDisplay();
    s_display.sendBuffer();
  }

  char passwordCharacterForSelection(uint8_t selection)
  {
    if (selection == 0) {
      return '\b';
    }

    return static_cast<char>(31 + selection);
  }

  void resetPasswordInputState()
  {
    s_passwordLength = 0;
    std::memset(s_passwordInput, 0, sizeof(s_passwordInput));
    s_passwordCharSelection = static_cast<uint8_t>('a' - 31);
    s_passwordConnectPending = false;
    s_pendingConnectFromPasswordEntry = false;
    s_passwordConnectStartedAt = 0;
  }

  void resetAccountLinkInputState()
  {
    s_accountDigitSelection = 0;
    s_accountCodeLength = 0;
    std::memset(s_accountCodeInput, 0, sizeof(s_accountCodeInput));
    s_accountLinkPending = false;
  }

  void showInfoPopup(std::string_view message)
  {
    const std::size_t len = std::min<std::size_t>(message.size(), sizeof(s_infoPopupMessage) - 1);
    std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
    if (len > 0) {
      std::memcpy(s_infoPopupMessage, message.data(), len);
    }
    s_infoPopupHideAt = OpenShock::millis() + kInfoPopupDurationMs;
    s_persistentInfoPopup = false;
    s_infoPopupWasVisible = true;
  }

  void showPersistentInfoPopup(std::string_view message)
  {
    const std::size_t len = std::min<std::size_t>(message.size(), sizeof(s_infoPopupMessage) - 1);
    std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
    if (len > 0) {
      std::memcpy(s_infoPopupMessage, message.data(), len);
    }
    s_infoPopupHideAt = 0;
    s_persistentInfoPopup = true;
    s_infoPopupWasVisible = true;
  }

  bool isInfoPopupVisible()
  {
    if (s_infoPopupMessage[0] == '\0') return false;
    if (s_persistentInfoPopup) return true;
    return s_infoPopupHideAt > OpenShock::millis();
  }

  bool updateInfoPopupVisibility()
  {
    const bool visible = isInfoPopupVisible();

    if (!visible && s_infoPopupMessage[0] != '\0') {
      std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
      s_infoPopupHideAt = 0;
    }

    if (visible != s_infoPopupWasVisible) {
      s_infoPopupWasVisible = visible;
      return true;
    }

    return false;
  }

  void keepConnectingPopupAlive()
  {
    if (!s_passwordConnectPending) {
      return;
    }

    constexpr std::string_view kConnecting = "Connecting...";
    const bool hasMessage = s_infoPopupMessage[0] != '\0';
    const bool isConnectingMessage = std::strncmp(s_infoPopupMessage, kConnecting.data(), kConnecting.size()) == 0;

    if (!hasMessage || isConnectingMessage) {
      std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
      std::memcpy(s_infoPopupMessage, kConnecting.data(), kConnecting.size());
      s_infoPopupHideAt = OpenShock::millis() + 1200;
      s_infoPopupWasVisible = true;
    }
  }

  void keepAccountLinkingPopupAlive()
  {
    if (!s_accountLinkPending) {
      return;
    }

    constexpr std::string_view kLinking = "Linking account...";
    const bool hasMessage = s_infoPopupMessage[0] != '\0';
    const bool isLinkingMessage = std::strncmp(s_infoPopupMessage, kLinking.data(), kLinking.size()) == 0;

    if (!hasMessage || isLinkingMessage) {
      std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
      std::memcpy(s_infoPopupMessage, kLinking.data(), kLinking.size());
      s_infoPopupHideAt = OpenShock::millis() + 1200;
      s_infoPopupWasVisible = true;
    }
  }

  const char* accountLinkResultMessage(OpenShock::AccountLinkResultCode result)
  {
    switch (result) {
      case OpenShock::AccountLinkResultCode::Success:
        return "Account linked";
      case OpenShock::AccountLinkResultCode::CodeRequired:
      case OpenShock::AccountLinkResultCode::InvalidCodeLength:
      case OpenShock::AccountLinkResultCode::InvalidCode:
        return "Invalid link code";
      case OpenShock::AccountLinkResultCode::NoInternetConnection:
        return "No internet";
      case OpenShock::AccountLinkResultCode::RateLimited:
        return "Link rate limited";
      default:
        return "Account link failed";
    }
  }

  struct AccountLinkTaskParams {
    char code[kAccountCodeLength + 1];
  };

  void accountLinkTask(void* param)
  {
    AccountLinkTaskParams local {};
    if (param != nullptr) {
      auto* params = static_cast<AccountLinkTaskParams*>(param);
      std::memcpy(local.code, params->code, sizeof(local.code));
      delete params;
    }

    const auto result = OpenShock::GatewayConnectionManager::Link(std::string_view(local.code, std::strlen(local.code)));

    s_accountLinkPending = false;
    showInfoPopup(accountLinkResultMessage(result));
    if (result == OpenShock::AccountLinkResultCode::Success) {
      OpenShock::CaptivePortal::SetUserDone();
      resetAccountLinkInputState();
      s_settingsView = SettingsView::Root;
      s_settingsSelection = 2;
      s_settingsFirstVisible = 0;
    }

    s_forceRedraw.store(true, std::memory_order_relaxed);
    OpenShock::OledDisplayManager::RequestRefresh();

    s_accountLinkTask = nullptr;
    vTaskDelete(nullptr);
  }

  void keepBatteryCalibrationPopupAlive()
  {
    if (!s_batteryCalibrationPending) {
      return;
    }

    constexpr std::string_view kCalibrating = "Calibrating...";
    const bool hasMessage = s_infoPopupMessage[0] != '\0';
    const bool isCalibratingMessage = std::strncmp(s_infoPopupMessage, kCalibrating.data(), kCalibrating.size()) == 0;

    if (!hasMessage || isCalibratingMessage) {
      std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
      std::memcpy(s_infoPopupMessage, kCalibrating.data(), kCalibrating.size());
      s_infoPopupHideAt = OpenShock::millis() + 1200;
      s_infoPopupWasVisible = true;
    }
  }

  void batteryCalibrationTask(void*)
  {
    uint16_t measuredMv = 0;

    if (kBatterySensePin >= 0) {
      uint32_t rawSum = 0;
      for (uint16_t i = 0; i < kBatteryCalibrationSamples; ++i) {
        rawSum += static_cast<uint32_t>(analogRead(kBatterySensePin));
        delay(kBatteryCalibrationIntervalMs);
      }

      const uint32_t rawAvg = rawSum / kBatteryCalibrationSamples;
      const uint32_t sensedMv = (rawAvg * 3300u + 2047u) / 4095u;
      measuredMv = static_cast<uint16_t>((sensedMv * 3u) / 2u);
    }

    if (measuredMv > kBatteryMinMv) {
      const int calibratedMv = static_cast<int>(std::lround(measuredMv * kBatteryCalibrationMarginRatio));
      s_batteryMaxMv = static_cast<uint16_t>(std::clamp(calibratedMv, static_cast<int>(kBatteryMinMv) + 100, 4500));

      if (s_oledPrefsReady) {
        s_oledPrefs.putUShort(kPrefBatteryMaxMv, s_batteryMaxMv);
      }

      s_batteryInitialized = false;
      showInfoPopup("Battery calibrated");
    } else {
      showInfoPopup("Calibration failed");
    }

    s_batteryCalibrationPending = false;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    OpenShock::OledDisplayManager::RequestRefresh();

    s_batteryCalibrationTask = nullptr;
    vTaskDelete(nullptr);
  }

  bool updatePendingPasswordConnection()
  {
    if (!s_passwordConnectPending) {
      return false;
    }

    OpenShock::WiFiNetwork connectedNetwork {};
    if (OpenShock::WiFiManager::GetConnectedNetwork(connectedNetwork) && std::strncmp(connectedNetwork.ssid, s_selectedConnectSsid, sizeof(connectedNetwork.ssid)) == 0) {
      s_passwordConnectPending = false;
      s_pendingConnectFromPasswordEntry = false;
      s_passwordConnectStartedAt = 0;
      showInfoPopup("Connected");
      s_settingsView = SettingsView::Network;
      resetPasswordInputState();
      return true;
    }

    const int64_t now = OpenShock::millis();
    if ((now - s_passwordConnectStartedAt) > 30000) {
      s_passwordConnectPending = false;
      s_passwordConnectStartedAt = 0;
      if (s_pendingConnectFromPasswordEntry) {
        showInfoPopup("Failed to connect");
      } else {
        showInfoPopup("Failed to connect");
        resetPasswordInputState();
        s_settingsView = SettingsView::ConnectPassword;
      }
      s_pendingConnectFromPasswordEntry = false;
      return true;
    }

    return false;
  }

  void drawScrollingText(std::string_view text, int x, int baselineY, int widthPx)
  {
    if (widthPx <= 0 || text.empty()) {
      return;
    }

    const int textWidth = s_display.getStrWidth(text.data());
    if (textWidth <= widthPx) {
      s_display.drawStr(x, baselineY, text.data());
      return;
    }

    constexpr int kGapPx = 12;
    constexpr int kStepMs = 110;

    const int cycleWidth = textWidth + kGapPx;
    const int offset = static_cast<int>((OpenShock::millis() / kStepMs) % cycleWidth);
    const int yTop = baselineY - s_display.getAscent();
    const int yBottom = baselineY - s_display.getDescent();

    s_display.setClipWindow(x, yTop, x + widthPx - 1, yBottom);
    s_display.drawStr(x - offset, baselineY, text.data());
    s_display.drawStr(x - offset + cycleWidth, baselineY, text.data());
    s_display.setMaxClipWindow();
  }

  void drawScrollIndicator(uint8_t itemCount, uint8_t firstVisible, uint8_t visibleCount)
  {
    if (itemCount <= visibleCount || visibleCount == 0) {
      return;
    }

    constexpr int kRailX = 123;
    constexpr int kTopY = 12;
    constexpr int kBottomY = 50;
    constexpr int kTrackTopY = kTopY + 3;
    constexpr int kTrackBottomY = kBottomY - 3;

    // Explicit 3px arrowheads to avoid rasterization artifacts on tiny filled triangles.
    s_display.drawPixel(kRailX, kTopY);
    s_display.drawLine(kRailX - 1, kTopY + 1, kRailX + 1, kTopY + 1);
    s_display.drawLine(kRailX - 1, kBottomY - 1, kRailX + 1, kBottomY - 1);
    s_display.drawPixel(kRailX, kBottomY);
    s_display.drawLine(kRailX, kTrackTopY, kRailX, kTrackBottomY);

    const int trackHeight = kTrackBottomY - kTrackTopY + 1;
    const uint8_t maxFirstVisible = itemCount - visibleCount;
    const int thumbHeight = std::max(5, static_cast<int>((trackHeight * visibleCount) / itemCount));
    const int travel = std::max(1, trackHeight - thumbHeight);
    const int thumbOffset = (maxFirstVisible == 0) ? 0 : static_cast<int>((travel * firstVisible) / maxFirstVisible);
    const int thumbY = kTrackTopY + thumbOffset;

    s_display.drawBox(kRailX - 1, thumbY, 3, thumbHeight);
  }

  bool isSettingsMarqueeActive()
  {
    if (!s_initialized || s_currentPage.load(std::memory_order_relaxed) != kPageSettings) {
      return false;
    }

    const SettingsView view = s_settingsView;
    const uint8_t* firstVisible = &s_settingsFirstVisible;
    uint8_t itemCount = kSettingsItemCount;
    std::vector<OpenShock::WiFiNetwork> connectNetworks;

    if (view == SettingsView::Network) {
      firstVisible = &s_networkFirstVisible;
      itemCount = kNetworkItemCount;
    } else if (view == SettingsView::Connect) {
      firstVisible = &s_connectFirstVisible;
      connectNetworks = OpenShock::WiFiManager::GetDiscoveredWiFiNetworks();
      std::sort(connectNetworks.begin(), connectNetworks.end(), [](const OpenShock::WiFiNetwork& a, const OpenShock::WiFiNetwork& b) { return a.rssi > b.rssi; });
      itemCount = static_cast<uint8_t>(std::min<std::size_t>(connectNetworks.size(), 255));
    } else if (view == SettingsView::ConnectNetwork) {
      firstVisible = &s_connectNetworkFirstVisible;
      itemCount = kConnectNetworkItemCount;
    } else if (view == SettingsView::System) {
      firstVisible = &s_systemFirstVisible;
      itemCount = kSystemItemCount;
    } else if (view == SettingsView::ConnectPassword) {
      s_display.setFont(u8g2_font_6x10_tf);
      const char* title = (s_selectedConnectSsid[0] != '\0') ? s_selectedConnectSsid : "Password";
      return s_display.getStrWidth(title) > 122;
    } else if (view == SettingsView::AccountLink) {
      return false;
    } else if (view == SettingsView::SystemScreenSleepEdit || view == SettingsView::SystemDeviceSleepEdit || view == SettingsView::BatteryCalibrationConfirmClear || view == SettingsView::BatteryCalibrationConfirmCharged || view == SettingsView::About || view == SettingsView::AccountMenu || view == SettingsView::Update || view == SettingsView::UpdatePrompt) {
      return false;
    }
    else if (view == SettingsView::ShockerNameEdit || view == SettingsView::UpdateRepoEdit) {
      s_display.setFont(u8g2_font_6x10_tf);
      return s_display.getStrWidth(view == SettingsView::ShockerNameEdit ? getSelectedShockerName() : "GitHub Source") > 122;
    } else if (view == SettingsView::ShockerList || view == SettingsView::ShockerDetail || view == SettingsView::ShockerLimitEdit || view == SettingsView::ShockerProtocolEdit) {
      // Check visible items for overflow
      const auto onlineShockers = getOnlineShockersSnapshot();
      s_display.setFont(u8g2_font_6x10_tf);
      const uint8_t fv = (view == SettingsView::ShockerList) ? s_shockerListFirstVisible : s_shockerDetailFirstVisible;
      const uint8_t ic = (view == SettingsView::ShockerList) ? getTotalShockerListItemCount(onlineShockers) : getShockerDetailItemCount();
      for (uint8_t row = 0; row < kSettingsVisibleCount; ++row) {
        const uint8_t idx = fv + row;
        if (idx >= ic) break;
        if (view == SettingsView::ShockerList) {
          char onlineLabel[24] = {};
          const char* label = "Keep Alive";
          if (idx == 1) {
            label = "Add Shocker";
          } else if (idx >= kShockerListStaticItemCount) {
            const uint8_t localIndex = static_cast<uint8_t>(idx - kShockerListStaticItemCount);
            if (localIndex < s_shockerCount) {
              label = s_shockers[localIndex].name;
            } else {
              const uint8_t onlineIndex = static_cast<uint8_t>(localIndex - s_shockerCount);
              if (onlineIndex < onlineShockers.size()) {
                std::strncpy(onlineLabel, onlineShockers[onlineIndex].displayName.c_str(), sizeof(onlineLabel) - 1);
                onlineLabel[sizeof(onlineLabel) - 1] = '\0';
                label = onlineLabel;
              }
            }
          }

          if (s_display.getStrWidth(label) > (119 - 13)) return true;
        } else if (view == SettingsView::ShockerDetail) {
          char buf[24]; getShockerDetailItem(idx, buf, sizeof(buf));
          if (s_display.getStrWidth(buf) > (119 - 13)) return true;
        }
      }
      return false;
    }

    const uint8_t visibleCount = std::min<uint8_t>(kSettingsVisibleCount, itemCount);
    const bool showScroll = itemCount > visibleCount;
    const int textRegionMaxX = showScroll ? 119 : 127;
    const auto onlineShockers = (view == SettingsView::ShockerList || view == SettingsView::ShockerDetail) ? getOnlineShockersSnapshot() : std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> {};
    s_display.setFont(u8g2_font_6x10_tf);

    if (view == SettingsView::ConnectNetwork) {
      const char* title = (s_selectedConnectSsid[0] != '\0') ? s_selectedConnectSsid : "<hidden>";
      if (s_display.getStrWidth(title) > 122) {
        return true;
      }
    }

    for (uint8_t row = 0; row < kSettingsVisibleCount; ++row) {
      const uint8_t itemIndex = static_cast<uint8_t>(*firstVisible + row);
      if (itemIndex >= itemCount) {
        break;
      }

      if (view == SettingsView::Connect) {
        if (itemIndex < connectNetworks.size()) {
          const char* name = (connectNetworks[itemIndex].ssid[0] != '\0') ? connectNetworks[itemIndex].ssid : "<hidden>";
          if (s_display.getStrWidth(name) > (textRegionMaxX - 24)) {
            return true;
          }
        }
        continue;
      }

      const std::string_view item = getSettingsItem(view, itemIndex);
      const int textX = (view == SettingsView::Network && (itemIndex == 0 || itemIndex == 1 || itemIndex == 2)) ? 24 : 13;
      if (s_display.getStrWidth(item.data()) > (textRegionMaxX - textX)) {
        return true;
      }
    }

    return false;
  }

  void drawSystemValueEditor(const char* unitLabel)
  {
    s_display.setDrawColor(0);
    s_display.drawRBox(9, 13, 110, 42, 4);
    s_display.setDrawColor(1);
    s_display.drawRFrame(8, 12, 110, 42, 4);

    char numBuf[6];
    std::snprintf(numBuf, sizeof(numBuf), "%u", s_pendingSystemValue);
    s_display.setFont(u8g2_font_logisoso24_tn);
    const int numW = s_display.getStrWidth(numBuf);
    s_display.drawStr((128 - numW) / 2, 39, numBuf);

    s_display.setFont(u8g2_font_5x8_tf);
    const int unitW = s_display.getStrWidth(unitLabel);
    s_display.drawStr((128 - unitW) / 2, 49, unitLabel);

    constexpr int kButtonY = 54;
    constexpr int kButtonH = 9;
    constexpr int kButtonW = 38;
    constexpr int kButtonGap = 5;
    constexpr int kButtonX0 = 2;
    constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
    constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;
    s_display.drawRFrame(kButtonX0, kButtonY, kButtonW, kButtonH, 2);
    s_display.drawStr(kButtonX0 + ((kButtonW - s_display.getStrWidth("Back")) / 2), kButtonY + 7, "Back");
    s_display.drawRFrame(kButtonX1, kButtonY, kButtonW, kButtonH, 2);
    s_display.drawRFrame(kButtonX2, kButtonY, kButtonW, kButtonH, 2);
    s_display.drawStr(kButtonX2 + ((kButtonW - s_display.getStrWidth("Enter")) / 2), kButtonY + 7, "Enter");
    s_display.sendBuffer();
  }

  void drawEncodedIcon(std::string_view code, int x, int y, uint8_t drawColor = 1)
  {
    if (code.size() < 4) {
      return;
    }

    const int widthDigits = code[0] - '0';
    const int heightDigits = code[1] - '0';
    const std::size_t headerLen = 2u + static_cast<std::size_t>(std::max(widthDigits, 0)) + static_cast<std::size_t>(std::max(heightDigits, 0));
    if (widthDigits <= 0 || heightDigits <= 0 || headerLen > code.size()) {
      return;
    }

    int width = 0;
    int height = 0;
    int index = 2;

    for (int i = 0; i < widthDigits; ++i) {
      width = (width * 10) + (code[index++] - '0');
    }
    for (int i = 0; i < heightDigits; ++i) {
      height = (height * 10) + (code[index++] - '0');
    }

    if (width <= 0 || height <= 0) {
      return;
    }

    const uint8_t previousColor = s_display.getDrawColor();
    s_display.setDrawColor(drawColor);

    const int expected = width * height;
    for (int pixel = 0; pixel < expected && index < static_cast<int>(code.size()); ++pixel, ++index) {
      if (code[index] == '1') {
        const int px = pixel % width;
        const int py = pixel / width;
        s_display.drawPixel(x + px, y + py);
      }
    }

    s_display.setDrawColor(previousColor);
  }

  std::string_view wifiStrengthIconForRssi(int8_t rssi)
  {
    if (rssi >= -67) {
      return kWifiStrengthIconCodes[0];
    }

    if (rssi >= -80) {
      return kWifiStrengthIconCodes[1];
    }

    return kWifiStrengthIconCodes[2];
  }

  uint8_t wifiStrengthBucketForRssi(int8_t rssi)
  {
    if (rssi >= -67) {
      return 2;
    }

    if (rssi >= -80) {
      return 1;
    }

    return 0;
  }

  uint8_t batteryPercentFromMv(uint16_t batteryMv)
  {
    const uint32_t clampedMv = std::clamp<uint32_t>(batteryMv, kBatteryMinMv, s_batteryMaxMv);
    const float normalized = static_cast<float>(clampedMv - kBatteryMinMv) / static_cast<float>(s_batteryMaxMv - kBatteryMinMv);
    const int percent = 1 + static_cast<int>(std::lround(normalized * 98.0f));
    return static_cast<uint8_t>(std::clamp(percent, 1, 99));
  }

  bool updateBatterySample(int64_t nowMs)
  {
    if (kBatterySensePin < 0) {
      return false;
    }

    if ((nowMs - s_lastBatterySampleAt) < kBatterySamplePeriodMs) {
      return false;
    }
    s_lastBatterySampleAt = nowMs;

    uint32_t rawSum = 0;
    for (uint8_t i = 0; i < kBatterySamplesPerCycle; ++i) {
      rawSum += static_cast<uint32_t>(analogRead(kBatterySensePin));
      if (i + 1 < kBatterySamplesPerCycle) delayMicroseconds(kBatterySampleDelayUs);
    }

    const uint32_t rawAvg = rawSum / kBatterySamplesPerCycle;
    const uint32_t sensedMv = (rawAvg * 3300u + 2047u) / 4095u;
    const uint32_t measuredBatteryMv = (sensedMv * 3u) / 2u;
    const uint16_t clampedBatteryMv = static_cast<uint16_t>(std::clamp<uint32_t>(measuredBatteryMv, kBatteryMinMv, s_batteryMaxMv));

    if (!s_batteryInitialized) {
      s_batteryFilteredMv = clampedBatteryMv;
      s_mainBatteryPercent = batteryPercentFromMv(clampedBatteryMv);
      s_batteryInitialized = true;
      return true;
    }

    s_batteryFilteredMv = static_cast<uint16_t>((static_cast<uint32_t>(s_batteryFilteredMv) * 7u + clampedBatteryMv) / 8u);
    const uint8_t candidatePercent = batteryPercentFromMv(s_batteryFilteredMv);

    if (candidatePercent == s_mainBatteryPercent) {
      s_batteryPendingDirection = 0;
      s_batteryPendingCount = 0;
      return false;
    }

    const int diff = static_cast<int>(candidatePercent) - static_cast<int>(s_mainBatteryPercent);
    const int8_t direction = (diff > 0) ? 1 : -1;

    if (s_batteryPendingDirection == direction) {
      ++s_batteryPendingCount;
    } else {
      s_batteryPendingDirection = direction;
      s_batteryPendingCount = 1;
    }

    if (s_batteryPendingCount >= kBatteryNeighborConfirmCount) {
      s_mainBatteryPercent = static_cast<uint8_t>(std::clamp(static_cast<int>(s_mainBatteryPercent) + direction, 1, 99));
      s_batteryPendingDirection = 0;
      s_batteryPendingCount = 0;
      return true;
    }

    return false;
  }

  void drawBatteryStatusIcon(int x, int y, uint8_t percent)
  {
    constexpr int kBodyW = 10;
    constexpr int kBodyH = 7;
    constexpr int kCapW = 1;
    constexpr int kCapH = 3;
    constexpr int kInnerW = kBodyW - 2;
    constexpr int kInnerH = kBodyH - 2;

    s_display.drawFrame(x, y, kBodyW, kBodyH);
    s_display.drawBox(x + kBodyW, y + 2, kCapW, kCapH);

    const int fillW = std::clamp((static_cast<int>(percent) * kInnerW + 98) / 99, 1, kInnerW);
    s_display.drawBox(x + 1, y + 1, fillW, kInnerH);
  }

  // ── Main-page shocker helpers ──────────────────────────────────────────────

  struct MainShockerRef {
    bool isOnline;
    uint8_t index; // local index or online vector index
  };

  struct MainShockerTarget {
    OpenShock::ShockerModelType model;
    uint16_t rfId;
    uint8_t limit;
    bool isOnline;
    uint8_t index;
    bool valid;
  };

  std::vector<MainShockerRef> buildMainShockerList()
  {
    std::vector<MainShockerRef> list;
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      list.push_back({false, i});
    }
    const auto online = getOnlineShockersSnapshot();
    for (uint8_t i = 0; i < static_cast<uint8_t>(online.size()); ++i) {
      if (!online[i].disabled) {
        list.push_back({true, i});
      }
    }
    return list;
  }

  uint8_t getLimitForRef(const MainShockerRef& ref)
  {
    if (ref.isOnline) {
      const auto online = getOnlineShockersSnapshot();
      if (ref.index < static_cast<uint8_t>(online.size()) && !online[ref.index].disabled) {
        return std::min<uint8_t>(online[ref.index].limit, 99);
      }
      return 99;
    }
    if (ref.index < s_shockerCount) {
      return std::min<uint8_t>(s_shockers[ref.index].limit, 99);
    }
    return 0;
  }

  uint16_t getRfIdForRef(const MainShockerRef& ref)
  {
    if (ref.isOnline) {
      const auto online = getOnlineShockersSnapshot();
      if (ref.index < static_cast<uint8_t>(online.size()) && !online[ref.index].disabled) {
        return online[ref.index].mappedRfId;
      }
      return 0;
    }

    if (ref.index < s_shockerCount) {
      return s_shockers[ref.index].rfId;
    }

    return 0;
  }

  bool getActiveShockerRef(MainShockerRef& out)
  {
    const auto list = buildMainShockerList();
    for (const auto& ref : list) {
      if (ref.isOnline == s_activeShockerIsOnline && ref.index == s_activeShockerIdx) {
        out = ref;
        return true;
      }
    }
    return false;
  }

  void setActiveShockerRef(const MainShockerRef& ref)
  {
    s_activeShockerIsOnline = ref.isOnline;
    s_activeShockerIdx = ref.index;
  }

  uint8_t readStoredIntensityForRef(const MainShockerRef& ref, uint8_t fallback)
  {
    const uint16_t rfId = getRfIdForRef(ref);
    const uint8_t limit = getLimitForRef(ref);
    if (rfId == 0) {
      return std::min<uint8_t>(fallback, limit);
    }

    auto& table = ref.isOnline ? s_onlineMainIntensityByRf : s_localMainIntensityByRf;
    for (auto& kv : table) {
      if (kv.first == rfId) {
        kv.second = std::min<uint8_t>(kv.second, limit);
        return kv.second;
      }
    }

    return std::min<uint8_t>(fallback, limit);
  }

  void writeStoredIntensityForRef(const MainShockerRef& ref, uint8_t intensity)
  {
    const uint16_t rfId = getRfIdForRef(ref);
    const uint8_t limit = getLimitForRef(ref);
    if (rfId == 0) {
      return;
    }

    const uint8_t clamped = std::min<uint8_t>(intensity, limit);
    auto& table = ref.isOnline ? s_onlineMainIntensityByRf : s_localMainIntensityByRf;
    for (auto& kv : table) {
      if (kv.first == rfId) {
        kv.second = clamped;
        return;
      }
    }

    table.push_back({rfId, clamped});
  }

  void applyActiveShockerIntensity(uint8_t intensity)
  {
    MainShockerRef ref {};
    if (!getActiveShockerRef(ref)) {
      OpenShock::RotaryEncoderManager::SetMaxIntensityLimit(0);
      return;
    }

    const uint8_t limit = getLimitForRef(ref);
    const uint8_t clamped = std::min<uint8_t>(intensity, limit);
    OpenShock::RotaryEncoderManager::SetMaxIntensityLimit(clamped);
    writeStoredIntensityForRef(ref, clamped);
  }

  void resetMainShockerIntensitiesOnBoot()
  {
    s_localMainIntensityByRf.clear();
    s_onlineMainIntensityByRf.clear();

    // Force a known-safe startup state: every shocker starts at intensity 0.
    for (uint8_t i = 0; i < s_shockerCount; ++i) {
      MainShockerRef localRef {
        .isOnline = false,
        .index = i,
      };
      writeStoredIntensityForRef(localRef, 0);
    }

    OpenShock::RotaryEncoderManager::SetMaxIntensityLimit(0);
  }

  void ensureActiveMainShockerSelected()
  {
    const auto list = buildMainShockerList();
    if (list.empty()) {
      s_activeShockerIsOnline = false;
      s_activeShockerIdx = 0;
      OpenShock::RotaryEncoderManager::SetMaxIntensityLimit(0);
      return;
    }

    MainShockerRef activeRef {};
    if (!getActiveShockerRef(activeRef)) {
      activeRef = list[0];
      setActiveShockerRef(activeRef);
    }

    const uint8_t fallback = std::min<uint8_t>(OpenShock::RotaryEncoderManager::GetMaxIntensityLimit(), 99);
    const uint8_t selectedIntensity = readStoredIntensityForRef(activeRef, fallback);
    applyActiveShockerIntensity(selectedIntensity);
  }

  uint8_t getActiveShockerLimit()
  {
    MainShockerRef ref {};
    if (!getActiveShockerRef(ref)) {
      return 0;
    }
    return getLimitForRef(ref);
  }

  uint8_t getCurrentActiveIntensity()
  {
    const uint8_t limit = getActiveShockerLimit();
    return std::min<uint8_t>(OpenShock::RotaryEncoderManager::GetMaxIntensityLimit(), limit);
  }

  // Returns the display name of the currently active main-page shocker.
  const char* getActiveShockerName()
  {
    ensureActiveMainShockerSelected();

    if (!s_activeShockerIsOnline) {
      if (s_activeShockerIdx < s_shockerCount) {
        if (s_shockers[s_activeShockerIdx].name[0] != '\0') {
          return s_shockers[s_activeShockerIdx].name;
        }
        return "Unnamed";
      }
      return "none";
    }

    static char s_onlineNameBuf[24];
    const auto online = getOnlineShockersSnapshot();
    if (s_activeShockerIdx < static_cast<uint8_t>(online.size()) && !online[s_activeShockerIdx].disabled) {
      std::strncpy(s_onlineNameBuf, online[s_activeShockerIdx].displayName.c_str(), sizeof(s_onlineNameBuf) - 1);
      s_onlineNameBuf[sizeof(s_onlineNameBuf) - 1] = '\0';
      if (s_onlineNameBuf[0] == '\0') {
        std::strncpy(s_onlineNameBuf, "Unnamed", sizeof(s_onlineNameBuf) - 1);
        s_onlineNameBuf[sizeof(s_onlineNameBuf) - 1] = '\0';
      }
      return s_onlineNameBuf;
    }

    return "none";
  }

  MainShockerTarget getActiveShockerTarget()
  {
    ensureActiveMainShockerSelected();

    MainShockerRef ref {};
    if (!getActiveShockerRef(ref)) {
      return {{}, 0, 0, false, 0, false};
    }

    if (ref.isOnline) {
      const auto online = getOnlineShockersSnapshot();
      if (ref.index < static_cast<uint8_t>(online.size()) && !online[ref.index].disabled) {
        return {online[ref.index].model, online[ref.index].mappedRfId, getLimitForRef(ref), true, ref.index, true};
      }
      return {{}, 0, 0, true, ref.index, false};
    }

    if (ref.index < s_shockerCount) {
      return {
        shockerModelForStoredValue(s_shockers[ref.index].protocol),
        s_shockers[ref.index].rfId,
        getLimitForRef(ref),
        false,
        ref.index,
        true
      };
    }

    return {{}, 0, 0, false, ref.index, false};
  }

  void sendMainShockerCommand(OpenShock::ShockerCommandType type, uint8_t intensity)
  {
    const auto target = getActiveShockerTarget();
    if (!target.valid) {
      return;
    }

    const uint8_t clamped = std::min<uint8_t>(intensity, target.limit);
    OpenShock::CommandHandler::HandleCommand(target.model, target.rfId, type, clamped, 2000);
    s_mainCommandLastSentMs = OpenShock::millis();
  }

  void stopMainShockerCommand()
  {
    const auto target = getActiveShockerTarget();
    if (!target.valid) {
      return;
    }

    OpenShock::CommandHandler::HandleCommand(target.model, target.rfId, OpenShock::ShockerCommandType::Stop, 0, 0);
  }

  void drawRightAlignedTextOrScroll(std::string_view text, int leftX, int rightX, int baselineY)
  {
    if (rightX < leftX) {
      return;
    }

    const int widthPx = rightX - leftX + 1;
    const int textWidth = s_display.getStrWidth(text.data());
    if (textWidth <= widthPx) {
      const int textX = rightX - textWidth + 1;
      s_display.drawStr(textX, baselineY, text.data());
      return;
    }

    drawScrollingText(text, leftX, baselineY, widthPx);
  }

  bool isMainPageMarqueeActive()
  {
    if (!s_initialized || s_currentPage.load(std::memory_order_relaxed) != kPageMain) {
      return false;
    }
    s_display.setFont(u8g2_font_5x8_tf);
    return s_display.getStrWidth(getActiveShockerName()) > 41;
  }

  // ── End main-page shocker helpers ──────────────────────────────────────────

  void drawWifiStatusIcon(int x, int y, bool connected, int8_t rssi)
  {
    if (!connected) {
      return;
    }

    const std::string_view icon = wifiStrengthIconForRssi(rssi);
    drawEncodedIcon(icon, x, y, 1);
  }

  void requestRefresh()
  {
    s_forceRedraw.store(true, std::memory_order_relaxed);
    if (s_refreshTask != nullptr) {
      xTaskNotifyGive(s_refreshTask);
    }
  }

  void drawSettingsPage()
  {
    s_display.clearBuffer();

    const uint8_t* const menuFont = u8g2_font_6x10_tf;
    s_display.setFont(menuFont);
    s_display.setDrawColor(1);
    s_display.drawBox(0, 0, 128, 10);
    s_display.setDrawColor(0);

    const SettingsView view = s_settingsView;
    const bool inSubmenu = view != SettingsView::Root;
    const char* pageTitle = "Settings";
    uint8_t* selection = &s_settingsSelection;
    uint8_t* firstVisible = &s_settingsFirstVisible;
    uint8_t itemCount = kSettingsItemCount;
    std::vector<OpenShock::WiFiNetwork> connectNetworks;
    OpenShock::WiFiNetwork connectedNetwork {};
    bool hasConnectedNetwork = false;

    if (view == SettingsView::Network) {
      pageTitle = "Network";
      selection = &s_networkSelection;
      firstVisible = &s_networkFirstVisible;
      itemCount = kNetworkItemCount;

      const wifi_mode_t mode = WiFi.getMode();
      s_networkWifiEnabled = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
      s_networkAccessPointEnabled = OpenShock::CaptivePortal::IsApEnabled();
      s_networkCaptivePortalEnabled = OpenShock::CaptivePortal::IsAlwaysEnabled();
    } else if (view == SettingsView::Connect) {
      pageTitle = "Connect";
      selection = &s_connectSelection;
      firstVisible = &s_connectFirstVisible;
      connectNetworks = OpenShock::WiFiManager::GetDiscoveredWiFiNetworks();
      std::sort(connectNetworks.begin(), connectNetworks.end(), [](const OpenShock::WiFiNetwork& a, const OpenShock::WiFiNetwork& b) { return a.rssi > b.rssi; });
      itemCount = static_cast<uint8_t>(std::min<std::size_t>(connectNetworks.size(), 255));
      hasConnectedNetwork = OpenShock::WiFiManager::GetConnectedNetwork(connectedNetwork);
    } else if (view == SettingsView::ConnectNetwork) {
      pageTitle = (s_selectedConnectSsid[0] != '\0') ? s_selectedConnectSsid : "<hidden>";
      selection = &s_connectNetworkSelection;
      firstVisible = &s_connectNetworkFirstVisible;
      itemCount = kConnectNetworkItemCount;
    } else if (view == SettingsView::ConnectPassword) {
      pageTitle = (s_selectedConnectSsid[0] != '\0') ? s_selectedConnectSsid : "Password";
      itemCount = 0;
    } else if (view == SettingsView::System) {
      pageTitle = "System";
      selection = &s_systemSelection;
      firstVisible = &s_systemFirstVisible;
      itemCount = kSystemItemCount;
    } else if (view == SettingsView::SystemScreenSleepEdit) {
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::SystemDeviceSleepEdit) {
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::BatteryCalibrationConfirmClear) {
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::BatteryCalibrationConfirmCharged) {
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::About) {
      pageTitle = "About";
      itemCount = 0;
    } else if (view == SettingsView::AccountMenu) {
      pageTitle = "Account";
      selection = &s_accountMenuSelection;
      firstVisible = &s_accountMenuFirstVisible;
      itemCount = getAccountMenuItemCount();
    } else if (view == SettingsView::AccountLink) {
      pageTitle = "Account Link";
      itemCount = 0;
    } else if (view == SettingsView::Update) {
      pageTitle = "Update";
      selection = &s_updateSelection;
      firstVisible = &s_updateFirstVisible;
      itemCount = kUpdateItemCount;
    } else if (view == SettingsView::UpdatePrompt) {
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::UpdateRepoEdit) {
      pageTitle = "GitHub Source";
      itemCount = 0;
    } else if (view == SettingsView::ShockerList) {
      pageTitle = "Shockers";
      selection = &s_shockerListSelection;
      firstVisible = &s_shockerListFirstVisible;
      itemCount = getTotalShockerListItemCount(getOnlineShockersSnapshot());

      bool keepAliveEnabled = s_shockerKeepAliveEnabled;
      if (OpenShock::Config::GetRFConfigKeepAliveEnabled(keepAliveEnabled)) {
        s_shockerKeepAliveEnabled = keepAliveEnabled;
      }
    } else if (view == SettingsView::ShockerDetail) {
      pageTitle = getSelectedShockerName();
      selection = &s_shockerDetailSelection;
      firstVisible = &s_shockerDetailFirstVisible;
      itemCount = getShockerDetailItemCount();
    } else if (view == SettingsView::ShockerNameEdit) {
      pageTitle = getSelectedShockerName();
      itemCount = 0;
    } else if (view == SettingsView::ShockerLimitEdit) {
      // Full overlay — skip normal list rendering
      pageTitle = "";
      itemCount = 0;
    } else if (view == SettingsView::ShockerProtocolEdit) {
      // Full overlay — skip normal list rendering
      pageTitle = "";
      itemCount = 0;
    }

    if (view == SettingsView::ShockerLimitEdit) {
      // ── Limit edit popup ──
      s_display.setDrawColor(0);
      s_display.drawRBox(9, 13, 110, 42, 4);
      s_display.setDrawColor(1);
      s_display.drawRFrame(8, 12, 110, 42, 4);
      char numBuf[4];
      std::snprintf(numBuf, sizeof(numBuf), "%u", s_pendingLimit);
      s_display.setFont(u8g2_font_logisoso32_tn);
      const int numW = s_display.getStrWidth(numBuf);
      s_display.drawStr((128 - numW) / 2, 48, numBuf);
      s_display.setFont(u8g2_font_5x8_tf);
      s_display.setDrawColor(1);
      constexpr int kButtonY = 54;
      constexpr int kButtonH = 9;
      constexpr int kButtonW = 38;
      constexpr int kButtonGap = 5;
      constexpr int kButtonX0 = 2;
      constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
      constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;
      s_display.drawRFrame(kButtonX0, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawStr(kButtonX0 + ((kButtonW - s_display.getStrWidth("Back")) / 2), kButtonY + 7, "Back");
      s_display.drawRFrame(kButtonX1, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawRFrame(kButtonX2, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawStr(kButtonX2 + ((kButtonW - s_display.getStrWidth("Enter")) / 2), kButtonY + 7, "Enter");
      s_display.sendBuffer();
      return;
    }

    if (view == SettingsView::ShockerProtocolEdit) {
      // ── Protocol edit popup ──
      const OpenShock::ShockerModelType model = kProtocolOptions[s_pendingProtocol];
      const char* modelLabel = shockerModelLabel(model);

      s_display.setDrawColor(0);
      s_display.drawRBox(9, 13, 110, 42, 4);
      s_display.setDrawColor(1);
      s_display.drawRFrame(8, 12, 110, 42, 4);

      s_display.setFont(u8g2_font_6x10_tf);
      const int textW = s_display.getStrWidth(modelLabel);
      s_display.drawStr((128 - textW) / 2, 36, modelLabel);
      s_display.drawTriangle(17, 34, 22, 30, 22, 38);
      s_display.drawTriangle(111, 34, 106, 30, 106, 38);

      s_display.setFont(u8g2_font_5x8_tf);
      s_display.setDrawColor(1);
      constexpr int kButtonY = 54;
      constexpr int kButtonH = 9;
      constexpr int kButtonW = 38;
      constexpr int kButtonGap = 5;
      constexpr int kButtonX0 = 2;
      constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
      constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;
      s_display.drawRFrame(kButtonX0, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawStr(kButtonX0 + ((kButtonW - s_display.getStrWidth("Back")) / 2), kButtonY + 7, "Back");
      s_display.drawRFrame(kButtonX1, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawRFrame(kButtonX2, kButtonY, kButtonW, kButtonH, 2);
      s_display.drawStr(kButtonX2 + ((kButtonW - s_display.getStrWidth("Enter")) / 2), kButtonY + 7, "Enter");
      s_display.sendBuffer();
      return;
    }

    if (view == SettingsView::SystemScreenSleepEdit) {
      drawSystemValueEditor("seconds");
      return;
    }

    if (view == SettingsView::SystemDeviceSleepEdit) {
      drawSystemValueEditor("minutes");
      return;
    }

    if (view == SettingsView::BatteryCalibrationConfirmClear) {
      drawYesNoPromptPage("Clear", "calibration?");
      return;
    }

    if (view == SettingsView::BatteryCalibrationConfirmCharged) {
      drawYesNoPromptPage("Battery", "fully charged?");
      return;
    }

    if (view == SettingsView::ConnectNetwork || view == SettingsView::ConnectPassword) {
      drawScrollingText(pageTitle, 2, 8, 122);
    } else {
      s_display.drawStr(2, 8, pageTitle);
    }
    s_display.setDrawColor(1);

    s_display.setFont(menuFont);

    const int labelX = 13;
    const int lineYs[kSettingsVisibleCount] = {18, 29, 40, 51};
    const int ascent = s_display.getAscent();
    const int descent = s_display.getDescent();
    if (itemCount > 0) {
      const uint8_t maxFirstVisible = (itemCount > kSettingsVisibleCount) ? (itemCount - kSettingsVisibleCount) : 0;
      *firstVisible = std::min(*firstVisible, maxFirstVisible);

      const int selectedRow = static_cast<int>(*selection) - static_cast<int>(*firstVisible);
      const int selectedBaselineY = lineYs[std::clamp(selectedRow, 0, static_cast<int>(kSettingsVisibleCount) - 1)];
      constexpr int kTriangleYOffset = 2;
      const int textCenterY = selectedBaselineY - ((ascent + descent) / 2) - kTriangleYOffset;
      s_display.drawTriangle(2, textCenterY - 4, 9, textCenterY, 2, textCenterY + 4);
    }

    if (view == SettingsView::ConnectPassword) {
      constexpr int kFieldX = 8;
      constexpr int kFieldY = 14;
      constexpr int kFieldW = 112;
      constexpr int kFieldH = 16;

      s_display.drawFrame(kFieldX, kFieldY, kFieldW, kFieldH);
      s_display.setFont(u8g2_font_5x8_tf);

      char passwordLine[21] = {};
      const uint8_t visibleChars = static_cast<uint8_t>(sizeof(passwordLine) - 1);
      const uint8_t start = (s_passwordLength > visibleChars) ? static_cast<uint8_t>(s_passwordLength - visibleChars) : 0;
      const uint8_t copyLen = static_cast<uint8_t>(std::min<int>(s_passwordLength - start, visibleChars));
      for (uint8_t i = 0; i < copyLen; ++i) {
        passwordLine[i] = s_passwordInput[start + i];
      }
      s_display.drawStr(kFieldX + 4, kFieldY + 11, passwordLine);

      constexpr int kCenterX = 64;
      constexpr int kPickerY = 40;
      constexpr int kSlotW = 14;
      constexpr int kHighlightW = 12;
      constexpr int kHighlightH = 12;

      for (int offset = -3; offset <= 3; ++offset) {
        int idx = static_cast<int>(s_passwordCharSelection) + offset;
        while (idx < 0) {
          idx += kPasswordPickerItemCount;
        }
        idx %= kPasswordPickerItemCount;

        const int x = kCenterX + (offset * kSlotW);
        const bool selected = (offset == 0);
        if (selected) {
          s_display.setDrawColor(1);
          s_display.drawBox(x - (kHighlightW / 2), kPickerY - 9, kHighlightW, kHighlightH);
          s_display.setDrawColor(0);
        }

        char token[3] = {};
        const char value = passwordCharacterForSelection(static_cast<uint8_t>(idx));
        if (value == '\b') {
          token[0] = '<';
          token[1] = '-';
        } else {
          token[0] = value;
        }

        const int tokenX = x - (s_display.getStrWidth(token) / 2);
        s_display.drawStr(tokenX, kPickerY, token);

        if (selected) {
          s_display.setDrawColor(1);
        }
      }

      s_display.setFont(menuFont);
    }

    if (view == SettingsView::ShockerNameEdit) {
      constexpr int kFieldX = 8;
      constexpr int kFieldY = 14;
      constexpr int kFieldW = 112;
      constexpr int kFieldH = 16;

      s_display.drawFrame(kFieldX, kFieldY, kFieldW, kFieldH);
      s_display.setFont(u8g2_font_5x8_tf);

      char nameLine[21] = {};
      const uint8_t visibleChars = static_cast<uint8_t>(sizeof(nameLine) - 1);
      const uint8_t start = (s_passwordLength > visibleChars) ? static_cast<uint8_t>(s_passwordLength - visibleChars) : 0;
      const uint8_t copyLen = static_cast<uint8_t>(std::min<int>(s_passwordLength - start, visibleChars));
      for (uint8_t i = 0; i < copyLen; ++i) {
        nameLine[i] = s_passwordInput[start + i];
      }
      s_display.drawStr(kFieldX + 4, kFieldY + 11, nameLine);

      constexpr int kCenterX = 64;
      constexpr int kPickerY = 40;
      constexpr int kSlotW = 14;
      constexpr int kHighlightW = 12;
      constexpr int kHighlightH = 12;

      for (int offset = -3; offset <= 3; ++offset) {
        int idx = static_cast<int>(s_passwordCharSelection) + offset;
        while (idx < 0) {
          idx += kPasswordPickerItemCount;
        }
        idx %= kPasswordPickerItemCount;

        const int x = kCenterX + (offset * kSlotW);
        const bool selected = (offset == 0);
        if (selected) {
          s_display.setDrawColor(1);
          s_display.drawBox(x - (kHighlightW / 2), kPickerY - 9, kHighlightW, kHighlightH);
          s_display.setDrawColor(0);
        }

        char token[3] = {};
        const char value = passwordCharacterForSelection(static_cast<uint8_t>(idx));
        if (value == '\b') {
          token[0] = '<';
          token[1] = '-';
        } else {
          token[0] = value;
        }

        const int tokenX = x - (s_display.getStrWidth(token) / 2);
        s_display.drawStr(tokenX, kPickerY, token);

        if (selected) {
          s_display.setDrawColor(1);
        }
      }

      s_display.setFont(menuFont);
    }

    if (view == SettingsView::UpdateRepoEdit) {
      constexpr int kFieldX = 8;
      constexpr int kFieldY = 14;
      constexpr int kFieldW = 112;
      constexpr int kFieldH = 16;

      s_display.drawFrame(kFieldX, kFieldY, kFieldW, kFieldH);
      s_display.setFont(u8g2_font_5x8_tf);

      char urlLine[21] = {};
      const uint8_t visibleChars = static_cast<uint8_t>(sizeof(urlLine) - 1);
      const uint8_t start = (s_passwordLength > visibleChars) ? static_cast<uint8_t>(s_passwordLength - visibleChars) : 0;
      const uint8_t copyLen = static_cast<uint8_t>(std::min<int>(s_passwordLength - start, visibleChars));
      for (uint8_t i = 0; i < copyLen; ++i) {
        urlLine[i] = s_passwordInput[start + i];
      }
      s_display.drawStr(kFieldX + 4, kFieldY + 11, urlLine);

      constexpr int kCenterX = 64;
      constexpr int kPickerY = 40;
      constexpr int kSlotW = 14;
      constexpr int kHighlightW = 12;
      constexpr int kHighlightH = 12;

      for (int offset = -3; offset <= 3; ++offset) {
        int idx = static_cast<int>(s_passwordCharSelection) + offset;
        while (idx < 0) {
          idx += kPasswordPickerItemCount;
        }
        idx %= kPasswordPickerItemCount;

        const int x = kCenterX + (offset * kSlotW);
        const bool selected = (offset == 0);
        if (selected) {
          s_display.setDrawColor(1);
          s_display.drawBox(x - (kHighlightW / 2), kPickerY - 9, kHighlightW, kHighlightH);
          s_display.setDrawColor(0);
        }

        char token[3] = {};
        const char value = passwordCharacterForSelection(static_cast<uint8_t>(idx));
        if (value == '\b') {
          token[0] = '<';
          token[1] = '-';
        } else {
          token[0] = value;
        }

        const int tokenX = x - (s_display.getStrWidth(token) / 2);
        s_display.drawStr(tokenX, kPickerY, token);

        if (selected) {
          s_display.setDrawColor(1);
        }
      }

      s_display.setFont(menuFont);
    }

    if (view == SettingsView::AccountLink) {
      constexpr int kSlotW = 16;
      constexpr int kSlotH = 16;
      constexpr int kSlotGap = 2;
      constexpr int kSlotsX = 13;
      constexpr int kSlotsY = 14;
      constexpr int kCenterX = 64;
      constexpr int kPickerY = 40;
      constexpr int kPickerSlotW = 14;
      constexpr int kHighlightW = 12;
      constexpr int kHighlightH = 12;

      s_display.setFont(u8g2_font_6x10_tf);

      for (uint8_t i = 0; i < kAccountCodeLength; ++i) {
        const int x = kSlotsX + (i * (kSlotW + kSlotGap));
        s_display.drawFrame(x, kSlotsY, kSlotW, kSlotH);

        char digit[2] = {'-', '\0'};
        if (i < s_accountCodeLength) {
          digit[0] = s_accountCodeInput[i];
        }

        const int textX = x + ((kSlotW - s_display.getStrWidth(digit)) / 2);
        s_display.drawStr(textX, kSlotsY + 11, digit);

        if (i == s_accountCodeLength && s_accountCodeLength < kAccountCodeLength) {
          s_display.drawFrame(x + 1, kSlotsY + 1, kSlotW - 2, kSlotH - 2);
        }
      }

      for (int offset = -3; offset <= 3; ++offset) {
        int idx = static_cast<int>(s_accountDigitSelection) + offset;
        while (idx < 0) {
          idx += kAccountDigitItemCount;
        }
        idx %= kAccountDigitItemCount;

        const int x = kCenterX + (offset * kPickerSlotW);
        const bool selected = (offset == 0);

        if (selected) {
          s_display.setDrawColor(1);
          s_display.drawBox(x - (kHighlightW / 2), kPickerY - 9, kHighlightW, kHighlightH);
          s_display.setDrawColor(0);
        }

        char token[2] = {static_cast<char>('0' + idx), '\0'};
        const int tokenX = x - (s_display.getStrWidth(token) / 2);
        s_display.drawStr(tokenX, kPickerY, token);

        if (selected) {
          s_display.setDrawColor(1);
        }
      }

      s_display.setFont(menuFont);
    }

    if (view == SettingsView::About) {
      s_display.setFont(u8g2_font_5x8_tf);
      constexpr const char* kAboutLines[] = {
        "Elec-Toys",
        "Tamer Hub",
        "",
        "Powered by OpenShock",
      };
      constexpr int kLineCount = 4;
      constexpr int kLineH = 9;
      const int totalH = (kLineCount - 1) * kLineH;
      int lineY = (54 - totalH) / 2 + 7;  // vertically center in content area
      for (int i = 0; i < kLineCount; ++i) {
        if (kAboutLines[i][0] != '\0') {
          const int textW = s_display.getStrWidth(kAboutLines[i]);
          drawScrollingText(kAboutLines[i], (128 - textW) / 2, lineY, textW + 2);
        }
        lineY += kLineH;
      }
      s_display.setFont(menuFont);
    }

    const uint8_t visibleCount = std::min<uint8_t>(kSettingsVisibleCount, itemCount);
    const bool showScroll = itemCount > visibleCount;
    const int textRegionMaxX = showScroll ? 119 : 127;
    const auto onlineShockers = (view == SettingsView::ShockerList) ? getOnlineShockersSnapshot() : std::vector<OpenShock::GatewayConnectionManager::OnlineShockerInfo> {};

    for (uint8_t row = 0; row < kSettingsVisibleCount && view != SettingsView::ConnectPassword && view != SettingsView::ShockerNameEdit && view != SettingsView::AccountLink && view != SettingsView::UpdateRepoEdit && view != SettingsView::UpdatePrompt; ++row) {
      const uint8_t itemIndex = static_cast<uint8_t>(*firstVisible + row);
      if (itemIndex >= itemCount) {
        break;
      }

      if (view == SettingsView::Network && (itemIndex == 0 || itemIndex == 1 || itemIndex == 2)) {
        const std::string_view item = getSettingsItem(view, itemIndex);
        const int checkX = 13;
        const int checkY = lineYs[row] - 8;
        const bool enabled = (itemIndex == 0) ? s_networkWifiEnabled :
                             (itemIndex == 1) ? s_networkAccessPointEnabled :
                             s_networkCaptivePortalEnabled;
        s_display.drawFrame(checkX, checkY, 8, 8);
        if (enabled) {
          s_display.drawLine(checkX + 1, checkY + 1, checkX + 6, checkY + 6);
          s_display.drawLine(checkX + 6, checkY + 1, checkX + 1, checkY + 6);
        }
        drawScrollingText(item, 24, lineYs[row], textRegionMaxX - 24);
      } else if (view == SettingsView::Connect) {
        const std::string_view item = getSettingsItem(view, itemIndex);
        const int iconX = 13;
        const int iconY = lineYs[row] - 7;
        const OpenShock::WiFiNetwork& net = connectNetworks[itemIndex];
        const bool isConnected = hasConnectedNetwork && std::strncmp(net.ssid, connectedNetwork.ssid, sizeof(net.ssid)) == 0;
        const std::string_view icon = wifiStrengthIconForRssi(net.rssi);

        if (isConnected) {
          s_display.drawBox(iconX, iconY, 8, 8);
          drawEncodedIcon(icon, iconX, iconY, 0);
        } else {
          drawEncodedIcon(icon, iconX, iconY, 1);
        }

        const char* name = (net.ssid[0] != '\0') ? net.ssid : "<hidden>";
        drawScrollingText(name, 24, lineYs[row], textRegionMaxX - 24);
      } else if (view == SettingsView::ShockerList) {
        if (itemIndex == 0) {
          const int checkX = kShockerListPrefixX;
          const int checkY = lineYs[row] - 8;
          s_display.drawFrame(checkX, checkY, 8, 8);
          if (s_shockerKeepAliveEnabled) {
            s_display.drawLine(checkX + 1, checkY + 1, checkX + 6, checkY + 6);
            s_display.drawLine(checkX + 6, checkY + 1, checkX + 1, checkY + 6);
          }
          drawScrollingText("Keep Alive", kShockerListTextX, lineYs[row], textRegionMaxX - kShockerListTextX);
        } else if (itemIndex == 1) {
          drawScrollingText("Add Shocker", kShockerListTextX, lineYs[row], textRegionMaxX - kShockerListTextX);
          s_display.drawStr(kShockerListPrefixX, lineYs[row], "+");
        } else {
          const uint8_t localIndex = static_cast<uint8_t>(itemIndex - kShockerListStaticItemCount);
          if (localIndex < s_shockerCount) {
            drawScrollingText(s_shockers[localIndex].name, kShockerListTextX, lineYs[row], textRegionMaxX - kShockerListTextX);
          } else {
            const uint8_t onlineIndex = static_cast<uint8_t>(localIndex - s_shockerCount);
            if (onlineIndex < onlineShockers.size()) {
              char onlineLabel[24] = {};
              std::strncpy(onlineLabel, onlineShockers[onlineIndex].displayName.c_str(), sizeof(onlineLabel) - 1);
              onlineLabel[sizeof(onlineLabel) - 1] = '\0';
              drawEncodedIcon(kLinkIconCode, kShockerListPrefixX, lineYs[row] - 7, 1);
              drawScrollingText(onlineLabel, kShockerListTextX, lineYs[row], textRegionMaxX - kShockerListTextX);
            }
          }
        }
      } else if (view == SettingsView::ShockerDetail) {
        char detailBuf[24];
        getShockerDetailItem(itemIndex, detailBuf, sizeof(detailBuf));
        drawScrollingText(detailBuf, labelX, lineYs[row], textRegionMaxX - labelX);
      } else if (view == SettingsView::System) {
        if (itemIndex == 1 || itemIndex == 3 || itemIndex == 4) {
          const int checkX = 13;
          const int checkY = lineYs[row] - 8;
          const bool enabled = (itemIndex == 1) ? s_screenSaverEnabled :
                               (itemIndex == 3) ? s_batteryIconEnabled :
                               s_batteryPercentEnabled;
          const char* label = (itemIndex == 1) ? "Screen Saver" :
                              (itemIndex == 3) ? "Battery Icon" :
                              "Battery Level";
          s_display.drawFrame(checkX, checkY, 8, 8);
          if (enabled) {
            s_display.drawLine(checkX + 1, checkY + 1, checkX + 6, checkY + 6);
            s_display.drawLine(checkX + 6, checkY + 1, checkX + 1, checkY + 6);
          }
          drawScrollingText(label, 24, lineYs[row], textRegionMaxX - 24);
        } else if (itemIndex == 0) {
          char line[28] = {};
          std::snprintf(line, sizeof(line), "Screen Sleep: %us", static_cast<unsigned>(s_screenSleepSeconds));
          drawScrollingText(line, labelX, lineYs[row], textRegionMaxX - labelX);
        } else if (itemIndex == 2) {
          char line[28] = {};
          std::snprintf(line, sizeof(line), "Device Sleep: %um", static_cast<unsigned>(s_deviceSleepMinutes));
          drawScrollingText(line, labelX, lineYs[row], textRegionMaxX - labelX);
        } else {
          drawScrollingText("Battery Calibration", labelX, lineYs[row], textRegionMaxX - labelX);
        }
      } else if (view == SettingsView::Update && (itemIndex == 1 || itemIndex == 2)) {
        // Auto Update (1) and Prompt To Update (2) have checkbox indicators.
        const bool checked = (itemIndex == 1) ? s_otaAutoUpdate : (s_otaPromptUpdates && !s_otaNeverPrompt);
        const int checkX = 13;
        const int checkY = lineYs[row] - 8;
        s_display.drawFrame(checkX, checkY, 8, 8);
        if (checked) {
          s_display.drawLine(checkX + 1, checkY + 1, checkX + 6, checkY + 6);
          s_display.drawLine(checkX + 6, checkY + 1, checkX + 1, checkY + 6);
        }
        drawScrollingText(kUpdateItems[itemIndex], 24, lineYs[row], textRegionMaxX - 24);
      } else {
        const std::string_view item = getSettingsItem(view, itemIndex);
        drawScrollingText(item, labelX, lineYs[row], textRegionMaxX - labelX);
      }
    }

    if (view != SettingsView::ConnectPassword && view != SettingsView::ShockerNameEdit && view != SettingsView::AccountLink && view != SettingsView::UpdateRepoEdit && view != SettingsView::UpdatePrompt) {
      drawScrollIndicator(itemCount, *firstVisible, visibleCount);
    }

    if (view == SettingsView::Connect && itemCount == 0) {
      const bool scanning = OpenShock::WiFiScanManager::IsScanning();
      const uint8_t phase = static_cast<uint8_t>((OpenShock::millis() / 180) % 8);
      constexpr int kSpinnerDx[8] = {0, 1, 2, 1, 0, -1, -2, -1};
      constexpr int kSpinnerDy[8] = {-2, -1, 0, 1, 2, 1, 0, -1};

      s_display.setFont(u8g2_font_5x8_tf);
      const char* const statusText = scanning ? "Scanning" : "No networks";
      const int statusX = (128 - s_display.getStrWidth(statusText)) / 2;
      s_display.drawStr(statusX, 29, statusText);

      const int cx = 64;
      const int cy = 39;
      for (uint8_t i = 0; i < 8; ++i) {
        const bool on = scanning ? (i == phase) : false;
        const int x = cx + kSpinnerDx[i];
        const int y = cy + kSpinnerDy[i];
        if (on) {
          s_display.drawBox(x - 1, y - 1, 2, 2);
        } else {
          s_display.drawPixel(x, y);
        }
      }

      s_display.setFont(menuFont);
    }

    if (isInfoPopupVisible()) {
      constexpr uint8_t kMaxLines = 6;
      constexpr int kScreenW = 128;
      constexpr int kScreenH = 64;
      constexpr int kMinPopupW = 64;
      constexpr int kMaxPopupW = 120;
      constexpr int kPaddingX = 4;
      constexpr int kPaddingY = 4;
      constexpr int kLineHeight = 8;
      constexpr int kMaxTextWidth = kMaxPopupW - (kPaddingX * 2);

      s_display.setFont(u8g2_font_5x8_tf);

      std::array<std::array<char, 64>, kMaxLines> lines {};
      uint8_t lineCount = 0;

      auto appendLine = [&](const std::string& text) {
        if (lineCount >= kMaxLines) {
          return;
        }

        std::memset(lines[lineCount].data(), 0, lines[lineCount].size());
        std::strncpy(lines[lineCount].data(), text.c_str(), lines[lineCount].size() - 1);
        ++lineCount;
      };

      auto wrapParagraph = [&](std::string_view paragraph) {
        std::string line;
        std::size_t pos = 0;

        while (pos < paragraph.size()) {
          while (pos < paragraph.size() && paragraph[pos] == ' ') {
            ++pos;
          }

          if (pos >= paragraph.size()) {
            break;
          }

          std::size_t wordEnd = pos;
          while (wordEnd < paragraph.size() && paragraph[wordEnd] != ' ') {
            ++wordEnd;
          }

          std::string currentWord(paragraph.substr(pos, wordEnd - pos));
          std::string candidate = line;
          if (!candidate.empty()) {
            candidate.push_back(' ');
          }
          candidate += currentWord;

          if (!line.empty() && s_display.getStrWidth(candidate.c_str()) > kMaxTextWidth) {
            appendLine(line);
            if (lineCount >= kMaxLines) {
              return;
            }
            line.clear();
            continue;
          }

          if (line.empty() && s_display.getStrWidth(currentWord.c_str()) > kMaxTextWidth) {
            std::string fragment;
            for (char c : currentWord) {
              std::string next = fragment;
              next.push_back(c);
              if (!fragment.empty() && s_display.getStrWidth(next.c_str()) > kMaxTextWidth) {
                appendLine(fragment);
                if (lineCount >= kMaxLines) {
                  return;
                }
                fragment.clear();
              }
              fragment.push_back(c);
            }
            line = fragment;
          } else {
            line = candidate;
          }

          pos = wordEnd;
        }

        appendLine(line);
      };

      std::size_t paragraphStart = 0;
      const std::size_t messageLen = std::strlen(s_infoPopupMessage);
      while (paragraphStart <= messageLen && lineCount < kMaxLines) {
        std::size_t paragraphEnd = paragraphStart;
        while (paragraphEnd < messageLen && s_infoPopupMessage[paragraphEnd] != '\n') {
          ++paragraphEnd;
        }

        wrapParagraph(std::string_view(s_infoPopupMessage + paragraphStart, paragraphEnd - paragraphStart));

        if (paragraphEnd >= messageLen) {
          break;
        }

        paragraphStart = paragraphEnd + 1;
      }

      if (lineCount == 0) {
        appendLine("");
      }

      int contentW = 0;
      for (uint8_t i = 0; i < lineCount; ++i) {
        contentW = std::max(contentW, static_cast<int>(s_display.getStrWidth(lines[i].data())));
      }

      const int popupW = std::clamp(contentW + (kPaddingX * 2), kMinPopupW, kMaxPopupW);
      const int popupH = std::clamp(static_cast<int>(lineCount) * kLineHeight + (kPaddingY * 2), 20, 52);
      const int popupX = (kScreenW - popupW) / 2;
      const int popupY = (kScreenH - popupH) / 2;

      s_display.setDrawColor(0);
      s_display.drawRBox(popupX + 1, popupY + 1, popupW - 2, popupH - 2, 2);
      s_display.setDrawColor(1);
      s_display.drawRFrame(popupX, popupY, popupW, popupH, 2);

      int textY = popupY + kPaddingY + 7;
      for (uint8_t i = 0; i < lineCount; ++i) {
        const int textX = popupX + ((popupW - s_display.getStrWidth(lines[i].data())) / 2);
        s_display.drawStr(textX, textY, lines[i].data());
        textY += kLineHeight;
      }

      s_display.setFont(menuFont);
    }

    if (view == SettingsView::Network && s_networkStatusOverlayOpen) {
      constexpr int kOverlayX = 6;
      constexpr int kOverlayY = 14;
      constexpr int kOverlayW = 116;
      constexpr int kOverlayH = 38;

      char stationIp[16] = "-";
      char apIp[16] = "-";
      OpenShock::WiFiNetwork net {};
      const bool wifiConnected = OpenShock::WiFiManager::IsConnected();

      if (!OpenShock::WiFiManager::GetConnectedNetwork(net)) {
        std::snprintf(net.ssid, sizeof(net.ssid), "Not connected");
      }

      if (!OpenShock::WiFiManager::GetIPAddress(stationIp)) {
        std::snprintf(stationIp, sizeof(stationIp), "-");
      }

      const IPAddress softApIp = WiFi.softAPIP();
      std::snprintf(apIp, sizeof(apIp), "%u.%u.%u.%u", softApIp[0], softApIp[1], softApIp[2], softApIp[3]);

      const char* shownSsid = wifiConnected ? net.ssid : "Not connected";
      const char* shownIp = wifiConnected ? stationIp : apIp;

      s_display.setDrawColor(0);
      s_display.drawRBox(kOverlayX + 1, kOverlayY + 1, kOverlayW - 2, kOverlayH - 2, 3);
      s_display.setDrawColor(1);
      s_display.drawRFrame(kOverlayX, kOverlayY, kOverlayW, kOverlayH, 3);
      s_display.setFont(u8g2_font_5x8_tf);
      s_display.drawStr(kOverlayX + 6, kOverlayY + 11, "Network Status");
      char line1[32];
      char line2[32];
      std::snprintf(line1, sizeof(line1), "SSID: %.18s", shownSsid);
      std::snprintf(line2, sizeof(line2), "IP: %s", shownIp);
      s_display.drawStr(kOverlayX + 6, kOverlayY + 21, line1);
      s_display.drawStr(kOverlayX + 6, kOverlayY + 30, line2);
      s_display.setDrawColor(1);
      s_display.setFont(menuFont);
    }

    constexpr int kButtonY = 54;
    constexpr int kButtonH = 9;
    constexpr int kButtonW = 38;
    constexpr int kButtonGap = 5;
    constexpr int kButtonX0 = 2;
    constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
    constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;

    s_display.setFont(u8g2_font_5x8_tf);

    auto drawButton = [&](int x, std::string_view text, bool linkIcon) {
      s_display.drawRFrame(x, kButtonY, kButtonW, kButtonH, 2);
      if (linkIcon) {
        const int centerY = kButtonY + 4;
        s_display.drawCircle(x + 15, centerY, 2, U8G2_DRAW_ALL);
        s_display.drawCircle(x + 23, centerY, 2, U8G2_DRAW_ALL);
        s_display.drawLine(x + 17, centerY - 1, x + 21, centerY - 1);
        s_display.drawLine(x + 17, centerY + 1, x + 21, centerY + 1);
      } else if (!text.empty()) {
        const int textX = x + ((kButtonW - s_display.getStrWidth(text.data())) / 2);
        s_display.drawStr(textX, kButtonY + 7, text.data());
      }
    };

    const bool lockToBack = (view == SettingsView::Network && s_networkStatusOverlayOpen);
    const bool isTextInputView = (view == SettingsView::ConnectPassword || view == SettingsView::ShockerNameEdit || view == SettingsView::AccountLink || view == SettingsView::UpdateRepoEdit);
    const bool showHelp = isHelpButtonVisibleForCurrentView();
    drawButton(kButtonX0, "Back", false);
    drawButton(kButtonX1, isTextInputView ? "Done" : (showHelp ? "Help" : ""), false);
    drawButton(kButtonX2, lockToBack ? "" : "Enter", false);

    s_display.sendBuffer();
  }

  void drawLockSymbol(int cx, int cy, bool inverted)
  {
    const uint8_t fg = inverted ? 0 : 1;
    const uint8_t bg = inverted ? 1 : 0;

    if (inverted) {
      s_display.setDrawColor(1);
      s_display.drawBox(cx - 16, cy - 12, 33, 28);
    }

    s_display.setDrawColor(fg);
    // Shackle: two vertical posts + top bar (2px thick each)
    s_display.drawLine(cx - 6, cy - 1, cx - 6, cy - 9);
    s_display.drawLine(cx - 5, cy - 1, cx - 5, cy - 9);
    s_display.drawLine(cx + 5, cy - 1, cx + 5, cy - 9);
    s_display.drawLine(cx + 6, cy - 1, cx + 6, cy - 9);
    s_display.drawLine(cx - 6, cy - 9, cx + 6, cy - 9);
    s_display.drawLine(cx - 6, cy - 10, cx + 6, cy - 10);

    // Body
    s_display.drawRBox(cx - 11, cy - 1, 22, 14, 2);

    // Keyhole circle + slot
    s_display.setDrawColor(bg);
    s_display.drawDisc(cx, cy + 4, 3, U8G2_DRAW_ALL);
    s_display.drawBox(cx - 1, cy + 6, 3, 5);
    s_display.setDrawColor(1);
  }

  void drawMainPage()
  {
    s_display.clearBuffer();
    ensureActiveMainShockerSelected();

    OpenShock::WiFiNetwork connectedNetwork {};
    const bool wifiConnected = OpenShock::WiFiManager::GetConnectedNetwork(connectedNetwork);
    // Always show strong wifi icon when connected, regardless of actual signal strength
    drawWifiStatusIcon(2, 2, wifiConnected, -50);

    // Show link icon to the right of the wifi icon when linked to an account and gateway is online
    const bool gwOnline     = OpenShock::GatewayConnectionManager::IsConnected();
    const bool accountLinked = OpenShock::GatewayConnectionManager::IsLinked();
    const bool showLinkIcon = wifiConnected && gwOnline && accountLinked;
    int batteryIconX = 2;
    int batteryTextX = 14;
    if (wifiConnected) {
      if (showLinkIcon) {
        drawEncodedIcon(kLinkIconCode, 10, 2, 1);
        batteryIconX = 19;
        batteryTextX = 31;
      } else {
        batteryIconX = 11;
        batteryTextX = 23;
      }
    }

    s_display.setFont(u8g2_font_5x8_tf);
    if (s_batteryIconEnabled) {
      drawBatteryStatusIcon(batteryIconX, 2, s_mainBatteryPercent);
    }

    if (s_batteryPercentEnabled) {
      char batteryBuf[5];
      std::snprintf(batteryBuf, sizeof(batteryBuf), "%02u%%", static_cast<unsigned>(s_mainBatteryPercent));
      s_display.drawStr(s_batteryIconEnabled ? batteryTextX : batteryIconX, 8, batteryBuf);
    }

    const uint8_t maxLimit = std::min<uint8_t>(getCurrentActiveIntensity(), 99);

    char intensityBuf[4];
    std::snprintf(intensityBuf, sizeof(intensityBuf), "%02u", static_cast<unsigned>(maxLimit));
    // Keep layout anchored to "88" so all digit pairs use the same bounding box.
    const char* intensityText = intensityBuf;
    const char* const kLayoutRef = "88";
    s_display.setFont(u8g2_font_logisoso32_tn);

    // Keep relative alignment and move the whole gauge up so top outline is 2px from top.
    const int numberCenterX = 64;
    const int textWidth = s_display.getStrWidth(kLayoutRef);
    const int ascent = s_display.getAscent();
    const int descent = s_display.getDescent();
    const int textHeight = ascent - descent;

    const int radius = std::max(textWidth / 2, textHeight / 2) + 15;
    constexpr uint8_t width = 5;

    const int centerX = numberCenterX;
    const int centerY = radius + 2;
    const int numberCenterY = centerY;
    const int textX = numberCenterX - (textWidth / 2) + 1;
    const int textY = numberCenterY + ((ascent + descent) / 2);

    // Draw a 230 degree ring arc (180 + 25 + 25) around the text center.
    constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
    constexpr int kArcStartDeg = 155;
    constexpr int kArcEndDeg = 385;
    const int outerRadius = radius;
    const int innerRadius = outerRadius - static_cast<int>(width) + 1;

    const int startNorm = kArcStartDeg % 360;
    const int arcSpan = kArcEndDeg - kArcStartDeg;
    const int fillSpan = (arcSpan * static_cast<int>(maxLimit)) / 99;

    s_display.setDrawColor(1);
    constexpr float kAngleStep = 0.5f;
    const float startRad = static_cast<float>(kArcStartDeg) * kDegToRad;
    const float endRad = static_cast<float>(kArcEndDeg) * kDegToRad;
    const float stepRad = kAngleStep * kDegToRad;
    const float stepCos = std::cos(stepRad);
    const float stepSin = std::sin(stepRad);
    float cosValue = std::cos(startRad);
    float sinValue = std::sin(startRad);
    float arcProgressDeg = 0.0f;

    int prevOuterX = 0;
    int prevOuterY = 0;
    int prevInnerX = 0;
    int prevInnerY = 0;
    bool hasPrev = false;

    for (float rad = startRad; rad <= endRad + (stepRad * 0.5f); rad += stepRad) {
      int outerX = 0;
      int outerY = 0;
      int innerX = 0;
      int innerY = 0;

      outerX = centerX + static_cast<int>(std::lround(cosValue * static_cast<float>(outerRadius)));
      outerY = centerY + static_cast<int>(std::lround(sinValue * static_cast<float>(outerRadius)));
      innerX = centerX + static_cast<int>(std::lround(cosValue * static_cast<float>(innerRadius)));
      innerY = centerY + static_cast<int>(std::lround(sinValue * static_cast<float>(innerRadius)));

      if (hasPrev) {
        s_display.drawLine(prevOuterX, prevOuterY, outerX, outerY);
        s_display.drawLine(prevInnerX, prevInnerY, innerX, innerY);
      }

      if (arcProgressDeg <= static_cast<float>(fillSpan)) {
        s_display.drawLine(innerX, innerY, outerX, outerY);
      }

      prevOuterX = outerX;
      prevOuterY = outerY;
      prevInnerX = innerX;
      prevInnerY = innerY;
      hasPrev = true;

      const float nextCos = (cosValue * stepCos) - (sinValue * stepSin);
      const float nextSin = (sinValue * stepCos) + (cosValue * stepSin);
      cosValue = nextCos;
      sinValue = nextSin;
      arcProgressDeg += kAngleStep;
    }

    // Close the arc at both ends with radial caps so the ring is fully sealed.
    {
      int startOuterX = 0;
      int startOuterY = 0;
      int startInnerX = 0;
      int startInnerY = 0;
      int endOuterX = 0;
      int endOuterY = 0;
      int endInnerX = 0;
      int endInnerY = 0;

      const float startAngle = static_cast<float>(kArcStartDeg) * kDegToRad;
      const float endAngle = static_cast<float>(kArcEndDeg) * kDegToRad;
      const float startCos = std::cos(startAngle);
      const float startSin = std::sin(startAngle);
      const float endCos = std::cos(endAngle);
      const float endSin = std::sin(endAngle);

      startOuterX = centerX + static_cast<int>(std::lround(startCos * static_cast<float>(outerRadius)));
      startOuterY = centerY + static_cast<int>(std::lround(startSin * static_cast<float>(outerRadius)));
      startInnerX = centerX + static_cast<int>(std::lround(startCos * static_cast<float>(innerRadius)));
      startInnerY = centerY + static_cast<int>(std::lround(startSin * static_cast<float>(innerRadius)));
      endOuterX = centerX + static_cast<int>(std::lround(endCos * static_cast<float>(outerRadius)));
      endOuterY = centerY + static_cast<int>(std::lround(endSin * static_cast<float>(outerRadius)));
      endInnerX = centerX + static_cast<int>(std::lround(endCos * static_cast<float>(innerRadius)));
      endInnerY = centerY + static_cast<int>(std::lround(endSin * static_cast<float>(innerRadius)));

      s_display.drawLine(startInnerX, startInnerY, startOuterX, startOuterY);
      s_display.drawLine(endInnerX, endInnerY, endOuterX, endOuterY);
    }

    // Draw limit marker: radial line, 2px wide, extending 2px inside and outside the ring.
    {
      const int markerAngleDeg = (startNorm + fillSpan) % 360;
      const float markerRad = static_cast<float>(markerAngleDeg) * kDegToRad;
      const float cs = std::cos(markerRad);
      const float sn = std::sin(markerRad);
      const float tx = -sn;
      const float ty = cs;

      const float innerX = static_cast<float>(centerX) + cs * static_cast<float>(innerRadius);
      const float innerY = static_cast<float>(centerY) + sn * static_cast<float>(innerRadius);
      const float outerX = static_cast<float>(centerX) + cs * static_cast<float>(outerRadius);
      const float outerY = static_cast<float>(centerY) + sn * static_cast<float>(outerRadius);

      const float startX = innerX - (cs * 2.0f);
      const float startY = innerY - (sn * 2.0f);
      const float endX = outerX + (cs * 2.0f);
      const float endY = outerY + (sn * 2.0f);

      const int x0 = static_cast<int>(std::lround(startX));
      const int y0 = static_cast<int>(std::lround(startY));
      const int x1 = static_cast<int>(std::lround(endX));
      const int y1 = static_cast<int>(std::lround(endY));
      const int deltaX = x1 - x0;
      const int deltaY = y1 - y0;
      const int steps = std::max(std::abs(deltaX), std::abs(deltaY));

      if (steps <= 0) {
        s_display.drawPixel(x0, y0);
      } else {
        for (int step = 0; step <= steps; ++step) {
          const float t = static_cast<float>(step) / static_cast<float>(steps);
          const int centerXStep = static_cast<int>(std::lround(static_cast<float>(x0) + static_cast<float>(deltaX) * t));
          const int centerYStep = static_cast<int>(std::lround(static_cast<float>(y0) + static_cast<float>(deltaY) * t));

          for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
              s_display.drawPixel(centerXStep + offsetX, centerYStep + offsetY);
            }
          }
        }
      }

      const float capScale = 1.5f;
      const int capX0 = static_cast<int>(std::lround(endX - tx * capScale));
      const int capY0 = static_cast<int>(std::lround(endY - ty * capScale));
      const int capX1 = static_cast<int>(std::lround(endX + tx * capScale));
      const int capY1 = static_cast<int>(std::lround(endY + ty * capScale));
      s_display.drawLine(capX0, capY0, capX1, capY1);
    }

    s_display.drawStr(textX, textY, intensityText);

    // Draw active shocker details in the top-right corner (name + two-row limit).
    // The arc outer edge at y≈8 is at x≈84, so we use x=86 to x=127 (41px wide).
    {
      constexpr int kInfoLeftX = 86;
      constexpr int kInfoRightX = 127;
      constexpr int kNameY = 8;
      constexpr int kLimitLabelY = 17;
      constexpr int kLimitValueY = 25;
      s_display.setFont(u8g2_font_5x8_tf);
      s_display.setDrawColor(1);
      const char* name = getActiveShockerName();
      drawRightAlignedTextOrScroll(name, kInfoLeftX, kInfoRightX, kNameY);

      char limitText[4] = { '-', '-', '\0', '\0' };
      const uint8_t limit = getActiveShockerLimit();
      std::snprintf(limitText, sizeof(limitText), "%02u", static_cast<unsigned>(limit));
      drawRightAlignedTextOrScroll("Limit:", kInfoLeftX, kInfoRightX, kLimitLabelY);
      drawRightAlignedTextOrScroll(limitText, kInfoLeftX, kInfoRightX, kLimitValueY);
    }

    // Draw button labels at the bottom using the settings-menu button style.
    constexpr int kButtonY = 54;
    constexpr int kButtonH = 9;
    constexpr int kButtonW = 38;
    constexpr int kButtonGap = 5;
    constexpr int kButtonX0 = 2;
    constexpr int kButtonX1 = kButtonX0 + kButtonW + kButtonGap;
    constexpr int kButtonX2 = kButtonX1 + kButtonW + kButtonGap;

    s_display.setFont(u8g2_font_5x8_tf);
    s_display.setDrawColor(1);

    auto drawMainButton = [&](int x, const char* text, bool inverted) {
      if (inverted) {
        s_display.drawRBox(x, kButtonY, kButtonW, kButtonH, 2);
        s_display.setDrawColor(0);
        s_display.drawRFrame(x, kButtonY, kButtonW, kButtonH, 2);
      } else {
        s_display.drawRFrame(x, kButtonY, kButtonW, kButtonH, 2);
      }

      const int textX = x + ((kButtonW - s_display.getStrWidth(text)) / 2);
      s_display.drawStr(textX, kButtonY + 7, text);

      if (inverted) {
        s_display.setDrawColor(1);
      }
    };

    if (s_persistentInfoPopup) {
      drawMainButton(kButtonX0, "Back", false);
      drawMainButton(kButtonX1, "", false);
      drawMainButton(kButtonX2, "", false);
    } else if (s_inputLocked) {
      drawMainButton(kButtonX0, "", false);
      drawMainButton(kButtonX1, "Unlock", false);
      drawMainButton(kButtonX2, "", false);

      if (s_lockFlashUntilMs > 0 && OpenShock::millis() < s_lockFlashUntilMs) {
        s_display.setDrawColor(0);
        s_display.drawBox(44, 14, 40, 28);
        s_display.setDrawColor(1);
        s_display.drawRFrame(43, 13, 42, 30, 3);
        drawLockSymbol(64, 27, false);
      }
    } else {
      drawMainButton(kButtonX0, "Shock", s_mainShockActive);
      drawMainButton(kButtonX1, "CH", false);
      drawMainButton(kButtonX2, "Vibrate", s_mainVibrateActive);
    }

    // Draw persistent info popup overlay (e.g. post-update notification).
    if (s_persistentInfoPopup && s_infoPopupMessage[0] != '\0') {
      constexpr uint8_t kMaxLines = 4;
      constexpr int kScreenW = 128;
      constexpr int kScreenH = 64;
      constexpr int kMinPopupW = 64;
      constexpr int kMaxPopupW = 120;
      constexpr int kPaddingX = 4;
      constexpr int kPaddingY = 4;
      constexpr int kLineHeight = 8;
      constexpr int kMaxTextWidth = kMaxPopupW - (kPaddingX * 2);

      s_display.setFont(u8g2_font_5x8_tf);

      std::array<std::array<char, 64>, kMaxLines> lines {};
      uint8_t lineCount = 0;

      auto appendLine = [&](const std::string& text) {
        if (lineCount >= kMaxLines) return;
        std::memset(lines[lineCount].data(), 0, lines[lineCount].size());
        std::strncpy(lines[lineCount].data(), text.c_str(), lines[lineCount].size() - 1);
        ++lineCount;
      };

      auto wrapParagraph = [&](std::string_view paragraph) {
        std::string line;
        std::size_t pos = 0;
        while (pos < paragraph.size()) {
          while (pos < paragraph.size() && paragraph[pos] == ' ') ++pos;
          if (pos >= paragraph.size()) break;
          std::size_t wordEnd = pos;
          while (wordEnd < paragraph.size() && paragraph[wordEnd] != ' ') ++wordEnd;
          std::string currentWord(paragraph.substr(pos, wordEnd - pos));
          std::string candidate = line;
          if (!candidate.empty()) candidate.push_back(' ');
          candidate += currentWord;
          if (!line.empty() && s_display.getStrWidth(candidate.c_str()) > kMaxTextWidth) {
            appendLine(line);
            if (lineCount >= kMaxLines) return;
            line.clear();
            continue;
          }
          line = candidate;
          pos = wordEnd;
        }
        appendLine(line);
      };

      std::size_t paragraphStart = 0;
      const std::size_t messageLen = std::strlen(s_infoPopupMessage);
      while (paragraphStart <= messageLen && lineCount < kMaxLines) {
        std::size_t paragraphEnd = paragraphStart;
        while (paragraphEnd < messageLen && s_infoPopupMessage[paragraphEnd] != '\n') ++paragraphEnd;
        wrapParagraph(std::string_view(s_infoPopupMessage + paragraphStart, paragraphEnd - paragraphStart));
        if (paragraphEnd >= messageLen) break;
        paragraphStart = paragraphEnd + 1;
      }

      if (lineCount == 0) appendLine("");

      int contentW = 0;
      for (uint8_t i = 0; i < lineCount; ++i)
        contentW = std::max(contentW, static_cast<int>(s_display.getStrWidth(lines[i].data())));

      const int popupW = std::clamp(contentW + (kPaddingX * 2), kMinPopupW, kMaxPopupW);
      const int popupH = std::clamp(static_cast<int>(lineCount) * kLineHeight + (kPaddingY * 2), 20, 48);
      const int popupX = (kScreenW - popupW) / 2;
      const int popupY = (kScreenH - popupH) / 2;

      s_display.setDrawColor(0);
      s_display.drawRBox(popupX + 1, popupY + 1, popupW - 2, popupH - 2, 2);
      s_display.setDrawColor(1);
      s_display.drawRFrame(popupX, popupY, popupW, popupH, 2);

      int textY = popupY + kPaddingY + 7;
      for (uint8_t i = 0; i < lineCount; ++i) {
        const int textX = popupX + ((popupW - s_display.getStrWidth(lines[i].data())) / 2);
        s_display.drawStr(textX, textY, lines[i].data());
        textY += kLineHeight;
      }
    }

    s_display.sendBuffer();
  }

  void processRotationEvents()
  {
    OpenShock::RotaryEncoderManager::RotationEvent evt {};
    bool changed = false;

    while (OpenShock::RotaryEncoderManager::TryPopRotationEvent(evt)) {
      if (wakeOnlyInputIfSleeping()) {
        continue;
      }

      const uint8_t page = s_currentPage.load(std::memory_order_relaxed);

      if (page == kPageMain && s_inputLocked) {
        s_lockFlashUntilMs = OpenShock::millis() + 250;
        changed = true;
        continue;
      }

      if (page == kPageMain) {
        ensureActiveMainShockerSelected();
        const int current = static_cast<int>(getCurrentActiveIntensity());
        const int next = std::clamp(current + static_cast<int>(evt.delta), 0, static_cast<int>(getActiveShockerLimit()));
        if (next != current) {
          applyActiveShockerIntensity(static_cast<uint8_t>(next));
          changed = true;
        }
      } else if (page == kPageSettings) {
        if (s_settingsView == SettingsView::Network && s_networkStatusOverlayOpen) {
          continue;
        }

        uint8_t* selection = &s_settingsSelection;
        uint8_t* firstVisible = &s_settingsFirstVisible;
        uint8_t itemCount = kSettingsItemCount;

        if (s_settingsView == SettingsView::Network) {
          selection = &s_networkSelection;
          firstVisible = &s_networkFirstVisible;
          itemCount = kNetworkItemCount;
        } else if (s_settingsView == SettingsView::Connect) {
          selection = &s_connectSelection;
          firstVisible = &s_connectFirstVisible;
          const auto networks = OpenShock::WiFiManager::GetDiscoveredWiFiNetworks();
          itemCount = static_cast<uint8_t>(std::min<std::size_t>(networks.size(), 255));
        } else if (s_settingsView == SettingsView::ConnectNetwork) {
          selection = &s_connectNetworkSelection;
          firstVisible = &s_connectNetworkFirstVisible;
          itemCount = kConnectNetworkItemCount;
        } else if (s_settingsView == SettingsView::System) {
          selection = &s_systemSelection;
          firstVisible = &s_systemFirstVisible;
          itemCount = kSystemItemCount;
        } else if (s_settingsView == SettingsView::SystemScreenSleepEdit) {
          const int next = std::clamp<int>(static_cast<int>(s_pendingSystemValue) + static_cast<int>(evt.delta), 0, 3600);
          s_pendingSystemValue = static_cast<uint16_t>(next);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::SystemDeviceSleepEdit) {
          const int next = std::clamp<int>(static_cast<int>(s_pendingSystemValue) + static_cast<int>(evt.delta), 0, 1440);
          s_pendingSystemValue = static_cast<uint16_t>(next);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::BatteryCalibrationConfirmClear || s_settingsView == SettingsView::BatteryCalibrationConfirmCharged) {
          continue;
        } else if (s_settingsView == SettingsView::ConnectPassword) {
          int current = static_cast<int>(s_passwordCharSelection);
          current += static_cast<int>(evt.delta);
          while (current < 0) {
            current += kPasswordPickerItemCount;
          }
          s_passwordCharSelection = static_cast<uint8_t>(current % kPasswordPickerItemCount);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::ShockerList) {
          selection = &s_shockerListSelection;
          firstVisible = &s_shockerListFirstVisible;
          itemCount = getTotalShockerListItemCount(getOnlineShockersSnapshot());
        } else if (s_settingsView == SettingsView::ShockerDetail) {
          selection = &s_shockerDetailSelection;
          firstVisible = &s_shockerDetailFirstVisible;
          itemCount = getShockerDetailItemCount();
        } else if (s_settingsView == SettingsView::ShockerLimitEdit) {
          // encoder scrolls the pending limit 0-99
          const int cur = static_cast<int>(s_pendingLimit);
          const int next = std::clamp(cur + static_cast<int>(evt.delta), 0, 99);
          s_pendingLimit = static_cast<uint8_t>(next);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::ShockerProtocolEdit) {
          const int count = static_cast<int>(kProtocolOptions.size());
          int cur = static_cast<int>(s_pendingProtocol);
          cur += static_cast<int>(evt.delta);
          while (cur < 0) {
            cur += count;
          }
          s_pendingProtocol = static_cast<uint8_t>(cur % count);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::ShockerNameEdit) {
          int current = static_cast<int>(s_passwordCharSelection);
          current += static_cast<int>(evt.delta);

          while (current < 0) {
            current += kPasswordPickerItemCount;
          }

          s_passwordCharSelection = static_cast<uint8_t>(current % kPasswordPickerItemCount);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::AccountMenu) {
          selection = &s_accountMenuSelection;
          firstVisible = &s_accountMenuFirstVisible;
          itemCount = getAccountMenuItemCount();
        } else if (s_settingsView == SettingsView::Update) {
          selection = &s_updateSelection;
          firstVisible = &s_updateFirstVisible;
          itemCount = kUpdateItemCount;
        } else if (s_settingsView == SettingsView::UpdatePrompt) {
          // Encoder does nothing on the update prompt — use Left/Middle/Right buttons
          continue;
        } else if (s_settingsView == SettingsView::UpdateRepoEdit) {
          int current = static_cast<int>(s_passwordCharSelection);
          current += static_cast<int>(evt.delta);
          while (current < 0) current += kPasswordPickerItemCount;
          s_passwordCharSelection = static_cast<uint8_t>(current % kPasswordPickerItemCount);
          changed = true;
          continue;
        } else if (s_settingsView == SettingsView::AccountLink) {
          int current = static_cast<int>(s_accountDigitSelection);
          current += static_cast<int>(evt.delta);

          while (current < 0) {
            current += kAccountDigitItemCount;
          }

          s_accountDigitSelection = static_cast<uint8_t>(current % kAccountDigitItemCount);
          changed = true;
          continue;
        }

        if (itemCount == 0) {
          *selection = 0;
          *firstVisible = 0;
          continue;
        }

        const int current = static_cast<int>(*selection);
        const int next = std::clamp(current - static_cast<int>(evt.delta), 0, static_cast<int>(itemCount) - 1);
        if (next != current) {
          *selection = static_cast<uint8_t>(next);

          if (*selection < *firstVisible) {
            *firstVisible = *selection;
          } else {
            const uint8_t bottomVisible = static_cast<uint8_t>(*firstVisible + kSettingsVisibleCount - 1);
            if (*selection > bottomVisible) {
              *firstVisible = static_cast<uint8_t>(*selection - (kSettingsVisibleCount - 1));
            }
          }

          changed = true;
        }
      }
    }

    if (changed) {
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
  }

  void refreshCurrentPage()
  {
    const int64_t now = OpenShock::millis();
    if (updatePowerUiState(now)) {
      return;
    }

    if (s_powerUiState == PowerUiState::BootDelay) {
      if (s_forceRedraw.exchange(false, std::memory_order_relaxed)) {
        drawScreenSaverPage();
      }
      return;
    }

    if (s_powerUiState == PowerUiState::ConfirmPowerOff) {
      if (s_forceRedraw.exchange(false, std::memory_order_relaxed)) {
        drawPowerOffPromptPage();
      }
      return;
    }

    if (s_powerUiState == PowerUiState::Goodbye || s_powerUiState == PowerUiState::PowerCut) {
      if (s_forceRedraw.exchange(false, std::memory_order_relaxed) || s_powerUiState == PowerUiState::Goodbye) {
        drawGoodbyePage();
      }
      return;
    }

    processRotationEvents();

    // Expire lock flash
    if (s_inputLocked && s_lockFlashUntilMs != 0 && now >= s_lockFlashUntilMs) {
      s_lockFlashUntilMs = 0;
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }

    updateScreenSleepState();

    if (s_deviceSleepMinutes > 0) {
      // Any active RF command (local or gateway) counts as activity.
      if (s_mainShockActive || s_mainVibrateActive) {
        markUserActivity();
      }
      const uint32_t nowMs = static_cast<uint32_t>(now);
      const uint32_t lastInputAt = s_lastUserInputAt.load(std::memory_order_relaxed);
      const uint32_t elapsedMs = nowMs - lastInputAt;
      const uint32_t timeoutMs = static_cast<uint32_t>(s_deviceSleepMinutes) * 60000U;
      if (elapsedMs >= timeoutMs) {
        beginPowerOffSequence();
        drawGoodbyePage();
        return;
      }
    }

    if (s_screenSleepActive && !s_screenSaverEnabled) {
      s_forceRedraw.store(false, std::memory_order_relaxed);
      return;
    }

    if (s_screenSleepActive && s_screenSaverEnabled) {
      if (s_forceRedraw.exchange(false, std::memory_order_relaxed)) {
        drawScreenSaverPage();
      }
      return;
    }

    // ── Firmware update in progress ──────────────────────────────────────────
    if (OpenShock::OtaUpdateManager::IsUpdateInProgress()) {
      drawUpdateInProgressPage();
      s_forceRedraw.store(true, std::memory_order_relaxed);  // keep animating
      return;
    }
    // ────────────────────────────────────────────────────────────────────────

    // ── Update prompt modal ──────────────────────────────────────────────────
    {
      char pendingVer[32] = {};
      const bool hasPendingPrompt = OpenShock::OtaUpdateManager::HasPendingUpdatePrompt(pendingVer, sizeof(pendingVer));
      if (hasPendingPrompt) {
        if (s_settingsView != SettingsView::UpdatePrompt) {
          s_prevSettingsViewBeforePrompt = s_settingsView;
          s_prevPageBeforePrompt         = s_currentPage.load(std::memory_order_relaxed);
          strncpy(s_pendingUpdateVersionDisplay, pendingVer, sizeof(s_pendingUpdateVersionDisplay) - 1);
          s_pendingUpdateVersionDisplay[sizeof(s_pendingUpdateVersionDisplay) - 1] = '\0';
          s_updatePromptSelection = 0;
          s_currentPage.store(kPageSettings, std::memory_order_relaxed);
          s_settingsView = SettingsView::UpdatePrompt;
          s_forceRedraw.store(true, std::memory_order_relaxed);
        }
        if (s_forceRedraw.exchange(false, std::memory_order_relaxed)) {
          drawUpdatePromptPage();
        }
        return;
      } else if (s_settingsView == SettingsView::UpdatePrompt) {
        // Prompt was resolved — go back to where we were.
        s_settingsView = s_prevSettingsViewBeforePrompt;
        s_currentPage.store(s_prevPageBeforePrompt, std::memory_order_relaxed);
        s_forceRedraw.store(true, std::memory_order_relaxed);
      }
    }
    // ────────────────────────────────────────────────────────────────────────

    // ── "Check For Updates" result feedback ──────────────────────────────────
    if (s_checkInProgress) {
      const int8_t status = OpenShock::OtaUpdateManager::GetLastCheckStatus();
      if (status != 1) {  // No longer actively checking
        s_checkInProgress = false;
        char resultBuf[64];
        if (status == 2) {
          const char* ver = OpenShock::OtaUpdateManager::GetCachedLatestVersion();
          snprintf(resultBuf, sizeof(resultBuf), "Up to date!\nv%s", (ver && ver[0]) ? ver : OPENSHOCK_FW_VERSION);
        } else if (status == 3) {
          snprintf(resultBuf, sizeof(resultBuf), "Check failed");
        } else if (status == 4) {
          snprintf(resultBuf, sizeof(resultBuf), "No network");
        } else if (status == 5) {
          snprintf(resultBuf, sizeof(resultBuf), "Update failed!");
        } else {
          resultBuf[0] = '\0';
        }
        if (resultBuf[0] != '\0') {
          showInfoPopup(resultBuf);
          s_infoPopupHideAt = OpenShock::millis() + 5000;
          s_forceRedraw.store(true, std::memory_order_relaxed);
        }
      }
    }
    // ────────────────────────────────────────────────────────────────────────

    const uint8_t page = s_currentPage.load(std::memory_order_relaxed);

    if (page == kPageMain) {
      ensureActiveMainShockerSelected();

      if (s_mainCommandAutoStopAtMs > 0 && now >= s_mainCommandAutoStopAtMs) {
        s_mainShockActive = false;
        s_mainVibrateActive = false;
        s_mainCommandAutoStopAtMs = 0;
        s_forceRedraw.store(true, std::memory_order_relaxed);
      }

      const bool commandActive = s_mainShockActive || s_mainVibrateActive;

      // Keep command alive while button is held: resend every 1000ms with 2000ms duration.
      if (commandActive && s_mainCommandAutoStopAtMs == 0) {
        if ((now - s_mainCommandLastSentMs) > 1000) {
          const uint8_t intensity = getCurrentActiveIntensity();
          if (s_mainShockActive) {
            sendMainShockerCommand(OpenShock::ShockerCommandType::Shock, intensity);
          } else {
            sendMainShockerCommand(OpenShock::ShockerCommandType::Vibrate, intensity);
          }
        }
      }

      const uint8_t limit = getCurrentActiveIntensity();
      OpenShock::WiFiNetwork net {};
      const bool wifiConnected = OpenShock::WiFiManager::GetConnectedNetwork(net);
      const uint8_t wifiStrength = wifiConnected ? wifiStrengthBucketForRssi(net.rssi) : 255;
      const bool gwOnline      = OpenShock::GatewayConnectionManager::IsConnected();
      const bool accountLinked = OpenShock::GatewayConnectionManager::IsLinked();
      const bool batteryChanged = updateBatterySample(now);
      const bool marqueeActive = isMainPageMarqueeActive();
      if (s_forceRedraw.exchange(false, std::memory_order_relaxed) || marqueeActive || limit != s_lastMainLimitDrawn || wifiConnected != s_lastMainWifiConnected || wifiStrength != s_lastMainWifiStrength || gwOnline != s_lastMainGwOnline || accountLinked != s_lastMainAccountLinked || batteryChanged) {
        drawMainPage();
        s_lastMainLimitDrawn    = limit;
        s_lastMainWifiConnected = wifiConnected;
        s_lastMainWifiStrength  = wifiStrength;
        s_lastMainGwOnline      = gwOnline;
        s_lastMainAccountLinked = accountLinked;
      }
      return;
    }

    if (page == kPageSettings) {
      const bool marqueeActive = isSettingsMarqueeActive();

      keepConnectingPopupAlive();
      keepAccountLinkingPopupAlive();
      keepBatteryCalibrationPopupAlive();

      if (updateInfoPopupVisibility()) {
        s_forceRedraw.store(true, std::memory_order_relaxed);
      }

      if (updatePendingPasswordConnection()) {
        s_forceRedraw.store(true, std::memory_order_relaxed);
      }

      if (s_defaultNetworkSsid[0] != '\0' && s_networkWifiEnabled && !OpenShock::WiFiManager::IsConnected()) {
        const int64_t now = OpenShock::millis();
        if ((now - s_lastDefaultConnectAttemptAt) > 5000) {
          (void)OpenShock::WiFiManager::Connect(s_defaultNetworkSsid);
          s_lastDefaultConnectAttemptAt = now;
        }
      }

      if (s_settingsView == SettingsView::Connect) {
        const int64_t now = OpenShock::millis();
        if ((now - s_lastConnectScanAt) > 4000 && !OpenShock::WiFiScanManager::IsScanning()) {
          (void)OpenShock::WiFiScanManager::StartScan();
          s_lastConnectScanAt = now;
        }

        drawSettingsPage();
        s_forceRedraw.store(false, std::memory_order_relaxed);
        return;
      }

      if (s_settingsView == SettingsView::ConnectNetwork) {
        if (marqueeActive || s_forceRedraw.exchange(false, std::memory_order_relaxed) || isInfoPopupVisible() || s_passwordConnectPending || s_accountLinkPending) {
          drawSettingsPage();
          s_forceRedraw.store(false, std::memory_order_relaxed);
        }
        return;
      }

      if (s_settingsView == SettingsView::AccountLink) {
        if (marqueeActive || s_forceRedraw.exchange(false, std::memory_order_relaxed) || isInfoPopupVisible() || s_accountLinkPending) {
          drawSettingsPage();
          s_forceRedraw.store(false, std::memory_order_relaxed);
        }
        return;
      }

      if (s_settingsView == SettingsView::ConnectPassword && marqueeActive) {
        drawSettingsPage();
        s_forceRedraw.store(false, std::memory_order_relaxed);
        return;
      }

      if (marqueeActive) {
        drawSettingsPage();
        s_forceRedraw.store(false, std::memory_order_relaxed);
        return;
      }

      if (isInfoPopupVisible() || s_passwordConnectPending || s_accountLinkPending) {
        drawSettingsPage();
        s_forceRedraw.store(false, std::memory_order_relaxed);
        return;
      }

      if (s_forceRedraw.exchange(false, std::memory_order_relaxed)) {
        drawSettingsPage();
      }
      return;
    }
  }

  void handleAnyEvent(void*, esp_event_base_t, int32_t, void*)
  {
    requestRefresh();
  }

  void refreshTask(void*)
  {
    while (true) {
      const uint8_t page = s_currentPage.load(std::memory_order_relaxed);
      const bool settingsMarquee = isSettingsMarqueeActive();
      const bool mainMarquee     = isMainPageMarqueeActive();
      const bool commandActive   = s_mainShockActive || s_mainVibrateActive;
      const int64_t now          = OpenShock::millis();
      const bool batteryPollDue  = (page == kPageMain) && ((now - s_lastBatterySampleAt) >= kBatterySamplePeriodMs);
      const bool marqueeActive   = settingsMarquee || mainMarquee;
      const bool sleepTrackingArmed = (s_screenSleepSeconds > 0 && !s_screenSleepActive) || (s_deviceSleepMinutes > 0);
      const bool lockFlashActive  = s_inputLocked && (s_lockFlashUntilMs > 0);
      const bool pendingOtaPrompt = OpenShock::OtaUpdateManager::HasPendingUpdatePrompt(nullptr, 0);
      const bool isUpdating       = OpenShock::OtaUpdateManager::IsUpdateInProgress();
      const bool inShutdown       = (s_powerUiState == PowerUiState::Goodbye || s_powerUiState == PowerUiState::PowerCut);
      const TickType_t waitTicks  = (marqueeActive || commandActive || lockFlashActive || pendingOtaPrompt || inShutdown) ? pdMS_TO_TICKS(50) : pdMS_TO_TICKS(500);
      const uint32_t notified = ulTaskNotifyTake(pdTRUE, waitTicks);
      if (notified == 0
          && !(page == kPageSettings && (settingsMarquee || s_settingsView == SettingsView::Connect || isInfoPopupVisible() || s_infoPopupMessage[0] != '\0' || s_passwordConnectPending || s_accountLinkPending))
          && !(page == kPageMain && (mainMarquee || commandActive || batteryPollDue || lockFlashActive))
          && !sleepTrackingArmed
          && !pendingOtaPrompt
          && !isUpdating
          && !inShutdown) {
        continue;
      }
      refreshCurrentPage();
    }
  }
#endif
}  // namespace

using namespace OpenShock;

void OledDisplayManager::RunChargingModeIfNeeded(gpio_num_t encoderButtonPin)
{
#if !OPENSHOCK_OLED_ENABLED
  return;
#else
  // Always cache the pin — updatePowerUiState() uses it to configure ext1 deep-sleep wakeup.
  s_encoderButtonPin = encoderButtonPin;

  // Only enter charging-mode logic on a genuine cold boot (power-on or external hardware reset).
  // Any software-initiated reset — OTA reboot, watchdog, crash, serial restart, or deep-sleep
  // wakeup — must reach Init() normally so pin 13 is raised during the boot splash instead of
  // being pulled LOW here, which would cut the latch and power off the device.
  const esp_reset_reason_t resetReason = esp_reset_reason();
  if (resetReason != ESP_RST_POWERON && resetReason != ESP_RST_EXT) {
    OS_LOGI(TAG, "Reset reason %d — skipping charging-mode check, proceeding to normal boot", static_cast<int>(resetReason));
    return;
  }

  // Configure the encoder button pin as input (matching RotaryEncoderManager's pull-up config).
  gpio_config_t btnCfg {};
  btnCfg.intr_type    = GPIO_INTR_DISABLE;
  btnCfg.mode         = GPIO_MODE_INPUT;
  btnCfg.pin_bit_mask = 1ULL << static_cast<uint32_t>(encoderButtonPin);
  btnCfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  btnCfg.pull_up_en   = GPIO_PULLUP_ENABLE;
  gpio_config(&btnCfg);

  // Encoder button is active-HIGH (pressedWhenHigh = true). LOW at boot means
  // the charger woke the device — the user was not holding the button.
  const bool buttonPressedAtBoot = gpio_get_level(encoderButtonPin) != 0;
  if (buttonPressedAtBoot) {
    return;
  }

  OS_LOGI(TAG, "Encoder button not held at boot — charger woke device, entering charging mode");

  // Explicitly hold pin 13 LOW — the USB charger provides power, not the latch.
  // Disconnecting the charger while in charging mode will power the device off.
  setPowerHoldState(false);

  // Bring up the display.
  setDisplayPower(true);
  delay(10);
  Wire.begin(OPENSHOCK_OLED_SDA_PIN, OPENSHOCK_OLED_SCL_PIN, 400000U);
  s_detectedControllerType = detectOledControllerType();
  s_displayPtr = (s_detectedControllerType == OledControllerType::SSD1309)
    ? static_cast<U8G2*>(&s_ssd1309Display)
    : static_cast<U8G2*>(&s_sh1106Display);
  s_display.setI2CAddress(detectOledI2CAddress());
  s_display.begin();

  // Draw "Charging" centered on the 128×64 screen.
  s_display.clearBuffer();
  s_display.setFont(u8g2_font_logisoso24_tf);
  const char* kChargingLabel = "Charging";
  const int textX = (128 - static_cast<int>(s_display.getStrWidth(kChargingLabel))) / 2;
  const int textY = (64 + static_cast<int>(s_display.getAscent()) + static_cast<int>(s_display.getDescent())) / 2;
  s_display.drawStr(textX, textY, kChargingLabel);
  s_display.sendBuffer();

  // Hold the charging screen for the configured duration.
  vTaskDelay(pdMS_TO_TICKS(kChargingScreenDurationMs));

  // Blank and power-save the display while we wait for the user.
  s_display.clearBuffer();
  s_display.sendBuffer();
  s_display.setPowerSave(1);
  setDisplayPower(false);

  // Block until the encoder button is held for the boot long-press threshold.
  OS_LOGI(TAG, "Charging mode: waiting for encoder long press to boot");
  int64_t pressedSince = 0;
  bool wasPressed = false;

  while (true) {
    const bool pressed    = gpio_get_level(encoderButtonPin) != 0;
    const int64_t nowMs   = OpenShock::millis();

    if (pressed && !wasPressed) {
      pressedSince = nowMs;
    } else if (!pressed) {
      pressedSince = 0;
    }
    wasPressed = pressed;

    if (pressed && pressedSince != 0 && (nowMs - pressedSince) >= kChargingModeLongPressMs) {
      OS_LOGI(TAG, "Charging mode: long press detected, continuing to normal boot");
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }

  // Restore display power so Init() can take over cleanly.
  setDisplayPower(true);
  delay(10);
  s_display.setPowerSave(0);
  s_display.clearBuffer();
  s_display.sendBuffer();
#endif
}

bool OledDisplayManager::Init()
{
#if !OPENSHOCK_OLED_ENABLED
  return true;
#else
  esp_err_t err;

  setDisplayPower(true);
  delay(10);

  Wire.begin(OPENSHOCK_OLED_SDA_PIN, OPENSHOCK_OLED_SCL_PIN, 400000U);

  s_detectedControllerType = detectOledControllerType();
  s_displayPtr = (s_detectedControllerType == OledControllerType::SSD1309)
    ? static_cast<U8G2*>(&s_ssd1309Display)
    : static_cast<U8G2*>(&s_sh1106Display);

  const uint8_t oledI2cAddress = detectOledI2CAddress();
  s_display.setI2CAddress(oledI2cAddress);
  s_display.begin();
  OS_LOGI(TAG, "OLED controller in use: %s", s_detectedControllerType == OledControllerType::SSD1309 ? "SSD1309" : "SH1106");

#if OPENSHOCK_DISPLAY_ONLY_BOOT
  s_mainBatteryPercent = 99;
  s_currentPage.store(kPageMain, std::memory_order_relaxed);
  s_display.clearBuffer();
  s_display.setDrawColor(1);
  s_display.drawBox(0, 0, 128, 64);
  s_display.sendBuffer();

  s_initialized = true;
  OS_LOGW(TAG, "Main-screen-only boot active; skipping prefs, FS, inputs, outputs, WiFi, gateway, and event hooks");
  OS_LOGI(TAG, "OLED initialized on I2C address 0x%02X", oledI2cAddress);
  return true;
#endif

  if (kBatterySensePin >= 0) {
    pinMode(kBatterySensePin, INPUT);
    analogReadResolution(12);
    analogSetPinAttenuation(kBatterySensePin, ADC_11db);
    updateBatterySample(OpenShock::millis());
  }

  loadNetworkSettingsPreferenceState();
  loadShockerPrefs();
  resetMainShockerIntensitiesOnBoot();

  bool keepAliveEnabled = true;
  if (OpenShock::Config::GetRFConfigKeepAliveEnabled(keepAliveEnabled)) {
    s_shockerKeepAliveEnabled = keepAliveEnabled;
  }

  if (OpenShock::OtaUpdateManager::GetFirmwareBootType() == OpenShock::FirmwareBootType::NewFirmware) {
    char popupMsg[48];
    std::snprintf(popupMsg, sizeof(popupMsg), "Updated!\nv" OPENSHOCK_FW_VERSION);
    showPersistentInfoPopup(popupMsg);
  }

  s_initialized = true;
  s_powerUiState = PowerUiState::BootDelay;
  s_powerUiDeadlineMs = OpenShock::millis() + kPowerHoldBootDelayMs;
  s_powerHoldPinConfigured = false;
  s_powerCutIssued = false;
  setPowerHoldState(true);  // Engage power hold as soon as display is up
  s_lastUserInputAt.store(static_cast<uint32_t>(OpenShock::millis()), std::memory_order_relaxed);
  ensureActiveMainShockerSelected();

  if (OpenShock::TaskUtils::TaskCreateExpensive(refreshTask, "oled_refresh", 4096, nullptr, 1, &s_refreshTask) != pdPASS) {
    OS_LOGE(TAG, "Failed to create OLED refresh task");
    return false;
  }

  err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handleAnyEvent, nullptr);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to register event handler for WIFI_EVENT: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, handleAnyEvent, nullptr);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to register event handler for IP_EVENT: %s", esp_err_to_name(err));
    return false;
  }

  err = esp_event_handler_register(OPENSHOCK_EVENTS, OPENSHOCK_EVENT_GATEWAY_CLIENT_STATE_CHANGED, handleAnyEvent, nullptr);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to register event handler for gateway state changes: %s", esp_err_to_name(err));
    return false;
  }

  requestRefresh();

  OS_LOGI(TAG, "OLED initialized on I2C address 0x%02X", oledI2cAddress);
  if (kBatterySensePin >= 0) {
    OS_LOGI(TAG, "Battery sense enabled on GPIO %d", kBatterySensePin);
  } else {
    OS_LOGI(TAG, "Battery sense disabled");
  }

  return true;
#endif
}

void OledDisplayManager::RequestRefresh()
{
#if OPENSHOCK_OLED_ENABLED
  requestRefresh();
#endif
}

void OledDisplayManager::HandleEncoderButtonPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && (s_settingsView == SettingsView::ConnectPassword || s_settingsView == SettingsView::ShockerNameEdit || s_settingsView == SettingsView::AccountLink)) {
    if (s_settingsView == SettingsView::AccountLink) {
      if (s_accountCodeLength > 0) {
        --s_accountCodeLength;
        s_accountCodeInput[s_accountCodeLength] = '\0';
        s_forceRedraw.store(true, std::memory_order_relaxed);
      }
    } else if (s_passwordLength > 0) {
      --s_passwordLength;
      s_passwordInput[s_passwordLength] = '\0';
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
    requestRefresh();
    return;
  }

  const uint8_t nextPage = static_cast<uint8_t>((s_currentPage.load(std::memory_order_relaxed) + 1) % kPageCount);

  if (nextPage == kPageSettings) {
    s_settingsView = SettingsView::Root;
    s_settingsSelection = 0;
    s_settingsFirstVisible = 0;
    s_networkStatusOverlayOpen = false;
  }

  s_currentPage.store(nextPage, std::memory_order_relaxed);
  s_forceRedraw.store(true, std::memory_order_relaxed);
  requestRefresh();
#endif
}

void OledDisplayManager::HandleEncoderButtonLongPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  markUserActivity();
  s_powerUiState = PowerUiState::ConfirmPowerOff;
  s_forceRedraw.store(true, std::memory_order_relaxed);
  requestRefresh();
#endif
}

void OledDisplayManager::HandleLeftButtonPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState == PowerUiState::ConfirmPowerOff) {
    markUserActivity();
    s_powerUiState = PowerUiState::Ready;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_persistentInfoPopup && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_persistentInfoPopup = false;
    std::memset(s_infoPopupMessage, 0, sizeof(s_infoPopupMessage));
    s_infoPopupHideAt = 0;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings) {
    if (s_settingsView == SettingsView::Network && s_networkStatusOverlayOpen) {
      s_networkStatusOverlayOpen = false;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::Root) {
      s_currentPage.store(kPageMain, std::memory_order_relaxed);
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView != SettingsView::Root) {
      if (s_settingsView == SettingsView::UpdatePrompt) {
        // Left = "No": dismiss without updating
        OpenShock::OtaUpdateManager::SetUserUpdateDecision(0);
        OpenShock::OtaUpdateManager::GetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
        s_settingsView = s_prevSettingsViewBeforePrompt;
        s_currentPage.store(s_prevPageBeforePrompt, std::memory_order_relaxed);
      } else if (s_settingsView == SettingsView::UpdateRepoEdit) {
        // Left = cancel: discard URL edits
        s_settingsView = SettingsView::Update;
      } else if (s_settingsView == SettingsView::ShockerNameEdit) {
        s_settingsView = SettingsView::ShockerDetail;
      } else if (s_settingsView == SettingsView::SystemScreenSleepEdit || s_settingsView == SettingsView::SystemDeviceSleepEdit) {
        s_pendingSystemValue = s_originalSystemValue;
        s_settingsView = SettingsView::System;
      } else if (s_settingsView == SettingsView::BatteryCalibrationConfirmClear) {
        s_settingsView = SettingsView::System;
      } else if (s_settingsView == SettingsView::BatteryCalibrationConfirmCharged) {
        s_settingsView = SettingsView::System;
        showInfoPopup("Charge fully, keep charger on");
      } else if (s_settingsView == SettingsView::ShockerLimitEdit) {
        // revert limit
        s_pendingLimit = s_originalLimit;
        s_settingsView = SettingsView::ShockerDetail;
      } else if (s_settingsView == SettingsView::ShockerProtocolEdit) {
        // revert protocol
        s_pendingProtocol = s_originalProtocol;
        s_settingsView = SettingsView::ShockerDetail;
      } else if (s_settingsView == SettingsView::ShockerDetail) {
        s_settingsView = SettingsView::ShockerList;
      } else if (s_settingsView == SettingsView::ShockerList) {
        s_settingsView = SettingsView::Root;
      } else if (s_settingsView == SettingsView::System || s_settingsView == SettingsView::About || s_settingsView == SettingsView::AccountMenu || s_settingsView == SettingsView::Update) {
        s_settingsView = SettingsView::Root;
      } else if (s_settingsView == SettingsView::ConnectPassword) {
        s_settingsView = SettingsView::ConnectNetwork;
      } else if (s_settingsView == SettingsView::AccountLink) {
        s_settingsView = SettingsView::AccountMenu;
      } else if (s_settingsView == SettingsView::ConnectNetwork) {
        s_settingsView = SettingsView::Connect;
      } else if (s_settingsView == SettingsView::Connect) {
        s_settingsView = SettingsView::Network;
      } else {
        s_settingsView = SettingsView::Root;
      }
      s_networkStatusOverlayOpen = false;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
    }
  }
#endif
}

void OledDisplayManager::HandleLeftButtonLongPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  requestRefresh();
#endif
}

void OledDisplayManager::HandleMiddleButtonPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  // UpdatePrompt — Middle = "Never" (decision 1)
  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::UpdatePrompt) {
    OpenShock::OtaUpdateManager::SetUserUpdateDecision(1);
    OpenShock::OtaUpdateManager::GetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
    s_settingsView = s_prevSettingsViewBeforePrompt;
    s_currentPage.store(s_prevPageBeforePrompt, std::memory_order_relaxed);
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  // On the main page the middle button cycles between shockers.
  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    ensureActiveMainShockerSelected();
    const auto list = buildMainShockerList();
    if (list.empty()) {
      requestRefresh();
      return;
    }

    MainShockerRef activeRef {};
    (void)getActiveShockerRef(activeRef);

    int currentPos = -1;
    for (uint8_t i = 0; i < static_cast<uint8_t>(list.size()); ++i) {
      if (list[i].isOnline == activeRef.isOnline && list[i].index == activeRef.index) {
        currentPos = i;
        break;
      }
    }

    const uint8_t nextPos = static_cast<uint8_t>((currentPos < 0) ? 0 : ((currentPos + 1) % static_cast<int>(list.size())));
    setActiveShockerRef(list[nextPos]);
    const uint8_t fallback = std::min<uint8_t>(OpenShock::RotaryEncoderManager::GetMaxIntensityLimit(), 99);
    const uint8_t nextIntensity = readStoredIntensityForRef(list[nextPos], fallback);
    applyActiveShockerIntensity(nextIntensity);

    // Stop any active command when switching shockers.
    if (s_mainShockActive || s_mainVibrateActive) {
      stopMainShockerCommand();
      s_mainShockActive = false;
      s_mainVibrateActive = false;
    }

    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::ShockerNameEdit) {
    // Confirm name edit
    if (s_passwordLength > 0) {
      if (s_selectedShockerSource == ShockerSelectionSource::Online) {
        const auto onlineShockers = getOnlineShockersSnapshot();
        if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
          OpenShock::GatewayConnectionManager::SetOnlineShockerDisplayName(onlineShockers[s_selectedOnlineShockerIndex].id, s_passwordInput);
        }
      } else {
        std::memset(s_shockers[s_selectedShockerIndex].name, 0, sizeof(s_shockers[s_selectedShockerIndex].name));
        std::strncpy(s_shockers[s_selectedShockerIndex].name, s_passwordInput, sizeof(s_shockers[s_selectedShockerIndex].name) - 1);
        saveShockerPrefs();
      }
    }
    s_settingsView = SettingsView::ShockerDetail;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::UpdateRepoEdit) {
    if (s_passwordLength > 0) {
      strncpy(s_otaRepoSlug, s_passwordInput, sizeof(s_otaRepoSlug) - 1);
      s_otaRepoSlug[sizeof(s_otaRepoSlug) - 1] = '\0';
      OpenShock::OtaUpdateManager::SetOtaRepoSlug(s_otaRepoSlug);
      showInfoPopup("GitHub Source saved");
    } else {
      showInfoPopup("URL cannot be empty");
    }
    s_settingsView = SettingsView::Update;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::ConnectPassword) {
    if (s_selectedConnectSsid[0] == '\0') {
      showInfoPopup("Invalid network");
    } else if (s_passwordConnectPending) {
      showInfoPopup("Connecting...");
    } else {
      const bool ok = OpenShock::WiFiManager::Save(s_selectedConnectSsid, s_passwordInput, true, s_selectedConnectAuthMode);
      if (ok) {
        s_passwordConnectPending = true;
        s_pendingConnectFromPasswordEntry = true;
        s_passwordConnectStartedAt = OpenShock::millis();
        showInfoPopup("Connecting...");
      } else {
        showInfoPopup("Failed to connect");
      }
    }

    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::AccountLink) {
    if (s_accountLinkPending) {
      showInfoPopup("Linking account...");
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_accountCodeLength != kAccountCodeLength) {
      showInfoPopup("Enter 6-digit code");
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    s_accountLinkPending = true;
    showInfoPopup("Linking account...");
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();

    auto* params = new (std::nothrow) AccountLinkTaskParams {};
    if (params == nullptr) {
      s_accountLinkPending = false;
      showInfoPopup("Account link failed");
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    std::memcpy(params->code, s_accountCodeInput, sizeof(params->code));

    if (OpenShock::TaskUtils::TaskCreateExpensive(accountLinkTask, "oled_acc_link", 8192, params, 1, &s_accountLinkTask) != pdPASS) {
      delete params;
      s_accountLinkPending = false;
      showInfoPopup("Account link failed");
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && isHelpButtonVisibleForCurrentView()) {
    showInfoPopup(getCurrentHelpText());
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  requestRefresh();
#endif
}

void OledDisplayManager::HandleMiddleButtonLongPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_inputLocked = !s_inputLocked;
    s_lockFlashUntilMs = 0;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  requestRefresh();
#endif
}

void OledDisplayManager::HandleRightButtonPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState == PowerUiState::ConfirmPowerOff) {
    markUserActivity();
    beginPowerOffSequence();
    return;
  }

  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  // UpdatePrompt — Right = "Yes" (decision 2)
  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::UpdatePrompt) {
    s_checkInProgress = true;  // Track so failure (status=5) shows a bubble regardless of how update was triggered
    OpenShock::OtaUpdateManager::SetUserUpdateDecision(2);
    OpenShock::OtaUpdateManager::GetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
    s_settingsView = s_prevSettingsViewBeforePrompt;
    s_currentPage.store(s_prevPageBeforePrompt, std::memory_order_relaxed);
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings) {
    if (s_settingsView == SettingsView::Root) {
      if (s_settingsSelection == 0) {
        s_settingsView = SettingsView::ShockerList;
        s_shockerListSelection = 0;
        s_shockerListFirstVisible = 0;
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_settingsSelection == 1) {
        s_settingsView = SettingsView::Network;
        s_networkSelection = 0;
        s_networkFirstVisible = 0;
        s_networkStatusOverlayOpen = false;

        const wifi_mode_t mode = WiFi.getMode();
        s_networkWifiEnabled = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
        s_networkAccessPointEnabled = OpenShock::CaptivePortal::IsApEnabled();

        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_settingsSelection == 2) {
        s_settingsView = SettingsView::AccountMenu;
        s_accountMenuSelection = 0;
        s_accountMenuFirstVisible = 0;
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_settingsSelection == 3) {
        s_settingsView = SettingsView::System;
        s_systemSelection = 0;
        s_systemFirstVisible = 0;
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_settingsSelection == 4) {
        // Load OTA settings fresh each time we open the Update menu.
        OpenShock::OtaUpdateManager::GetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
        OpenShock::OtaUpdateManager::GetOtaRepoSlug(s_otaRepoSlug, sizeof(s_otaRepoSlug));
        s_settingsView = SettingsView::Update;
        s_updateSelection = 0;
        s_updateFirstVisible = 0;
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_settingsSelection == 5) {
        s_settingsView = SettingsView::About;
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }
      return;
    }

    if (s_settingsView == SettingsView::AccountMenu) {
      if (s_accountMenuSelection == 0) {
        s_settingsView = SettingsView::AccountLink;
        resetAccountLinkInputState();
      } else {
        OpenShock::Config::ClearBackendAuthToken();
        s_accountMenuSelection = 0;
        showInfoPopup("Token deleted");
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::Update) {
      if (s_updateSelection == 0) {
        // Check Now
        if (!OpenShock::WiFiManager::IsConnected()) {
          showInfoPopup("No network!");
        } else {
          showInfoPopup("Checking...");
          s_infoPopupHideAt = OpenShock::millis() + 60000;  // Stay up until result arrives
          s_checkInProgress = true;
          OpenShock::OtaUpdateManager::TriggerManualCheck();
        }
      } else if (s_updateSelection == 1) {
        // Auto Update toggle
        s_otaAutoUpdate = !s_otaAutoUpdate;
        OpenShock::OtaUpdateManager::SetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
        showInfoPopup(s_otaAutoUpdate ? "Auto update: ON" : "Auto update: OFF");
      } else if (s_updateSelection == 2) {
        // Toggle based on the composite displayed state (promptUpdates && !neverPrompt).
        // If "Never" was pressed at a prompt, neverPrompt=true makes the box appear unchecked
        // even though promptUpdates is still true — so toggling must look at the display state,
        // not just s_otaPromptUpdates, to avoid a double-click to re-enable.
        const bool currentlyEnabled = s_otaPromptUpdates && !s_otaNeverPrompt;
        s_otaPromptUpdates = !currentlyEnabled;
        s_otaNeverPrompt = false;  // always clear neverPrompt so re-enabling sticks
        OpenShock::OtaUpdateManager::SetOtaUpdateSettings(s_otaAutoUpdate, s_otaPromptUpdates, s_otaNeverPrompt);
        showInfoPopup(s_otaPromptUpdates ? "Prompts: ON" : "Prompts: OFF");
      } else if (s_updateSelection == 3) {
        // GitHub Source — open text editor pre-loaded with current slug
        std::memset(s_passwordInput, 0, sizeof(s_passwordInput));
        s_passwordLength = static_cast<uint8_t>(std::min<size_t>(strlen(s_otaRepoSlug), sizeof(s_passwordInput) - 1));
        std::memcpy(s_passwordInput, s_otaRepoSlug, s_passwordLength);
        // Start the picker at a sensible character
        s_passwordCharSelection = static_cast<uint8_t>('a' - 31);
        s_settingsView = SettingsView::UpdateRepoEdit;
      } else if (s_updateSelection == 4) {
        // Version Info
        char versionMsg[96];
        const char* latestVer = OpenShock::OtaUpdateManager::GetCachedLatestVersion();
        if (latestVer != nullptr && latestVer[0] != '\0') {
          std::snprintf(versionMsg, sizeof(versionMsg), "Installed: v" OPENSHOCK_FW_VERSION "\nLatest: v%s", latestVer);
        } else {
          std::snprintf(versionMsg, sizeof(versionMsg), "Installed: v" OPENSHOCK_FW_VERSION "\nLatest: unknown");
        }
        showInfoPopup(versionMsg);
        s_infoPopupHideAt = OpenShock::millis() + 5000;
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    // UpdatePrompt — encoder button does nothing, use Left/Middle/Right buttons instead
    if (s_settingsView == SettingsView::UpdatePrompt) {
      return;
    }

    // UpdateRepoEdit — encoder press appends the current character
    if (s_settingsView == SettingsView::UpdateRepoEdit) {
      const char c = passwordCharacterForSelection(s_passwordCharSelection);
      if (c == '\b') {
        if (s_passwordLength > 0) {
          s_passwordInput[--s_passwordLength] = '\0';
        }
      } else if (s_passwordLength < static_cast<uint8_t>(sizeof(s_passwordInput) - 1)) {
        s_passwordInput[s_passwordLength++] = c;
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::System) {
      if (s_systemSelection == 0) {
        s_originalSystemValue = s_screenSleepSeconds;
        s_pendingSystemValue = s_screenSleepSeconds;
        s_settingsView = SettingsView::SystemScreenSleepEdit;
      } else if (s_systemSelection == 1) {
        s_screenSaverEnabled = !s_screenSaverEnabled;
        saveNetworkSettingsPreferenceState();
        showInfoPopup(s_screenSaverEnabled ? "Screen saver on" : "Screen saver off");
      } else if (s_systemSelection == 2) {
        s_originalSystemValue = s_deviceSleepMinutes;
        s_pendingSystemValue = s_deviceSleepMinutes;
        s_settingsView = SettingsView::SystemDeviceSleepEdit;
      } else if (s_systemSelection == 3) {
        s_batteryIconEnabled = !s_batteryIconEnabled;
        saveNetworkSettingsPreferenceState();
        showInfoPopup(s_batteryIconEnabled ? "Battery icon on" : "Battery icon off");
      } else if (s_systemSelection == 4) {
        s_batteryPercentEnabled = !s_batteryPercentEnabled;
        saveNetworkSettingsPreferenceState();
        showInfoPopup(s_batteryPercentEnabled ? "Battery level on" : "Battery level off");
      } else if (s_systemSelection == 5) {
        s_settingsView = SettingsView::BatteryCalibrationConfirmClear;
      }

      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::BatteryCalibrationConfirmClear) {
      s_settingsView = SettingsView::BatteryCalibrationConfirmCharged;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::BatteryCalibrationConfirmCharged) {
      s_settingsView = SettingsView::System;
      s_batteryCalibrationPending = true;
      showInfoPopup("Calibrating...");
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();

      if (OpenShock::TaskUtils::TaskCreateExpensive(batteryCalibrationTask, "oled_batt_cal", 4096, nullptr, 1, &s_batteryCalibrationTask) != pdPASS) {
        s_batteryCalibrationPending = false;
        showInfoPopup("Calibration failed");
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
      }
      return;
    }

    if (s_settingsView == SettingsView::SystemScreenSleepEdit) {
      s_screenSleepSeconds = s_pendingSystemValue;
      saveNetworkSettingsPreferenceState();
      markUserActivity();
      s_settingsView = SettingsView::System;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::SystemDeviceSleepEdit) {
      s_deviceSleepMinutes = s_pendingSystemValue;
      saveNetworkSettingsPreferenceState();
      s_settingsView = SettingsView::System;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ShockerList) {
      const auto onlineShockers = getOnlineShockersSnapshot();

      if (s_shockerListSelection == 0) {
        const bool nextState = !s_shockerKeepAliveEnabled;
        if (OpenShock::CommandHandler::SetKeepAliveEnabled(nextState)) {
          s_shockerKeepAliveEnabled = nextState;
          showInfoPopup(nextState ? "Keep Alive enabled" : "Keep Alive disabled");
        } else {
          showInfoPopup("Failed to set keepalive");
        }
      } else if (s_shockerListSelection == 1) {
        // +Add Shocker
        if (s_shockerCount < kMaxShockers) {
          const uint8_t idx = s_shockerCount;
          std::memset(&s_shockers[idx], 0, sizeof(s_shockers[idx]));
          std::snprintf(s_shockers[idx].name, sizeof(s_shockers[idx].name), "Shocker%u", static_cast<unsigned>(idx + 1));
          s_shockers[idx].protocol = static_cast<uint8_t>(OpenShock::ShockerModelType::CaiXianlin);
          s_shockers[idx].limit = 99;
          s_shockers[idx].rfId = findNextLocalRfId();
          ++s_shockerCount;
          saveShockerPrefs();
          s_shockerListSelection = static_cast<uint8_t>(kShockerListStaticItemCount + s_shockerCount - 1);  // scroll to the new entry
          const uint8_t totalItems = getTotalShockerListItemCount(onlineShockers);
          const uint8_t maxFV = (totalItems > kSettingsVisibleCount) ? (totalItems - kSettingsVisibleCount) : 0;
          s_shockerListFirstVisible = std::min(s_shockerListSelection, maxFV);
        } else {
          showInfoPopup("Max shockers reached");
        }
      } else {
        const uint8_t selected = static_cast<uint8_t>(s_shockerListSelection - kShockerListStaticItemCount);
        if (selected < s_shockerCount) {
          s_selectedShockerSource = ShockerSelectionSource::Local;
          s_selectedShockerIndex = selected;
        } else {
          const uint8_t onlineIndex = static_cast<uint8_t>(selected - s_shockerCount);
          if (onlineIndex >= onlineShockers.size()) {
            s_forceRedraw.store(true, std::memory_order_relaxed);
            requestRefresh();
            return;
          }
          s_selectedShockerSource = ShockerSelectionSource::Online;
          s_selectedOnlineShockerIndex = onlineIndex;
        }

        s_shockerDetailSelection = 0;
        s_shockerDetailFirstVisible = 0;
        s_settingsView = SettingsView::ShockerDetail;
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ShockerDetail) {
      if (s_selectedShockerSource == ShockerSelectionSource::Online) {
        if (s_shockerDetailSelection == 0) {
          const auto onlineShockers = getOnlineShockersSnapshot();
          if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
            std::memset(s_shockerNameBackup, 0, sizeof(s_shockerNameBackup));
            std::strncpy(s_shockerNameBackup, onlineShockers[s_selectedOnlineShockerIndex].displayName.c_str(), sizeof(s_shockerNameBackup) - 1);
            s_passwordLength = static_cast<uint8_t>(std::min<std::size_t>(onlineShockers[s_selectedOnlineShockerIndex].displayName.size(), sizeof(s_passwordInput) - 1));
            std::memset(s_passwordInput, 0, sizeof(s_passwordInput));
            std::strncpy(s_passwordInput, onlineShockers[s_selectedOnlineShockerIndex].displayName.c_str(), sizeof(s_passwordInput) - 1);
            s_passwordCharSelection = static_cast<uint8_t>('a' - 31);
            s_settingsView = SettingsView::ShockerNameEdit;
          }
        } else if (s_shockerDetailSelection == 2) {
          const auto onlineShockers = getOnlineShockersSnapshot();
          if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
            s_originalLimit = std::min<uint8_t>(onlineShockers[s_selectedOnlineShockerIndex].limit, 99);
            s_pendingLimit = s_originalLimit;
            s_settingsView = SettingsView::ShockerLimitEdit;
          }
        } else if (s_shockerDetailSelection == 3) {
          const auto onlineShockers = getOnlineShockersSnapshot();
          if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
            const auto& shocker = onlineShockers[s_selectedOnlineShockerIndex];
            if (shocker.disabled) {
              showInfoPopup("Shocker disabled");
            } else {
              OpenShock::CommandHandler::HandleCommand(
                shocker.model,
                shocker.mappedRfId,
                OpenShock::ShockerCommandType::Sound,
                std::min<uint8_t>(50, shocker.limit),
                1000
              );
              showInfoPopup("Testing...");
            }
          }
        } else if (s_shockerDetailSelection == 4) {
          const auto onlineShockers = getOnlineShockersSnapshot();
          if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
            const auto& shocker = onlineShockers[s_selectedOnlineShockerIndex];
            if (OpenShock::GatewayConnectionManager::IsConnected()) {
              OpenShock::GatewayConnectionManager::SetOnlineShockerDisabled(shocker.id, !shocker.disabled);
              showInfoPopup(shocker.disabled ? "Shocker enabled" : "Shocker disabled");
            } else {
              OpenShock::GatewayConnectionManager::RemoveOnlineShocker(shocker.id);
              s_settingsView = SettingsView::ShockerList;
              const uint8_t totalItems = getTotalShockerListItemCount(getOnlineShockersSnapshot());
              const uint8_t maxSelection = totalItems > 0 ? static_cast<uint8_t>(totalItems - 1) : 0;
              s_shockerListSelection = std::min(s_shockerListSelection, maxSelection);
              const uint8_t maxFV = (totalItems > kSettingsVisibleCount) ? (totalItems - kSettingsVisibleCount) : 0;
              s_shockerListFirstVisible = std::min(s_shockerListFirstVisible, maxFV);
              showInfoPopup("Shocker deleted");
            }
          }
        }

        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      if (s_shockerDetailSelection == 0) {
        // Edit name — load current name into picker state
        std::memcpy(s_shockerNameBackup, s_shockers[s_selectedShockerIndex].name, sizeof(s_shockerNameBackup));
        s_passwordLength = static_cast<uint8_t>(std::strlen(s_shockers[s_selectedShockerIndex].name));
        std::memset(s_passwordInput, 0, sizeof(s_passwordInput));
        std::strncpy(s_passwordInput, s_shockers[s_selectedShockerIndex].name, sizeof(s_passwordInput) - 1);
        s_passwordCharSelection = static_cast<uint8_t>('a' - 31);
        s_settingsView = SettingsView::ShockerNameEdit;
      } else if (s_shockerDetailSelection == 1) {
        // Edit protocol popup
        const OpenShock::ShockerModelType currentModel = shockerModelForStoredValue(s_shockers[s_selectedShockerIndex].protocol);
        s_originalProtocol = findProtocolOptionIndex(currentModel);
        s_pendingProtocol = s_originalProtocol;
        s_settingsView = SettingsView::ShockerProtocolEdit;
      } else if (s_shockerDetailSelection == 2) {
        // Edit limit popup
        s_originalLimit = s_shockers[s_selectedShockerIndex].limit;
        s_pendingLimit = s_originalLimit;
        s_settingsView = SettingsView::ShockerLimitEdit;
      } else if (s_shockerDetailSelection == 3) {
        // Test: send beep command for 1s
        OpenShock::CommandHandler::HandleCommand(
          shockerModelForStoredValue(s_shockers[s_selectedShockerIndex].protocol),
          s_shockers[s_selectedShockerIndex].rfId,
          OpenShock::ShockerCommandType::Sound,
          std::min<uint8_t>(50, s_shockers[s_selectedShockerIndex].limit),
          1000
        );
        showInfoPopup("Testing...");
      } else if (s_shockerDetailSelection == 4) {
        // Delete this shocker profile
        for (uint8_t i = s_selectedShockerIndex; i + 1 < s_shockerCount; ++i) {
          s_shockers[i] = s_shockers[i + 1];
        }

        if (s_shockerCount > 0) {
          --s_shockerCount;
        }

        if (s_shockerCount < kMaxShockers) {
          std::memset(&s_shockers[s_shockerCount], 0, sizeof(s_shockers[s_shockerCount]));
        }

        s_shockerListSelection = static_cast<uint8_t>(kShockerListStaticItemCount + std::min<uint8_t>(s_selectedShockerIndex, (s_shockerCount > 0) ? static_cast<uint8_t>(s_shockerCount - 1) : 0));
        const uint8_t totalItems = getTotalShockerListItemCount(getOnlineShockersSnapshot());
        if (totalItems > 0) {
          const uint8_t maxSelection = static_cast<uint8_t>(totalItems - 1);
          s_shockerListSelection = std::min<uint8_t>(s_shockerListSelection, maxSelection);
        } else {
          s_shockerListSelection = 0;
        }
        const uint8_t maxFV = (totalItems > kSettingsVisibleCount) ? (totalItems - kSettingsVisibleCount) : 0;
        s_shockerListFirstVisible = std::min(s_shockerListSelection, maxFV);
        saveShockerPrefs();
        s_settingsView = SettingsView::ShockerList;
        showInfoPopup("Shocker deleted");
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ShockerLimitEdit) {
      // Confirm new limit
      if (s_selectedShockerSource == ShockerSelectionSource::Online) {
        const auto onlineShockers = getOnlineShockersSnapshot();
        if (s_selectedOnlineShockerIndex < onlineShockers.size()) {
          OpenShock::GatewayConnectionManager::SetOnlineShockerLimit(onlineShockers[s_selectedOnlineShockerIndex].id, std::min<uint8_t>(s_pendingLimit, 99));
          if (s_activeShockerIsOnline && s_activeShockerIdx == s_selectedOnlineShockerIndex) {
            applyActiveShockerIntensity(getCurrentActiveIntensity());
          }
        }
      } else {
        s_shockers[s_selectedShockerIndex].limit = s_pendingLimit;
        saveShockerPrefs();
        if (!s_activeShockerIsOnline && s_activeShockerIdx == s_selectedShockerIndex) {
          applyActiveShockerIntensity(getCurrentActiveIntensity());
        }
      }
      s_settingsView = SettingsView::ShockerDetail;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ShockerProtocolEdit) {
      // Confirm protocol selection
      s_shockers[s_selectedShockerIndex].protocol = static_cast<uint8_t>(kProtocolOptions[s_pendingProtocol]);
      saveShockerPrefs();
      s_settingsView = SettingsView::ShockerDetail;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ShockerNameEdit) {
      // Append character from picker (same as WiFi password)
      const char selected = passwordCharacterForSelection(s_passwordCharSelection);
      if (selected == '\b') {
        if (s_passwordLength > 0) {
          --s_passwordLength;
          s_passwordInput[s_passwordLength] = '\0';
        }
      } else if (s_passwordLength < (sizeof(s_passwordInput) - 1)) {
        s_passwordInput[s_passwordLength++] = selected;
        s_passwordInput[s_passwordLength] = '\0';
      }
      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::Network) {
      if (s_networkStatusOverlayOpen) {
        return;
      }

      if (s_networkSelection == 0) {
        s_networkWifiEnabled = !s_networkWifiEnabled;
        OpenShock::WiFiManager::SetStaEnabled(s_networkWifiEnabled);
        if (s_networkWifiEnabled) {
          s_lastConnectScanAt = OpenShock::millis();
        }
        saveNetworkSettingsPreferenceState();
      } else if (s_networkSelection == 1) {
        s_networkAccessPointEnabled = !s_networkAccessPointEnabled;
        if (s_networkAccessPointEnabled) {
          // Enabling AP also enables the captive portal so it's reachable at 4.3.2.1
          s_networkCaptivePortalEnabled = true;
          OpenShock::CaptivePortal::SetEnabled(true);
          OpenShock::CaptivePortal::SetAlwaysEnabled(true);
        }
        OpenShock::CaptivePortal::SetApEnabled(s_networkAccessPointEnabled);
        saveNetworkSettingsPreferenceState();
      } else if (s_networkSelection == 2) {
        s_networkCaptivePortalEnabled = !s_networkCaptivePortalEnabled;
        OpenShock::CaptivePortal::SetEnabled(s_networkCaptivePortalEnabled);
        OpenShock::CaptivePortal::SetAlwaysEnabled(s_networkCaptivePortalEnabled);
        saveNetworkSettingsPreferenceState();
      } else if (s_networkSelection == 4) {
        s_networkStatusOverlayOpen = true;
      } else if (s_networkSelection == 3) {
        s_settingsView = SettingsView::Connect;
        s_connectSelection = 0;
        s_connectFirstVisible = 0;
        if (!s_networkWifiEnabled) {
          s_networkWifiEnabled = true;
          OpenShock::WiFiManager::SetStaEnabled(true);
        }
        (void)OpenShock::WiFiScanManager::StartScan();
        s_lastConnectScanAt = OpenShock::millis();
      }

      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::Connect) {
      auto connectNetworks = OpenShock::WiFiManager::GetDiscoveredWiFiNetworks();
      std::sort(connectNetworks.begin(), connectNetworks.end(), [](const OpenShock::WiFiNetwork& a, const OpenShock::WiFiNetwork& b) { return a.rssi > b.rssi; });
      if (connectNetworks.empty()) {
        showInfoPopup("No networks found");
        s_forceRedraw.store(true, std::memory_order_relaxed);
        requestRefresh();
        return;
      }

      const int idx = std::clamp<int>(s_connectSelection, 0, static_cast<int>(connectNetworks.size() - 1));
      const OpenShock::WiFiNetwork& net = connectNetworks[idx];
      std::memset(s_selectedConnectSsid, 0, sizeof(s_selectedConnectSsid));
      std::strncpy(s_selectedConnectSsid, net.ssid, sizeof(s_selectedConnectSsid) - 1);
      s_selectedConnectCredentialsId = net.credentialsID;
      s_selectedConnectAuthMode = net.authMode;
      s_connectNetworkSelection = 0;
      s_connectNetworkFirstVisible = 0;
      s_settingsView = SettingsView::ConnectNetwork;
      s_forceRedraw.store(true, std::memory_order_relaxed);
      drawSettingsPage();
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ConnectNetwork) {
      if (s_connectNetworkSelection == 0) {
        if (s_selectedConnectSsid[0] == '\0') {
          showInfoPopup("Invalid network");
        } else if (s_selectedConnectCredentialsId != 0) {
          const bool ok = OpenShock::WiFiManager::Connect(s_selectedConnectSsid);
          if (ok) {
            s_passwordConnectPending = true;
            s_pendingConnectFromPasswordEntry = false;
            s_passwordConnectStartedAt = OpenShock::millis();
          }
          showInfoPopup(ok ? "Connecting..." : "Failed to connect");
        } else if (s_selectedConnectAuthMode == WIFI_AUTH_OPEN) {
          const bool ok = OpenShock::WiFiManager::Save(s_selectedConnectSsid, "", true, s_selectedConnectAuthMode);
          showInfoPopup(ok ? "Connecting..." : "Failed to connect");
        } else {
          resetPasswordInputState();
          s_settingsView = SettingsView::ConnectPassword;
        }
      } else if (s_connectNetworkSelection == 1) {
        if (s_selectedConnectSsid[0] == '\0') {
          showInfoPopup("Invalid network");
        } else {
          std::memset(s_defaultNetworkSsid, 0, sizeof(s_defaultNetworkSsid));
          std::strncpy(s_defaultNetworkSsid, s_selectedConnectSsid, sizeof(s_defaultNetworkSsid) - 1);
          saveNetworkSettingsPreferenceState();
          if (s_selectedConnectCredentialsId != 0) {
            (void)OpenShock::WiFiManager::Connect(s_selectedConnectSsid);
          }
          showInfoPopup("Preferred saved");
        }
      } else if (s_connectNetworkSelection == 2) {
        if (s_selectedConnectSsid[0] == '\0') {
          showInfoPopup("Invalid network");
        } else {
          const bool ok = OpenShock::WiFiManager::Forget(s_selectedConnectSsid);
          if (ok) {
            s_selectedConnectCredentialsId = 0;
          }
          if (std::strncmp(s_defaultNetworkSsid, s_selectedConnectSsid, sizeof(s_defaultNetworkSsid)) == 0) {
            std::memset(s_defaultNetworkSsid, 0, sizeof(s_defaultNetworkSsid));
            saveNetworkSettingsPreferenceState();
          }
          showInfoPopup(ok ? "Forgot network" : "Forget failed");
        }
      }

      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::ConnectPassword) {
      if (s_passwordConnectPending) {
        showInfoPopup("Connecting...");
      } else {
        const char selected = passwordCharacterForSelection(s_passwordCharSelection);
        if (selected == '\b') {
          if (s_passwordLength > 0) {
            --s_passwordLength;
            s_passwordInput[s_passwordLength] = '\0';
          }
        } else if (s_passwordLength < (sizeof(s_passwordInput) - 1)) {
          s_passwordInput[s_passwordLength++] = selected;
          s_passwordInput[s_passwordLength] = '\0';
        }
      }

      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }

    if (s_settingsView == SettingsView::AccountLink) {
      if (s_accountLinkPending) {
        showInfoPopup("Linking account...");
      } else if (s_accountCodeLength < kAccountCodeLength) {
        s_accountCodeInput[s_accountCodeLength++] = static_cast<char>('0' + s_accountDigitSelection);
        s_accountCodeInput[s_accountCodeLength] = '\0';
      }

      s_forceRedraw.store(true, std::memory_order_relaxed);
      requestRefresh();
      return;
    }
  }
#endif
}

void OledDisplayManager::HandleRightButtonLongPressed()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::ConnectPassword && !s_passwordConnectPending) {
    if (s_passwordLength > 0) {
      --s_passwordLength;
      s_passwordInput[s_passwordLength] = '\0';
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::ShockerNameEdit) {
    if (s_passwordLength > 0) {
      --s_passwordLength;
      s_passwordInput[s_passwordLength] = '\0';
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageSettings && s_settingsView == SettingsView::AccountLink && !s_accountLinkPending) {
    if (s_accountCodeLength > 0) {
      --s_accountCodeLength;
      s_accountCodeInput[s_accountCodeLength] = '\0';
      s_forceRedraw.store(true, std::memory_order_relaxed);
    }
  }

  requestRefresh();
#endif
}

void OledDisplayManager::HandleLeftButtonDown()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    ensureActiveMainShockerSelected();
    const uint8_t intensity = getCurrentActiveIntensity();
    s_mainShockActive = true;
    s_mainVibrateActive = false;
    s_mainCommandAutoStopAtMs = 0;
    sendMainShockerCommand(OpenShock::ShockerCommandType::Shock, intensity);
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  }
#endif
}

void OledDisplayManager::HandleLeftButtonReleased()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain && s_mainShockActive) {
    s_mainShockActive = false;
    s_mainCommandAutoStopAtMs = 0;
    stopMainShockerCommand();
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  }
#endif
}

void OledDisplayManager::HandleRightButtonDown()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    s_lockFlashUntilMs = OpenShock::millis() + 250;
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    ensureActiveMainShockerSelected();
    const uint8_t intensity = getCurrentActiveIntensity();
    s_mainVibrateActive = true;
    s_mainShockActive = false;
    s_mainCommandAutoStopAtMs = 0;
    sendMainShockerCommand(OpenShock::ShockerCommandType::Vibrate, intensity);
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  }
#endif
}

void OledDisplayManager::HandleRightButtonReleased()
{
#if OPENSHOCK_OLED_ENABLED
  if (s_powerUiState != PowerUiState::Ready) {
    return;
  }

  if (wakeOnlyInputIfSleeping()) {
    return;
  }

  if (s_inputLocked && s_currentPage.load(std::memory_order_relaxed) == kPageMain) {
    return;
  }

  if (s_currentPage.load(std::memory_order_relaxed) == kPageMain && s_mainVibrateActive) {
    s_mainVibrateActive = false;
    s_mainCommandAutoStopAtMs = 0;
    stopMainShockerCommand();
    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  }
#endif
}

void OledDisplayManager::NotifyGatewayShockerCommand(uint16_t mappedRfId, uint8_t intensity, OpenShock::ShockerCommandType type, uint16_t durationMs)
{
#if OPENSHOCK_OLED_ENABLED
  auto applyGatewayUiState = [&](bool targetFound) {
    if (!targetFound) {
      return;
    }

    if (type == OpenShock::ShockerCommandType::Shock) {
      markUserActivity();
      s_mainShockActive = true;
      s_mainVibrateActive = false;
      s_mainCommandAutoStopAtMs = OpenShock::millis() + static_cast<int64_t>(std::max<uint16_t>(durationMs, 200));
    } else if (type == OpenShock::ShockerCommandType::Vibrate) {
      markUserActivity();
      s_mainVibrateActive = true;
      s_mainShockActive = false;
      s_mainCommandAutoStopAtMs = OpenShock::millis() + static_cast<int64_t>(std::max<uint16_t>(durationMs, 200));
    } else if (type == OpenShock::ShockerCommandType::Stop) {
      s_mainShockActive = false;
      s_mainVibrateActive = false;
      s_mainCommandAutoStopAtMs = 0;
    }

    s_forceRedraw.store(true, std::memory_order_relaxed);
    requestRefresh();
  };

  // Switch the main-page active shocker to the one that just received a gateway command.
  for (uint8_t i = 0; i < s_shockerCount; ++i) {
    if (s_shockers[i].rfId == mappedRfId) {
      setActiveShockerRef({false, i});
      applyActiveShockerIntensity(intensity);
      applyGatewayUiState(true);
      return;
    }
  }
  const auto online = getOnlineShockersSnapshot();
  for (uint8_t i = 0; i < static_cast<uint8_t>(online.size()); ++i) {
    if (online[i].mappedRfId == mappedRfId && !online[i].disabled) {
      setActiveShockerRef({true, i});
      applyActiveShockerIntensity(intensity);
      applyGatewayUiState(true);
      return;
    }
  }
#else
  (void)mappedRfId;
  (void)intensity;
  (void)type;
  (void)durationMs;
#endif
}
