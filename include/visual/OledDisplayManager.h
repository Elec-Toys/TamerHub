#pragma once

#include "ShockerCommandType.h"

#include <hal/gpio_types.h>
#include <cstdint>
#include <string_view>

namespace OpenShock::OledDisplayManager {
  // Must be called before Init(). Reads the encoder button; if not held at
  // power-on (charger woke the device) it shows a Charging screen, waits,
  // then blocks until the button is held for the boot threshold before returning.
  void RunChargingModeIfNeeded(gpio_num_t encoderButtonPin);

  [[nodiscard]] bool Init();
  void RequestRefresh();
  void HandleEncoderButtonPressed();
  void HandleEncoderButtonLongPressed();
  void HandleLeftButtonDown();
  void HandleLeftButtonPressed();
  void HandleLeftButtonReleased();
  void HandleLeftButtonLongPressed();
  void HandleMiddleButtonPressed();
  void HandleMiddleButtonLongPressed();
  void HandleRightButtonDown();
  void HandleRightButtonPressed();
  void HandleRightButtonReleased();
  void HandleRightButtonLongPressed();
  void NotifyGatewayShockerCommand(uint16_t mappedRfId, uint8_t intensity, OpenShock::ShockerCommandType type, uint16_t durationMs);
}  // namespace OpenShock::OledDisplayManager
