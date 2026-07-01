#include <freertos/FreeRTOS.h>

const char* const TAG = "main";

#include "captiveportal/Manager.h"
#include "CommandHandler.h"
#include "Common.h"
#include "config/Config.h"
#include "estop/EStopManager.h"
#include "events/Events.h"
#include "GatewayConnectionManager.h"
#include "input/RotaryEncoderManager.h"
#include "Logging.h"
#include "OtaUpdateManager.h"
#include "serial/SerialInputHandler.h"
#include "util/TaskUtils.h"
#include "visual/OledDisplayManager.h"
#include "visual/VisualStateManager.h"
#include "wifi/WiFiManager.h"
#include "wifi/WiFiScanManager.h"

#include <Arduino.h>

#include <memory>

#ifndef OPENSHOCK_ENCODER_PIN_A
#define OPENSHOCK_ENCODER_PIN_A 3
#endif

#ifndef OPENSHOCK_ENCODER_PIN_B
#define OPENSHOCK_ENCODER_PIN_B 2
#endif

#ifndef OPENSHOCK_ENCODER_BUTTON_PIN
#define OPENSHOCK_ENCODER_BUTTON_PIN 1
#endif

#ifndef OPENSHOCK_MIDDLE_BUTTON_PIN
#define OPENSHOCK_MIDDLE_BUTTON_PIN 0
#endif

#ifndef OPENSHOCK_LEFT_BUTTON_PIN
#define OPENSHOCK_LEFT_BUTTON_PIN 21
#endif

#ifndef OPENSHOCK_RIGHT_BUTTON_PIN
#define OPENSHOCK_RIGHT_BUTTON_PIN 20
#endif

#ifndef OPENSHOCK_DISPLAY_ONLY_BOOT
#define OPENSHOCK_DISPLAY_ONLY_BOOT 0
#endif

// Internal setup function, returns true if setup succeeded, false otherwise.
bool trySetup()
{
#if OPENSHOCK_DISPLAY_ONLY_BOOT
  OS_LOGW(TAG, "Main-screen-only boot enabled; skipping non-I2C subsystems");

  if (!OpenShock::OledDisplayManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize OledDisplayManager");
    return false;
  }

  if (!OpenShock::RotaryEncoderManager::Init(
        static_cast<gpio_num_t>(OPENSHOCK_ENCODER_PIN_A),
        static_cast<gpio_num_t>(OPENSHOCK_ENCODER_PIN_B),
        static_cast<gpio_num_t>(OPENSHOCK_ENCODER_BUTTON_PIN),
        static_cast<gpio_num_t>(OPENSHOCK_MIDDLE_BUTTON_PIN),
        static_cast<gpio_num_t>(OPENSHOCK_LEFT_BUTTON_PIN),
        static_cast<gpio_num_t>(OPENSHOCK_RIGHT_BUTTON_PIN))) {
    OS_LOGE(TAG, "Unable to initialize RotaryEncoderManager (encoder+buttons mode)");
    return false;
  }

  return true;
#else
  if (!OpenShock::VisualStateManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize VisualStateManager");
    return false;
  }

  if (!OpenShock::OledDisplayManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize OledDisplayManager");
    return false;
  }

  if (!OpenShock::EStopManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize EStopManager");
    return false;
  }

  if (!OpenShock::SerialInputHandler::Init()) {
    OS_LOGE(TAG, "Unable to initialize SerialInputHandler");
    return false;
  }

  if (!OpenShock::RotaryEncoderManager::Init(static_cast<gpio_num_t>(OPENSHOCK_ENCODER_PIN_A), static_cast<gpio_num_t>(OPENSHOCK_ENCODER_PIN_B), static_cast<gpio_num_t>(OPENSHOCK_ENCODER_BUTTON_PIN), static_cast<gpio_num_t>(OPENSHOCK_MIDDLE_BUTTON_PIN), static_cast<gpio_num_t>(OPENSHOCK_LEFT_BUTTON_PIN), static_cast<gpio_num_t>(OPENSHOCK_RIGHT_BUTTON_PIN))) {
    OS_LOGE(TAG, "Unable to initialize RotaryEncoderManager");
    return false;
  }

  if (!OpenShock::CommandHandler::Init()) {
    OS_LOGW(TAG, "Unable to initialize CommandHandler");
    return false;
  }

  if (!OpenShock::WiFiManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize WiFiManager");
    return false;
  }

  if (!OpenShock::GatewayConnectionManager::Init()) {
    OS_LOGE(TAG, "Unable to initialize GatewayConnectionManager");
    return false;
  }

  if (!OpenShock::CaptivePortal::Init()) {
    OS_LOGE(TAG, "Unable to initialize CaptivePortal");
    return false;
  }

  return true;
#endif
}

// OTA setup is the same as normal setup, but we invalidate the currently running app, and roll back if it fails.
void otaSetup()
{
  OS_LOGI(TAG, "Validating OTA app");

  if (!trySetup()) {
    OS_LOGE(TAG, "Unable to validate OTA app, rolling back");
    OpenShock::OtaUpdateManager::InvalidateAndRollback();
  }

  OS_LOGI(TAG, "Marking OTA app as valid");

  OpenShock::OtaUpdateManager::ValidateApp();

  OS_LOGI(TAG, "Done validating OTA app");
}

// App setup is the same as normal setup, but we restart if it fails.
void appSetup()
{
  if (!trySetup()) {
    OS_LOGI(TAG, "Restarting in 5 seconds...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    esp_restart();
  }
}

// Arduino setup function
void setup()
{
  OS_SERIAL.begin(115'200);
  OS_SERIAL.println("[BOOT] OpenShock app entered setup()");

#if ARDUINO_USB_MODE
  OS_SERIAL_USB.begin(115'200);
#endif

  // Detect charger-only power-on: if the encoder button is not held at boot
  // the charger woke the device. Show a Charging screen and wait for a long
  // press before proceeding with the full boot sequence.
  OpenShock::OledDisplayManager::RunChargingModeIfNeeded(
    static_cast<gpio_num_t>(OPENSHOCK_ENCODER_BUTTON_PIN)
  );

  OpenShock::Config::Init();

  if (!OpenShock::Events::Init()) {
    OS_PANIC(TAG, "Unable to initialize Events");
  }

  if (!OpenShock::OtaUpdateManager::Init()) {
    OS_PANIC(TAG, "Unable to initialize OTA Update Manager");
  }

  if (OpenShock::OtaUpdateManager::IsValidatingApp()) {
    otaSetup();
  } else {
    appSetup();
  }
}

void main_app(void* arg)
{
  while (true) {
    OpenShock::GatewayConnectionManager::Update();

    vTaskDelay(5);  // 5 ticks update interval
  }
}

void loop()
{
#if OPENSHOCK_DISPLAY_ONLY_BOOT
  vTaskDelete(nullptr);
#else
  // Start the main task
  if (OpenShock::TaskUtils::TaskCreateExpensive(main_app, "main_app", 8192, nullptr, 1, nullptr) != pdPASS) {  // PROFILED: 6KB stack usage
    OS_PANIC(TAG, "Failed to create main_app task");
    return;
  }

  // Kill the loop task (Arduino is stinky)
  vTaskDelete(nullptr);
#endif
}
