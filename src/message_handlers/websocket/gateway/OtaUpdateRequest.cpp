#include "message_handlers/impl/WSGateway.h"

const char* const TAG = "ServerMessageHandlers";

#include "captiveportal/Manager.h"
#include "config/Config.h"
#include "Logging.h"
#include "OtaUpdateManager.h"
#include "wifi/WiFiManager.h"

#include <cstdint>

using namespace OpenShock::MessageHandlers::Server;

void _Private::HandleOtaUpdateRequest(const OpenShock::Serialization::Gateway::GatewayToHubMessage* root)
{
  auto msg = root->payload_as_OtaUpdateRequest();
  if (msg == nullptr) {
    OS_LOGE(TAG, "Payload cannot be parsed as OtaUpdate");
    return;
  }

  Config::OtaUpdateConfig cfg;
  if (!Config::GetOtaUpdateConfig(cfg) || !cfg.allowBackendManagement) {
    OS_LOGW(TAG, "OTA update request ignored: backend management is disabled");
    return;
  }

  if (!OpenShock::WiFiManager::IsConnected()) {
    OS_LOGW(TAG, "OTA update request ignored: no network");
    return;
  }

  OS_LOGI(TAG, "Gateway triggered OTA check — checking GitHub for latest version");
  OpenShock::OtaUpdateManager::TriggerManualCheck();
}
