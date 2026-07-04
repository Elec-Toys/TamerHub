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
#include <string>
#include <string_view>

namespace {
  using namespace OpenShock;

  // Step 1: auto-detects the caller's timezone from its public IP.
  // Note: the shared HTTP client (HTTPRequestManager) only supports https:// URLs — it hardcodes
  // the "https" protocol check in beginInternal(), so plain http:// requests always fail.
  constexpr const char* kGeoApiUrl = "https://ipapi.co/json/";
  // Step 2: resolves the current wall-clock time (DST already applied) for a named IANA timezone.
  constexpr const char* kZoneTimeApiUrlPrefix = "https://timeapi.io/api/time/current/zone?timeZone=";

  constexpr uint32_t kTimeApiTimeoutMs = 8'000;
  constexpr int64_t kRetryIntervalMs   = 30'000;    // Retry cadence until the first successful fetch.
  constexpr int64_t kResyncIntervalMs  = 3'600'000; // Opportunistic resync cadence once time is known (1h).

  std::atomic<bool> s_enabled { false };
  std::atomic<bool> s_fetchInProgress { false };
  std::atomic<bool> s_timeKnown { false };
  std::atomic<int64_t> s_lastFetchAttemptMs { 0 };
  std::atomic<int32_t> s_secondsOfDayAtFetch { 0 };
  std::atomic<int64_t> s_fetchMillisAnchor { 0 };

  struct GeoResult {
    std::string timezone;
    bool valid = false;
  };

  struct ZoneTimeResult {
    int hour   = 0;
    int minute = 0;
    int second = 0;
    bool valid  = false;
  };

  std::string PercentEncode(std::string_view input)
  {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out.push_back(static_cast<char>(c));
      } else {
        out.push_back('%');
        out.push_back(kHex[(c >> 4) & 0xF]);
        out.push_back(kHex[c & 0xF]);
      }
    }
    return out;
  }

  bool TryFetchTimezone(std::string& outTimezone)
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

        const cJSON* zoneItem = cJSON_GetObjectItemCaseSensitive(json, "timezone");
        if (!cJSON_IsString(zoneItem) || zoneItem->valuestring == nullptr) {
          return false;
        }

        out.timezone = zoneItem->valuestring;
        out.valid    = true;
        return true;
      },
      std::array<uint16_t, 1> {200},
      kTimeApiTimeoutMs
    );

    if (response.result != OpenShock::HTTP::RequestResult::Success || !response.data.valid) {
      OS_LOGW(TAG, "Failed to fetch timezone: %s [%d]", response.ResultToString(), response.code);
      return false;
    }

    outTimezone = std::move(response.data.timezone);
    return true;
  }

  bool TryFetchZoneTime(const std::string& timezone, ZoneTimeResult& outTime)
  {
    const std::string url = std::string(kZoneTimeApiUrlPrefix) + PercentEncode(timezone);

    auto response = OpenShock::HTTP::GetJSON<ZoneTimeResult>(
      url,
      {
        {"Accept",     "application/json"},
        {"User-Agent", OpenShock::Constants::FW_USERAGENT}
      },
      [](int, const cJSON* json, ZoneTimeResult& out) -> bool {
        const cJSON* hourItem   = cJSON_GetObjectItemCaseSensitive(json, "hour");
        const cJSON* minuteItem = cJSON_GetObjectItemCaseSensitive(json, "minute");
        const cJSON* secondItem = cJSON_GetObjectItemCaseSensitive(json, "seconds");

        if (!cJSON_IsNumber(hourItem) || !cJSON_IsNumber(minuteItem)) {
          return false;
        }

        out.hour   = hourItem->valueint;
        out.minute = minuteItem->valueint;
        out.second = cJSON_IsNumber(secondItem) ? secondItem->valueint : 0;
        out.valid  = true;
        return true;
      },
      std::array<uint16_t, 1> {200},
      kTimeApiTimeoutMs
    );

    if (response.result != OpenShock::HTTP::RequestResult::Success || !response.data.valid) {
      OS_LOGW(TAG, "Failed to fetch zone time: %s [%d]", response.ResultToString(), response.code);
      return false;
    }

    outTime = response.data;
    return true;
  }

  void networkTimeFetchTask(void*)
  {
    std::string timezone;
    ZoneTimeResult zoneTime;

    if (TryFetchTimezone(timezone) && TryFetchZoneTime(timezone, zoneTime)) {
      const int32_t secOfDay = (zoneTime.hour * 3600) + (zoneTime.minute * 60) + zoneTime.second;
      s_secondsOfDayAtFetch.store(secOfDay, std::memory_order_relaxed);
      s_fetchMillisAnchor.store(OpenShock::millis(), std::memory_order_relaxed);
      s_timeKnown.store(true, std::memory_order_relaxed);
      OS_LOGI(TAG, "Network time synced for zone %s", timezone.c_str());
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
  const int64_t secOfDay       = ((s_secondsOfDayAtFetch.load(std::memory_order_relaxed) + elapsedSeconds) % 86400 + 86400) % 86400;

  hour24Out = static_cast<uint8_t>(secOfDay / 3600);
  minuteOut = static_cast<uint8_t>((secOfDay / 60) % 60);
  return true;
}
