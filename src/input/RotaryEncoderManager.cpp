#include "input/RotaryEncoderManager.h"

const char* const TAG = "RotaryEncoder";

#include "Chipset.h"
#include "Core.h"
#include "Logging.h"
#include "util/TaskUtils.h"
#include "visual/OledDisplayManager.h"

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <algorithm>
#include <atomic>
#include <limits>

namespace {
  constexpr uint8_t kMaxLimit = 100;
  constexpr UBaseType_t kQueueSize = 32;
  constexpr uint32_t kTaskStackSize = 4096;
  constexpr TickType_t kPollInterval = pdMS_TO_TICKS(1);
  constexpr int64_t kButtonDebounceMs = 25;
  constexpr int64_t kLongPressMs = 2000;
  constexpr int64_t kEncoderLongPressMs = 1000;

  QueueHandle_t s_rotationQueue = nullptr;
  QueueHandle_t s_buttonQueue = nullptr;
  TaskHandle_t s_task = nullptr;
  gpio_num_t s_pinA = GPIO_NUM_NC;
  gpio_num_t s_pinB = GPIO_NUM_NC;
  gpio_num_t s_buttonPin = GPIO_NUM_NC;
  gpio_num_t s_middleButtonPin = GPIO_NUM_NC;
  gpio_num_t s_leftButtonPin = GPIO_NUM_NC;
  gpio_num_t s_rightButtonPin = GPIO_NUM_NC;

  std::atomic<bool> s_initialized = false;
  std::atomic<uint8_t> s_maxIntensityLimit = kMaxLimit;

  // Quadrature transition table: [prevAB << 2 | currAB] => delta.
  constexpr int8_t kTransitionLut[16] = {
    0, -1, 1, 0,
    1, 0, 0, -1,
    -1, 0, 0, 1,
    0, 1, -1, 0,
  };

  uint8_t readState()
  {
    const uint8_t a = static_cast<uint8_t>(gpio_get_level(s_pinA) ? 1 : 0);
    const uint8_t b = static_cast<uint8_t>(gpio_get_level(s_pinB) ? 1 : 0);

    return static_cast<uint8_t>((a << 1) | b);
  }

  struct ButtonState {
    bool lastLevel = true;
    int64_t lastChangeAt = 0;
    int64_t pressedAt = 0;
    bool longPressFired = false;
  };

  enum class ButtonAction : uint8_t {
    None,
    Down,
    ShortPress,
    LongPress,
    Released,
  };

  ButtonAction processButton(gpio_num_t pin, ButtonState& state, bool pressedWhenHigh, int64_t longPressMs)
  {
    const bool level = gpio_get_level(pin) != 0;
    const bool isPressed = pressedWhenHigh ? level : !level;

    // Fire long press as soon as threshold is reached while button is still held.
    if (isPressed && state.pressedAt != 0 && !state.longPressFired) {
      const int64_t now = OpenShock::millis();
      if ((now - state.pressedAt) >= longPressMs) {
        state.longPressFired = true;
        return ButtonAction::LongPress;
      }
    }

    if (level == state.lastLevel) {
      return ButtonAction::None;
    }

    const int64_t now = OpenShock::millis();
    if ((now - state.lastChangeAt) < kButtonDebounceMs) {
      return ButtonAction::None;
    }

    state.lastChangeAt = now;
    state.lastLevel = level;

    if (isPressed) {
      state.pressedAt = now;
      state.longPressFired = false;
      return ButtonAction::Down;
    }

    if (state.pressedAt == 0) {
      return ButtonAction::None;
    }

    const bool longPressAlreadyFired = state.longPressFired;
    state.pressedAt = 0;
    state.longPressFired = false;

    if (longPressAlreadyFired) {
      return ButtonAction::Released;
    }

    return ButtonAction::ShortPress;
  }

