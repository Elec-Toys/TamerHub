#include "NetworkTimeManager.h"

const char* const TAG = "NetworkTimeManager";

#include "Common.h"
#include "Core.h"
#include "http/HTTPRequestManager.h"
#include "Logging.h"
#include "util/TaskUtils.h"
#include "wifi/WiFiManager.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cJSON.h>
#include <string_view>

namespace {
  using namespace OpenShock;

  constexpr const char* kTimeApiUrl        = "https://worldtimeapi.org/api/ip.json";
  constexpr uint32_t kTimeApiTimeoutMs     = 8'000;
  constexpr int64_t kRetryIntervalMs       = 30'000;    // Retry cadence until the first successful fetch.
  constexpr int64_t kResyncIntervalMs      = 3'600'000; // Opportunistic resync cadence once time is known (1h).

  std::atomic<bool> s_enabled { false };
  std::atomic<bool> s_fetchInProgress { false };
  std::atomic<bool> s_timeKnown { false };
  std::atomic<int64_t> s_lastFetchAttemptMs { 0 };
  std::atomic<int64_t> s_localEpochAtFetch { 0 };
  std::atomic<int64_t> s_fetchMillisAnchor { 0 };

  struct TimeApiResult {
    int64_t unixtimeSeconds  = 0;
    int32_t utcOffsetSeconds = 0;
    bool valid                = false;
  };

  bool TryParseUtcOffset(std::string_view offset, int32_t& outSeconds)
  {
    // Expected format: "+HH:MM" or "-HH:MM"
    if (offset.size() < 6 || (offset[0] != '+' && offset[0] != '-')) {
      return false;
    }
    if (!isdigit(static_cast<unsigned char>(offset[1])) || !isdigit(static_cast<unsigned char>(offset[2])) || offset[3] != ':' || !isdigit(static_cast<unsigned char>(offset[4])) || !isdigit(static_cast<unsigned char>(offset[5]))) {
      return false;
    }

    const int sign = (offset[0] == '-') ? -1 : 1;
    const int hh   = (offset[1] - '0') * 10 + (offset[2] - '0');
    const int mm   = (offset[4] - '0') * 10 + (offset[5] - '0');

    outSeconds = sign * ((hh * 3600) + (mm * 60));
    return true;
  }

  void networkTimeFetchTask(void*)
  {
    auto response = OpenShock::HTTP::GetJSON<TimeApiResult>(
      kTimeApiUrl,
      {
        {"Accept",     "application/json"},
        {"User-Agent", OpenShock::Constants::FW_USERAGENT}
      },
      [](int, const cJSON* json, TimeApiResult& out) -> bool {
        const cJSON* unixtimeItem = cJSON_GetObjectItemCaseSensitive(json, "unixtime");
        const cJSON* offsetItem   = cJSON_GetObjectItemCaseSensitive(json, "utc_offset");

        if (!cJSON_IsNumber(unixtimeItem)) {
          return false;
        }
        if (!cJSON_IsString(offsetItem) || offsetItem->valuestring == nullptr) {
          return false;
        }

        int32_t offsetSeconds = 0;
        if (!TryParseUtcOffset(offsetItem->valuestring, offsetSeconds)) {
          return false;
        }

        out.unixtimeSeconds  = static_cast<int64_t>(unixtimeItem->valuedouble);
        out.utcOffsetSeconds = offsetSeconds;
        out.valid            = true;
        return true;
      },
      std::array<uint16_t, 1> {200},
      kTimeApiTimeoutMs
    );

    if (response.result == OpenShock::HTTP::RequestResult::Success && response.data.valid) {
      s_localEpochAtFetch.store(response.data.unixtimeSeconds + response.data.utcOffsetSeconds, std::memory_order_relaxed);
      s_fetchMillisAnchor.store(OpenShock::millis(), std::memory_order_relaxed);
      s_timeKnown.store(true, std::memory_order_relaxed);
      OS_LOGI(TAG, "Network time synced (utc_offset applied)");
    } else {
      OS_LOGW(TAG, "Failed to fetch network time: %s [%d]", response.ResultToString(), response.code);
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

  if (s_fetchInProgress.load(std::memory_order_relaxed)) {
    return;
  }

  if (!OpenShock::WiFiManager::IsConnected()) {
    return;
  }

  const bool known               = s_timeKnown.load(std::memory_order_relaxed);
  const int64_t retryIntervalMs  = known ? kResyncIntervalMs : kRetryIntervalMs;
  const int64_t lastAttempt      = s_lastFetchAttemptMs.load(std::memory_order_relaxed);
  const int64_t now              = OpenShock::millis();

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
  return s_timeKnown.load(std::memory_order_relaxed);
}

bool OpenShock::NetworkTimeManager::GetCurrentLocalTime(uint8_t& hour24Out, uint8_t& minuteOut)
{
  if (!s_timeKnown.load(std::memory_order_relaxed)) {
    return false;
  }

  const int64_t elapsedSeconds = (OpenShock::millis() - s_fetchMillisAnchor.load(std::memory_order_relaxed)) / 1000;
  const int64_t localEpoch     = s_localEpochAtFetch.load(std::memory_order_relaxed) + elapsedSeconds;
  const int64_t secOfDay       = ((localEpoch % 86400) + 86400) % 86400;

  hour24Out = static_cast<uint8_t>(secOfDay / 3600);
  minuteOut = static_cast<uint8_t>((secOfDay / 60) % 60);
  return true;
}
