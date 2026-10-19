#pragma once
#include "esp_partition.h"
const esp_partition_t* esp_ota_get_running_partition();
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*);
