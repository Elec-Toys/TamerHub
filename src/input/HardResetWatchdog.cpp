#include "input/HardResetWatchdog.h"

const char* const TAG = "HardResetWatchdog";

#include "Core.h"
#include "Logging.h"
#include "OtaUpdateManager.h"

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
  constexpr TickType_t kPollInterval = pdMS_TO_TICKS(50);
  constexpr int64_t kHoldDurationMs  = 10'000;
  constexpr uint32_t kTaskStackSize  = 2048;

  // Core 0 handles WiFi/system internals and is otherwise left mostly idle by this app - all
  // app-level work deliberately runs on core 1 instead (see TaskUtils::TaskCreateExpensive).
  // Running the watchdog here keeps it independent of anything that could deadlock or block
  // on core 1 (the exact class of bug that motivated this feature) - it keeps getting
  // scheduled as long as the chip itself is alive.
  constexpr BaseType_t kWatchdogCore      = 0;
  constexpr UBaseType_t kWatchdogPriority = 3;

  gpio_num_t s_buttonPin = GPIO_NUM_NC;
  bool s_pressedWhenHigh = true;

  void watchdogTask(void*)
  {
    int64_t pressedSinceMs = 0;

    while (true) {
      const bool level   = gpio_get_level(s_buttonPin) != 0;
      const bool pressed = s_pressedWhenHigh ? level : !level;
      const int64_t now  = OpenShock::millis();

      if (!pressed) {
        pressedSinceMs = 0;
      } else if (pressedSinceMs == 0) {
        pressedSinceMs = now;
      } else if ((now - pressedSinceMs) >= kHoldDurationMs) {
        // Never force-reboot mid-flash-write - that can brick the device with a half-written
        // firmware image. Keep waiting instead; releasing and re-holding will try again.
        if (!OpenShock::OtaUpdateManager::IsUpdateInProgress()) {
          OS_LOGE(TAG, "Button held for %lld ms, forcing hardware reset", static_cast<long long>(kHoldDurationMs));
          esp_restart();
        }
      }

      vTaskDelay(kPollInterval);
    }
  }
}  // namespace

bool OpenShock::HardResetWatchdog::Init(gpio_num_t buttonPin, bool pressedWhenHigh)
{
  if (buttonPin < 0 || !GPIO_IS_VALID_GPIO(buttonPin)) {
    OS_LOGE(TAG, "Invalid button pin %d, hard-reset watchdog disabled", static_cast<int>(buttonPin));
    return false;
  }

  s_buttonPin       = buttonPin;
  s_pressedWhenHigh = pressedWhenHigh;

  // Configured independently of RotaryEncoderManager - this task must not depend on any
  // other module's state, or its init ordering, to keep working.
  gpio_config_t ioConf {};
  ioConf.intr_type    = GPIO_INTR_DISABLE;
  ioConf.mode         = GPIO_MODE_INPUT;
  ioConf.pin_bit_mask = 1ULL << static_cast<uint32_t>(buttonPin);
  ioConf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ioConf.pull_up_en   = GPIO_PULLUP_ENABLE;
  gpio_config(&ioConf);

  TaskHandle_t task = nullptr;
  BaseType_t result;
#ifndef CONFIG_FREERTOS_UNICORE
  result = xTaskCreatePinnedToCore(watchdogTask, "hard_reset_wd", kTaskStackSize, nullptr, kWatchdogPriority, &task, kWatchdogCore);
#else
  result = xTaskCreate(watchdogTask, "hard_reset_wd", kTaskStackSize, nullptr, kWatchdogPriority, &task);
#endif

  if (result != pdPASS) {
    OS_LOGE(TAG, "Failed to create hard-reset watchdog task");
    return false;
  }

  OS_LOGI(TAG, "Hard-reset watchdog armed on GPIO %d (hold %lld ms to force reset)", static_cast<int>(buttonPin), static_cast<long long>(kHoldDurationMs));
  return true;
}
