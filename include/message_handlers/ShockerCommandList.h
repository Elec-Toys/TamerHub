#pragma once

#include "serialization/_fbs/ShockerCommand_generated.h"

namespace OpenShock::MessageHandlers {
  void HandleShockerCommandList(const OpenShock::Serialization::Common::ShockerCommandList* cmdList, bool fromGateway = false);
}  // namespace OpenShock::MessageHandlers
