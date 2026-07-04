#pragma once

#include <cstdint>

namespace OpenShock::NetworkTimeManager {
  // Enables/disables network time syncing. When disabled, no fetch attempts are made
  // and IsTimeKnown() will keep reporting whatever it last reported (does not reset).
  void SetEnabled(bool enabled);

  // Call periodically (cheap, non-blocking) — kicks off a background fetch task when
  // conditions are met (enabled, WiFi connected, and due for a (re)try).
  void Update();

  // True once a wall-clock time has been successfully fetched from the network at least
  // once. Never becomes false again after that (even if WiFi later disconnects), until reboot.
  bool IsTimeKnown();

  // Fills in the current local (network-timezone + DST aware) 24h wall-clock time.
  // Returns false (and leaves outputs untouched) if IsTimeKnown() is false.
  bool GetCurrentLocalTime(uint8_t& hour24Out, uint8_t& minuteOut);
}  // namespace OpenShock::NetworkTimeManager