  void encoderTask(void*)
  {
    uint8_t prevState = readState();
    int8_t quarterStepAccumulator = 0;
    ButtonState encoderButtonState {};
    ButtonState middleButtonState {};
    ButtonState leftButtonState {};
    ButtonState rightButtonState {};

    while (true) {
      const uint8_t currState = readState();
      const uint8_t idx = static_cast<uint8_t>((prevState << 2) | currState);
      const int8_t delta = kTransitionLut[idx & 0x0F];

      if (delta != 0) {
        quarterStepAccumulator = static_cast<int8_t>(quarterStepAccumulator + delta);

        if (quarterStepAccumulator >= 4 || quarterStepAccumulator <= -4) {
          const int8_t logicalStep = quarterStepAccumulator > 0 ? 1 : -1;
          quarterStepAccumulator = 0;

          OpenShock::RotaryEncoderManager::RotationEvent evt {
            .delta = logicalStep,
            .timestampMs = OpenShock::millis(),
            .maxIntensity = s_maxIntensityLimit.load(std::memory_order_relaxed),
          };

          if (s_rotationQueue != nullptr) {
            (void)xQueueSend(s_rotationQueue, &evt, 0);
          }

          OpenShock::OledDisplayManager::RequestRefresh();
        }
      }

      const ButtonAction encoderAction = processButton(s_buttonPin, encoderButtonState, true, kEncoderLongPressMs);
      if (encoderAction == ButtonAction::ShortPress || encoderAction == ButtonAction::LongPress) {
        OpenShock::RotaryEncoderManager::ButtonEvent btn {
          .pressed = encoderAction == ButtonAction::ShortPress,
          .timestampMs = OpenShock::millis(),
        };

        if (s_buttonQueue != nullptr) {
          (void)xQueueSend(s_buttonQueue, &btn, 0);
        }

        if (encoderAction == ButtonAction::LongPress) {
          OpenShock::OledDisplayManager::HandleEncoderButtonLongPressed();
        } else {
          OpenShock::OledDisplayManager::HandleEncoderButtonPressed();
        }
      }

      if (s_middleButtonPin != GPIO_NUM_NC) {
        const ButtonAction middleAction = processButton(s_middleButtonPin, middleButtonState, false, kLongPressMs);
        if (middleAction == ButtonAction::LongPress) {
          OpenShock::OledDisplayManager::HandleMiddleButtonLongPressed();
        } else if (middleAction == ButtonAction::ShortPress) {
          OpenShock::OledDisplayManager::HandleMiddleButtonPressed();
        }
      }

      if (s_leftButtonPin != GPIO_NUM_NC) {
        const ButtonAction leftAction = processButton(s_leftButtonPin, leftButtonState, false, kLongPressMs);
        if (leftAction == ButtonAction::Down) {
          OpenShock::OledDisplayManager::HandleLeftButtonDown();
        } else if (leftAction == ButtonAction::LongPress) {
          OpenShock::OledDisplayManager::HandleLeftButtonLongPressed();
        } else if (leftAction == ButtonAction::ShortPress) {
          OpenShock::OledDisplayManager::HandleLeftButtonPressed();
          OpenShock::OledDisplayManager::HandleLeftButtonReleased();
        } else if (leftAction == ButtonAction::Released) {
          OpenShock::OledDisplayManager::HandleLeftButtonReleased();
        }
      }

      if (s_rightButtonPin != GPIO_NUM_NC) {
        const ButtonAction rightAction = processButton(s_rightButtonPin, rightButtonState, false, kLongPressMs);
        if (rightAction == ButtonAction::Down) {
          OpenShock::OledDisplayManager::HandleRightButtonDown();
        } else if (rightAction == ButtonAction::LongPress) {
          OpenShock::OledDisplayManager::HandleRightButtonLongPressed();
        } else if (rightAction == ButtonAction::ShortPress) {
          OpenShock::OledDisplayManager::HandleRightButtonPressed();
          OpenShock::OledDisplayManager::HandleRightButtonReleased();
        } else if (rightAction == ButtonAction::Released) {
          OpenShock::OledDisplayManager::HandleRightButtonReleased();
        }
      }

      prevState = currState;
      vTaskDelay(kPollInterval);
    }
  }

  bool isGpioValid(gpio_num_t pin)
  {
    const int pinInt = static_cast<int>(pin);
    return pinInt >= 0 && pinInt < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin);
  }

  bool isOptionalGpioValid(gpio_num_t pin)
  {
    return pin == GPIO_NUM_NC || isGpioValid(pin);
  }

}

using namespace OpenShock;

