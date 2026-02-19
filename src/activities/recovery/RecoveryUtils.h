#pragma once

#include <esp_partition.h>

namespace Recovery {

void getPartitions(const esp_partition_t*& appPartition, const esp_partition_t*& recoveryPartition);
void reboot(bool toRecovery);

}; // namespace Recovery
