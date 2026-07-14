#pragma once

#include <cstdint>
#include <string_view>

#include "span.h"

namespace OpenShock::CaptivePortal {
  [[nodiscard]] bool Init();

  /// @brief Re-applies the persisted AP/captive-portal-enabled state. Init() forces AP + captive
  ///        portal off regardless of what's persisted (to avoid a boot-time crash when AP was
  ///        saved on while STA was off) — call this a few seconds after boot to restore the
  ///        user's actual saved preference.
  void ApplyPersistedState();

  void SetEnabled(bool enabled);
  bool IsEnabled();

  void SetApEnabled(bool enabled, bool persistConfig = true);
  bool IsApEnabled();

  void SetAlwaysEnabled(bool alwaysEnabled, bool persistConfig = true);
  bool IsAlwaysEnabled();

  /// @brief Signal that the user has completed setup. The portal will close once the device is fully online.
  void SetUserDone();

  bool ForceClose(uint32_t timeoutMs);

  bool IsRunning();

  bool SendMessageTXT(uint8_t socketId, std::string_view data);
  bool SendMessageBIN(uint8_t socketId, tcb::span<const uint8_t> data);

  bool BroadcastMessageTXT(std::string_view data);
  bool BroadcastMessageBIN(tcb::span<const uint8_t> data);
}  // namespace OpenShock::CaptivePortal
