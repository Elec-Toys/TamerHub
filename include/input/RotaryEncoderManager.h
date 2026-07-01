#pragma once

#include <hal/gpio_types.h>

#include <cstdint>

namespace OpenShock::RotaryEncoderManager {
  struct RotationEvent {
    int8_t delta;
    int64_t timestampMs;
    uint8_t maxIntensity;
  };

  struct ButtonEvent {
    bool pressed;
    int64_t timestampMs;
  };

  [[nodiscard]] bool Init(gpio_num_t pinA = GPIO_NUM_3, gpio_num_t pinB = GPIO_NUM_2, gpio_num_t buttonPin = GPIO_NUM_1, gpio_num_t middleButtonPin = GPIO_NUM_0, gpio_num_t leftButtonPin = GPIO_NUM_21, gpio_num_t rightButtonPin = GPIO_NUM_20);

  [[nodiscard]] bool TryPopRotationEvent(RotationEvent& out);
  [[nodiscard]] bool TryPopButtonEvent(ButtonEvent& out);
  [[nodiscard]] uint8_t GetMaxIntensityLimit();
  void SetMaxIntensityLimit(uint8_t limit);
}  // namespace OpenShock::RotaryEncoderManager
