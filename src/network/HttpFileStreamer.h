#pragma once

#include <HalStorage.h>
#include <NetworkClient.h>

namespace HttpFileStreamer {
bool streamFileToClient(FsFile& file, NetworkClient& client);
}
