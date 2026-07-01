#pragma once

#include "AccountLinkResultCode.h"
#include "ShockerModelType.h"
#include "span.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace OpenShock::GatewayConnectionManager {
  struct OnlineShockerInfo {
    std::string id;
    std::string displayName;
    uint16_t sourceRfId;
    uint16_t mappedRfId;
    OpenShock::ShockerModelType model;
    bool disabled;
    uint8_t limit;
  };

  [[nodiscard]] bool Init();

  bool IsConnected();
  void Disconnect();
  void SetReconnectDelay(int64_t delayMs);

  bool IsLinked();
  AccountLinkResultCode Link(std::string_view linkCode, bool persistConfig = true);
  void UnLink();

  bool SendMessageTXT(std::string_view data);
  bool SendMessageBIN(tcb::span<const uint8_t> data);

  void MarkPingReceived();

  std::vector<OnlineShockerInfo> GetOnlineShockers();
  uint16_t ResolveOnlineRfId(uint16_t sourceRfId);
  bool IsOnlineRfIdReserved(uint16_t rfId);
  void SetLocalRfIds(tcb::span<const uint16_t> rfIds);
  bool SetOnlineShockerDisplayName(std::string_view id, std::string_view displayName);
  bool SetOnlineShockerDisabled(std::string_view id, bool disabled);
  bool SetOnlineShockerLimit(std::string_view id, uint8_t limit);
  bool RemoveOnlineShocker(std::string_view id);

  void Update();
}  // namespace OpenShock::GatewayConnectionManager
