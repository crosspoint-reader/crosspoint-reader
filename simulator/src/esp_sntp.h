#pragma once
inline bool esp_sntp_enabled() { return true; }
inline void esp_sntp_setoperatingmode(int) {}
inline void esp_sntp_setservername(int, const char *) {}
inline void esp_sntp_init() {}
inline void esp_sntp_stop() {}
inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval *)) {}
#define ESP_SNTP_OPMODE_POLL 0
#define SNTP_OPMODE_POLL 0

#define SNTP_SYNC_STATUS_COMPLETED 1
inline int sntp_get_sync_status() { return 1; }
