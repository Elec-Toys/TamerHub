#pragma once

#include "FirmwareBootType.h"
#include "OtaUpdateChannel.h"
#include "SemVer.h"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace OpenShock::OtaUpdateManager {
  [[nodiscard]] bool Init();

  struct FirmwareRelease {
    std::string appBinaryUrl;
    uint8_t appBinaryHash[32];
    std::string filesystemBinaryUrl;
    uint8_t filesystemBinaryHash[32];
  };

  bool TryGetLatestVersion(OpenShock::SemVer& version);
  bool TryGetFirmwareRelease(const OpenShock::SemVer& version, FirmwareRelease& release);

  const char* GetCachedLatestVersion();

  // Manual check trigger — signals the update task to check right now.
  void TriggerManualCheck();

  // Persistent OTA settings (stored in NVS, not flatbuffers).
  bool GetOtaUpdateSettings(bool& autoUpdate, bool& promptUpdates, bool& neverPrompt);
  bool SetOtaUpdateSettings(bool autoUpdate, bool promptUpdates, bool neverPrompt);
  bool GetOtaRepoSlug(char* buf, size_t len);
  bool SetOtaRepoSlug(const char* slug);

  // Update prompt handshake between OTA task and OLED.
  // Returns true (and fills versionBuf) when waiting for the user's decision.
  bool HasPendingUpdatePrompt(char* versionBuf = nullptr, size_t len = 0);
  // decision: 0=No, 1=Never, 2=Yes
  void SetUserUpdateDecision(int8_t decision);

  // Last check result: 0=idle, 1=checking, 2=upToDate, 3=failed, 4=noNetwork
  int8_t GetLastCheckStatus();

  bool TryStartFirmwareUpdate(const OpenShock::SemVer& version);

  bool IsUpdateInProgress();

  FirmwareBootType GetFirmwareBootType();
  bool IsValidatingApp();

  void InvalidateAndRollback();
  void ValidateApp();
}  // namespace OpenShock::OtaUpdateManager
