#include "message_handlers/ShockerCommandList.h"

const char* const TAG = "ShockerCommandHandler";

#include "CommandHandler.h"
#include "GatewayConnectionManager.h"
#include "Logging.h"
#include "ShockerModelType.h"
#include "visual/OledDisplayManager.h"

#include <cstdint>

using FbsModelType   = OpenShock::Serialization::Types::ShockerModelType;
using FbsCommandType = OpenShock::Serialization::Types::ShockerCommandType;

void OpenShock::MessageHandlers::HandleShockerCommandList(const OpenShock::Serialization::Common::ShockerCommandList* cmdList, bool fromGateway)
{
  auto commands = cmdList->commands();
  if (commands == nullptr) {
    OS_LOGE(TAG, "Received invalid command list");
    return;
  }

  OS_LOGV(TAG, "Received command list (%u commands)", commands->size());

  for (auto command : *commands) {
    uint16_t id                   = command->id();
    uint8_t intensity             = command->intensity();
    uint16_t durationMs           = command->duration();
    FbsModelType fbsModel         = command->model();
    FbsCommandType fbsCommandType = command->type();

    ShockerModelType model;
    switch (fbsModel) {
      case FbsModelType::CaiXianlin:
        model = ShockerModelType::CaiXianlin;
        break;
      case FbsModelType::Petrainer:
        model = ShockerModelType::Petrainer;
        break;
      case FbsModelType::Petrainer998DR:
        model = ShockerModelType::Petrainer998DR;
        break;
      case FbsModelType::WellturnT330:
        model = ShockerModelType::WellturnT330;
        break;
      default:
        OS_LOGE(TAG, "Unsupported shocker model: %s", OpenShock::Serialization::Types::EnumNameShockerModelType(fbsModel));
        continue;
    }

    ShockerCommandType commandType;
    switch (fbsCommandType) {
      case FbsCommandType::Stop:
        commandType = ShockerCommandType::Stop;
        break;
      case FbsCommandType::Shock:
        commandType = ShockerCommandType::Shock;
        break;
      case FbsCommandType::Vibrate:
        commandType = ShockerCommandType::Vibrate;
        break;
      case FbsCommandType::Sound:
        commandType = ShockerCommandType::Sound;
        break;
      default:
        OS_LOGE(TAG, "Unsupported command type: %s", OpenShock::Serialization::Types::EnumNameShockerCommandType(fbsCommandType));
        continue;
    }

    if (fromGateway) {
      uint16_t mappedId = id;
      if (!GatewayConnectionManager::ResolveOnlineRfId(id, mappedId)) {
        // Unknown shocker ID — the cached online shocker list is stale (e.g. a shocker was
        // just added on the backend). Refresh it immediately and retry before giving up.
        OS_LOGW(TAG, "Shocker ID %u not found in cached online list, refreshing and retrying", id);
        if (GatewayConnectionManager::RefreshOnlineShockers()) {
          GatewayConnectionManager::ResolveOnlineRfId(id, mappedId);
        }
      }
      id = mappedId;
    }

    // Keep OLED state synchronized with web-triggered commands.
    if (fromGateway) {
      OpenShock::OledDisplayManager::NotifyGatewayShockerCommand(id, intensity, commandType, durationMs);
    }

    if (!CommandHandler::HandleCommand(model, id, commandType, intensity, durationMs)) {
      OS_LOGE(TAG, "Command failed/rejected!");
    }
  }
}
