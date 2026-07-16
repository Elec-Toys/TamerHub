#include "NetworkTimeManager.h"

const char* const TAG = "NetworkTimeManager";

#include "Common.h"
#include "Core.h"
#include "http/HTTPRequestManager.h"
#include "Logging.h"
#include "util/TaskUtils.h"
#include "wifi/WiFiManager.h"

#include <Arduino.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cJSON.h>
#include <cstring>
#include <ctime>
#include <string_view>

namespace {
  using namespace OpenShock;

  // Auto-detects the caller's timezone UTC offset (DST already applied) from its public IP.
  // Actual wall-clock time comes from SNTP below, not this API — third-party "current time"
  // APIs have their own server clocks, which can (and did, e.g. timeapi.io) drift several
  // minutes behind real time. SNTP talks directly to purpose-built time servers instead.
  constexpr const char* kGeoApiUrl  = "https://ipapi.co/json/";
  constexpr const char* kNtpServer1 = "pool.ntp.org";
  constexpr const char* kNtpServer2 = "time.nist.gov";

  constexpr uint32_t kGeoApiTimeoutMs = 8'000;
  constexpr int64_t kRetryIntervalMs  = 30'000;        // Retry cadence until the first successful geo lookup.
  constexpr int64_t kResyncIntervalMs = 3'600'000;     // Re-check cadence once known (1h) — covers the device moving to a new timezone.
  constexpr time_t kMinPlausibleEpoch = 1'700'000'000; // 2023-11-14 — anything earlier means SNTP hasn't synced yet.

  std::atomic<bool> s_enabled { false };
  std::atomic<bool> s_fetchInProgress { false };
  std::atomic<bool> s_offsetKnown { false };
  std::atomic<bool> s_sntpStarted { false };
  std::atomic<int64_t> s_lastFetchAttemptMs { 0 };
  std::atomic<int32_t> s_utcOffsetSeconds { 0 };

  struct GeoResult {
    int32_t utcOffsetSeconds = 0;
    bool valid               = false;
  };

  // Parses ipapi.co's "utc_offset" field, formatted as "+HHMM" / "-HHMM" (e.g. "+0200", "-0530").
  bool TryParseUtcOffset(const char* str, int32_t& outSeconds)
  {
    if (str == nullptr || (str[0] != '+' && str[0] != '-')) {
      return false;
    }

    if (std::strlen(str) != 5 || !std::isdigit(static_cast<unsigned char>(str[1])) || !std::isdigit(static_cast<unsigned char>(str[2])) || !std::isdigit(static_cast<unsigned char>(str[3])) || !std::isdigit(static_cast<unsigned char>(str[4]))) {
      return false;
    }

    const int hours   = (str[1] - '0') * 10 + (str[2] - '0');
    const int minutes = (str[3] - '0') * 10 + (str[4] - '0');
    const int32_t total = hours * 3600 + minutes * 60;

    outSeconds = (str[0] == '-') ? -total : total;
    return true;
  }

  bool TryFetchUtcOffset(int32_t& outSeconds)
  {
    auto response = OpenShock::HTTP::GetJSON<GeoResult>(
      kGeoApiUrl,
      {
        {"Accept",     "application/json"},
        {"User-Agent", OpenShock::Constants::FW_USERAGENT}
      },
      [](int, const cJSON* json, GeoResult& out) -> bool {
        const cJSON* errorItem = cJSON_GetObjectItemCaseSensitive(json, "error");
        if (cJSON_IsTrue(errorItem)) {
          return false;
        }

        const cJSON* offsetItem = cJSON_GetObjectItemCaseSensitive(json, "utc_offset");
        if (!cJSON_IsString(offsetItem) || offsetItem->valuestring == nullptr) {
          return false;
        }

        if (!TryParseUtcOffset(offsetItem->valuestring, out.utcOffsetSeconds)) {
          return false;
        }

        out.valid = true;
        return true;
      },
      std::array<uint16_t, 1> {200},
      kGeoApiTimeoutMs
    );

    if (response.result != OpenShock::HTTP::RequestResult::Success || !response.data.valid) {
      OS_LOGW(TAG, "Failed to fetch UTC offset: %s [%d]", response.ResultToString(), response.code);
      return false;
    }

    outSeconds = response.data.utcOffsetSeconds;
    return true;
  }

  void networkTimeFetchTask(void*)
  {
    int32_t offsetSeconds = 0;
    if (TryFetchUtcOffset(offsetSeconds)) {
      s_utcOffsetSeconds.store(offsetSeconds, std::memory_order_relaxed);
      s_offsetKnown.store(true, std::memory_order_relaxed);
      OS_LOGI(TAG, "UTC offset synced: %+d minutes", static_cast<int>(offsetSeconds / 60));
    }

    s_fetchInProgress.store(false, std::memory_order_relaxed);
    vTaskDelete(nullptr);
  }
}  // namespace

void OpenShock::NetworkTimeManager::SetEnabled(bool enabled)
{
  s_enabled.store(enabled, std::memory_order_relaxed);
}

void OpenShock::NetworkTimeManager::Update()
{
  if (!s_enabled.load(std::memory_order_relaxed)) {
    return;
  }

  if (!OpenShock::WiFiManager::IsConnected()) {
    return;
  }

  // SNTP syncs the system clock in the background once started (and keeps it in sync
  // for as long as the device is up) — this only needs to be kicked off once. It is the
  // actual source of wall-clock time; the geo lookup below only supplies the UTC offset.
  if (!s_sntpStarted.exchange(true, std::memory_order_relaxed)) {
    configTime(0, 0, kNtpServer1, kNtpServer2);
  }

  if (s_fetchInProgress.load(std::memory_order_relaxed)) {
    return;
  }

  const bool known              = s_offsetKnown.load(std::memory_order_relaxed);
  const int64_t retryIntervalMs = known ? kResyncIntervalMs : kRetryIntervalMs;
  const int64_t lastAttempt     = s_lastFetchAttemptMs.load(std::memory_order_relaxed);
  const int64_t now             = OpenShock::millis();

  if (lastAttempt != 0 && (now - lastAttempt) < retryIntervalMs) {
    return;
  }

  s_lastFetchAttemptMs.store(now, std::memory_order_relaxed);
  s_fetchInProgress.store(true, std::memory_order_relaxed);

  TaskHandle_t task = nullptr;
  if (OpenShock::TaskUtils::TaskCreateExpensive(networkTimeFetchTask, "net_time_fetch", 8192, nullptr, 1, &task) != pdPASS) {
    OS_LOGE(TAG, "Failed to create network time fetch task");
    s_fetchInProgress.store(false, std::memory_order_relaxed);
  }
}

bool OpenShock::NetworkTimeManager::IsTimeKnown()
{
  if (!s_offsetKnown.load(std::memory_order_relaxed)) {
    return false;
  }

  return time(nullptr) >= kMinPlausibleEpoch;
}

bool OpenShock::NetworkTimeManager::GetCurrentLocalTime(uint8_t& hour24Out, uint8_t& minuteOut)
{
  if (!IsTimeKnown()) {
    return false;
  }

  const time_t nowUtc         = time(nullptr);
  const int32_t offsetSeconds = s_utcOffsetSeconds.load(std::memory_order_relaxed);
  const int64_t secOfDay      = ((static_cast<int64_t>(nowUtc) + offsetSeconds) % 86400 + 86400) % 86400;

  hour24Out = static_cast<uint8_t>(secOfDay / 3600);
  minuteOut = static_cast<uint8_t>((secOfDay / 60) % 60);
  return true;
}
