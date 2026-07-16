#pragma once

#include <driver/gpio.h>

namespace OpenShock::HardResetWatchdog {
  // Starts an independent, dedicated-core watchdog task that polls the given button pin
  // directly (bypassing RotaryEncoderManager and all other input handling/queues). Holding
  // it continuously for the configured duration force-reboots the device via esp_restart(),
  // even if every other task is deadlocked or blocked - as long as the FreeRTOS scheduler is
  // still running on that core.
  bool Init(gpio_num_t buttonPin, bool pressedWhenHigh);
}  // namespace OpenShock::HardResetWatchdog