bool RotaryEncoderManager::Init(gpio_num_t pinA, gpio_num_t pinB, gpio_num_t buttonPin, gpio_num_t middleButtonPin, gpio_num_t leftButtonPin, gpio_num_t rightButtonPin)
{
  if (s_initialized.load(std::memory_order_acquire)) {
    return true;
  }

  if (!isGpioValid(pinA) || !isGpioValid(pinB) || !isGpioValid(buttonPin) || !isOptionalGpioValid(middleButtonPin) || !isOptionalGpioValid(leftButtonPin) || !isOptionalGpioValid(rightButtonPin)) {
    OS_LOGE(TAG, "Encoder GPIO pins are invalid (A=%d, B=%d, BTN=%d, MID=%d, LEFT=%d, RIGHT=%d)", static_cast<int>(pinA), static_cast<int>(pinB), static_cast<int>(buttonPin), static_cast<int>(middleButtonPin), static_cast<int>(leftButtonPin), static_cast<int>(rightButtonPin));
    return false;
  }

  const bool unsafeMainPins = !OpenShock::IsValidInputPin(static_cast<int8_t>(pinA))
      || !OpenShock::IsValidInputPin(static_cast<int8_t>(pinB))
      || !OpenShock::IsValidInputPin(static_cast<int8_t>(buttonPin));
    const bool unsafeAuxPins = (middleButtonPin != GPIO_NUM_NC && !OpenShock::IsValidInputPin(static_cast<int8_t>(middleButtonPin)))
      || (leftButtonPin != GPIO_NUM_NC && !OpenShock::IsValidInputPin(static_cast<int8_t>(leftButtonPin)))
      || (rightButtonPin != GPIO_NUM_NC && !OpenShock::IsValidInputPin(static_cast<int8_t>(rightButtonPin)));

  if (unsafeMainPins || unsafeAuxPins) {
    OS_LOGW(TAG, "Encoder pins A=%d/B=%d/BTN=%d/MID=%d/LEFT=%d/RIGHT=%d are marked as unsafe for this chip profile; continuing because pins were explicitly requested", static_cast<int>(pinA), static_cast<int>(pinB), static_cast<int>(buttonPin), static_cast<int>(middleButtonPin), static_cast<int>(leftButtonPin), static_cast<int>(rightButtonPin));
  }

  gpio_config_t ioConf {};
  ioConf.intr_type = GPIO_INTR_DISABLE;
  ioConf.mode = GPIO_MODE_INPUT;
  ioConf.pin_bit_mask = (1ULL << static_cast<uint8_t>(pinA)) | (1ULL << static_cast<uint8_t>(pinB)) | (1ULL << static_cast<uint8_t>(buttonPin));
  if (middleButtonPin != GPIO_NUM_NC) {
    ioConf.pin_bit_mask |= (1ULL << static_cast<uint8_t>(middleButtonPin));
  }
  if (leftButtonPin != GPIO_NUM_NC) {
    ioConf.pin_bit_mask |= (1ULL << static_cast<uint8_t>(leftButtonPin));
  }
  if (rightButtonPin != GPIO_NUM_NC) {
    ioConf.pin_bit_mask |= (1ULL << static_cast<uint8_t>(rightButtonPin));
  }
  ioConf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  ioConf.pull_up_en = GPIO_PULLUP_ENABLE;

  esp_err_t err = gpio_config(&ioConf);
  if (err != ESP_OK) {
    OS_LOGE(TAG, "Failed to configure encoder GPIO: %s", esp_err_to_name(err));
    return false;
  }

  s_rotationQueue = xQueueCreate(kQueueSize, sizeof(RotationEvent));
  if (s_rotationQueue == nullptr) {
    OS_LOGE(TAG, "Failed to create encoder rotation queue");
    return false;
  }

  s_buttonQueue = xQueueCreate(kQueueSize, sizeof(ButtonEvent));
  if (s_buttonQueue == nullptr) {
    OS_LOGE(TAG, "Failed to create encoder button queue");
    vQueueDelete(s_rotationQueue);
    s_rotationQueue = nullptr;
    return false;
  }

  s_pinA = pinA;
  s_pinB = pinB;
  s_buttonPin = buttonPin;
  s_middleButtonPin = middleButtonPin;
  s_leftButtonPin = leftButtonPin;
  s_rightButtonPin = rightButtonPin;

  if (TaskUtils::TaskCreateExpensive(encoderTask, "rotary_enc", kTaskStackSize, nullptr, 1, &s_task) != pdPASS) {
    OS_LOGE(TAG, "Failed to create encoder task");
    vQueueDelete(s_rotationQueue);
    s_rotationQueue = nullptr;
    vQueueDelete(s_buttonQueue);
    s_buttonQueue = nullptr;
    return false;
  }

  s_initialized.store(true, std::memory_order_release);
  OS_LOGI(TAG, "Rotary encoder initialized (A=%d, B=%d, BTN=%d, MID=%d, LEFT=%d, RIGHT=%d, max=%u)", static_cast<int>(pinA), static_cast<int>(pinB), static_cast<int>(buttonPin), static_cast<int>(middleButtonPin), static_cast<int>(leftButtonPin), static_cast<int>(rightButtonPin), static_cast<unsigned>(kMaxLimit));

  return true;
}

bool RotaryEncoderManager::TryPopRotationEvent(RotationEvent& out)
{
  if (s_rotationQueue == nullptr) {
    return false;
  }

  return xQueueReceive(s_rotationQueue, &out, 0) == pdTRUE;
}

bool RotaryEncoderManager::TryPopButtonEvent(ButtonEvent& out)
{
  if (s_buttonQueue == nullptr) {
    return false;
  }

  return xQueueReceive(s_buttonQueue, &out, 0) == pdTRUE;
}

uint8_t RotaryEncoderManager::GetMaxIntensityLimit()
{
  return s_maxIntensityLimit.load(std::memory_order_relaxed);
}

void RotaryEncoderManager::SetMaxIntensityLimit(uint8_t limit)
{
  const uint8_t next = static_cast<uint8_t>(std::clamp<int>(limit, 0, kMaxLimit));
  s_maxIntensityLimit.store(next, std::memory_order_relaxed);
}
